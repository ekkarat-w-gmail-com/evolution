/*
 * e-web-view.c
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) version 3.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with the program; if not, see <http://www.gnu.org/licenses/>
 *
 */

#include "e-web-view.h"

#include <config.h>
#include <glib/gi18n-lib.h>

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <pango/pango.h>

#include <camel/camel.h>
#include <libebackend/libebackend.h>

#define LIBSOUP_USE_UNSTABLE_REQUEST_API
#include <libsoup/soup.h>
#include <libsoup/soup-requester.h>

#include "e-alert-dialog.h"
#include "e-alert-sink.h"
#include "e-file-request.h"
#include "e-misc-utils.h"
#include "e-plugin-ui.h"
#include "e-popup-action.h"
#include "e-selectable.h"
#include "e-stock-request.h"

#include "../web-extensions/evolution-web-extension.h"

#define E_WEB_VIEW_GET_PRIVATE(obj) \
	(G_TYPE_INSTANCE_GET_PRIVATE \
	((obj), E_TYPE_WEB_VIEW, EWebViewPrivate))

typedef struct _AsyncContext AsyncContext;

struct _EWebViewPrivate {
	GtkUIManager *ui_manager;
	gchar *selected_uri;
	gchar *cursor_image_src;

	GQueue highlights;

	GtkAction *open_proxy;
	GtkAction *print_proxy;
	GtkAction *save_as_proxy;

	/* Lockdown Options */
	gboolean disable_printing;
	gboolean disable_save_to_disk;

	gboolean caret_mode;

	GSettings *font_settings;
	gulong font_name_changed_handler_id;
	gulong monospace_font_name_changed_handler_id;

	GSettings *aliasing_settings;
	gulong antialiasing_changed_handler_id;

	GDBusProxy *web_extension;
	guint web_extension_watch_name_id;

	WebKitFindController *find_controller;
};

struct _AsyncContext {
	EActivity *activity;
	GFile *destination;
	GInputStream *input_stream;
};

enum {
	PROP_0,
	PROP_CARET_MODE,
	PROP_COPY_TARGET_LIST,
	PROP_CURSOR_IMAGE_SRC,
	PROP_DISABLE_PRINTING,
	PROP_DISABLE_SAVE_TO_DISK,
	PROP_INLINE_SPELLING,
	PROP_MAGIC_LINKS,
	PROP_MAGIC_SMILEYS,
	PROP_OPEN_PROXY,
	PROP_PRINT_PROXY,
	PROP_SAVE_AS_PROXY,
	PROP_SELECTED_URI
};

enum {
	NEW_ACTIVITY,
	POPUP_EVENT,
	STATUS_MESSAGE,
	STOP_LOADING,
	UPDATE_ACTIONS,
	PROCESS_MAILTO,
	LAST_SIGNAL
};

static guint signals[LAST_SIGNAL];

static const gchar *ui =
"<ui>"
"  <popup name='context'>"
"    <menuitem action='copy-clipboard'/>"
"    <separator/>"
"    <placeholder name='custom-actions-1'>"
"      <menuitem action='open'/>"
"      <menuitem action='save-as'/>"
"      <menuitem action='http-open'/>"
"      <menuitem action='send-message'/>"
"      <menuitem action='print'/>"
"    </placeholder>"
"    <placeholder name='custom-actions-2'>"
"      <menuitem action='uri-copy'/>"
"      <menuitem action='mailto-copy'/>"
"      <menuitem action='image-copy'/>"
"      <menuitem action='image-save'/>"
"    </placeholder>"
"    <placeholder name='custom-actions-3'/>"
"    <separator/>"
"    <menuitem action='select-all'/>"
"    <placeholder name='inspect-menu' />"
"  </popup>"
"</ui>";

/* Forward Declarations */
static void e_web_view_alert_sink_init (EAlertSinkInterface *interface);
static void e_web_view_selectable_init (ESelectableInterface *interface);

G_DEFINE_TYPE_WITH_CODE (
	EWebView,
	e_web_view,
	WEBKIT_TYPE_WEB_VIEW,
	G_IMPLEMENT_INTERFACE (
		E_TYPE_EXTENSIBLE, NULL)
	G_IMPLEMENT_INTERFACE (
		E_TYPE_ALERT_SINK,
		e_web_view_alert_sink_init)
	G_IMPLEMENT_INTERFACE (
		E_TYPE_SELECTABLE,
		e_web_view_selectable_init))

static void
async_context_free (AsyncContext *async_context)
{
	g_clear_object (&async_context->activity);
	g_clear_object (&async_context->destination);
	g_clear_object (&async_context->input_stream);

	g_slice_free (AsyncContext, async_context);
}

static void
action_copy_clipboard_cb (GtkAction *action,
                          EWebView *web_view)
{
	e_web_view_copy_clipboard (web_view);
}

static void
action_http_open_cb (GtkAction *action,
                     EWebView *web_view)
{
	const gchar *uri;
	gpointer parent;

	parent = gtk_widget_get_toplevel (GTK_WIDGET (web_view));
	parent = gtk_widget_is_toplevel (parent) ? parent : NULL;

	uri = e_web_view_get_selected_uri (web_view);
	g_return_if_fail (uri != NULL);

	e_show_uri (parent, uri);
}

static void
action_mailto_copy_cb (GtkAction *action,
                       EWebView *web_view)
{
	CamelURL *curl;
	CamelInternetAddress *inet_addr;
	GtkClipboard *clipboard;
	const gchar *uri;
	gchar *text;

	uri = e_web_view_get_selected_uri (web_view);
	g_return_if_fail (uri != NULL);

	/* This should work because we checked it in update_actions(). */
	curl = camel_url_new (uri, NULL);
	g_return_if_fail (curl != NULL);

	inet_addr = camel_internet_address_new ();
	camel_address_decode (CAMEL_ADDRESS (inet_addr), curl->path);
	text = camel_address_format (CAMEL_ADDRESS (inet_addr));
	if (text == NULL || *text == '\0')
		text = g_strdup (uri + strlen ("mailto:"));

	g_object_unref (inet_addr);
	camel_url_free (curl);

	clipboard = gtk_clipboard_get (GDK_SELECTION_PRIMARY);
	gtk_clipboard_set_text (clipboard, text, -1);
	gtk_clipboard_store (clipboard);

	clipboard = gtk_clipboard_get (GDK_SELECTION_CLIPBOARD);
	gtk_clipboard_set_text (clipboard, text, -1);
	gtk_clipboard_store (clipboard);

	g_free (text);
}

static void
action_select_all_cb (GtkAction *action,
                      EWebView *web_view)
{
	e_web_view_select_all (web_view);
}

static void
action_send_message_cb (GtkAction *action,
                        EWebView *web_view)
{
	const gchar *uri;
	gpointer parent;
	gboolean handled;

	parent = gtk_widget_get_toplevel (GTK_WIDGET (web_view));
	parent = gtk_widget_is_toplevel (parent) ? parent : NULL;

	uri = e_web_view_get_selected_uri (web_view);
	g_return_if_fail (uri != NULL);

	handled = FALSE;
	g_signal_emit (web_view, signals[PROCESS_MAILTO], 0, uri, &handled);

	if (!handled)
		e_show_uri (parent, uri);
}

static void
action_uri_copy_cb (GtkAction *action,
                    EWebView *web_view)
{
	GtkClipboard *clipboard;
	const gchar *uri;

	clipboard = gtk_clipboard_get (GDK_SELECTION_CLIPBOARD);
	uri = e_web_view_get_selected_uri (web_view);
	g_return_if_fail (uri != NULL);

	gtk_clipboard_set_text (clipboard, uri, -1);
	gtk_clipboard_store (clipboard);
}

static void
action_image_copy_cb (GtkAction *action,
                      EWebView *web_view)
{
	e_web_view_cursor_image_copy (web_view);
}

static void
action_image_save_cb (GtkAction *action,
                      EWebView *web_view)
{
	e_web_view_cursor_image_save (web_view);
}

static GtkActionEntry uri_entries[] = {

	{ "uri-copy",
	  GTK_STOCK_COPY,
	  N_("_Copy Link Location"),
	  NULL,
	  N_("Copy the link to the clipboard"),
	  G_CALLBACK (action_uri_copy_cb) }
};

static GtkActionEntry http_entries[] = {

	{ "http-open",
	  "emblem-web",
	  N_("_Open Link in Browser"),
	  NULL,
	  N_("Open the link in a web browser"),
	  G_CALLBACK (action_http_open_cb) }
};

static GtkActionEntry mailto_entries[] = {

	{ "mailto-copy",
	  GTK_STOCK_COPY,
	  N_("_Copy Email Address"),
	  NULL,
	  N_("Copy the email address to the clipboard"),
	  G_CALLBACK (action_mailto_copy_cb) },

	{ "send-message",
	  "mail-message-new",
	  N_("_Send New Message To..."),
	  NULL,
	  N_("Send a mail message to this address"),
	  G_CALLBACK (action_send_message_cb) }
};

static GtkActionEntry image_entries[] = {

	{ "image-copy",
	  GTK_STOCK_COPY,
	  N_("_Copy Image"),
	  NULL,
	  N_("Copy the image to the clipboard"),
	  G_CALLBACK (action_image_copy_cb) },

	{ "image-save",
	  GTK_STOCK_SAVE,
	  N_("Save _Image..."),
	  NULL,
	  N_("Save the image to a file"),
	  G_CALLBACK (action_image_save_cb) }
};

static GtkActionEntry selection_entries[] = {

	{ "copy-clipboard",
	  GTK_STOCK_COPY,
	  NULL,
	  NULL,
	  N_("Copy the selection"),
	  G_CALLBACK (action_copy_clipboard_cb) },
};

static GtkActionEntry standard_entries[] = {

	{ "select-all",
	  GTK_STOCK_SELECT_ALL,
	  NULL,
	  NULL,
	  N_("Select all text and images"),
	  G_CALLBACK (action_select_all_cb) }
};

static void
web_view_init_webkit_settings (EWebView *web_view)
{
	WebKitSettings *webkit_settings;

	webkit_settings =
		webkit_settings_new_with_settings (
			"enable-frame-flattening", TRUE,
			"auto-load-images", TRUE,
			"enable-page-cache", FALSE,
			"enable-java", FALSE,
			"enable-javascript", FALSE,
			"enable-plugins", FALSE,
			"enable-html5-database", FALSE,
			"enable-html5-local-storage", FALSE,
			"enable-private-browsing", TRUE,
			"enable-smooth-scrolling", TRUE,
			"enable-offline-web-application-cache", FALSE,
			"media-playback-allows-inline", FALSE,
			NULL);

	g_object_bind_property (
		webkit_settings, "enable-caret-browsing",
		web_view, "caret-mode",
		G_BINDING_BIDIRECTIONAL |
		G_BINDING_SYNC_CREATE);

	webkit_web_view_set_settings (
		WEBKIT_WEB_VIEW (web_view), webkit_settings);

	g_object_unref (webkit_settings);
}

static void
web_view_menu_item_select_cb (EWebView *web_view,
                              GtkWidget *widget)
{
	GtkAction *action;
	GtkActivatable *activatable;
	const gchar *tooltip;

	activatable = GTK_ACTIVATABLE (widget);
	action = gtk_activatable_get_related_action (activatable);
	tooltip = gtk_action_get_tooltip (action);

	if (tooltip == NULL)
		return;

	e_web_view_status_message (web_view, tooltip);
}

static void
webkit_find_controller_found_text_cb (WebKitFindController *find_controller,
                                      guint match_count,
                                      EWebView *web_view)
{
}

static void
webkit_find_controller_failed_to_found_text_cb (WebKitFindController *find_controller,
                                                EWebView *web_view)
{
}

static void
web_view_update_document_highlights (EWebView *web_view)
{
	WebKitFindController *find_controller;
	GList *head, *link;

	find_controller = web_view->priv->find_controller;

	head = g_queue_peek_head_link (&web_view->priv->highlights);

	for (link = head; link != NULL; link = g_list_next (link)) {
		webkit_find_controller_search (
			find_controller,
			link->data,
			WEBKIT_FIND_OPTIONS_NONE,
			G_MAXUINT);
	}
}

static void
web_view_set_find_controller (EWebView *web_view)
{
	WebKitFindController *find_controller;

	find_controller =
		webkit_web_view_get_find_controller (WEBKIT_WEB_VIEW (web_view));

	g_signal_connect (
		find_controller, "found-text",
		G_CALLBACK (webkit_find_controller_found_text_cb), web_view);

	g_signal_connect (
		find_controller, "failed-to-find-text",
		G_CALLBACK (webkit_find_controller_failed_to_found_text_cb), web_view);

	web_view->priv->find_controller = find_controller;
}

static void
web_view_menu_item_deselect_cb (EWebView *web_view)
{
	e_web_view_status_message (web_view, NULL);
}

static void
web_view_connect_proxy_cb (EWebView *web_view,
                           GtkAction *action,
                           GtkWidget *proxy)
{
	if (!GTK_IS_MENU_ITEM (proxy))
		return;

	g_signal_connect_swapped (
		proxy, "select",
		G_CALLBACK (web_view_menu_item_select_cb), web_view);

	g_signal_connect_swapped (
		proxy, "deselect",
		G_CALLBACK (web_view_menu_item_deselect_cb), web_view);
}

static gboolean
web_view_context_menu_cb (WebKitWebView *webkit_web_view,
                          GtkWidget *default_menu,
                          WebKitHitTestResult *hit_test_result,
                          gboolean triggered_with_keyboard)
{
	WebKitHitTestResultContext context;
	EWebView *web_view;
	gboolean event_handled = FALSE;
	gchar *link_uri = NULL;

	web_view = E_WEB_VIEW (webkit_web_view);

	g_free (web_view->priv->cursor_image_src);
	web_view->priv->cursor_image_src = NULL;

	if (hit_test_result == NULL)
		return FALSE;

	context = webkit_hit_test_result_get_context (hit_test_result);

	if (context & WEBKIT_HIT_TEST_RESULT_CONTEXT_IMAGE) {
		gchar *image_uri = NULL;

		g_object_get (hit_test_result, "image-uri", &image_uri, NULL);

		if (image_uri != NULL) {
			g_free (web_view->priv->cursor_image_src);
			web_view->priv->cursor_image_src = image_uri;
		}
	}

	if (context & WEBKIT_HIT_TEST_RESULT_CONTEXT_LINK)
		g_object_get (hit_test_result, "link-uri", &link_uri, NULL);

	g_signal_emit (
		web_view,
		signals[POPUP_EVENT], 0,
		link_uri, &event_handled);

	g_free (link_uri);

	return event_handled;
}

