/*
 * e-editor-link-dialog.h
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

#if !defined (__E_UTIL_H_INSIDE__) && !defined (LIBEUTIL_COMPILATION)
#error "Only <e-util/e-util.h> should be included directly."
#endif

#ifndef E_EDITOR_LINK_DIALOG_H
#define E_EDITOR_LINK_DIALOG_H

#include <e-util/e-editor-dialog.h>

/* Standard GObject macros */
#define E_TYPE_EDITOR_LINK_DIALOG \
	(e_editor_link_dialog_get_type ())
#define E_EDITOR_LINK_DIALOG(obj) \
	(G_TYPE_CHECK_INSTANCE_CAST \
	((obj), E_TYPE_EDITOR_LINK_DIALOG, EEditorLinkDialog))
#define E_EDITOR_LINK_DIALOG_CLASS(cls) \
	(G_TYPE_CHECK_CLASS_CAST \
	((cls), E_TYPE_EDITOR_LINK_DIALOG, EEditorLinkDialogClass))
#define E_IS_EDITOR_LINK_DIALOG(obj) \
	(G_TYPE_CHECK_INSTANCE_TYPE \
	((obj), E_TYPE_EDITOR_LINK_DIALOG))
#define E_IS_EDITOR_LINK_DIALOG_CLASS(cls) \
	(G_TYPE_CHECK_CLASS_TYPE \
	((cls), E_TYPE_EDITOR_LINK_DIALOG))
#define E_EDITOR_LINK_DIALOG_GET_CLASS(obj) \
	(G_TYPE_INSTANCE_GET_CLASS \
	((obj), E_TYPE_EDITOR_LINK_DIALOG, EEditorLinkDialogClass))

G_BEGIN_DECLS

typedef struct _EEditorLinkDialog EEditorLinkDialog;
typedef struct _EEditorLinkDialogClass EEditorLinkDialogClass;
typedef struct _EEditorLinkDialogPrivate EEditorLinkDialogPrivate;

struct _EEditorLinkDialog {
	EEditorDialog parent;

	EEditorLinkDialogPrivate *priv;
};

struct _EEditorLinkDialogClass {
	EEditorDialogClass parent_class;
};

GType		e_editor_link_dialog_get_type	(void);

GtkWidget*	e_editor_link_dialog_new	(EEditor *editor);

G_END_DECLS

#endif /* E_EDITOR_LINK_DIALOG_H */
