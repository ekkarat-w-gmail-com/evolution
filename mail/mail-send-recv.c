/*
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
 *
 * Authors:
 *		Michael Zucchi <NotZed@ximian.com>
 *
 * Copyright (C) 1999-2008 Novell, Inc. (www.novell.com)
 *
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdio.h>
#include <string.h>

#include <glib/gi18n.h>

#include <shell/e-shell.h>
#include <e-util/e-util.h>

#include <libemail-utils/mail-mt.h>

/* This is our hack, not part of libcamel. */
#include <libemail-engine/camel-null-store.h>

#include <libemail-engine/e-mail-folder-utils.h>
#include <libemail-engine/e-mail-session.h>
#include <libemail-engine/mail-folder-cache.h>
#include <libemail-engine/mail-ops.h>
#include <libemail-engine/mail-tools.h>

#include "e-mail-account-store.h"
#include "e-mail-ui-session.h"
#include "em-event.h"
#include "em-filter-rule.h"
#include "em-utils.h"
#include "mail-send-recv.h"

#define d(x)

/* ms between status updates to the gui */
#define STATUS_TIMEOUT (250)

/* pseudo-uri to key the send task on */
#define SEND_URI_KEY "send-task:"

#define SEND_RECV_ICON_SIZE GTK_ICON_SIZE_LARGE_TOOLBAR

/* send/receive email */

/* ********************************************************************** */
/*  This stuff below is independent of the stuff above */

/* this stuff is used to keep track of which folders filters have accessed, and
 * what not. the thaw/refreeze thing doesn't really seem to work though */
struct _folder_info {
	gchar *uri;
	CamelFolder *folder;
	time_t update;

	/* How many times updated, to slow it
	 * down as we go, if we have lots. */
	gint count;
};

struct _send_data {
	GList *infos;

	GtkDialog *gd;
	gint cancelled;

	/* Since we're never asked to update
	 * this one, do it ourselves. */
	CamelFolder *inbox;
	time_t inbox_update;

	GMutex lock;
	GHashTable *folders;

	GHashTable *active;	/* send_info's by uri */
};

typedef enum {
	SEND_RECEIVE,		/* receiver */
	SEND_SEND,		/* sender */
	SEND_UPDATE,		/* imap-like 'just update folder info' */
	SEND_INVALID
} send_info_t;

typedef enum {
	SEND_ACTIVE,
	SEND_CANCELLED,
	SEND_COMPLETE
} send_state_t;

struct _send_info {
	send_info_t type;		/* 0 = fetch, 1 = send */
	GCancellable *cancellable;
	CamelSession *session;
	CamelService *service;
	gboolean keep_on_server;
	send_state_t state;
	GtkWidget *progress_bar;
	GtkWidget *cancel_button;

	gint again;		/* need to run send again */

	gint timeout_id;
	gchar *what;
	gint pc;

	GtkWidget *send_account_label;
	gchar *send_url;

	/*time_t update;*/
	struct _send_data *data;
};

static CamelFolder *
		receive_get_folder		(CamelFilterDriver *d,
						 const gchar *uri,
						 gpointer data,
						 GError **error);
static void	send_done (gpointer data);

static struct _send_data *send_data = NULL;
static GtkWidget *send_recv_dialog = NULL;

static void
free_folder_info (struct _folder_info *info)
{
	/*camel_folder_thaw (info->folder);	*/
	mail_sync_folder (info->folder, FALSE, NULL, NULL);
	g_object_unref (info->folder);
	g_free (info->uri);
	g_free (info);
}

static void
free_send_info (struct _send_info *info)
{
	if (info->cancellable != NULL)
		g_object_unref (info->cancellable);
	if (info->session != NULL)
		g_object_unref (info->session);
	if (info->service != NULL)
		g_object_unref (info->service);
	if (info->timeout_id != 0)
		g_source_remove (info->timeout_id);
	g_free (info->what);
	g_free (info->send_url);
	g_free (info);
}

static struct _send_data *
setup_send_data (EMailSession *session)
{
	struct _send_data *data;

	if (send_data == NULL) {
		send_data = data = g_malloc0 (sizeof (*data));
		g_mutex_init (&data->lock);
		data->folders = g_hash_table_new_full (
			g_str_hash, g_str_equal,
			(GDestroyNotify) NULL,
			(GDestroyNotify) free_folder_info);
		data->inbox =
			e_mail_session_get_local_folder (
			session, E_MAIL_LOCAL_FOLDER_LOCAL_INBOX);
		g_object_ref (data->inbox);
		data->active = g_hash_table_new_full (
			g_str_hash, g_str_equal,
			(GDestroyNotify) g_free,
			(GDestroyNotify) free_send_info);
	}

	return send_data;
}

static void
receive_cancel (GtkButton *button,
                struct _send_info *info)
{
	if (info->state == SEND_ACTIVE) {
		g_cancellable_cancel (info->cancellable);
		if (info->progress_bar != NULL)
			gtk_progress_bar_set_text (
				GTK_PROGRESS_BAR (info->progress_bar),
				_("Canceling..."));
		info->state = SEND_CANCELLED;
	}
	if (info->cancel_button)
		gtk_widget_set_sensitive (info->cancel_button, FALSE);
}