static GtkWidget *
web_view_create_plugin_widget_cb (EWebView *web_view,
                                  const gchar *mime_type,
                                  const gchar *uri,
                                  GHashTable *param)
{
	EWebViewClass *class;

	/* XXX WebKitWebView does not provide a class method for
	 *     this signal, so we do so we can override the default
	 *     behavior from subclasses for special URI types. */

	class = E_WEB_VIEW_GET_CLASS (web_view);
	g_return_val_if_fail (class->create_plugin_widget != NULL, NULL);

	return class->create_plugin_widget (web_view, mime_type, uri, param);
}

static void
web_view_mouse_target_changed_cb (EWebView *web_view,
                                  WebKitHitTestResult *hit_test_result,
                                  guint modifiers,
                                  gpointer user_data)
{
	EWebViewClass *class;
	const gchar *title, *uri;

	/* We can test there is we are hovering over link, image .. */
	if (!webkit_hit_test_result_context_is_image (hit_test_result))
		return;

	/* XXX WebKitWebView does not provide a class method for
	 *     this signal, so we do so we can override the default
	 *     behavior from subclasses for special URI types. */

	class = E_WEB_VIEW_GET_CLASS (web_view);
	g_return_if_fail (class->hovering_over_link != NULL);

	title = webkit_hit_test_result_get_link_title (hit_test_result);
	uri = webkit_hit_test_result_get_link_uri (hit_test_result);

	class->hovering_over_link (web_view, title, uri);
}

static gboolean
web_view_decide_policy_cb (EWebView *web_view,
                           WebKitPolicyDecision *decision,
                           WebKitPolicyDecisionType type)
{
	EWebViewClass *class;
	WebKitNavigationPolicyDecision *navigation_decision;
	WebKitURIRequest *request;
	const gchar *uri;

	if (type != WEBKIT_POLICY_DECISION_TYPE_NAVIGATION_ACTION)
		return FALSE;

	navigation_decision = WEBKIT_NAVIGATION_POLICY_DECISION (decision);

	if (navigation_decision != WEBKIT_NAVIGATION_TYPE_LINK_CLICKED)
		return FALSE;

	/* XXX WebKitWebView does not provide a class method for
	 *     this signal, so we do so we can override the default
	 *     behavior from subclasses for special URI types. */

	class = E_WEB_VIEW_GET_CLASS (web_view);
	g_return_val_if_fail (class->link_clicked != NULL, FALSE);

	webkit_policy_decision_ignore (decision);
	request = webkit_navigation_policy_decision_get_request (navigation_decision);
	uri = webkit_uri_request_get_uri (request);

	class->link_clicked (web_view, uri);

	return TRUE;
}

static void
style_updated_cb (EWebView *web_view)
{
	GdkRGBA color;
	gchar *color_value;
	gchar *style;
	GtkStateFlags state_flags;
	GtkStyleContext *style_context;
	GtkWidgetPath *widget_path;

	state_flags = gtk_widget_get_state_flags (GTK_WIDGET (web_view));
	style_context = gtk_style_context_new ();
	widget_path = gtk_widget_path_new ();
	gtk_widget_path_append_type (widget_path, GTK_TYPE_WINDOW);
	gtk_style_context_set_path (style_context, widget_path);
	gtk_style_context_add_class (style_context, GTK_STYLE_CLASS_ENTRY);

	gtk_style_context_get_background_color (
		style_context,
		state_flags | GTK_STATE_FLAG_FOCUSED,
		&color);
	color_value = g_strdup_printf ("#%06x", e_rgba_to_value (&color));
	style = g_strconcat ("background-color: ", color_value, ";", NULL);

	e_web_view_add_css_rule_into_style_sheet_sync (
		web_view,
		"-e-web-view-css-sheet",
		".-e-web-view-background-color",
		style,
		NULL,
		NULL);

	g_free (color_value);
	g_free (style);

	gtk_style_context_get_color (
		style_context,
		state_flags | GTK_STATE_FLAG_FOCUSED,
		&color);
	color_value = g_strdup_printf ("#%06x", e_rgba_to_value (&color));
	style = g_strconcat ("color: ", color_value, ";", NULL);

	e_web_view_add_css_rule_into_style_sheet_sync (
		web_view,
		"-e-web-view-css-sheet",
		".-e-web-view-text-color",
		style,
		NULL,
		NULL);

	gtk_widget_path_free (widget_path);
	g_object_unref (style_context);

	g_free (color_value);
	g_free (style);
}

static void
web_view_load_changed_cb (WebKitWebView *webkit_web_view,
                          WebKitLoadEvent load_event,
                          gpointer user_data)
{
	EWebView *web_view;

	web_view = E_WEB_VIEW (webkit_web_view);

	if (load_event != WEBKIT_LOAD_FINISHED)
       		return;

	style_updated_cb (web_view);

	web_view_update_document_highlights (web_view);

	/* Workaround webkit bug:
	 * https://bugs.webkit.org/show_bug.cgi?id=89553 */
	e_web_view_zoom_in (web_view);
	e_web_view_zoom_out (web_view);
}

static void
web_view_set_property (GObject *object,
                       guint property_id,
                       const GValue *value,
                       GParamSpec *pspec)
{
	switch (property_id) {
		case PROP_CARET_MODE:
			e_web_view_set_caret_mode (
				E_WEB_VIEW (object),
				g_value_get_boolean (value));
			return;

		case PROP_CURSOR_IMAGE_SRC:
			e_web_view_set_cursor_image_src (
				E_WEB_VIEW (object),
				g_value_get_string (value));
			return;

		case PROP_DISABLE_PRINTING:
			e_web_view_set_disable_printing (
				E_WEB_VIEW (object),
				g_value_get_boolean (value));
			return;

		case PROP_DISABLE_SAVE_TO_DISK:
			e_web_view_set_disable_save_to_disk (
				E_WEB_VIEW (object),
				g_value_get_boolean (value));
			return;

		case PROP_INLINE_SPELLING:
			e_web_view_set_inline_spelling (
				E_WEB_VIEW (object),
				g_value_get_boolean (value));
			return;

		case PROP_MAGIC_LINKS:
			e_web_view_set_magic_links (
				E_WEB_VIEW (object),
				g_value_get_boolean (value));
			return;

		case PROP_MAGIC_SMILEYS:
			e_web_view_set_magic_smileys (
				E_WEB_VIEW (object),
				g_value_get_boolean (value));
			return;

		case PROP_OPEN_PROXY:
			e_web_view_set_open_proxy (
				E_WEB_VIEW (object),
				g_value_get_object (value));
			return;

		case PROP_PRINT_PROXY:
			e_web_view_set_print_proxy (
				E_WEB_VIEW (object),
				g_value_get_object (value));
			return;

		case PROP_SAVE_AS_PROXY:
			e_web_view_set_save_as_proxy (
				E_WEB_VIEW (object),
				g_value_get_object (value));
			return;

		case PROP_SELECTED_URI:
			e_web_view_set_selected_uri (
				E_WEB_VIEW (object),
				g_value_get_string (value));
			return;
	}
	G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
}

static void
web_view_get_property (GObject *object,
                       guint property_id,
                       GValue *value,
                       GParamSpec *pspec)
{
	switch (property_id) {
		case PROP_CARET_MODE:
			g_value_set_boolean (
				value, e_web_view_get_caret_mode (
				E_WEB_VIEW (object)));
			return;

		case PROP_CURSOR_IMAGE_SRC:
			g_value_set_string (
				value, e_web_view_get_cursor_image_src (
				E_WEB_VIEW (object)));
			return;

		case PROP_DISABLE_PRINTING:
			g_value_set_boolean (
				value, e_web_view_get_disable_printing (
				E_WEB_VIEW (object)));
			return;

		case PROP_DISABLE_SAVE_TO_DISK:
			g_value_set_boolean (
				value, e_web_view_get_disable_save_to_disk (
				E_WEB_VIEW (object)));
			return;

		case PROP_INLINE_SPELLING:
			g_value_set_boolean (
				value, e_web_view_get_inline_spelling (
				E_WEB_VIEW (object)));
			return;

		case PROP_MAGIC_LINKS:
			g_value_set_boolean (
				value, e_web_view_get_magic_links (
				E_WEB_VIEW (object)));
			return;

		case PROP_MAGIC_SMILEYS:
			g_value_set_boolean (
				value, e_web_view_get_magic_smileys (
				E_WEB_VIEW (object)));
			return;

		case PROP_OPEN_PROXY:
			g_value_set_object (
				value, e_web_view_get_open_proxy (
				E_WEB_VIEW (object)));
			return;

		case PROP_PRINT_PROXY:
			g_value_set_object (
				value, e_web_view_get_print_proxy (
				E_WEB_VIEW (object)));
			return;

		case PROP_SAVE_AS_PROXY:
			g_value_set_object (
				value, e_web_view_get_save_as_proxy (
				E_WEB_VIEW (object)));
			return;

		case PROP_SELECTED_URI:
			g_value_set_string (
				value, e_web_view_get_selected_uri (
				E_WEB_VIEW (object)));
			return;

	}

	G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
}

static void
web_view_dispose (GObject *object)
{
	EWebViewPrivate *priv;

	priv = E_WEB_VIEW_GET_PRIVATE (object);

	if (priv->font_name_changed_handler_id > 0) {
		g_signal_handler_disconnect (
			priv->font_settings,
			priv->font_name_changed_handler_id);
		priv->font_name_changed_handler_id = 0;
	}

	if (priv->monospace_font_name_changed_handler_id > 0) {
		g_signal_handler_disconnect (
			priv->font_settings,
			priv->monospace_font_name_changed_handler_id);
		priv->monospace_font_name_changed_handler_id = 0;
	}

	if (priv->antialiasing_changed_handler_id > 0) {
		g_signal_handler_disconnect (
			priv->aliasing_settings,
			priv->antialiasing_changed_handler_id);
		priv->antialiasing_changed_handler_id = 0;
	}

	if (priv->web_extension_watch_name_id > 0) {
		g_bus_unwatch_name (priv->web_extension_watch_name_id);
		priv->web_extension_watch_name_id = 0;
	}

	g_clear_object (&priv->ui_manager);
	g_clear_object (&priv->open_proxy);
	g_clear_object (&priv->print_proxy);
	g_clear_object (&priv->save_as_proxy);
	g_clear_object (&priv->aliasing_settings);
	g_clear_object (&priv->font_settings);
	g_clear_object (&priv->web_extension);

	/* Chain up to parent's dispose() method. */
	G_OBJECT_CLASS (e_web_view_parent_class)->dispose (object);
}

static void
web_view_finalize (GObject *object)
{
	EWebViewPrivate *priv;

	priv = E_WEB_VIEW_GET_PRIVATE (object);

	g_free (priv->selected_uri);
	g_free (priv->cursor_image_src);

	while (!g_queue_is_empty (&priv->highlights))
		g_free (g_queue_pop_head (&priv->highlights));

	/* Chain up to parent's finalize() method. */
	G_OBJECT_CLASS (e_web_view_parent_class)->finalize (object);
}

static void
web_view_constructed (GObject *object)
{
#ifndef G_OS_WIN32
	GSettings *settings;

	settings = g_settings_new ("org.gnome.desktop.lockdown");

	g_settings_bind (
		settings, "disable-printing",
		object, "disable-printing",
		G_SETTINGS_BIND_GET);

	g_settings_bind (
		settings, "disable-save-to-disk",
		object, "disable-save-to-disk",
		G_SETTINGS_BIND_GET);

	g_object_unref (settings);
#endif

	e_extensible_load_extensions (E_EXTENSIBLE (object));

	/* Chain up to parent's constructed() method. */
	G_OBJECT_CLASS (e_web_view_parent_class)->constructed (object);
}

static gboolean
web_view_scroll_event (GtkWidget *widget,
                       GdkEventScroll *event)
{
	if (event->state & GDK_CONTROL_MASK) {
		GdkScrollDirection direction = event->direction;

		if (direction == GDK_SCROLL_SMOOTH) {
			static gdouble total_delta_y = 0.0;

			total_delta_y += event->delta_y;

			if (total_delta_y >= 1.0) {
				total_delta_y = 0.0;
				direction = GDK_SCROLL_DOWN;
			} else if (total_delta_y <= -1.0) {
				total_delta_y = 0.0;
				direction = GDK_SCROLL_UP;
			} else {
				return FALSE;
			}
		}

		switch (direction) {
			case GDK_SCROLL_UP:
				e_web_view_zoom_in (E_WEB_VIEW (widget));
				return TRUE;
			case GDK_SCROLL_DOWN:
				e_web_view_zoom_out (E_WEB_VIEW (widget));
				return TRUE;
			default:
				break;
		}
	}

	return FALSE;
}

static gboolean
web_view_drag_motion (GtkWidget *widget,
                      GdkDragContext *context,
                      gint x,
                      gint y,
                      guint time_)
{
	return FALSE;
}

