/* Evolution calendar - Recurrence page of the calendar component dialogs
 *
 * Copyright (C) 1999-2008 Novell, Inc. (www.novell.com)
 *
 * Authors: Federico Mena-Quintero <federico@ximian.com>
 *          Miguel de Icaza <miguel@ximian.com>
 *          Seth Alves <alves@hungry.com>
 *          JP Rosevear <jpr@ximian.com>
 *          Hans Petter Jansson <hpj@ximian.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of version 2 of the GNU General Public
 * License as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 */

#ifndef RECURRENCE_PAGE_H
#define RECURRENCE_PAGE_H

#include "comp-editor.h"
#include "comp-editor-page.h"

/* Standard GObject macros */
#define TYPE_RECURRENCE_PAGE \
	(recurrence_page_get_type ())
#define RECURRENCE_PAGE(obj) \
	(G_TYPE_CHECK_INSTANCE_CAST \
	((obj), TYPE_RECURRENCE_PAGE, RecurrencePage))
#define RECURRENCE_PAGE_CLASS(cls) \
	(G_TYPE_CHECK_CLASS_CAST \
	((cls), TYPE_RECURRENCE_PAGE, RecurrencePageClass))
#define IS_RECURRENCE_PAGE(obj) \
	(G_TYPE_CHECK_INSTANCE_TYPE \
	((obj), TYPE_RECURRENCE_PAGE))
#define IS_RECURRENCE_PAGE_CLASS(cls) \
	(G_TYPE_CHECK_CLASS_TYPE \
	((obj), TYPE_RECURRENCE_PAGE))
#define RECURRENCE_PAGE_GET_CLASS(obj) \
	(G_TYPE_INSTANCE_GET_CLASS \
	((obj), TYPE_RECURRENCE_PAGE, RecurrencePageClass))

G_BEGIN_DECLS

typedef struct _RecurrencePage RecurrencePage;
typedef struct _RecurrencePageClass RecurrencePageClass;
typedef struct _RecurrencePagePrivate RecurrencePagePrivate;

struct _RecurrencePage {
	CompEditorPage page;
	RecurrencePagePrivate *priv;
};

struct _RecurrencePageClass {
	CompEditorPageClass parent_class;
};

GType		recurrence_page_get_type	(void);
RecurrencePage *recurrence_page_construct	(RecurrencePage *rpage);
RecurrencePage *recurrence_page_new		(CompEditor *editor);

G_END_DECLS

#endif /* RECURRENCE_PAGE_H */