static void
free_send_data (void)
{
	struct _send_data *data = send_data;

	g_return_if_fail (g_hash_table_size (data->active) == 0);

	if (data->inbox) {
		mail_sync_folder (data->inbox, FALSE, NULL, NULL);
		/*camel_folder_thaw (data->inbox);		*/
		g_object_unref (data->inbox);
	}

	g_list_free (data->infos);
	g_hash_table_destroy (data->active);
	g_hash_table_destroy (data->folders);
	g_mutex_clear (&data->lock);
	g_free (data);
	send_data = NULL;
}

static void
cancel_send_info (gpointer key,
                  struct _send_info *info,
                  gpointer data)
{
	receive_cancel (GTK_BUTTON (info->cancel_button), info);
}

static void
hide_send_info (gpointer key,
                struct _send_info *info,
                gpointer data)
{
	info->cancel_button = NULL;
	info->progress_bar = NULL;

	if (info->timeout_id != 0) {
		g_source_remove (info->timeout_id);
		info->timeout_id = 0;
	}
}

static void
dialog_destroy_cb (struct _send_data *data,
                   GObject *deadbeef)
{
	g_hash_table_foreach (data->active, (GHFunc) hide_send_info, NULL);
	data->gd = NULL;
	send_recv_dialog = NULL;
}

static void
dialog_response (GtkDialog *gd,
                 gint button,
                 struct _send_data *data)
{
	switch (button) {
	case GTK_RESPONSE_CANCEL:
		d (printf ("cancelled whole thing\n"));
		if (!data->cancelled) {
			data->cancelled = TRUE;
			g_hash_table_foreach (data->active, (GHFunc) cancel_send_info, NULL);
		}
		gtk_dialog_set_response_sensitive (gd, GTK_RESPONSE_CANCEL, FALSE);
		break;
	default:
		d (printf ("hiding dialog\n"));
		g_hash_table_foreach (data->active, (GHFunc) hide_send_info, NULL);
		data->gd = NULL;
		/*gtk_widget_destroy((GtkWidget *)gd);*/
		break;
	}
}

static GStaticMutex status_lock = G_STATIC_MUTEX_INIT;
static gchar *format_service_name (CamelService *service);

static gint
operation_status_timeout (gpointer data)
{
	struct _send_info *info = data;

	if (info->progress_bar) {
		GtkProgressBar *progress_bar;

		g_static_mutex_lock (&status_lock);

		progress_bar = GTK_PROGRESS_BAR (info->progress_bar);

		gtk_progress_bar_set_fraction (progress_bar, info->pc / 100.0);
		if (info->what != NULL)
			gtk_progress_bar_set_text (progress_bar, info->what);
		if (info->service != NULL && info->send_account_label) {
			gchar *tmp = format_service_name (info->service);

			gtk_label_set_markup (
				GTK_LABEL (info->send_account_label), tmp);

			g_free (tmp);
		}

		g_static_mutex_unlock (&status_lock);

		return TRUE;
	}

	return FALSE;
}

static void
set_send_status (struct _send_info *info,
                 const gchar *desc,
                 gint pc)
{
	g_static_mutex_lock (&status_lock);

	g_free (info->what);
	info->what = g_strdup (desc);
	info->pc = pc;

	g_static_mutex_unlock (&status_lock);
}

static void
set_transport_service (struct _send_info *info,
                       const gchar *transport_uid)
{
	CamelService *service;

	g_static_mutex_lock (&status_lock);

	service = camel_session_ref_service (info->session, transport_uid);

	if (CAMEL_IS_TRANSPORT (service)) {
		if (info->service != NULL)
			g_object_unref (info->service);
		info->service = g_object_ref (service);
	}

	if (service != NULL)
		g_object_unref (service);

	g_static_mutex_unlock (&status_lock);
}

/* for camel operation status */
static void
operation_status (CamelOperation *op,
                  const gchar *what,
                  gint pc,
                  struct _send_info *info)
{
	set_send_status (info, what, pc);
}

static gchar *
format_service_name (CamelService *service)
{
	CamelProvider *provider;
	CamelSettings *settings;
	gchar *service_name = NULL;
	const gchar *display_name;
	gchar *pretty_url = NULL;
	gchar *host = NULL;
	gchar *path = NULL;
	gchar *user = NULL;
	gchar *cp;
	gboolean have_host = FALSE;
	gboolean have_path = FALSE;
	gboolean have_user = FALSE;

	provider = camel_service_get_provider (service);
	display_name = camel_service_get_display_name (service);

	settings = camel_service_ref_settings (service);

	if (CAMEL_IS_NETWORK_SETTINGS (settings)) {
		host = camel_network_settings_dup_host (
			CAMEL_NETWORK_SETTINGS (settings));
		have_host = (host != NULL) && (*host != '\0');

		user = camel_network_settings_dup_user (
			CAMEL_NETWORK_SETTINGS (settings));
		have_user = (user != NULL) && (*user != '\0');
	}

	if (CAMEL_IS_LOCAL_SETTINGS (settings)) {
		path = camel_local_settings_dup_path (
			CAMEL_LOCAL_SETTINGS (settings));
		have_path = (path != NULL) && (*path != '\0');
	}

	g_object_unref (settings);

	/* Shorten user names with '@', since multiple '@' in a
	 * 'user@host' label look weird.  This is just supposed
	 * to be a hint anyway so it doesn't matter if it's not
	 * strictly correct. */
	if (have_user && (cp = strchr (user, '@')) != NULL)
		*cp = '\0';

	g_return_val_if_fail (provider != NULL, NULL);

	/* This should never happen, but if the service has no
	 * display name, fall back to the generic service name. */
	if (display_name == NULL || *display_name == '\0') {
		service_name = camel_service_get_name (service, TRUE);
		display_name = service_name;
	}

	if (have_host && have_user) {
		pretty_url = g_markup_printf_escaped (
			"<b>%s</b> <small>(%s@%s)</small>",
			display_name, user, host);
	} else if (have_host) {
		pretty_url = g_markup_printf_escaped (
			"<b>%s</b> <small>(%s)</small>",
			display_name, host);
	} else if (have_path) {
		pretty_url = g_markup_printf_escaped (
			"<b>%s</b> <small>(%s)</small>",
			display_name, path);
	} else {
		pretty_url = g_markup_printf_escaped (
			"<b>%s</b>", display_name);
	}

	g_free (service_name);
	g_free (host);
	g_free (path);
	g_free (user);

	return pretty_url;
}