static GtkWidget *
web_view_create_plugin_widget (EWebView *web_view,
                               const gchar *mime_type,
                               const gchar *uri,
                               GHashTable *param)
{
	GtkWidget *widget = NULL;

	if (g_strcmp0 (mime_type, "image/x-themed-icon") == 0) {
		GtkIconTheme *icon_theme;
		GdkPixbuf *pixbuf;
		gpointer data;
		glong size = 0;
		GError *error = NULL;

		icon_theme = gtk_icon_theme_get_default ();

		if (size == 0) {
			data = g_hash_table_lookup (param, "width");
			if (data != NULL)
				size = MAX (size, strtol (data, NULL, 10));
		}

		if (size == 0) {
			data = g_hash_table_lookup (param, "height");
			if (data != NULL)
				size = MAX (size, strtol (data, NULL, 10));
		}

		if (size == 0)
			size = 32;  /* arbitrary default */

		pixbuf = gtk_icon_theme_load_icon (
			icon_theme, uri, size, 0, &error);
		if (pixbuf != NULL) {
			widget = gtk_image_new_from_pixbuf (pixbuf);
			g_object_unref (pixbuf);
		} else if (error != NULL) {
			g_warning ("%s", error->message);
			g_error_free (error);
		}
	}

	return widget;
}

static void
web_view_hovering_over_link (EWebView *web_view,
                             const gchar *title,
                             const gchar *uri)
{
	CamelInternetAddress *address;
	CamelURL *curl;
	const gchar *format = NULL;
	gchar *message = NULL;
	gchar *who;

	if (uri == NULL || *uri == '\0')
		goto exit;

	if (g_str_has_prefix (uri, "mailto:"))
		format = _("Click to mail %s");
	else if (g_str_has_prefix (uri, "callto:"))
		format = _("Click to call %s");
	else if (g_str_has_prefix (uri, "h323:"))
		format = _("Click to call %s");
	else if (g_str_has_prefix (uri, "sip:"))
		format = _("Click to call %s");
	else if (g_str_has_prefix (uri, "##"))
		message = g_strdup (_("Click to hide/unhide addresses"));
	else
		message = g_strdup_printf (_("Click to open %s"), uri);

	if (format == NULL)
		goto exit;

	/* XXX Use something other than Camel here.  Surely
	 *     there's other APIs around that can do this. */
	curl = camel_url_new (uri, NULL);
	address = camel_internet_address_new ();
	camel_address_decode (CAMEL_ADDRESS (address), curl->path);
	who = camel_address_format (CAMEL_ADDRESS (address));
	g_object_unref (address);
	camel_url_free (curl);

	if (who == NULL)
		who = g_strdup (strchr (uri, ':') + 1);

	message = g_strdup_printf (format, who);

	g_free (who);

exit:
	e_web_view_status_message (web_view, message);

	g_free (message);
}

static void
web_view_link_clicked (EWebView *web_view,
                       const gchar *uri)
{
	gpointer parent;

	if (uri && g_ascii_strncasecmp (uri, "mailto:", 7) == 0) {
		gboolean handled = FALSE;

		g_signal_emit (
			web_view, signals[PROCESS_MAILTO], 0, uri, &handled);

		if (handled)
			return;
	}

	parent = gtk_widget_get_toplevel (GTK_WIDGET (web_view));
	parent = gtk_widget_is_toplevel (parent) ? parent : NULL;

	e_show_uri (parent, uri);
}

static void
web_view_load_string (EWebView *web_view,
                      const gchar *string)
{
	if (string == NULL)
		string = "";

	webkit_web_view_load_html (
		WEBKIT_WEB_VIEW (web_view),
		string, "evo-file:///");
}

static void
web_view_load_uri (EWebView *web_view,
                   const gchar *uri)
{
	if (uri == NULL)
		uri = "about:blank";

	webkit_web_view_load_uri (WEBKIT_WEB_VIEW (web_view), uri);
}

static gchar *
web_view_redirect_uri (EWebView *web_view,
                       const gchar *uri)
{
	return g_strdup (uri);
}

static gchar *
web_view_suggest_filename (EWebView *web_view,
                           const gchar *uri)
{
	const gchar *cp;

	/* Try to derive a filename from the last path segment. */

	cp = strrchr (uri, '/');
	if (cp != NULL) {
		if (strchr (cp, '?') == NULL)
			cp++;
		else
			cp = NULL;
	}

	return g_strdup (cp);
}

static gboolean
web_view_popup_event (EWebView *web_view,
                      const gchar *uri)
{
	e_web_view_set_selected_uri (web_view, uri);
	e_web_view_show_popup_menu (web_view);

	return TRUE;
}

static void
web_view_stop_loading (EWebView *web_view)
{
	webkit_web_view_stop_loading (WEBKIT_WEB_VIEW (web_view));
}

static void
web_extension_proxy_created_cb (GDBusProxy *proxy,
                                GAsyncResult *result,
                                EWebView *web_view)
{
	GError *error = NULL;

	web_view->priv->web_extension = g_dbus_proxy_new_finish (result, &error);
	if (!web_view->priv->web_extension) {
		g_warning ("Error creating web extension proxy: %s\n", error->message);
		g_error_free (error);
	}
}

static void
web_extension_appeared_cb (GDBusConnection *connection,
                           const gchar *name,
                           const gchar *name_owner,
                           EWebView *web_view)
{
	g_dbus_proxy_new (
		connection,
		G_DBUS_PROXY_FLAGS_DO_NOT_AUTO_START |
		G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES |
		G_DBUS_PROXY_FLAGS_DO_NOT_CONNECT_SIGNALS,
		NULL,
		name,
		EVOLUTION_WEB_EXTENSION_OBJECT_PATH,
		EVOLUTION_WEB_EXTENSION_INTERFACE,
		NULL,
		(GAsyncReadyCallback)web_extension_proxy_created_cb,
		web_view);
}

static void
web_extension_vanished_cb (GDBusConnection *connection,
                           const gchar *name,
                           EWebView *web_view)
{
	g_clear_object (&web_view->priv->web_extension);
}

static void
web_view_watch_web_extension (EWebView *web_view)
{
	char *service_name;

	service_name = g_strdup_printf ("%s-%u", EVOLUTION_WEB_EXTENSION_SERVICE_NAME, getpid ());
	web_view->priv->web_extension_watch_name_id =
		g_bus_watch_name (
			G_BUS_TYPE_SESSION,
			service_name,
			G_BUS_NAME_WATCHER_FLAGS_NONE,
			(GBusNameAppearedCallback) web_extension_appeared_cb,
			(GBusNameVanishedCallback) web_extension_vanished_cb,
			web_view, NULL);

	g_free (service_name);
}

GDBusProxy *
e_web_view_get_web_extension_proxy (EWebView *web_view)
{
	g_return_val_if_fail (E_IS_WEB_VIEW (web_view), NULL);

	return web_view->priv->web_extension;
}

static void
web_view_update_actions_cb (WebKitWebView *webkit_web_view,
                            GAsyncResult *result,
                            gpointer user_data)
{
	EWebView *web_view;
	GtkActionGroup *action_group;
	gboolean can_copy;
	gboolean scheme_is_http = FALSE;
	gboolean scheme_is_mailto = FALSE;
	gboolean uri_is_valid = FALSE;
	gboolean visible;
	const gchar *cursor_image_src;
	const gchar *group_name;
	const gchar *uri;

	web_view = E_WEB_VIEW (webkit_web_view);

	uri = e_web_view_get_selected_uri (web_view);
	can_copy =
		webkit_web_view_can_execute_editing_command_finish (
			webkit_web_view,
			result,
			NULL);
	cursor_image_src = e_web_view_get_cursor_image_src (web_view);

	/* Parse the URI early so we know if the actions will work. */
	if (uri != NULL) {
		CamelURL *curl;

		curl = camel_url_new (uri, NULL);
		uri_is_valid = (curl != NULL);
		camel_url_free (curl);

		scheme_is_http =
			(g_ascii_strncasecmp (uri, "http:", 5) == 0) ||
			(g_ascii_strncasecmp (uri, "https:", 6) == 0);

		scheme_is_mailto =
			(g_ascii_strncasecmp (uri, "mailto:", 7) == 0);
	}

	/* Allow copying the URI even if it's malformed. */
	group_name = "uri";
	visible = (uri != NULL) && !scheme_is_mailto;
	action_group = e_web_view_get_action_group (web_view, group_name);
	gtk_action_group_set_visible (action_group, visible);

	group_name = "http";
	visible = uri_is_valid && scheme_is_http;
	action_group = e_web_view_get_action_group (web_view, group_name);
	gtk_action_group_set_visible (action_group, visible);

	group_name = "mailto";
	visible = uri_is_valid && scheme_is_mailto;
	action_group = e_web_view_get_action_group (web_view, group_name);
	gtk_action_group_set_visible (action_group, visible);

	group_name = "image";
	visible = (cursor_image_src != NULL);
	action_group = e_web_view_get_action_group (web_view, group_name);
	gtk_action_group_set_visible (action_group, visible);

	group_name = "selection";
	visible = can_copy;
	action_group = e_web_view_get_action_group (web_view, group_name);
	gtk_action_group_set_visible (action_group, visible);

	group_name = "standard";
	visible = (uri == NULL);
	action_group = e_web_view_get_action_group (web_view, group_name);
	gtk_action_group_set_visible (action_group, visible);

	group_name = "lockdown-printing";
	visible = (uri == NULL) && !web_view->priv->disable_printing;
	action_group = e_web_view_get_action_group (web_view, group_name);
	gtk_action_group_set_visible (action_group, visible);

	group_name = "lockdown-save-to-disk";
	visible = (uri == NULL) && !web_view->priv->disable_save_to_disk;
	action_group = e_web_view_get_action_group (web_view, group_name);
	gtk_action_group_set_visible (action_group, visible);
}

static void
web_view_update_actions (EWebView *web_view)
{
	webkit_web_view_can_execute_editing_command (
		WEBKIT_WEB_VIEW (web_view),
		WEBKIT_EDITING_COMMAND_COPY,
		NULL, /* cancellable */
		(GAsyncReadyCallback) web_view_update_actions_cb,
		NULL);
}

static void
web_view_submit_alert (EAlertSink *alert_sink,
                       EAlert *alert)
{
	EWebView *web_view;
	GtkWidget *dialog;
	GString *buffer;
	const gchar *icon_name = NULL;
	const gchar *primary_text;
	const gchar *secondary_text;
	gpointer parent;

	web_view = E_WEB_VIEW (alert_sink);

	parent = gtk_widget_get_toplevel (GTK_WIDGET (web_view));
	parent = gtk_widget_is_toplevel (parent) ? parent : NULL;

	switch (e_alert_get_message_type (alert)) {
		case GTK_MESSAGE_INFO:
			icon_name = GTK_STOCK_DIALOG_INFO;
			break;

		case GTK_MESSAGE_WARNING:
			icon_name = GTK_STOCK_DIALOG_WARNING;
			break;

		case GTK_MESSAGE_ERROR:
			icon_name = GTK_STOCK_DIALOG_ERROR;
			break;

		default:
			dialog = e_alert_dialog_new (parent, alert);
			gtk_dialog_run (GTK_DIALOG (dialog));
			gtk_widget_destroy (dialog);
			return;
	}

	/* Primary text is required. */
	primary_text = e_alert_get_primary_text (alert);
	g_return_if_fail (primary_text != NULL);

	/* Secondary text is optional. */
	secondary_text = e_alert_get_secondary_text (alert);
	if (secondary_text == NULL)
		secondary_text = "";

	buffer = g_string_sized_new (512);

	g_string_append (
		buffer,
		"<html>"
		"<head>"
		"<meta http-equiv=\"content-type\""
		" content=\"text/html; charset=utf-8\">"
		"</head>"
		"<body>");

	g_string_append (
		buffer,
		"<table bgcolor='#000000' width='100%'"
		" cellpadding='1' cellspacing='0'>"
		"<tr>"
		"<td>"
		"<table bgcolor='#dddddd' width='100%' cellpadding='6'>"
		"<tr>");

	g_string_append_printf (
		buffer,
		"<tr>"
		"<td valign='top'>"
		"<img src='gtk-stock://%s/?size=%d'/>"
		"</td>"
		"<td align='left' width='100%%'>"
		"<h3>%s</h3>"
		"%s"
		"</td>"
		"</tr>",
		icon_name,
		GTK_ICON_SIZE_DIALOG,
		primary_text,
		secondary_text);

	g_string_append (
		buffer,
		"</table>"
		"</td>"
		"</tr>"
		"</table>"
		"</body>"
		"</html>");

	e_web_view_load_string (web_view, buffer->str);

	g_string_free (buffer, TRUE);
}


static void
web_view_can_execute_editing_command_cb (WebKitWebView *webkit_web_view,
                                         GAsyncResult *result,
                                         GtkAction *action)
{
	gboolean can_do_command;

	can_do_command =
		webkit_web_view_can_execute_editing_command_finish (
			webkit_web_view,
			result,
			NULL);

	gtk_action_set_sensitive (action, can_do_command);
}

static void
web_view_selectable_update_actions (ESelectable *selectable,
                                    EFocusTracker *focus_tracker,
                                    GdkAtom *clipboard_targets,
                                    gint n_clipboard_targets)
{
	WebKitWebView *web_view;
	GtkAction *action;
	gboolean sensitive;
	const gchar *tooltip;

	web_view = WEBKIT_WEB_VIEW (selectable);

	action = e_focus_tracker_get_cut_clipboard_action (focus_tracker);
	webkit_web_view_can_execute_editing_command (
		WEBKIT_WEB_VIEW (web_view),
		WEBKIT_EDITING_COMMAND_CUT,
		NULL, /* cancellable */
		(GAsyncReadyCallback) web_view_can_execute_editing_command_cb,
		action);
	tooltip = _("Cut the selection");
	gtk_action_set_tooltip (action, tooltip);

	action = e_focus_tracker_get_copy_clipboard_action (focus_tracker);
	webkit_web_view_can_execute_editing_command (
		WEBKIT_WEB_VIEW (web_view),
		WEBKIT_EDITING_COMMAND_COPY,
		NULL, /* cancellable */
		(GAsyncReadyCallback) web_view_can_execute_editing_command_cb,
		action);
	tooltip = _("Copy the selection");
	gtk_action_set_tooltip (action, tooltip);

	action = e_focus_tracker_get_paste_clipboard_action (focus_tracker);
	webkit_web_view_can_execute_editing_command (
		WEBKIT_WEB_VIEW (web_view),
		WEBKIT_EDITING_COMMAND_PASTE,
		NULL, /* cancellable */
		(GAsyncReadyCallback) web_view_can_execute_editing_command_cb,
		action);
	tooltip = _("Paste the clipboard");
	gtk_action_set_tooltip (action, tooltip);

	action = e_focus_tracker_get_select_all_action (focus_tracker);
	sensitive = TRUE;
	tooltip = _("Select all text and images");
	gtk_action_set_sensitive (action, sensitive);
	gtk_action_set_tooltip (action, tooltip);
}

