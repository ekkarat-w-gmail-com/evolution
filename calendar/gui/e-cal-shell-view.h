/* -*- Mode: C; indent-tabs-mode: t; c-basic-offset: 8; tab-width: 8 -*- */
/* e-cal-shell-view.h
 *
 * Copyright (C) 1999-2008 Novell, Inc. (www.novell.com)
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of version 2 of the GNU General Public
 * License as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public
 * License along with this program; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#ifndef E_CAL_SHELL_VIEW_H
#define E_CAL_SHELL_VIEW_H

#include <gnome-cal.h>
#include <e-shell-view.h>
#include <libedataserver/e-source-list.h>

/* Standard GObject macros */
#define E_TYPE_CAL_SHELL_VIEW \
	(e_cal_shell_view_type)
#define E_CAL_SHELL_VIEW(obj) \
	(G_TYPE_CHECK_INSTANCE_CAST \
	((obj), E_TYPE_CAL_SHELL_VIEW, ECalShellView))
#define E_CAL_SHELL_VIEW_CLASS(cls) \
	(G_TYPE_CHECK_CLASS_CAST \
	((cls), E_TYPE_CAL_SHELL_VIEW, ECalShellViewClass))
#define E_IS_CAL_SHELL_VIEW(obj) \
	(G_TYPE_CHECK_INSTANCE_TYPE \
	((obj), E_TYPE_CAL_SHELL_VIEW))
#define E_IS_CAL_SHELL_VIEW_CLASS(cls) \
	(G_TYPE_CHECK_CLASS_TYPE \
	((cls), E_TYPE_CAL_SHELL_VIEW))
#define E_CAL_SHELL_VIEW_GET_CLASS(obj) \
	(G_TYPE_INSTANCE_GET_CLASS \
	((obj), E_TYPE_CAL_SHELL_VIEW, ECalShellViewClass))

G_BEGIN_DECLS

extern GType e_cal_shell_view_type;

typedef struct _ECalShellView ECalShellView;
typedef struct _ECalShellViewClass ECalShellViewClass;
typedef struct _ECalShellViewPrivate ECalShellViewPrivate;

struct _ECalShellView {
	EShellView parent;
	ECalShellViewPrivate *priv;
};

struct _ECalShellViewClass {
	EShellViewClass parent_class;
};

GType		e_cal_shell_view_get_type
					(GTypeModule *type_module);
GnomeCalendar *	e_cal_shell_view_get_calendar
					(ECalShellView *cal_shell_view);
ESourceList *	e_cal_shell_view_get_source_list
					(ECalShellView *cal_shell_view);

G_END_DECLS

#endif /* E_CAL_SHELL_VIEW_H */