static send_info_t
get_receive_type (CamelService *service)
{
	CamelURL *url;
	CamelProvider *provider;
	const gchar *uid;
	gboolean is_local_delivery;

	/* Disregard CamelNullStores. */
	if (CAMEL_IS_NULL_STORE (service))
		return SEND_INVALID;

	url = camel_service_new_camel_url (service);
	is_local_delivery = em_utils_is_local_delivery_mbox_file (url);
	camel_url_free (url);

	/* mbox pointing to a file is a 'Local delivery'
	 * source which requires special processing. */
	if (is_local_delivery)
		return SEND_RECEIVE;

	provider = camel_service_get_provider (service);

	if (provider == NULL)
		return SEND_INVALID;

	/* skip some well-known services */
	uid = camel_service_get_uid (service);
	if (g_strcmp0 (uid, E_MAIL_SESSION_LOCAL_UID) == 0)
		return SEND_INVALID;
	if (g_strcmp0 (uid, E_MAIL_SESSION_VFOLDER_UID) == 0)
		return SEND_INVALID;

	if (provider->object_types[CAMEL_PROVIDER_STORE]) {
		if (provider->flags & CAMEL_PROVIDER_IS_STORAGE)
			return SEND_UPDATE;
		else
			return SEND_RECEIVE;
	}

	if (provider->object_types[CAMEL_PROVIDER_TRANSPORT])
		return SEND_SEND;

	return SEND_INVALID;
}

static gboolean
get_keep_on_server (CamelService *service)
{
	GObjectClass *class;
	CamelSettings *settings;
	gboolean keep_on_server = FALSE;

	settings = camel_service_ref_settings (service);
	class = G_OBJECT_GET_CLASS (settings);

	/* XXX This is a POP3-specific setting. */
	if (g_object_class_find_property (class, "keep-on-server") != NULL)
		g_object_get (
			settings, "keep-on-server",
			&keep_on_server, NULL);

	g_object_unref (settings);

	return keep_on_server;
}