static void
web_view_selectable_cut_clipboard (ESelectable *selectable)
{
	e_web_view_cut_clipboard (E_WEB_VIEW (selectable));
}

static void
web_view_selectable_copy_clipboard (ESelectable *selectable)
{
	e_web_view_copy_clipboard (E_WEB_VIEW (selectable));
}

static void
web_view_selectable_paste_clipboard (ESelectable *selectable)
{
	e_web_view_paste_clipboard (E_WEB_VIEW (selectable));
}

static void
web_view_selectable_select_all (ESelectable *selectable)
{
	e_web_view_select_all (E_WEB_VIEW (selectable));
}

static void
e_web_view_class_init (EWebViewClass *class)
{
	GObjectClass *object_class;
	GtkWidgetClass *widget_class;

	g_type_class_add_private (class, sizeof (EWebViewPrivate));

	object_class = G_OBJECT_CLASS (class);
	object_class->set_property = web_view_set_property;
	object_class->get_property = web_view_get_property;
	object_class->dispose = web_view_dispose;
	object_class->finalize = web_view_finalize;
	object_class->constructed = web_view_constructed;

	widget_class = GTK_WIDGET_CLASS (class);
	widget_class->scroll_event = web_view_scroll_event;
	widget_class->drag_motion = web_view_drag_motion;

	class->create_plugin_widget = web_view_create_plugin_widget;
	class->hovering_over_link = web_view_hovering_over_link;
	class->link_clicked = web_view_link_clicked;
	class->load_string = web_view_load_string;
	class->load_uri = web_view_load_uri;
	class->redirect_uri = web_view_redirect_uri;
	class->suggest_filename = web_view_suggest_filename;
	class->popup_event = web_view_popup_event;
	class->stop_loading = web_view_stop_loading;
	class->update_actions = web_view_update_actions;

	g_object_class_install_property (
		object_class,
		PROP_CARET_MODE,
		g_param_spec_boolean (
			"caret-mode",
			"Caret Mode",
			NULL,
			FALSE,
			G_PARAM_READWRITE));

	g_object_class_install_property (
		object_class,
		PROP_CURSOR_IMAGE_SRC,
		g_param_spec_string (
			"cursor-image-src",
			"Image source uri at the mouse cursor",
			NULL,
			NULL,
			G_PARAM_READWRITE));

	g_object_class_install_property (
		object_class,
		PROP_DISABLE_PRINTING,
		g_param_spec_boolean (
			"disable-printing",
			"Disable Printing",
			NULL,
			FALSE,
			G_PARAM_READWRITE |
			G_PARAM_CONSTRUCT));

	g_object_class_install_property (
		object_class,
		PROP_DISABLE_SAVE_TO_DISK,
		g_param_spec_boolean (
			"disable-save-to-disk",
			"Disable Save-to-Disk",
			NULL,
			FALSE,
			G_PARAM_READWRITE |
			G_PARAM_CONSTRUCT));

	g_object_class_install_property (
		object_class,
		PROP_INLINE_SPELLING,
		g_param_spec_boolean (
			"inline-spelling",
			"Inline Spelling",
			NULL,
			FALSE,
			G_PARAM_READWRITE));

	g_object_class_install_property (
		object_class,
		PROP_MAGIC_LINKS,
		g_param_spec_boolean (
			"magic-links",
			"Magic Links",
			NULL,
			FALSE,
			G_PARAM_READWRITE));

	g_object_class_install_property (
		object_class,
		PROP_MAGIC_SMILEYS,
		g_param_spec_boolean (
			"magic-smileys",
			"Magic Smileys",
			NULL,
			FALSE,
			G_PARAM_READWRITE));

	g_object_class_install_property (
		object_class,
		PROP_OPEN_PROXY,
		g_param_spec_object (
			"open-proxy",
			"Open Proxy",
			NULL,
			GTK_TYPE_ACTION,
			G_PARAM_READWRITE));

	g_object_class_install_property (
		object_class,
		PROP_PRINT_PROXY,
		g_param_spec_object (
			"print-proxy",
			"Print Proxy",
			NULL,
			GTK_TYPE_ACTION,
			G_PARAM_READWRITE));

	g_object_class_install_property (
		object_class,
		PROP_SAVE_AS_PROXY,
		g_param_spec_object (
			"save-as-proxy",
			"Save As Proxy",
			NULL,
			GTK_TYPE_ACTION,
			G_PARAM_READWRITE));

	g_object_class_install_property (
		object_class,
		PROP_SELECTED_URI,
		g_param_spec_string (
			"selected-uri",
			"Selected URI",
			NULL,
			NULL,
			G_PARAM_READWRITE));

	signals[NEW_ACTIVITY] = g_signal_new (
		"new-activity",
		G_TYPE_FROM_CLASS (class),
		G_SIGNAL_RUN_LAST,
		G_STRUCT_OFFSET (EWebViewClass, new_activity),
		NULL, NULL,
		g_cclosure_marshal_VOID__OBJECT,
		G_TYPE_NONE, 1,
		E_TYPE_ACTIVITY);

	signals[POPUP_EVENT] = g_signal_new (
		"popup-event",
		G_TYPE_FROM_CLASS (class),
		G_SIGNAL_RUN_LAST,
		G_STRUCT_OFFSET (EWebViewClass, popup_event),
		g_signal_accumulator_true_handled, NULL,
		e_marshal_BOOLEAN__STRING,
		G_TYPE_BOOLEAN, 1, G_TYPE_STRING);

	signals[STATUS_MESSAGE] = g_signal_new (
		"status-message",
		G_TYPE_FROM_CLASS (class),
		G_SIGNAL_RUN_LAST,
		G_STRUCT_OFFSET (EWebViewClass, status_message),
		NULL, NULL,
		g_cclosure_marshal_VOID__STRING,
		G_TYPE_NONE, 1,
		G_TYPE_STRING);

	signals[STOP_LOADING] = g_signal_new (
		"stop-loading",
		G_TYPE_FROM_CLASS (class),
		G_SIGNAL_RUN_LAST,
		G_STRUCT_OFFSET (EWebViewClass, stop_loading),
		NULL, NULL,
		g_cclosure_marshal_VOID__VOID,
		G_TYPE_NONE, 0);

	signals[UPDATE_ACTIONS] = g_signal_new (
		"update-actions",
		G_TYPE_FROM_CLASS (class),
		G_SIGNAL_RUN_LAST,
		G_STRUCT_OFFSET (EWebViewClass, update_actions),
		NULL, NULL,
		g_cclosure_marshal_VOID__VOID,
		G_TYPE_NONE, 0);

	/* return TRUE when a signal handler processed the mailto URI */
	signals[PROCESS_MAILTO] = g_signal_new (
		"process-mailto",
		G_TYPE_FROM_CLASS (class),
		G_SIGNAL_RUN_LAST,
		G_STRUCT_OFFSET (EWebViewClass, process_mailto),
		NULL, NULL,
		e_marshal_BOOLEAN__STRING,
		G_TYPE_BOOLEAN, 1, G_TYPE_STRING);
}

static void
e_web_view_alert_sink_init (EAlertSinkInterface *interface)
{
	interface->submit_alert = web_view_submit_alert;
}

static void
e_web_view_selectable_init (ESelectableInterface *interface)
{
	interface->update_actions = web_view_selectable_update_actions;
	interface->cut_clipboard = web_view_selectable_cut_clipboard;
	interface->copy_clipboard = web_view_selectable_copy_clipboard;
	interface->paste_clipboard = web_view_selectable_paste_clipboard;
	interface->select_all = web_view_selectable_select_all;
}

static void
web_view_cid_uri_scheme_appeared_cb (WebKitURISchemeRequest *request,
                                     EWebView *web_view)
{
}

static void
web_view_file_uri_scheme_appeared_cb (WebKitURISchemeRequest *request,
                                      EWebView *web_view)
{
}
static void
web_view_mail_uri_scheme_appeared_cb (WebKitURISchemeRequest *request,
                                      EWebView *web_view)
{
}

static void
web_view_http_uri_scheme_appeared_cb (WebKitURISchemeRequest *request,
                                      EWebView *web_view)
{
}

static void
web_view_gtk_stock_uri_scheme_appeared_cb (WebKitURISchemeRequest *request,
                                           EWebView *web_view)
{
}

void
e_web_view_register_uri_scheme (EWebView *web_view,
                                EURIScheme scheme,
                                gpointer user_callback,
				gpointer user_data)
{
	WebKitWebContext *context;
	gpointer callback = NULL;
	const gchar *uri_scheme;

	context = webkit_web_view_get_context (WEBKIT_WEB_VIEW (web_view));
	callback = user_callback;

	switch (scheme) {
		case CID_URI_SCHEME:
			uri_scheme = "cid";
			if (!callback)
				callback = web_view_cid_uri_scheme_appeared_cb;
			break;
		case FILE_URI_SCHEME:
			uri_scheme = "file";
			if (!callback)
				callback = web_view_file_uri_scheme_appeared_cb;
			break;
		case MAIL_URI_SCHEME:
			uri_scheme = "mail";
			if (!callback)
				callback = web_view_mail_uri_scheme_appeared_cb;
			break;
		case EVO_HTTP_URI_SCHEME:
			uri_scheme = "evo-http";
			if (!callback)
				callback = web_view_http_uri_scheme_appeared_cb;
			break;
		case EVO_HTTPS_URI_SCHEME:
			uri_scheme = "evo-https";
			if (!callback)
				callback = web_view_http_uri_scheme_appeared_cb;
			break;
		case GTK_STOCK_URI_SCHEME:
			uri_scheme = "gtk-stock";
			if (!callback)
				callback = web_view_gtk_stock_uri_scheme_appeared_cb;
			break;
		default:
			return;
	}

	webkit_web_context_register_uri_scheme (
		context,
		uri_scheme,
		(WebKitURISchemeRequestCallback) callback,
		user_data ? user_data : web_view,
		NULL);

}

