/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*- */
/* 
 * gal-view-instance.h
 * Copyright 2000, 2001, Ximian, Inc.
 *
 * Authors:
 *   Chris Lahey <clahey@ximian.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License, version 2, as published by the Free Software Foundation.
 *
 * This library is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA
 * 02111-1307, USA.
 */

#ifndef _GAL_VIEW_INSTANCE_H_
#define _GAL_VIEW_INSTANCE_H_

#include <gtk/gtkobject.h>
#include <gal/menus/gal-view-collection.h>
#include <libgnome/gnome-defs.h>

BEGIN_GNOME_DECLS


#define GAL_VIEW_INSTANCE_TYPE        (gal_view_instance_get_type ())
#define GAL_VIEW_INSTANCE(o)          (GTK_CHECK_CAST ((o), GAL_VIEW_INSTANCE_TYPE, GalViewInstance))
#define GAL_VIEW_INSTANCE_CLASS(k)    (GTK_CHECK_CLASS_CAST((k), GAL_VIEW_INSTANCE_TYPE, GalViewInstanceClass))
#define GAL_IS_VIEW_INSTANCE(o)       (GTK_CHECK_TYPE ((o), GAL_VIEW_INSTANCE_TYPE))
#define GAL_IS_VIEW_INSTANCE_CLASS(k) (GTK_CHECK_CLASS_TYPE ((k), GAL_VIEW_INSTANCE_TYPE))

typedef struct {
	GtkObject base;

	GalViewCollection *collection;

	char *instance_id;
	char *current_view_filename;
	char *custom_filename;

	char *current_title;
	char *current_type;
	char *current_id;

	GalView *current_view;
} GalViewInstance;

typedef struct {
	GtkObjectClass parent_class;

	/*
	 * Signals
	 */
	void (*display_view) (GalViewInstance *instance,
			      GalView    *view);
	void (*changed)      (GalViewInstance *instance);
} GalViewInstanceClass;

/* Standard functions */
GtkType          gal_view_instance_get_type             (void);

/* */
/*collection should be loaded when you call this.
  instance_id: Which instance of this type of object is this (for most of evo, this is the folder id.) */
GalViewInstance *gal_view_instance_new                  (GalViewCollection *collection,
							 const char        *instance_id);
GalViewInstance *gal_view_instance_construct            (GalViewInstance   *instance,
							 GalViewCollection *collection,
							 const char        *instance_id);

/* Manipulate the current view. */
char            *gal_view_instance_get_current_view_id  (GalViewInstance   *instance);
void             gal_view_instance_set_current_view_id  (GalViewInstance   *instance,
							 char              *view_id);
GalView         *gal_view_instance_get_current_view     (GalViewInstance   *instance);

/* Manipulate the view collection */
void             gal_view_instance_save_current_view    (GalViewInstance   *instance);
void             gal_view_instance_set_default          (GalViewInstance   *instance);


END_GNOME_DECLS


#endif /* _GAL_VIEW_INSTANCE_H_ */