static struct _send_data *
build_dialog (GtkWindow *parent,
              EMailSession *session,
              CamelFolder *outbox,
              CamelService *transport,
              gboolean allow_send)
{
	GtkDialog *gd;
	GtkWidget *wgrid;
	GtkGrid *grid;
	gint row;
	GList *list = NULL;
	struct _send_data *data;
	GtkWidget *container;
	GtkWidget *send_icon;
	GtkWidget *recv_icon;
	GtkWidget *scrolled_window;
	GtkWidget *label;
	GtkWidget *progress_bar;
	GtkWidget *cancel_button;
	EMailAccountStore *account_store;
	struct _send_info *info;
	gchar *pretty_url;
	EMEventTargetSendReceive *target;
	GQueue queue = G_QUEUE_INIT;

	account_store = e_mail_ui_session_get_account_store (E_MAIL_UI_SESSION (session));

	send_recv_dialog = gtk_dialog_new ();

	gd = GTK_DIALOG (send_recv_dialog);
	gtk_window_set_modal (GTK_WINDOW (send_recv_dialog), FALSE);
	gtk_window_set_icon_name (GTK_WINDOW (gd), "mail-send-receive");
	gtk_window_set_default_size (GTK_WINDOW (gd), 600, 200);
	gtk_window_set_title (GTK_WINDOW (gd), _("Send & Receive Mail"));
	gtk_window_set_transient_for (GTK_WINDOW (gd), parent);

	e_restore_window (
		GTK_WINDOW (gd),
		"/org/gnome/evolution/mail/send-recv-window/",
		E_RESTORE_WINDOW_SIZE);

	gtk_widget_ensure_style ((GtkWidget *) gd);

	container = gtk_dialog_get_action_area (gd);
	gtk_container_set_border_width (GTK_CONTAINER (container), 6);

	container = gtk_dialog_get_content_area (gd);
	gtk_container_set_border_width (GTK_CONTAINER (container), 0);

	cancel_button = gtk_button_new_with_mnemonic (_("Cancel _All"));
	gtk_button_set_image (
		GTK_BUTTON (cancel_button),
		gtk_image_new_from_stock (
			GTK_STOCK_CANCEL, GTK_ICON_SIZE_BUTTON));
	gtk_widget_show (cancel_button);
	gtk_dialog_add_action_widget (gd, cancel_button, GTK_RESPONSE_CANCEL);

	wgrid = gtk_grid_new ();
	grid = GTK_GRID (wgrid);
	gtk_container_set_border_width (GTK_CONTAINER (grid), 6);
	gtk_grid_set_column_spacing (grid, 6);

	scrolled_window = gtk_scrolled_window_new (NULL, NULL);
	gtk_container_set_border_width (
		GTK_CONTAINER (scrolled_window), 6);
	gtk_scrolled_window_set_policy (
		GTK_SCROLLED_WINDOW (scrolled_window),
		GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
	gtk_widget_set_size_request (scrolled_window, 50, 50);

	container = gtk_dialog_get_content_area (gd);
	gtk_scrolled_window_add_with_viewport (
		GTK_SCROLLED_WINDOW (scrolled_window), wgrid);
	gtk_box_pack_start (
		GTK_BOX (container), scrolled_window, TRUE, TRUE, 0);
	gtk_widget_show (scrolled_window);

	/* must bet setup after send_recv_dialog as it may re-trigger send-recv button */
	data = setup_send_data (session);

	row = 0;
	e_mail_account_store_queue_enabled_services (account_store, &queue);
	while (!g_queue_is_empty (&queue)) {
		CamelService *service;
		const gchar *uid;

		service = g_queue_pop_head (&queue);
		uid = camel_service_get_uid (service);

		/* see if we have an outstanding download active */
		info = g_hash_table_lookup (data->active, uid);
		if (info == NULL) {
			send_info_t type = SEND_INVALID;

			type = get_receive_type (service);

			if (type == SEND_INVALID || type == SEND_SEND)
				continue;

			info = g_malloc0 (sizeof (*info));
			info->type = type;
			info->session = g_object_ref (session);
			info->service = g_object_ref (service);
			info->keep_on_server = get_keep_on_server (service);
			info->cancellable = camel_operation_new ();
			info->state = allow_send ? SEND_ACTIVE : SEND_COMPLETE;
			info->timeout_id = g_timeout_add (
				STATUS_TIMEOUT, operation_status_timeout, info);

			g_signal_connect (
				info->cancellable, "status",
				G_CALLBACK (operation_status), info);

			g_hash_table_insert (
				data->active, g_strdup (uid), info);
			list = g_list_prepend (list, info);

		} else if (info->progress_bar != NULL) {
			/* incase we get the same source pop up again */
			continue;

		} else if (info->timeout_id == 0)
			info->timeout_id = g_timeout_add (
				STATUS_TIMEOUT, operation_status_timeout, info);

		recv_icon = gtk_image_new_from_icon_name (
			"mail-inbox", SEND_RECV_ICON_SIZE);
		gtk_widget_set_valign (recv_icon, GTK_ALIGN_START);

		pretty_url = format_service_name (service);
		label = gtk_label_new (NULL);
		gtk_label_set_ellipsize (
			GTK_LABEL (label), PANGO_ELLIPSIZE_END);
		gtk_label_set_markup (GTK_LABEL (label), pretty_url);
		g_free (pretty_url);

		progress_bar = gtk_progress_bar_new ();
		gtk_progress_bar_set_show_text (
			GTK_PROGRESS_BAR (progress_bar), TRUE);
		gtk_progress_bar_set_text (
			GTK_PROGRESS_BAR (progress_bar),
			(info->type == SEND_UPDATE) ?
			_("Updating...") : _("Waiting..."));
		gtk_widget_set_margin_bottom (progress_bar, 12);

		cancel_button = gtk_button_new_from_stock (GTK_STOCK_CANCEL);
		gtk_widget_set_valign (cancel_button, GTK_ALIGN_END);
		gtk_widget_set_margin_bottom (cancel_button, 12);

		/* g_object_set(data->label, "bold", TRUE, NULL); */
		gtk_misc_set_alignment (GTK_MISC (label), 0, .5);

		gtk_widget_set_hexpand (label, TRUE);
		gtk_widget_set_halign (label, GTK_ALIGN_FILL);

		gtk_grid_attach (grid, recv_icon, 0, row, 1, 2);
		gtk_grid_attach (grid, label, 1, row, 1, 1);
		gtk_grid_attach (grid, progress_bar, 1, row + 1, 1, 1);
		gtk_grid_attach (grid, cancel_button, 2, row, 1, 2);

		info->progress_bar = progress_bar;
		info->cancel_button = cancel_button;
		info->data = data;

		g_signal_connect (
			cancel_button, "clicked",
			G_CALLBACK (receive_cancel), info);

		row = row + 2;
	}

	/* we also need gd during emition to be able to catch Cancel All */
	data->gd = gd;
	target = em_event_target_new_send_receive (
		em_event_peek (), wgrid, data, row, EM_EVENT_SEND_RECEIVE);
	e_event_emit (
		(EEvent *) em_event_peek (), "mail.sendreceive",
		(EEventTarget *) target);

	/* Skip displaying the SMTP row if we've got no outbox,
	 * outgoing account or unsent mails. */
	if (allow_send && outbox && CAMEL_IS_TRANSPORT (transport)
	 && (camel_folder_get_message_count (outbox) -
		camel_folder_get_deleted_message_count (outbox)) != 0) {

		info = g_hash_table_lookup (data->active, SEND_URI_KEY);
		if (info == NULL) {
			info = g_malloc0 (sizeof (*info));
			info->type = SEND_SEND;
			info->session = g_object_ref (session);
			info->service = g_object_ref (transport);
			info->keep_on_server = FALSE;
			info->cancellable = camel_operation_new ();
			info->state = SEND_ACTIVE;
			info->timeout_id = g_timeout_add (
				STATUS_TIMEOUT, operation_status_timeout, info);

			g_signal_connect (
				info->cancellable, "status",
				G_CALLBACK (operation_status), info);

			g_hash_table_insert (
				data->active, g_strdup (SEND_URI_KEY), info);
			list = g_list_prepend (list, info);
		} else if (info->timeout_id == 0)
			info->timeout_id = g_timeout_add (
				STATUS_TIMEOUT, operation_status_timeout, info);

		send_icon = gtk_image_new_from_icon_name (
			"mail-outbox", SEND_RECV_ICON_SIZE);
		gtk_widget_set_valign (send_icon, GTK_ALIGN_START);

		pretty_url = format_service_name (transport);
		label = gtk_label_new (NULL);
		gtk_label_set_ellipsize (
			GTK_LABEL (label), PANGO_ELLIPSIZE_END);
		gtk_label_set_markup (GTK_LABEL (label), pretty_url);
		g_free (pretty_url);

		progress_bar = gtk_progress_bar_new ();
		gtk_progress_bar_set_show_text (
			GTK_PROGRESS_BAR (progress_bar), TRUE);
		gtk_progress_bar_set_text (
			GTK_PROGRESS_BAR (progress_bar), _("Waiting..."));
		gtk_widget_set_margin_bottom (progress_bar, 12);

		cancel_button = gtk_button_new_from_stock (GTK_STOCK_CANCEL);
		gtk_widget_set_valign (cancel_button, GTK_ALIGN_END);

		gtk_misc_set_alignment (GTK_MISC (label), 0, .5);

		gtk_widget_set_hexpand (label, TRUE);
		gtk_widget_set_halign (label, GTK_ALIGN_FILL);

		gtk_grid_attach (grid, send_icon, 0, row, 1, 2);
		gtk_grid_attach (grid, label, 1, row, 1, 1);
		gtk_grid_attach (grid, progress_bar, 1, row + 1, 1, 1);
		gtk_grid_attach (grid, cancel_button, 2, row, 1, 2);

		info->progress_bar = progress_bar;
		info->cancel_button = cancel_button;
		info->data = data;
		info->send_account_label = label;

		g_signal_connect (
			cancel_button, "clicked",
			G_CALLBACK (receive_cancel), info);
	}

	gtk_widget_show_all (wgrid);

	if (parent != NULL)
		gtk_widget_show (GTK_WIDGET (gd));

	g_signal_connect (
		gd, "response",
		G_CALLBACK (dialog_response), data);

	g_object_weak_ref ((GObject *) gd, (GWeakNotify) dialog_destroy_cb, data);

	data->infos = list;

	return data;
}

static void
update_folders (gchar *uri,
                struct _folder_info *info,
                gpointer data)
{
	time_t now = *((time_t *) data);

	d (printf ("checking update for folder: %s\n", info->uri));

	/* let it flow through to the folders every 10 seconds */
	/* we back off slowly as we progress */
	if (now > info->update + 10 + info->count *5) {
		d (printf ("upating a folder: %s\n", info->uri));
		/*camel_folder_thaw(info->folder);
		  camel_folder_freeze (info->folder);*/
		info->update = now;
		info->count++;
	}
}

static void
receive_status (CamelFilterDriver *driver,
                enum camel_filter_status_t status,
                gint pc,
                const gchar *desc,
                gpointer data)
{
	struct _send_info *info = data;
	time_t now = time (NULL);

	/* let it flow through to the folder, every now and then too? */
	g_hash_table_foreach (info->data->folders, (GHFunc) update_folders, &now);

	if (info->data->inbox && now > info->data->inbox_update + 20) {
		d (printf ("updating inbox too\n"));
		/* this doesn't seem to work right :( */
		/*camel_folder_thaw(info->data->inbox);
		  camel_folder_freeze (info->data->inbox);*/
		info->data->inbox_update = now;
	}

	/* we just pile them onto the port, assuming it can handle it.
	 * We could also have a receiver port and see if they've been processed
	 * yet, so if this is necessary its not too hard to add */
	/* the mail_gui_port receiver will free everything for us */
	switch (status) {
	case CAMEL_FILTER_STATUS_START:
	case CAMEL_FILTER_STATUS_END:
		set_send_status (info, desc, pc);
		break;
	case CAMEL_FILTER_STATUS_ACTION:
		set_transport_service (info, desc);
		break;
	default:
		break;
	}
}

/* when receive/send is complete */
static void
receive_done (gint still_more,
              gpointer data)
{
	struct _send_info *info = data;
	const gchar *uid;

	uid = camel_service_get_uid (info->service);
	g_return_if_fail (uid != NULL);

	/* if we've been called to run again - run again */
	if (info->type == SEND_SEND && info->state == SEND_ACTIVE && info->again) {
		CamelFolder *local_outbox;

		local_outbox =
			e_mail_session_get_local_folder (
			E_MAIL_SESSION (info->session),
			E_MAIL_LOCAL_FOLDER_OUTBOX);

		g_return_if_fail (CAMEL_IS_TRANSPORT (info->service));

		info->again = 0;
		mail_send_queue (
			E_MAIL_SESSION (info->session),
			local_outbox,
			CAMEL_TRANSPORT (info->service),
			E_FILTER_SOURCE_OUTGOING,
			info->cancellable,
			receive_get_folder, info,
			receive_status, info,
			send_done, info);
		return;
	}

	if (info->progress_bar) {
		const gchar *text;

		gtk_progress_bar_set_fraction (
			GTK_PROGRESS_BAR (info->progress_bar), 1.0);

		if (info->state == SEND_CANCELLED)
			text = _("Canceled");
		else {
			text = _("Complete");
			info->state = SEND_COMPLETE;
		}

		gtk_progress_bar_set_text (
			GTK_PROGRESS_BAR (info->progress_bar), text);
	}

	if (info->cancel_button)
		gtk_widget_set_sensitive (info->cancel_button, FALSE);

	/* remove/free this active download */
	d (printf ("%s: freeing info %p\n", G_STRFUNC, info));
	if (info->type == SEND_SEND) {
		gpointer key = NULL, value = NULL;
		if (!g_hash_table_lookup_extended (info->data->active, SEND_URI_KEY, &key, &value))
			key = NULL;

		g_hash_table_steal (info->data->active, SEND_URI_KEY);
		g_free (key);
	} else {
		gpointer key = NULL, value = NULL;
		if (!g_hash_table_lookup_extended (info->data->active, uid, &key, &value))
			key = NULL;

		g_hash_table_steal (info->data->active, uid);
		g_free (key);
	}
	info->data->infos = g_list_remove (info->data->infos, info);

	if (g_hash_table_size (info->data->active) == 0) {
		if (info->data->gd)
			gtk_widget_destroy ((GtkWidget *) info->data->gd);
		free_send_data ();
	}

	free_send_info (info);
}

static void
send_done (gpointer data)
{
	receive_done (-1, data);
}
/* although we dont do anythign smart here yet, there is no need for this interface to
 * be available to anyone else.
 * This can also be used to hook into which folders are being updated, and occasionally
 * let them refresh */
static CamelFolder *
receive_get_folder (CamelFilterDriver *d,
                    const gchar *uri,
                    gpointer data,
                    GError **error)
{
	struct _send_info *info = data;
	CamelFolder *folder;
	struct _folder_info *oldinfo;
	gpointer oldkey, oldinfoptr;

	g_mutex_lock (&info->data->lock);
	oldinfo = g_hash_table_lookup (info->data->folders, uri);
	g_mutex_unlock (&info->data->lock);

	if (oldinfo) {
		g_object_ref (oldinfo->folder);
		return oldinfo->folder;
	}

	/* FIXME Not passing a GCancellable here. */
	folder = e_mail_session_uri_to_folder_sync (
		E_MAIL_SESSION (info->session), uri, 0, NULL, error);
	if (!folder)
		return NULL;

	/* we recheck that the folder hasn't snuck in while we were loading it... */
	/* and we assume the newer one is the same, but unref the old one anyway */
	g_mutex_lock (&info->data->lock);

	if (g_hash_table_lookup_extended (
			info->data->folders, uri, &oldkey, &oldinfoptr)) {
		oldinfo = (struct _folder_info *) oldinfoptr;
		g_object_unref (oldinfo->folder);
		oldinfo->folder = folder;
	} else {
		oldinfo = g_malloc0 (sizeof (*oldinfo));
		oldinfo->folder = folder;
		oldinfo->uri = g_strdup (uri);
		g_hash_table_insert (info->data->folders, oldinfo->uri, oldinfo);
	}

	g_object_ref (folder);

	g_mutex_unlock (&info->data->lock);

	return folder;
}

/* ********************************************************************** */

static void
get_folders (CamelStore *store,
             GPtrArray *folders,
             CamelFolderInfo *info)
{
	while (info) {
		if (camel_store_can_refresh_folder (store, info, NULL)) {
			if ((info->flags & CAMEL_FOLDER_NOSELECT) == 0) {
				gchar *folder_uri;

				folder_uri = e_mail_folder_uri_build (
					store, info->full_name);
				g_ptr_array_add (folders, folder_uri);
			}
		}

		get_folders (store, folders, info->child);
		info = info->next;
	}
}

static void
main_op_cancelled_cb (GCancellable *main_op,
                      GCancellable *refresh_op)
{
	g_cancellable_cancel (refresh_op);
}

struct _refresh_folders_msg {
	MailMsg base;

	struct _send_info *info;
	GPtrArray *folders;
	CamelStore *store;
	CamelFolderInfo *finfo;
};

static gchar *
refresh_folders_desc (struct _refresh_folders_msg *m)
{
	return g_strdup_printf (
		_("Checking for new mail at '%s'"),
		camel_service_get_display_name (CAMEL_SERVICE (m->store)));
}

static void
refresh_folders_exec (struct _refresh_folders_msg *m,
                      GCancellable *cancellable,
                      GError **error)
{
	CamelFolder *folder;
	gint i;
	GError *local_error = NULL;
	gulong handler_id = 0;

	if (cancellable)
		handler_id = g_signal_connect (
			m->info->cancellable, "cancelled",
			G_CALLBACK (main_op_cancelled_cb), cancellable);

	get_folders (m->store, m->folders, m->finfo);

	camel_operation_push_message (m->info->cancellable, _("Updating..."));

	for (i = 0; i < m->folders->len; i++) {
		folder = e_mail_session_uri_to_folder_sync (
			E_MAIL_SESSION (m->info->session),
			m->folders->pdata[i], 0,
			cancellable, &local_error);
		if (folder && camel_folder_synchronize_sync (folder, FALSE, cancellable, &local_error))
			camel_folder_refresh_info_sync (folder, cancellable, &local_error);

		if (local_error != NULL) {
			if (!g_error_matches (local_error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
				const gchar *account_name = NULL, *full_name;

				if (folder) {
					CamelStore *store = camel_folder_get_parent_store (folder);

					account_name = camel_service_get_display_name (CAMEL_SERVICE (store));
					full_name = camel_folder_get_full_name (folder);
				} else
					full_name = (const gchar *) m->folders->pdata[i];

				g_warning ("Failed to refresh folder '%s%s%s': %s",
					account_name ? account_name : "",
					account_name ? ": " : "",
					full_name, local_error->message);
			}

			g_clear_error (&local_error);
		}

		if (folder)
			g_object_unref (folder);

		if (g_cancellable_is_cancelled (m->info->cancellable) ||
		    g_cancellable_is_cancelled (cancellable))
			break;

		if (m->info->state != SEND_CANCELLED)
			camel_operation_progress (
				m->info->cancellable, 100 * i / m->folders->len);
	}

	camel_operation_pop_message (m->info->cancellable);

	if (cancellable)
		g_signal_handler_disconnect (m->info->cancellable, handler_id);
}

static void
refresh_folders_done (struct _refresh_folders_msg *m)
{
	receive_done (-1, m->info);
}

static void
refresh_folders_free (struct _refresh_folders_msg *m)
{
	gint i;

	for (i = 0; i < m->folders->len; i++)
		g_free (m->folders->pdata[i]);
	g_ptr_array_free (m->folders, TRUE);

	camel_store_free_folder_info (m->store, m->finfo);
	g_object_unref (m->store);
}

static MailMsgInfo refresh_folders_info = {
	sizeof (struct _refresh_folders_msg),
	(MailMsgDescFunc) refresh_folders_desc,
	(MailMsgExecFunc) refresh_folders_exec,
	(MailMsgDoneFunc) refresh_folders_done,
	(MailMsgFreeFunc) refresh_folders_free
};

static gboolean
receive_update_got_folderinfo (MailFolderCache *folder_cache,
                               CamelStore *store,
                               CamelFolderInfo *info,
                               gpointer data)
{
	if (info) {
		GPtrArray *folders = g_ptr_array_new ();
		struct _refresh_folders_msg *m;
		struct _send_info *sinfo = data;

		m = mail_msg_new (&refresh_folders_info);
		m->store = store;
		g_object_ref (store);
		m->folders = folders;
		m->info = sinfo;
		m->finfo = info;

		mail_msg_unordered_push (m);

		/* do not free folder info, we will free it later */
		return FALSE;
	} else {
		receive_done (-1, data);
	}

	return TRUE;
}

static void
receive_update_got_store (CamelStore *store,
                          struct _send_info *info)
{
	MailFolderCache *folder_cache;

	folder_cache = e_mail_session_get_folder_cache (
		E_MAIL_SESSION (info->session));

	if (store != NULL) {
		mail_folder_cache_note_store (
			folder_cache, store, info->cancellable,
			receive_update_got_folderinfo, info);
	} else {
		receive_done (-1, info);
	}
}

static CamelService *
ref_default_transport (EMailSession *session)
{
	ESource *source;
	ESourceRegistry *registry;
	CamelService *service;
	const gchar *extension_name;
	const gchar *uid;

	registry = e_mail_session_get_registry (session);
	source = e_source_registry_ref_default_mail_identity (registry);

	if (source == NULL)
		return NULL;

	extension_name = E_SOURCE_EXTENSION_MAIL_SUBMISSION;
	if (e_source_has_extension (source, extension_name)) {
		ESourceMailSubmission *extension;
		gchar *uid;

		extension = e_source_get_extension (source, extension_name);
		uid = e_source_mail_submission_dup_transport_uid (extension);

		g_object_unref (source);
		source = e_source_registry_ref_source (registry, uid);

		g_free (uid);
	} else {
		g_object_unref (source);
		source = NULL;
	}

	if (source == NULL)
		return NULL;

	uid = e_source_get_uid (source);
	service = camel_session_ref_service (CAMEL_SESSION (session), uid);

	g_object_unref (source);

	return service;
}

static GtkWidget *
send_receive (GtkWindow *parent,
              EMailSession *session,
              gboolean allow_send)
{
	CamelFolder *local_outbox;
	CamelService *transport;
	struct _send_data *data;
	GList *scan;

	if (send_recv_dialog != NULL) {
		if (parent != NULL && gtk_widget_get_realized (send_recv_dialog)) {
			gtk_window_present (GTK_WINDOW (send_recv_dialog));
		}
		return send_recv_dialog;
	}

	if (!camel_session_get_online (CAMEL_SESSION (session)))
		return send_recv_dialog;

	transport = ref_default_transport (session);

	local_outbox =
		e_mail_session_get_local_folder (
		session, E_MAIL_LOCAL_FOLDER_OUTBOX);

	data = build_dialog (
		parent, session, local_outbox, transport, allow_send);

	if (transport != NULL)
		g_object_unref (transport);

	for (scan = data->infos; scan != NULL; scan = scan->next) {
		struct _send_info *info = scan->data;

		if (!CAMEL_IS_SERVICE (info->service))
			continue;

		switch (info->type) {
		case SEND_RECEIVE:
			mail_fetch_mail (
				CAMEL_STORE (info->service),
				CAMEL_FETCH_OLD_MESSAGES, -1,
				E_FILTER_SOURCE_INCOMING,
				NULL, NULL, NULL,
				info->cancellable,
				receive_get_folder, info,
				receive_status, info,
				receive_done, info);
			break;
		case SEND_SEND:
			/* todo, store the folder in info? */
			mail_send_queue (
				session, local_outbox,
				CAMEL_TRANSPORT (info->service),
				E_FILTER_SOURCE_OUTGOING,
				info->cancellable,
				receive_get_folder, info,
				receive_status, info,
				send_done, info);
			break;
		case SEND_UPDATE:
			receive_update_got_store (
				CAMEL_STORE (info->service), info);
			break;
		default:
			break;
		}
	}

	return send_recv_dialog;
}

GtkWidget *
mail_send_receive (GtkWindow *parent,
                   EMailSession *session)
{
	return send_receive (parent, session, TRUE);
}

GtkWidget *
mail_receive (GtkWindow *parent,
              EMailSession *session)
{
	return send_receive (parent, session, FALSE);
}

/* We setup the download info's in a hashtable, if we later
 * need to build the gui, we insert them in to add them. */
void
mail_receive_service (CamelService *service)
{
	struct _send_info *info;
	struct _send_data *data;
	CamelSession *session;
	CamelFolder *local_outbox;
	const gchar *uid;
	send_info_t type = SEND_INVALID;

	g_return_if_fail (CAMEL_IS_SERVICE (service));

	uid = camel_service_get_uid (service);
	session = camel_service_get_session (service);

	data = setup_send_data (E_MAIL_SESSION (session));
	info = g_hash_table_lookup (data->active, uid);

	if (info != NULL)
		return;

	type = get_receive_type (service);

	if (type == SEND_INVALID || type == SEND_SEND)
		return;

	info = g_malloc0 (sizeof (*info));
	info->type = type;
	info->progress_bar = NULL;
	info->session = g_object_ref (session);
	info->service = g_object_ref (service);
	info->keep_on_server = get_keep_on_server (service);
	info->cancellable = camel_operation_new ();
	info->cancel_button = NULL;
	info->data = data;
	info->state = SEND_ACTIVE;
	info->timeout_id = 0;

	g_signal_connect (
		info->cancellable, "status",
		G_CALLBACK (operation_status), info);

	d (printf ("Adding new info %p\n", info));

	g_hash_table_insert (data->active, g_strdup (uid), info);

	switch (info->type) {
	case SEND_RECEIVE:
		mail_fetch_mail (
			CAMEL_STORE (service),
			CAMEL_FETCH_OLD_MESSAGES, -1,
			E_FILTER_SOURCE_INCOMING,
			NULL, NULL, NULL,
			info->cancellable,
			receive_get_folder, info,
			receive_status, info,
			receive_done, info);
		break;
	case SEND_SEND:
		/* todo, store the folder in info? */
		local_outbox =
			e_mail_session_get_local_folder (
			E_MAIL_SESSION (session),
			E_MAIL_LOCAL_FOLDER_OUTBOX);
		mail_send_queue (
			E_MAIL_SESSION (session),
			local_outbox,
			CAMEL_TRANSPORT (service),
			E_FILTER_SOURCE_OUTGOING,
			info->cancellable,
			receive_get_folder, info,
			receive_status, info,
			send_done, info);
		break;
	case SEND_UPDATE:
		receive_update_got_store (CAMEL_STORE (service), info);
		break;
	default:
		g_return_if_reached ();
	}
}

void
mail_send (EMailSession *session)
{
	CamelFolder *local_outbox;
	CamelService *service;
	struct _send_info *info;
	struct _send_data *data;
	send_info_t type = SEND_INVALID;

	g_return_if_fail (E_IS_MAIL_SESSION (session));

	service = ref_default_transport (session);
	if (service == NULL)
		return;

	data = setup_send_data (session);
	info = g_hash_table_lookup (data->active, SEND_URI_KEY);
	if (info != NULL) {
		info->again++;
		d (printf ("send of %s still in progress\n", transport->url));
		g_object_unref (service);
		return;
	}

	d (printf ("starting non-interactive send of '%s'\n", transport->url));

	type = get_receive_type (service);
	if (type == SEND_INVALID) {
		g_object_unref (service);
		return;
	}

	info = g_malloc0 (sizeof (*info));
	info->type = SEND_SEND;
	info->progress_bar = NULL;
	info->session = g_object_ref (session);
	info->service = g_object_ref (service);
	info->keep_on_server = FALSE;
	info->cancellable = NULL;
	info->cancel_button = NULL;
	info->data = data;
	info->state = SEND_ACTIVE;
	info->timeout_id = 0;

	d (printf ("Adding new info %p\n", info));

	g_hash_table_insert (data->active, g_strdup (SEND_URI_KEY), info);

	/* todo, store the folder in info? */
	local_outbox =
		e_mail_session_get_local_folder (
		session, E_MAIL_LOCAL_FOLDER_OUTBOX);

	mail_send_queue (
		session, local_outbox,
		CAMEL_TRANSPORT (service),
		E_FILTER_SOURCE_OUTGOING,
		info->cancellable,
		receive_get_folder, info,
		receive_status, info,
		send_done, info);

	g_object_unref (service);
}