static void
e_web_view_init (EWebView *web_view)
{
	GtkUIManager *ui_manager;
	GtkActionGroup *action_group;
	EPopupAction *popup_action;
	GSettingsSchema *settings_schema;
	GSettings *settings;
	const gchar *domain = GETTEXT_PACKAGE;
	const gchar *id;
	gulong handler_id;
	GError *error = NULL;
	WebKitWebContext *web_context;

	web_view->priv = E_WEB_VIEW_GET_PRIVATE (web_view);

	/* XXX No WebKitWebView class method pointers to
	 *     override so we have to use signal handlers. */

	g_signal_connect (
		web_view, "create-plugin-widget",
		G_CALLBACK (web_view_create_plugin_widget_cb), NULL);

	g_signal_connect (
		web_view, "context-menu",
		G_CALLBACK (web_view_context_menu_cb), NULL);

	g_signal_connect (
		web_view, "mouse-target-changed",
		G_CALLBACK (web_view_mouse_target_changed_cb), NULL);

	g_signal_connect (
		web_view, "decide-policy",
		G_CALLBACK (web_view_decide_policy_cb),
		NULL);

	g_signal_connect (
		web_view, "notify::load-changed",
		G_CALLBACK (web_view_load_changed_cb), NULL);

	g_signal_connect (
		web_view, "style-updated",
		G_CALLBACK (style_updated_cb), NULL);

	g_signal_connect (
		web_view, "state-flags-changed",
		G_CALLBACK (style_updated_cb), NULL);

	ui_manager = gtk_ui_manager_new ();
	web_view->priv->ui_manager = ui_manager;

	g_signal_connect_swapped (
		ui_manager, "connect-proxy",
		G_CALLBACK (web_view_connect_proxy_cb), web_view);

	web_view_init_webkit_settings (web_view);
	web_view_watch_web_extension (web_view);
	web_view_set_find_controller (web_view);

	web_context = webkit_web_view_get_context (WEBKIT_WEB_VIEW (web_view));
	webkit_web_context_set_cache_model (web_context, WEBKIT_CACHE_MODEL_DOCUMENT_VIEWER);

	e_web_view_register_uri_scheme (web_view, FILE_URI_SCHEME, NULL, NULL);
	e_web_view_register_uri_scheme (web_view, GTK_STOCK_URI_SCHEME, NULL, NULL);
/*
	e_web_view_install_request_handler (web_view, E_TYPE_FILE_REQUEST);
	e_web_view_install_request_handler (web_view, E_TYPE_STOCK_REQUEST);
*/
	settings = g_settings_new ("org.gnome.desktop.interface");
	web_view->priv->font_settings = g_object_ref (settings);
	handler_id = g_signal_connect_swapped (
		settings, "changed::font-name",
		G_CALLBACK (e_web_view_update_fonts), web_view);
	web_view->priv->font_name_changed_handler_id = handler_id;
	handler_id = g_signal_connect_swapped (
		settings, "changed::monospace-font-name",
		G_CALLBACK (e_web_view_update_fonts), web_view);
	web_view->priv->monospace_font_name_changed_handler_id = handler_id;
	g_object_unref (settings);

	/* This schema is optional.  Use if available. */
	id = "org.gnome.settings-daemon.plugins.xsettings";
	settings_schema = g_settings_schema_source_lookup (
		g_settings_schema_source_get_default (), id, FALSE);
	if (settings_schema != NULL) {
		settings = g_settings_new (id);
		web_view->priv->aliasing_settings = g_object_ref (settings);
		handler_id = g_signal_connect_swapped (
			settings, "changed::antialiasing",
			G_CALLBACK (e_web_view_update_fonts), web_view);
		web_view->priv->antialiasing_changed_handler_id = handler_id;
		g_object_unref (settings);
	}

	e_web_view_update_fonts (web_view);

	action_group = gtk_action_group_new ("uri");
	gtk_action_group_set_translation_domain (action_group, domain);
	gtk_ui_manager_insert_action_group (ui_manager, action_group, 0);
	g_object_unref (action_group);

	gtk_action_group_add_actions (
		action_group, uri_entries,
		G_N_ELEMENTS (uri_entries), web_view);

	action_group = gtk_action_group_new ("http");
	gtk_action_group_set_translation_domain (action_group, domain);
	gtk_ui_manager_insert_action_group (ui_manager, action_group, 0);
	g_object_unref (action_group);

	gtk_action_group_add_actions (
		action_group, http_entries,
		G_N_ELEMENTS (http_entries), web_view);

	action_group = gtk_action_group_new ("mailto");
	gtk_action_group_set_translation_domain (action_group, domain);
	gtk_ui_manager_insert_action_group (ui_manager, action_group, 0);
	g_object_unref (action_group);

	gtk_action_group_add_actions (
		action_group, mailto_entries,
		G_N_ELEMENTS (mailto_entries), web_view);

	action_group = gtk_action_group_new ("image");
	gtk_action_group_set_translation_domain (action_group, domain);
	gtk_ui_manager_insert_action_group (ui_manager, action_group, 0);
	g_object_unref (action_group);

	gtk_action_group_add_actions (
		action_group, image_entries,
		G_N_ELEMENTS (image_entries), web_view);

	action_group = gtk_action_group_new ("selection");
	gtk_action_group_set_translation_domain (action_group, domain);
	gtk_ui_manager_insert_action_group (ui_manager, action_group, 0);
	g_object_unref (action_group);

	gtk_action_group_add_actions (
		action_group, selection_entries,
		G_N_ELEMENTS (selection_entries), web_view);

	action_group = gtk_action_group_new ("standard");
	gtk_action_group_set_translation_domain (action_group, domain);
	gtk_ui_manager_insert_action_group (ui_manager, action_group, 0);
	g_object_unref (action_group);

	gtk_action_group_add_actions (
		action_group, standard_entries,
		G_N_ELEMENTS (standard_entries), web_view);

	popup_action = e_popup_action_new ("open");
	gtk_action_group_add_action (action_group, GTK_ACTION (popup_action));
	g_object_unref (popup_action);

	g_object_bind_property (
		web_view, "open-proxy",
		popup_action, "related-action",
		G_BINDING_BIDIRECTIONAL |
		G_BINDING_SYNC_CREATE);

	/* Support lockdown. */

	action_group = gtk_action_group_new ("lockdown-printing");
	gtk_action_group_set_translation_domain (action_group, domain);
	gtk_ui_manager_insert_action_group (ui_manager, action_group, 0);
	g_object_unref (action_group);

	popup_action = e_popup_action_new ("print");
	gtk_action_group_add_action (action_group, GTK_ACTION (popup_action));
	g_object_unref (popup_action);

	g_object_bind_property (
		web_view, "print-proxy",
		popup_action, "related-action",
		G_BINDING_BIDIRECTIONAL |
		G_BINDING_SYNC_CREATE);

	action_group = gtk_action_group_new ("lockdown-save-to-disk");
	gtk_action_group_set_translation_domain (action_group, domain);
	gtk_ui_manager_insert_action_group (ui_manager, action_group, 0);
	g_object_unref (action_group);

	popup_action = e_popup_action_new ("save-as");
	gtk_action_group_add_action (action_group, GTK_ACTION (popup_action));
	g_object_unref (popup_action);

	g_object_bind_property (
		web_view, "save-as-proxy",
		popup_action, "related-action",
		G_BINDING_BIDIRECTIONAL |
		G_BINDING_SYNC_CREATE);

	/* Because we are loading from a hard-coded string, there is
	 * no chance of I/O errors.  Failure here implies a malformed
	 * UI definition.  Full stop. */
	gtk_ui_manager_add_ui_from_string (ui_manager, ui, -1, &error);
	if (error != NULL)
		g_error ("%s", error->message);

	id = "org.gnome.evolution.webview";
	e_plugin_ui_register_manager (ui_manager, id, web_view);
	e_plugin_ui_enable_manager (ui_manager, id);

	e_web_view_clear (E_WEB_VIEW (web_view));
}

GtkWidget *
e_web_view_new (void)
{
	return g_object_new (E_TYPE_WEB_VIEW, NULL);
}

void
e_web_view_clear (EWebView *web_view)
{
	g_return_if_fail (E_IS_WEB_VIEW (web_view));

	webkit_web_view_load_html (
		WEBKIT_WEB_VIEW (web_view),
		"<html> \
		 <head></head> \
		 <body class=\"-e-web-view-background-color -e-web-view-text-color\"></body> \
		 </html>",
		 NULL);
}

void
e_web_view_load_string (EWebView *web_view,
                        const gchar *string)
{
	EWebViewClass *class;

	g_return_if_fail (E_IS_WEB_VIEW (web_view));

	class = E_WEB_VIEW_GET_CLASS (web_view);
	g_return_if_fail (class->load_string != NULL);

	class->load_string (web_view, string);
}

void
e_web_view_load_uri (EWebView *web_view,
                     const gchar *uri)
{
	EWebViewClass *class;

	g_return_if_fail (E_IS_WEB_VIEW (web_view));

	class = E_WEB_VIEW_GET_CLASS (web_view);
	g_return_if_fail (class->load_uri != NULL);

	class->load_uri (web_view, uri);
}

/**
 * e_web_view_redirect_uri:
 * @web_view: an #EWebView
 * @uri: the requested URI
 *
 * Replaces @uri with a redirected URI as necessary, primarily for use
 * with custom #SoupRequest handlers.  Typically this function would be
 * called just prior to handing a request off to a #SoupSession, such as
 * from a #WebKitWebView #WebKitWebView::resource-request-starting signal
 * handler.
 *
 * A newly-allocated URI string is always returned, whether the @uri was
 * redirected or not.  Free the returned string with g_free().
 *
 * Returns: the redirected URI or a copy of @uri
 **/
gchar *
e_web_view_redirect_uri (EWebView *web_view,
                         const gchar *uri)
{
	EWebViewClass *class;

	g_return_val_if_fail (E_IS_WEB_VIEW (web_view), NULL);
	g_return_val_if_fail (uri != NULL, NULL);

	class = E_WEB_VIEW_GET_CLASS (web_view);
	g_return_val_if_fail (class->redirect_uri != NULL, NULL);

	return class->redirect_uri (web_view, uri);
}

/**
 * e_web_view_suggest_filename:
 * @web_view: an #EWebView
 * @uri: a URI string
 *
 * Attempts to derive a suggested filename from the @uri for use in a
 * "Save As" dialog.
 *
 * By default the suggested filename is the last path segment of the @uri
 * (unless @uri looks like a query), but subclasses can use other mechanisms
 * for custom URI schemes.  For example, "cid:" URIs in an email message may
 * refer to a MIME part with a suggested filename in its Content-Disposition
 * header.
 *
 * The returned string should be freed with g_free() when finished with it,
 * but callers should also be prepared for the function to return %NULL if
 * a filename cannot be determined.
 *
 * Returns: a suggested filename, or %NULL
 **/
gchar *
e_web_view_suggest_filename (EWebView *web_view,
                             const gchar *uri)
{
	EWebViewClass *class;
	gchar *filename;

	g_return_val_if_fail (E_IS_WEB_VIEW (web_view), NULL);
	g_return_val_if_fail (uri != NULL, NULL);

	class = E_WEB_VIEW_GET_CLASS (web_view);
	g_return_val_if_fail (class->suggest_filename != NULL, NULL);

	filename = class->suggest_filename (web_view, uri);

	if (filename != NULL)
		e_filename_make_safe (filename);

	return filename;
}

void
e_web_view_reload (EWebView *web_view)
{
	g_return_if_fail (E_IS_WEB_VIEW (web_view));

	webkit_web_view_reload (WEBKIT_WEB_VIEW (web_view));
}

static void
get_document_content_html_cb (GDBusProxy *web_extension,
                             GAsyncResult *result,
                             GTask *task)
{
	GVariant *result_variant;
	const gchar *html_content;

	result_variant = g_dbus_proxy_call_finish (web_extension, result, NULL);
	if (result_variant) {
		html_content = g_variant_get_string (result_variant, NULL);
		g_variant_unref (result_variant);
	}

	g_task_return_pointer (task, g_strdup (html_content), g_free);
	g_object_unref (task);
}

void
e_web_view_get_content_html (EWebView *web_view,
                             GCancellable *cancellable,
                             GAsyncReadyCallback callback,
                             gpointer user_data)
{
	GDBusProxy *web_extension;
	GTask *task;

	g_return_if_fail (E_IS_WEB_VIEW (web_view));

	task = g_task_new (web_view, cancellable, callback, user_data);

	web_extension = e_web_view_get_web_extension_proxy (web_view);
	if (web_extension) {
		g_dbus_proxy_call (
			web_extension,
			"GetDocumentContentHtml",
			g_variant_new (
				"(t)",
				webkit_web_view_get_page_id (
					WEBKIT_WEB_VIEW (web_view))),
			G_DBUS_CALL_FLAGS_NONE,
			-1,
			cancellable,
			(GAsyncReadyCallback) get_document_content_html_cb,
			g_object_ref (task));
	} else
		g_task_return_pointer (task, NULL, NULL);
}

const gchar *
e_web_view_get_content_html_finish (EWebView *web_view,
                                    GAsyncResult *result,
                                    GError **error)
{
	g_return_val_if_fail (E_IS_WEB_VIEW (web_view), NULL);
	g_return_val_if_fail (g_task_is_valid (result, web_view), FALSE);

	return g_task_propagate_pointer (G_TASK (result), error);
}

const gchar *
e_web_view_get_content_html_sync (EWebView *web_view,
                                  GCancellable *cancellable,
                                  GError **error)
{
	GDBusProxy *web_extension;

	g_return_val_if_fail (E_IS_WEB_VIEW (web_view), NULL);

	web_extension = e_web_view_get_web_extension_proxy (web_view);
	if (web_extension) {
		GVariant *result;
		const gchar *html_content = NULL;

		result = g_dbus_proxy_call_sync (
				web_extension,
				"GetDocumentContentHtml",
				g_variant_new (
					"(t)",
					webkit_web_view_get_page_id (
						WEBKIT_WEB_VIEW (web_view))),
				G_DBUS_CALL_FLAGS_NONE,
				-1,
				cancellable,
				error);

		if (result) {
			html_content = g_variant_get_string (result, NULL);
			g_variant_unref (result);
			return html_content;
		}
	}

	return NULL;
}

gboolean
e_web_view_get_caret_mode (EWebView *web_view)
{
	g_return_val_if_fail (E_IS_WEB_VIEW (web_view), FALSE);

	return web_view->priv->caret_mode;
}

void
e_web_view_set_caret_mode (EWebView *web_view,
                           gboolean caret_mode)
{
	g_return_if_fail (E_IS_WEB_VIEW (web_view));

	if (web_view->priv->caret_mode == caret_mode)
		return;

	web_view->priv->caret_mode = caret_mode;

	g_object_notify (G_OBJECT (web_view), "caret-mode");
}

GtkTargetList *
e_web_view_get_copy_target_list (EWebView *web_view)
{
	g_return_val_if_fail (E_IS_WEB_VIEW (web_view), NULL);

	return NULL;
	/* FIXME XXX */
//	return webkit_web_view_get_copy_target_list (
//		WEBKIT_WEB_VIEW (web_view));
}

gboolean
e_web_view_get_disable_printing (EWebView *web_view)
{
	g_return_val_if_fail (E_IS_WEB_VIEW (web_view), FALSE);

	return web_view->priv->disable_printing;
}

void
e_web_view_set_disable_printing (EWebView *web_view,
                                 gboolean disable_printing)
{
	g_return_if_fail (E_IS_WEB_VIEW (web_view));

	if (web_view->priv->disable_printing == disable_printing)
		return;

	web_view->priv->disable_printing = disable_printing;

	g_object_notify (G_OBJECT (web_view), "disable-printing");
}

gboolean
e_web_view_get_disable_save_to_disk (EWebView *web_view)
{
	g_return_val_if_fail (E_IS_WEB_VIEW (web_view), FALSE);

	return web_view->priv->disable_save_to_disk;
}

void
e_web_view_set_disable_save_to_disk (EWebView *web_view,
                                     gboolean disable_save_to_disk)
{
	g_return_if_fail (E_IS_WEB_VIEW (web_view));

	if (web_view->priv->disable_save_to_disk == disable_save_to_disk)
		return;

	web_view->priv->disable_save_to_disk = disable_save_to_disk;

	g_object_notify (G_OBJECT (web_view), "disable-save-to-disk");
}

gboolean
e_web_view_get_editable (EWebView *web_view)
{
	g_return_val_if_fail (E_IS_WEB_VIEW (web_view), FALSE);

/* FIXME XXX */
//	return webkit_web_view_get_editable (WEBKIT_WEB_VIEW (web_view));
	return TRUE;
}

void
e_web_view_set_editable (EWebView *web_view,
                         gboolean editable)
{
	g_return_if_fail (E_IS_WEB_VIEW (web_view));

/* FIXME XXX */
//	webkit_web_view_set_editable (WEBKIT_WEB_VIEW (web_view), editable);
}

gboolean
e_web_view_get_inline_spelling (EWebView *web_view)
{
#if 0  /* WEBKIT - XXX No equivalent property? */
	/* XXX This is just here to maintain symmetry
	 *     with e_web_view_set_inline_spelling(). */

	g_return_val_if_fail (E_IS_WEB_VIEW (web_view), FALSE);

	return gtk_html_get_inline_spelling (GTK_HTML (web_view));
#endif

	return FALSE;
}

