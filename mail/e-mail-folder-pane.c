/*
 * e-mail-folder-pane.c
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
 *
 * Copyright (C) 1999-2008 Novell, Inc. (www.novell.com)
 *
 */

#include "e-mail-folder-pane.h"

#include <string.h>
#include <glib/gi18n.h>

#include "e-util/e-util.h"
#include "e-util/e-plugin-ui.h"
#include "e-util/gconf-bridge.h"
#include "shell/e-shell.h"
#include "shell/e-shell-utils.h"
#include "widgets/misc/e-popup-action.h"
#include "widgets/misc/e-preview-pane.h"

#include "mail/e-mail-reader.h"
#include "mail/e-mail-reader-utils.h"
#include "mail/em-folder-tree-model.h"
#include "mail/em-format-html-display.h"
#include "mail/em-composer-utils.h"
#include "mail/em-utils.h"
#include "mail/mail-tools.h"
#include "mail/message-list.h"

#define E_MAIL_FOLDER_PANE_GET_PRIVATE(obj) \
	(G_TYPE_INSTANCE_GET_PRIVATE \
	((obj), E_TYPE_MAIL_FOLDER_PANE, EMailFolderPanePrivate))

struct _EMailFolderPanePrivate {
	gint placeholder;
};

enum {
	PROP_0,
	PROP_PREVIEW_VISIBLE
};

G_DEFINE_TYPE (EMailFolderPane, e_mail_folder_pane, E_TYPE_MAIL_PANED_VIEW)

static void
folder_pane_set_preview_visible (EMailView *view,
                                 gboolean preview_visible)
{
}

static gboolean
folder_pane_get_preview_visible (EMailView *view)
{

	return FALSE;
}

static void
mail_folder_pane_get_property (GObject *object,
                               guint property_id,
                               GValue *value,
                               GParamSpec *pspec)
{
	switch (property_id) {
		case PROP_PREVIEW_VISIBLE:
			g_value_set_boolean (
				value,
				FALSE);
			return;

	}

	G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
}

static void
mail_folder_pane_set_property (GObject *object,
                               guint property_id,
                               const GValue *value,
                               GParamSpec *pspec)
{
	switch (property_id) {
		case PROP_PREVIEW_VISIBLE:
			return;
	}

	G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
}

static guint
mfp_open_selected_mail (EMailPanedView *view)
{
	GPtrArray *uids;
	gint i;
	GtkWindow *window;
	CamelFolder *folder;
	const gchar *folder_uri;
	GPtrArray *views;
	guint n_views, ii;

	uids = e_mail_reader_get_selected_uids (E_MAIL_READER(view));
	window = e_mail_reader_get_window (E_MAIL_READER(view));
	if (!em_utils_ask_open_many (window, uids->len)) {
		em_utils_uids_free (uids);
		return 0;
	}

	folder = e_mail_reader_get_folder (E_MAIL_READER(view));
	folder_uri = e_mail_reader_get_folder_uri (E_MAIL_READER(view));
	if (em_utils_folder_is_drafts (folder, folder_uri) ||
		em_utils_folder_is_outbox (folder, folder_uri) ||
		em_utils_folder_is_templates (folder, folder_uri)) {
		em_utils_edit_messages (folder, uids, TRUE);
		return 0;
	}

	views = g_ptr_array_new ();
	/* For vfolders we need to edit the original, not the vfolder copy. */
	for (ii = 0; ii < uids->len; ii++) {
		const gchar *uid = uids->pdata[ii];
		CamelFolder *real_folder;
		CamelMessageInfo *info;
		gchar *real_folder_uri;
		gchar *real_uid;

		if (!CAMEL_IS_VEE_FOLDER (folder)) {
			g_ptr_array_add (views, g_strdup (uid));
			continue;
		}

		info = camel_folder_get_message_info (folder, uid);
		if (info == NULL)
			continue;

		real_folder = camel_vee_folder_get_location (
			CAMEL_VEE_FOLDER (folder),
			(CamelVeeMessageInfo *) info, &real_uid);
		real_folder_uri = mail_tools_folder_to_url (real_folder);

		if (em_utils_folder_is_drafts (real_folder, real_folder_uri) ||
			em_utils_folder_is_outbox (real_folder, real_folder_uri)) {
			GPtrArray *edits;

			edits = g_ptr_array_new ();
			g_ptr_array_add (edits, real_uid);
			em_utils_edit_messages (real_folder, edits, TRUE);
		} else {
			g_free (real_uid);
			g_ptr_array_add (views, g_strdup (uid));
		}

		g_free (real_folder_uri);
		camel_folder_free_message_info (folder, info);
	}

	n_views = views->len;
	for (i = 0; i < n_views; i++)
		g_signal_emit_by_name (view, "open-mail", views->pdata[i]);

	g_ptr_array_foreach (views, (GFunc) g_free, NULL);
	g_ptr_array_free (views, TRUE);

	em_utils_uids_free (uids);

	return n_views;
}

static void
e_mail_folder_pane_class_init (EMailFolderPaneClass *class)
{
	GObjectClass *object_class;
	EMailViewClass *mail_view_class;
	EMailPanedViewClass *mail_paned_view_class;

	g_type_class_add_private (class, sizeof (EMailFolderPanePrivate));

	object_class = G_OBJECT_CLASS (class);
	object_class->set_property = mail_folder_pane_set_property;
	object_class->get_property = mail_folder_pane_get_property;

	mail_view_class = E_MAIL_VIEW_CLASS (class);
	mail_view_class->set_preview_visible = folder_pane_set_preview_visible;
	mail_view_class->get_preview_visible = folder_pane_get_preview_visible;

	mail_paned_view_class = E_MAIL_PANED_VIEW_CLASS (class);
	mail_paned_view_class->open_selected_mail = mfp_open_selected_mail;

	g_object_class_override_property (
		object_class,
		PROP_PREVIEW_VISIBLE,
		"preview-visible");
}

static void
e_mail_folder_pane_init (EMailFolderPane *browser)
{
	browser->priv = E_MAIL_FOLDER_PANE_GET_PRIVATE (browser);
}

GtkWidget *
e_mail_folder_pane_new (EShellContent *content)
{
	g_return_val_if_fail (E_IS_SHELL_CONTENT (content), NULL);

	return g_object_new (
		E_TYPE_MAIL_FOLDER_PANE,
		"shell-content", content,
		"preview-visible", FALSE,
		NULL);
}