void
e_web_view_set_inline_spelling (EWebView *web_view,
                                gboolean inline_spelling)
{
#if 0  /* WEBKIT - XXX No equivalent property? */
	/* XXX GtkHTML does not utilize GObject properties as well
	 *     as it could.  This just wraps gtk_html_set_inline_spelling()
	 *     so we get a "notify::inline-spelling" signal. */

	g_return_if_fail (E_IS_WEB_VIEW (web_view));

	gtk_html_set_inline_spelling (GTK_HTML (web_view), inline_spelling);

	g_object_notify (G_OBJECT (web_view), "inline-spelling");
#endif
}

gboolean
e_web_view_get_magic_links (EWebView *web_view)
{
#if 0  /* WEBKIT - XXX No equivalent property? */
	/* XXX This is just here to maintain symmetry
	 *     with e_web_view_set_magic_links(). */

	g_return_val_if_fail (E_IS_WEB_VIEW (web_view), FALSE);

	return gtk_html_get_magic_links (GTK_HTML (web_view));
#endif

	return FALSE;
}

void
e_web_view_set_magic_links (EWebView *web_view,
                            gboolean magic_links)
{
#if 0  /* WEBKIT - XXX No equivalent property? */
	/* XXX GtkHTML does not utilize GObject properties as well
	 *     as it could.  This just wraps gtk_html_set_magic_links()
	 *     so we can get a "notify::magic-links" signal. */

	g_return_if_fail (E_IS_WEB_VIEW (web_view));

	gtk_html_set_magic_links (GTK_HTML (web_view), magic_links);

	g_object_notify (G_OBJECT (web_view), "magic-links");
#endif
}

gboolean
e_web_view_get_magic_smileys (EWebView *web_view)
{
#if 0  /* WEBKIT - No equivalent property? */
	/* XXX This is just here to maintain symmetry
	 *     with e_web_view_set_magic_smileys(). */

	g_return_val_if_fail (E_IS_WEB_VIEW (web_view), FALSE);

	return gtk_html_get_magic_smileys (GTK_HTML (web_view));
#endif

	return FALSE;
}

void
e_web_view_set_magic_smileys (EWebView *web_view,
                              gboolean magic_smileys)
{
#if 0  /* WEBKIT - No equivalent property? */
	/* XXX GtkHTML does not utilize GObject properties as well
	 *     as it could.  This just wraps gtk_html_set_magic_smileys()
	 *     so we can get a "notify::magic-smileys" signal. */

	g_return_if_fail (E_IS_WEB_VIEW (web_view));

	gtk_html_set_magic_smileys (GTK_HTML (web_view), magic_smileys);

	g_object_notify (G_OBJECT (web_view), "magic-smileys");
#endif
}

const gchar *
e_web_view_get_selected_uri (EWebView *web_view)
{
	g_return_val_if_fail (E_IS_WEB_VIEW (web_view), NULL);

	return web_view->priv->selected_uri;
}

void
e_web_view_set_selected_uri (EWebView *web_view,
                             const gchar *selected_uri)
{
	g_return_if_fail (E_IS_WEB_VIEW (web_view));

	if (g_strcmp0 (web_view->priv->selected_uri, selected_uri) == 0)
		return;

	g_free (web_view->priv->selected_uri);
	web_view->priv->selected_uri = g_strdup (selected_uri);

	g_object_notify (G_OBJECT (web_view), "selected-uri");
}

const gchar *
e_web_view_get_cursor_image_src (EWebView *web_view)
{
	g_return_val_if_fail (E_IS_WEB_VIEW (web_view), NULL);

	return web_view->priv->cursor_image_src;
}

void
e_web_view_set_cursor_image_src (EWebView *web_view,
                                 const gchar *src_uri)
{
	g_return_if_fail (E_IS_WEB_VIEW (web_view));

	if (g_strcmp0 (web_view->priv->cursor_image_src, src_uri) == 0)
		return;

	g_free (web_view->priv->cursor_image_src);
	web_view->priv->cursor_image_src = g_strdup (src_uri);

	g_object_notify (G_OBJECT (web_view), "cursor-image-src");
}

GtkAction *
e_web_view_get_open_proxy (EWebView *web_view)
{
	g_return_val_if_fail (E_IS_WEB_VIEW (web_view), FALSE);

	return web_view->priv->open_proxy;
}

void
e_web_view_set_open_proxy (EWebView *web_view,
                           GtkAction *open_proxy)
{
	g_return_if_fail (E_IS_WEB_VIEW (web_view));

	if (web_view->priv->open_proxy == open_proxy)
		return;

	if (open_proxy != NULL) {
		g_return_if_fail (GTK_IS_ACTION (open_proxy));
		g_object_ref (open_proxy);
	}

	if (web_view->priv->open_proxy != NULL)
		g_object_unref (web_view->priv->open_proxy);

	web_view->priv->open_proxy = open_proxy;

	g_object_notify (G_OBJECT (web_view), "open-proxy");
}

GtkTargetList *
e_web_view_get_paste_target_list (EWebView *web_view)
{
	g_return_val_if_fail (E_IS_WEB_VIEW (web_view), NULL);
/* FIXME XXX
	return webkit_web_view_get_paste_target_list (
		WEBKIT_WEB_VIEW (web_view));*/
	return NULL;
}

GtkAction *
e_web_view_get_print_proxy (EWebView *web_view)
{
	g_return_val_if_fail (E_IS_WEB_VIEW (web_view), FALSE);

	return web_view->priv->print_proxy;
}

void
e_web_view_set_print_proxy (EWebView *web_view,
                            GtkAction *print_proxy)
{
	g_return_if_fail (E_IS_WEB_VIEW (web_view));

	if (web_view->priv->print_proxy == print_proxy)
		return;

	if (print_proxy != NULL) {
		g_return_if_fail (GTK_IS_ACTION (print_proxy));
		g_object_ref (print_proxy);
	}

	if (web_view->priv->print_proxy != NULL)
		g_object_unref (web_view->priv->print_proxy);

	web_view->priv->print_proxy = print_proxy;

	g_object_notify (G_OBJECT (web_view), "print-proxy");
}

GtkAction *
e_web_view_get_save_as_proxy (EWebView *web_view)
{
	g_return_val_if_fail (E_IS_WEB_VIEW (web_view), FALSE);

	return web_view->priv->save_as_proxy;
}

void
e_web_view_set_save_as_proxy (EWebView *web_view,
                              GtkAction *save_as_proxy)
{
	g_return_if_fail (E_IS_WEB_VIEW (web_view));

	if (web_view->priv->save_as_proxy == save_as_proxy)
		return;

	if (save_as_proxy != NULL) {
		g_return_if_fail (GTK_IS_ACTION (save_as_proxy));
		g_object_ref (save_as_proxy);
	}

	if (web_view->priv->save_as_proxy != NULL)
		g_object_unref (web_view->priv->save_as_proxy);

	web_view->priv->save_as_proxy = save_as_proxy;

	g_object_notify (G_OBJECT (web_view), "save-as-proxy");
}

void
e_web_view_add_highlight (EWebView *web_view,
                          const gchar *highlight)
{
	g_return_if_fail (E_IS_WEB_VIEW (web_view));
	g_return_if_fail (highlight && *highlight);

	g_queue_push_tail (
		&web_view->priv->highlights,
		g_strdup (highlight));

	webkit_find_controller_search (
		web_view->priv->find_controller,
		highlight,
		WEBKIT_FIND_OPTIONS_NONE,
		G_MAXUINT);
}

void
e_web_view_clear_highlights (EWebView *web_view)
{
	g_return_if_fail (E_IS_WEB_VIEW (web_view));

	webkit_find_controller_search_finish (web_view->priv->find_controller);

	while (!g_queue_is_empty (&web_view->priv->highlights))
		g_free (g_queue_pop_head (&web_view->priv->highlights));
}

void
e_web_view_update_highlights (EWebView *web_view)
{
	g_return_if_fail (E_IS_WEB_VIEW (web_view));

	web_view_update_document_highlights (web_view);
}

GtkAction *
e_web_view_get_action (EWebView *web_view,
                       const gchar *action_name)
{
	GtkUIManager *ui_manager;

	g_return_val_if_fail (E_IS_WEB_VIEW (web_view), NULL);
	g_return_val_if_fail (action_name != NULL, NULL);

	ui_manager = e_web_view_get_ui_manager (web_view);

	return e_lookup_action (ui_manager, action_name);
}

GtkActionGroup *
e_web_view_get_action_group (EWebView *web_view,
                             const gchar *group_name)
{
	GtkUIManager *ui_manager;

	g_return_val_if_fail (E_IS_WEB_VIEW (web_view), NULL);
	g_return_val_if_fail (group_name != NULL, NULL);

	ui_manager = e_web_view_get_ui_manager (web_view);

	return e_lookup_action_group (ui_manager, group_name);
}

void
e_web_view_copy_clipboard (EWebView *web_view)
{
	g_return_if_fail (E_IS_WEB_VIEW (web_view));

	webkit_web_view_execute_editing_command (
		WEBKIT_WEB_VIEW (web_view), WEBKIT_EDITING_COMMAND_COPY);
}

void
e_web_view_cut_clipboard (EWebView *web_view)
{
	g_return_if_fail (E_IS_WEB_VIEW (web_view));

	webkit_web_view_execute_editing_command (
		WEBKIT_WEB_VIEW (web_view), WEBKIT_EDITING_COMMAND_CUT);
}

gboolean
e_web_view_is_selection_active (EWebView *web_view)
{
	g_return_val_if_fail (E_IS_WEB_VIEW (web_view), FALSE);
/* FIXME XXX
	return webkit_web_view_has_selection (WEBKIT_WEB_VIEW (web_view));*/
	return FALSE;
}

void
e_web_view_paste_clipboard (EWebView *web_view)
{
	g_return_if_fail (E_IS_WEB_VIEW (web_view));

	webkit_web_view_execute_editing_command (
		WEBKIT_WEB_VIEW (web_view), WEBKIT_EDITING_COMMAND_PASTE);
}

gboolean
e_web_view_scroll_forward (EWebView *web_view)
{
	g_return_val_if_fail (E_IS_WEB_VIEW (web_view), FALSE);
/* FIXME XXX
	webkit_web_view_move_cursor (
		WEBKIT_WEB_VIEW (web_view), GTK_MOVEMENT_PAGES, 1);
*/

	return TRUE;  /* XXX This means nothing. */
}

gboolean
e_web_view_scroll_backward (EWebView *web_view)
{
	g_return_val_if_fail (E_IS_WEB_VIEW (web_view), FALSE);
/* FIXME XXX
	webkit_web_view_move_cursor (
		WEBKIT_WEB_VIEW (web_view), GTK_MOVEMENT_PAGES, -1);
*/

	return TRUE;  /* XXX This means nothing. */
}

void
e_web_view_select_all (EWebView *web_view)
{
	g_return_if_fail (E_IS_WEB_VIEW (web_view));

	webkit_web_view_execute_editing_command (
		WEBKIT_WEB_VIEW (web_view), WEBKIT_EDITING_COMMAND_SELECT_ALL);
}

void
e_web_view_unselect_all (EWebView *web_view)
{
#if 0  /* WEBKIT */
	g_return_if_fail (E_IS_WEB_VIEW (web_view));

	gtk_html_command (GTK_HTML (web_view), "unselect-all");
#endif
}

void
e_web_view_zoom_100 (EWebView *web_view)
{
	g_return_if_fail (E_IS_WEB_VIEW (web_view));

	webkit_web_view_set_zoom_level (WEBKIT_WEB_VIEW (web_view), 1.0);
}

void
e_web_view_zoom_in (EWebView *web_view)
{
	gdouble zoom_level;

	g_return_if_fail (E_IS_WEB_VIEW (web_view));

	/* There is no webkit_web_view_zoom_in function in WK2, so emulate it */
	zoom_level = webkit_web_view_get_zoom_level (WEBKIT_WEB_VIEW (web_view));
	/* zoom-step in WK1 was 0.1 */
	zoom_level += 0.1;
	webkit_web_view_set_zoom_level (WEBKIT_WEB_VIEW (web_view), zoom_level);
/* FIXME XXX
	webkit_web_view_zoom_in (WEBKIT_WEB_VIEW (web_view));*/
}

void
e_web_view_zoom_out (EWebView *web_view)
{
	gdouble zoom_level;

	g_return_if_fail (E_IS_WEB_VIEW (web_view));

	/* There is no webkit_web_view_zoom_out function in WK2, so emulate it */
	zoom_level = webkit_web_view_get_zoom_level (WEBKIT_WEB_VIEW (web_view));
	/* zoom-step in WK1 was 0.1 */
	zoom_level -= 0.1;
	if (zoom_level >= 0)
		webkit_web_view_set_zoom_level (WEBKIT_WEB_VIEW (web_view), zoom_level);
/* FIXME XXX
	webkit_web_view_zoom_out (WEBKIT_WEB_VIEW (web_view));*/
}

GtkUIManager *
e_web_view_get_ui_manager (EWebView *web_view)
{
	g_return_val_if_fail (E_IS_WEB_VIEW (web_view), NULL);

	return web_view->priv->ui_manager;
}

GtkWidget *
e_web_view_get_popup_menu (EWebView *web_view)
{
	GtkUIManager *ui_manager;
	GtkWidget *menu;

	g_return_val_if_fail (E_IS_WEB_VIEW (web_view), NULL);

	ui_manager = e_web_view_get_ui_manager (web_view);
	menu = gtk_ui_manager_get_widget (ui_manager, "/context");
	g_return_val_if_fail (GTK_IS_MENU (menu), NULL);

	return menu;
}

void
e_web_view_show_popup_menu (EWebView *web_view)
{
	GtkWidget *menu;

	g_return_if_fail (E_IS_WEB_VIEW (web_view));

	e_web_view_update_actions (web_view);

	menu = e_web_view_get_popup_menu (web_view);

	gtk_menu_popup (
		GTK_MENU (menu), NULL, NULL, NULL, NULL,
		0, gtk_get_current_event_time ());
}

/**
 * e_web_view_new_activity:
 * @web_view: an #EWebView
 *
 * Returns a new #EActivity for an #EWebView-related asynchronous operation,
 * and emits the #EWebView::new-activity signal.  By default the #EActivity
 * comes loaded with a #GCancellable and sets the @web_view itself as the
 * #EActivity:alert-sink (which means alerts are displayed directly in the
 * content area).  The signal emission allows the #EActivity to be further
 * customized and/or tracked by the application.
 *
 * Returns: an #EActivity
 **/
EActivity *
e_web_view_new_activity (EWebView *web_view)
{
	EActivity *activity;
	EAlertSink *alert_sink;
	GCancellable *cancellable;

	g_return_val_if_fail (E_IS_WEB_VIEW (web_view), NULL);

	activity = e_activity_new ();

	alert_sink = E_ALERT_SINK (web_view);
	e_activity_set_alert_sink (activity, alert_sink);

	cancellable = g_cancellable_new ();
	e_activity_set_cancellable (activity, cancellable);
	g_object_unref (cancellable);

	g_signal_emit (web_view, signals[NEW_ACTIVITY], 0, activity);

	return activity;
}

void
e_web_view_status_message (EWebView *web_view,
                           const gchar *status_message)
{
	g_return_if_fail (E_IS_WEB_VIEW (web_view));

	g_signal_emit (web_view, signals[STATUS_MESSAGE], 0, status_message);
}

void
e_web_view_stop_loading (EWebView *web_view)
{
	g_return_if_fail (E_IS_WEB_VIEW (web_view));

	g_signal_emit (web_view, signals[STOP_LOADING], 0);
}

void
e_web_view_update_actions (EWebView *web_view)
{
	g_return_if_fail (E_IS_WEB_VIEW (web_view));

	g_signal_emit (web_view, signals[UPDATE_ACTIONS], 0);
}

static void
get_selection_content_html_cb (GDBusProxy *web_extension,
                               GAsyncResult *result,
                               GTask *task)
{
	GVariant *result_variant;
	const gchar *html_content;

	result_variant = g_dbus_proxy_call_finish (web_extension, result, NULL);
	if (result_variant) {
		html_content = g_variant_get_string (result_variant, NULL);
		g_variant_unref (result_variant);
	}

	g_task_return_pointer (task, g_strdup (html_content), g_free);
	g_object_unref (task);
}

void
e_web_view_get_selection_content_html (EWebView *web_view,
                                       GCancellable *cancellable,
                                       GAsyncReadyCallback callback,
                                       gpointer user_data)
{
	GDBusProxy *web_extension;
	GTask *task;

	g_return_if_fail (E_IS_WEB_VIEW (web_view));

	task = g_task_new (web_view, cancellable, callback, user_data);
/*FIXME XXX
	if (!webkit_web_view_has_selection (WEBKIT_WEB_VIEW (web_view)))
		return NULL;*/

	web_extension = e_web_view_get_web_extension_proxy (web_view);
	if (web_extension) {
		g_dbus_proxy_call (
			web_extension,
			"GetSelectionContentHtml",
			g_variant_new (
				"(t)",
				webkit_web_view_get_page_id (
					WEBKIT_WEB_VIEW (web_view))),
			G_DBUS_CALL_FLAGS_NONE,
			-1,
			cancellable,
			(GAsyncReadyCallback) get_selection_content_html_cb,
			g_object_ref (task));
	} else
		g_task_return_pointer (task, NULL, NULL);
}

const gchar *
e_web_view_get_selection_content_html_finish (EWebView *web_view,
                                              GAsyncResult *result,
                                              GError **error)
{
	g_return_val_if_fail (E_IS_WEB_VIEW (web_view), NULL);
	g_return_val_if_fail (g_task_is_valid (result, web_view), FALSE);

	return g_task_propagate_pointer (G_TASK (result), error);
}

const gchar *
e_web_view_get_selection_content_html_sync (EWebView *web_view,
                                            GCancellable *cancellable,
                                            GError **error)
{
	GDBusProxy *web_extension;

	g_return_val_if_fail (E_IS_WEB_VIEW (web_view), NULL);
/*FIXME XXX
	if (!webkit_web_view_has_selection (WEBKIT_WEB_VIEW (web_view)))
		return NULL;*/

	web_extension = e_web_view_get_web_extension_proxy (web_view);
	if (web_extension) {
		GVariant *result;
		const gchar *html_content = NULL;

		result = g_dbus_proxy_call_sync (
				web_extension,
				"GetSelectionContentHtml",
				g_variant_new (
					"(t)",
					webkit_web_view_get_page_id (
						WEBKIT_WEB_VIEW (web_view))),
				G_DBUS_CALL_FLAGS_NONE,
				-1,
				cancellable,
				error);

		if (result) {
			html_content = g_variant_get_string (result, NULL);
			g_variant_unref (result);
			return html_content;
		}
	}

	return NULL;
}

void
e_web_view_update_fonts (EWebView *web_view)
{
	EWebViewClass *class;
	GString *stylesheet;
	gchar *base64;
	gchar *aa = NULL;
	WebKitSettings *settings;
	PangoFontDescription *min_size, *ms, *vw;
	const gchar *styles[] = { "normal", "oblique", "italic" };
	const gchar *smoothing = NULL;
	GtkStyleContext *context;
	GdkColor *link = NULL;
	GdkColor *visited = NULL;

	g_return_if_fail (E_IS_WEB_VIEW (web_view));

	ms = NULL;
	vw = NULL;

	class = E_WEB_VIEW_GET_CLASS (web_view);
	if (class->set_fonts != NULL)
		class->set_fonts (web_view, &ms, &vw);

	if (ms == NULL) {
		gchar *font;

		font = g_settings_get_string (
			web_view->priv->font_settings,
			"monospace-font-name");

		ms = pango_font_description_from_string (
			(font != NULL) ? font : "monospace 10");

		g_free (font);
	}

	if (vw == NULL) {
		gchar *font;

		font = g_settings_get_string (
			web_view->priv->font_settings,
			"font-name");

		vw = pango_font_description_from_string (
			(font != NULL) ? font : "serif 10");

		g_free (font);
	}

	if (pango_font_description_get_size (ms) < pango_font_description_get_size (vw)) {
		min_size = ms;
	} else {
		min_size = vw;
	}

	stylesheet = g_string_new ("");
	g_string_append_printf (
		stylesheet,
		"body {\n"
		"  font-family: '%s';\n"
		"  font-size: %dpt;\n"
		"  font-weight: %d;\n"
		"  font-style: %s;\n",
		pango_font_description_get_family (vw),
		pango_font_description_get_size (vw) / PANGO_SCALE,
		pango_font_description_get_weight (vw),
		styles[pango_font_description_get_style (vw)]);

	if (web_view->priv->aliasing_settings != NULL)
		aa = g_settings_get_string (
			web_view->priv->aliasing_settings, "antialiasing");

	if (g_strcmp0 (aa, "none") == 0)
		smoothing = "none";
	else if (g_strcmp0 (aa, "grayscale") == 0)
		smoothing = "antialiased";
	else if (g_strcmp0 (aa, "rgba") == 0)
		smoothing = "subpixel-antialiased";

	if (smoothing != NULL)
		g_string_append_printf (
			stylesheet,
			" -webkit-font-smoothing: %s;\n",
			smoothing);

	g_free (aa);

	g_string_append (stylesheet, "}\n");

	g_string_append_printf (
		stylesheet,
		"pre,code,.pre {\n"
		"  font-family: '%s';\n"
		"  font-size: %dpt;\n"
		"  font-weight: %d;\n"
		"  font-style: %s;\n"
		"}",
		pango_font_description_get_family (ms),
		pango_font_description_get_size (ms) / PANGO_SCALE,
		pango_font_description_get_weight (ms),
		styles[pango_font_description_get_style (ms)]);

	context = gtk_widget_get_style_context (GTK_WIDGET (web_view));
	gtk_style_context_get_style (
		context,
		"link-color", &link,
		"visited-link-color", &visited,
		NULL);

	if (link == NULL) {
		link = g_slice_new0 (GdkColor);
		link->blue = G_MAXINT16;
	}

	if (visited == NULL) {
		visited = g_slice_new0 (GdkColor);
		visited->red = G_MAXINT16;
	}

	g_string_append_printf (
		stylesheet,
		"a {\n"
		"  color: #%06x;\n"
		"}\n"
		"a:visited {\n"
		"  color: #%06x;\n"
		"}\n",
		e_color_to_value (link),
		e_color_to_value (visited));

	gdk_color_free (link);
	gdk_color_free (visited);

	base64 = g_base64_encode ((guchar *) stylesheet->str, stylesheet->len);
	g_string_free (stylesheet, TRUE);

	stylesheet = g_string_new ("data:text/css;charset=utf-8;base64,");
	g_string_append (stylesheet, base64);
	g_free (base64);

	settings = webkit_web_view_get_settings (WEBKIT_WEB_VIEW (web_view));
	g_object_set (
		G_OBJECT (settings),
		"default-font-size",
		pango_font_description_get_size (vw) / PANGO_SCALE,
		"default-font-family",
		pango_font_description_get_family (vw),
		"monospace-font-family",
		pango_font_description_get_family (ms),
		"default-monospace-font-size",
		pango_font_description_get_size (ms) / PANGO_SCALE,
		"minimum-font-size",
		pango_font_description_get_size (min_size) / PANGO_SCALE,
/*		"user-stylesheet-uri",
		stylesheet->str,*/
		NULL);

/*FIXME XXX stylesheet uri*/
	g_string_free (stylesheet, TRUE);

	pango_font_description_free (ms);
	pango_font_description_free (vw);
}

/* Helper for e_web_view_cursor_image_copy() */
static void
web_view_cursor_image_copy_pixbuf_cb (GObject *source_object,
                                      GAsyncResult *result,
                                      gpointer user_data)
{
	EActivity *activity;
	EAlertSink *alert_sink;
	GdkPixbuf *pixbuf;
	GError *local_error = NULL;

	activity = E_ACTIVITY (user_data);
	alert_sink = e_activity_get_alert_sink (activity);

	pixbuf = gdk_pixbuf_new_from_stream_finish (result, &local_error);

	/* Sanity check. */
	g_return_if_fail (
		((pixbuf != NULL) && (local_error == NULL)) ||
		((pixbuf == NULL) && (local_error != NULL)));

	if (e_activity_handle_cancellation (activity, local_error)) {
		g_error_free (local_error);

	} else if (local_error != NULL) {
		e_alert_submit (
			alert_sink,
			"widgets:no-image-copy",
			local_error->message, NULL);
		g_error_free (local_error);

	} else {
		GtkClipboard *clipboard;

		clipboard = gtk_clipboard_get (GDK_SELECTION_CLIPBOARD);
		gtk_clipboard_set_image (clipboard, pixbuf);
		gtk_clipboard_store (clipboard);

		e_activity_set_state (activity, E_ACTIVITY_COMPLETED);
	}

	g_clear_object (&activity);
	g_clear_object (&pixbuf);
}

/* Helper for e_web_view_cursor_image_copy() */
static void
web_view_cursor_image_copy_request_cb (GObject *source_object,
                                       GAsyncResult *result,
                                       gpointer user_data)
{
	EActivity *activity;
	EAlertSink *alert_sink;
	GCancellable *cancellable;
	GInputStream *input_stream;
	GError *local_error = NULL;

	activity = E_ACTIVITY (user_data);
	alert_sink = e_activity_get_alert_sink (activity);
	cancellable = e_activity_get_cancellable (activity);

	input_stream = e_web_view_request_finish (
		E_WEB_VIEW (source_object), result, &local_error);

	/* Sanity check. */
	g_return_if_fail (
		((input_stream != NULL) && (local_error == NULL)) ||
		((input_stream == NULL) && (local_error != NULL)));

	if (e_activity_handle_cancellation (activity, local_error)) {
		g_error_free (local_error);

	} else if (local_error != NULL) {
		e_alert_submit (
			alert_sink,
			"widgets:no-image-copy",
			local_error->message, NULL);
		g_error_free (local_error);

	} else {
		gdk_pixbuf_new_from_stream_async (
			input_stream, cancellable,
			web_view_cursor_image_copy_pixbuf_cb,
			g_object_ref (activity));
	}

	g_clear_object (&activity);
	g_clear_object (&input_stream);
}

/**
 * e_web_view_cursor_image_copy:
 * @web_view: an #EWebView
 *
 * Asynchronously copies the image under the cursor to the clipboard.
 *
 * This function triggers a #EWebView::new-activity signal emission so
 * the asynchronous operation can be tracked and/or cancelled.
 **/
void
e_web_view_cursor_image_copy (EWebView *web_view)
{
	g_return_if_fail (E_IS_WEB_VIEW (web_view));

	if (web_view->priv->cursor_image_src != NULL) {
		EActivity *activity;
		GCancellable *cancellable;
		const gchar *text;

		activity = e_web_view_new_activity (web_view);
		cancellable = e_activity_get_cancellable (activity);

		text = _("Copying image to clipboard");
		e_activity_set_text (activity, text);

		e_web_view_request (
			web_view,
			web_view->priv->cursor_image_src,
			cancellable,
			web_view_cursor_image_copy_request_cb,
			g_object_ref (activity));

		g_object_unref (activity);
	}
}

/* Helper for e_web_view_cursor_image_save() */
static void
web_view_cursor_image_save_splice_cb (GObject *source_object,
                                      GAsyncResult *result,
                                      gpointer user_data)
{
	EActivity *activity;
	EAlertSink *alert_sink;
	AsyncContext *async_context;
	GError *local_error = NULL;

	async_context = (AsyncContext *) user_data;

	activity = async_context->activity;
	alert_sink = e_activity_get_alert_sink (activity);

	g_output_stream_splice_finish (
		G_OUTPUT_STREAM (source_object), result, &local_error);

	if (e_activity_handle_cancellation (activity, local_error)) {
		g_error_free (local_error);

	} else if (local_error != NULL) {
		e_alert_submit (
			alert_sink,
			"widgets:no-image-save",
			local_error->message, NULL);
		g_error_free (local_error);

	} else {
		e_activity_set_state (activity, E_ACTIVITY_COMPLETED);
	}

	async_context_free (async_context);
}

/* Helper for e_web_view_cursor_image_save() */
static void
web_view_cursor_image_save_replace_cb (GObject *source_object,
                                       GAsyncResult *result,
                                       gpointer user_data)
{
	EActivity *activity;
	EAlertSink *alert_sink;
	GCancellable *cancellable;
	GFileOutputStream *output_stream;
	AsyncContext *async_context;
	GError *local_error = NULL;

	async_context = (AsyncContext *) user_data;

	activity = async_context->activity;
	alert_sink = e_activity_get_alert_sink (activity);
	cancellable = e_activity_get_cancellable (activity);

	output_stream = g_file_replace_finish (
		G_FILE (source_object), result, &local_error);

	/* Sanity check. */
	g_return_if_fail (
		((output_stream != NULL) && (local_error == NULL)) ||
		((output_stream == NULL) && (local_error != NULL)));

	if (e_activity_handle_cancellation (activity, local_error)) {
		g_error_free (local_error);
		async_context_free (async_context);

	} else if (local_error != NULL) {
		e_alert_submit (
			alert_sink,
			"widgets:no-image-save",
			local_error->message, NULL);
		g_error_free (local_error);
		async_context_free (async_context);

	} else {
		g_output_stream_splice_async (
			G_OUTPUT_STREAM (output_stream),
			async_context->input_stream,
			G_OUTPUT_STREAM_SPLICE_CLOSE_SOURCE |
			G_OUTPUT_STREAM_SPLICE_CLOSE_TARGET,
			G_PRIORITY_DEFAULT,
			cancellable,
			web_view_cursor_image_save_splice_cb,
			async_context);
	}

	g_clear_object (&output_stream);
}

/* Helper for e_web_view_cursor_image_save() */
static void
web_view_cursor_image_save_request_cb (GObject *source_object,
                                       GAsyncResult *result,
                                       gpointer user_data)
{
	EActivity *activity;
	EAlertSink *alert_sink;
	GCancellable *cancellable;
	GInputStream *input_stream;
	AsyncContext *async_context;
	GError *local_error = NULL;

	async_context = (AsyncContext *) user_data;

	activity = async_context->activity;
	alert_sink = e_activity_get_alert_sink (activity);
	cancellable = e_activity_get_cancellable (activity);

	input_stream = e_web_view_request_finish (
		E_WEB_VIEW (source_object), result, &local_error);

	/* Sanity check. */
	g_return_if_fail (
		((input_stream != NULL) && (local_error == NULL)) ||
		((input_stream == NULL) && (local_error != NULL)));

	if (e_activity_handle_cancellation (activity, local_error)) {
		g_error_free (local_error);
		async_context_free (async_context);

	} else if (local_error != NULL) {
		e_alert_submit (
			alert_sink,
			"widgets:no-image-save",
			local_error->message, NULL);
		g_error_free (local_error);
		async_context_free (async_context);

	} else {
		async_context->input_stream = g_object_ref (input_stream);

		/* Open an output stream to the destination file. */
		g_file_replace_async (
			async_context->destination,
			NULL, FALSE,
			G_FILE_CREATE_REPLACE_DESTINATION,
			G_PRIORITY_DEFAULT,
			cancellable,
			web_view_cursor_image_save_replace_cb,
			async_context);
	}

	g_clear_object (&input_stream);
}

/**
 * e_web_view_cursor_image_save:
 * @web_view: an #EWebView
 *
 * Prompts the user to choose a destination file and then asynchronously
 * saves the image under the cursor to the destination file.
 *
 * This function triggers a #EWebView::new-activity signal emission so
 * the asynchronous operation can be tracked and/or cancelled.
 **/
void
e_web_view_cursor_image_save (EWebView *web_view)
{
	GtkFileChooser *file_chooser;
	GFile *destination = NULL;
	GtkWidget *dialog;
	gchar *suggestion;
	gpointer toplevel;

	g_return_if_fail (E_IS_WEB_VIEW (web_view));

	if (web_view->priv->cursor_image_src == NULL)
		return;

	toplevel = gtk_widget_get_toplevel (GTK_WIDGET (web_view));
	toplevel = gtk_widget_is_toplevel (toplevel) ? toplevel : NULL;

	dialog = gtk_file_chooser_dialog_new (
		_("Save Image"), toplevel,
		GTK_FILE_CHOOSER_ACTION_SAVE,
		GTK_STOCK_CANCEL, GTK_RESPONSE_CANCEL,
		GTK_STOCK_SAVE, GTK_RESPONSE_ACCEPT, NULL);

	gtk_dialog_set_default_response (
		GTK_DIALOG (dialog), GTK_RESPONSE_ACCEPT);

	file_chooser = GTK_FILE_CHOOSER (dialog);
	gtk_file_chooser_set_local_only (file_chooser, FALSE);
	gtk_file_chooser_set_do_overwrite_confirmation (file_chooser, TRUE);

	suggestion = e_web_view_suggest_filename (
		web_view, web_view->priv->cursor_image_src);

	if (suggestion != NULL) {
		gtk_file_chooser_set_current_name (file_chooser, suggestion);
		g_free (suggestion);
	}

	if (gtk_dialog_run (GTK_DIALOG (dialog)) == GTK_RESPONSE_ACCEPT)
		destination = gtk_file_chooser_get_file (file_chooser);

	gtk_widget_destroy (dialog);

	if (destination != NULL) {
		EActivity *activity;
		GCancellable *cancellable;
		AsyncContext *async_context;
		gchar *text;
		gchar *uri;

		activity = e_web_view_new_activity (web_view);
		cancellable = e_activity_get_cancellable (activity);

		uri = g_file_get_uri (destination);
		text = g_strdup_printf (_("Saving image to '%s'"), uri);
		e_activity_set_text (activity, text);
		g_free (text);
		g_free (uri);

		async_context = g_slice_new0 (AsyncContext);
		async_context->activity = g_object_ref (activity);
		async_context->destination = g_object_ref (destination);

		e_web_view_request (
			web_view,
			web_view->priv->cursor_image_src,
			cancellable,
			web_view_cursor_image_save_request_cb,
			async_context);

		g_object_unref (activity);
		g_object_unref (destination);
	}
}

/* Helper for e_web_view_request() */
static void
web_view_request_send_cb (GObject *source_object,
                          GAsyncResult *result,
                          gpointer user_data)
{
	GSimpleAsyncResult *simple;
	AsyncContext *async_context;
	GError *local_error = NULL;

	simple = G_SIMPLE_ASYNC_RESULT (user_data);
	async_context = g_simple_async_result_get_op_res_gpointer (simple);

	async_context->input_stream = soup_request_send_finish (
		SOUP_REQUEST (source_object), result, &local_error);

	if (local_error != NULL)
		g_simple_async_result_take_error (simple, local_error);

	g_simple_async_result_complete (simple);
}

/**
 * e_web_view_request:
 * @web_view: an #EWebView
 * @uri: the URI to load
 * @cancellable: optional #GCancellable object, or %NULL
 * @callback: a #GAsyncReadyCallback to call when the request is satisfied
 * @user_data: data to pass to the callback function
 *
 * Asynchronously requests data at @uri by way of a #SoupRequest to WebKit's
 * default #SoupSession, incorporating both e_web_view_redirect_uri() and the
 * custom request handlers installed via e_web_view_install_request_handler().
 *
 * When the operation is finished, @callback will be called.  You can then
 * call e_web_view_request_finish() to get the result of the operation.
 **/
void
e_web_view_request (EWebView *web_view,
                    const gchar *uri,
                    GCancellable *cancellable,
                    GAsyncReadyCallback callback,
                    gpointer user_data)
{
	SoupSession *session;
	SoupSessionFeature *feature;
	SoupRequester *requester;
	SoupRequest *request;
	gchar *real_uri;
	GSimpleAsyncResult *simple;
	AsyncContext *async_context;
	GError *local_error = NULL;

	g_return_if_fail (E_IS_WEB_VIEW (web_view));
	g_return_if_fail (uri != NULL);

//	session = webkit_get_default_session ();
	session = NULL;

	/* SoupRequester feature should have already been added. */
	feature = soup_session_get_feature (session, SOUP_TYPE_REQUESTER);
	g_return_if_fail (feature != NULL);

	async_context = g_slice_new0 (AsyncContext);

	simple = g_simple_async_result_new (
		G_OBJECT (web_view), callback,
		user_data, e_web_view_request);

	g_simple_async_result_set_check_cancellable (simple, cancellable);

	g_simple_async_result_set_op_res_gpointer (
		simple, async_context, (GDestroyNotify) async_context_free);

	requester = SOUP_REQUESTER (feature);
	real_uri = e_web_view_redirect_uri (web_view, uri);
	request = soup_requester_request (requester, real_uri, &local_error);
	g_free (real_uri);

	/* Sanity check. */
	g_return_if_fail (
		((request != NULL) && (local_error == NULL)) ||
		((request == NULL) && (local_error != NULL)));

	if (request != NULL) {
		soup_request_send_async (
			request, cancellable,
			web_view_request_send_cb,
			g_object_ref (simple));

		g_object_unref (request);

	} else {
		g_simple_async_result_take_error (simple, local_error);
		g_simple_async_result_complete_in_idle (simple);
	}

	g_object_unref (simple);
}

/**
 * e_web_view_request_finish:
 * @web_view: an #EWebView
 * @result: a #GAsyncResult
 * @error: return location for a #GError, or %NULL
 *
 * Finishes the operation started with e_web_view_request().
 *
 * Unreference the returned #GInputStream with g_object_unref() when finished
 * with it.  If an error occurred, the function will set @error and return
 * %NULL.
 *
 * Returns: a #GInputStream, or %NULL
 **/
GInputStream *
e_web_view_request_finish (EWebView *web_view,
                           GAsyncResult *result,
                           GError **error)
{
	GSimpleAsyncResult *simple;
	AsyncContext *async_context;

	g_return_val_if_fail (
		g_simple_async_result_is_valid (
		result, G_OBJECT (web_view), e_web_view_request), NULL);

	simple = G_SIMPLE_ASYNC_RESULT (result);
	async_context = g_simple_async_result_get_op_res_gpointer (simple);

	if (g_simple_async_result_propagate_error (simple, error))
		return NULL;

	g_return_val_if_fail (async_context->input_stream != NULL, NULL);

	return g_object_ref (async_context->input_stream);
}
/*
void
e_web_view_install_request_handler (EWebView *web_view,
                                    GType handler_type)
{
	SoupSession *session;
	SoupSessionFeature *feature;

//	session = webkit_get_default_session ();
	session = NULL;

	feature = soup_session_get_feature (session, SOUP_TYPE_REQUESTER);
	if (feature != NULL) {
		g_object_ref (feature);
	} else {
		feature = SOUP_SESSION_FEATURE (soup_requester_new ());
		soup_session_add_feature (session, feature);
	}

	soup_session_feature_add_feature (feature, handler_type);

	g_object_unref (feature);
}
*/
/**
 * e_web_view_create_and_add_css_style_sheet_sync:
 * @web_view: an #EWebView
 * @style_sheet_id: CSS style sheet's id
 *
 * Creates new CSS style sheet with given @style_sheel_id and inserts
 * it into given @web_view document.
 **/
void
e_web_view_create_and_add_css_style_sheet_sync (EWebView *web_view,
                                                const gchar *style_sheet_id,
                                                GCancellable *cancellable,
                                                GError **error)
{
	GDBusProxy *web_extension;

	g_return_if_fail (E_IS_WEB_VIEW (web_view));
	g_return_if_fail (style_sheet_id && *style_sheet_id);

	web_extension = e_web_view_get_web_extension_proxy (web_view);
	if (web_extension) {
		GVariant *result;

		result = g_dbus_proxy_call_sync (
				web_extension,
				"CreateAndAddCSSStyleSheet",
				g_variant_new (
					"(ts)",
					webkit_web_view_get_page_id (
						WEBKIT_WEB_VIEW (web_view)),
					style_sheet_id),
				G_DBUS_CALL_FLAGS_NONE,
				-1,
				cancellable,
				error);

		if (result)
			g_variant_unref (result);
	}
}

/**
 * e_web_view_add_css_rule_into_style_sheet_sync:
 * @web_view: an #EWebView
 * @style_sheet_id: CSS style sheet's id
 * @selector: CSS selector
 * @style: style for given selector
 *
 * Insert new CSS rule (defined with @selector and @style) into CSS style sheet
 * with given @style_sheet_id. If style sheet doesn't exist, it's created.
 *
 * The rule is inserted to every DOM document that is in page. That means also
 * into DOM documents inside iframe elements.
 **/
void
e_web_view_add_css_rule_into_style_sheet_sync (EWebView *web_view,
                                               const gchar *style_sheet_id,
                                               const gchar *selector,
                                               const gchar *style,
                                               GCancellable *cancellable,
                                               GError **error)
{
	GDBusProxy *web_extension;

	g_return_if_fail (E_IS_WEB_VIEW (web_view));
	g_return_if_fail (style_sheet_id && *style_sheet_id);
	g_return_if_fail (selector && *selector);
	g_return_if_fail (style && *style);

	web_extension = e_web_view_get_web_extension_proxy (web_view);
	if (web_extension) {
		GVariant *result;

		result = g_dbus_proxy_call_sync (
				web_extension,
				"AddCSSRuleIntoStyleSheet",
				g_variant_new (
					"(tsss)",
					webkit_web_view_get_page_id (
						WEBKIT_WEB_VIEW (web_view)),
					style_sheet_id,
					selector,
					style),
				G_DBUS_CALL_FLAGS_NONE,
				-1,
				cancellable,
				error);

		if (result)
			g_variant_unref (result);
	}
}
