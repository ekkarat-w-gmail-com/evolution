/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*- */
/* 
 * e-xml-utils.h
 * Copyright (C) 2000  Helix Code, Inc.
 * Author: Chris Lahey <clahey@helixcode.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

#ifndef __E_XML_UTILS__
#define __E_XML_UTILS__

#include <gnome.h>
#include <gnome-xml/tree.h>

xmlNode  *e_xml_get_child_by_name                   (const xmlNode *parent,
						     const xmlChar *child_name);
/* lang set to NULL means use the current locale. */
xmlNode  *e_xml_get_child_by_name_by_lang           (const xmlNode *parent,
						     const xmlChar *child_name,
						     const char    *lang);

int       e_xml_get_integer_prop_by_name            (const xmlNode *parent,
						     const xmlChar *prop_name);
void      e_xml_set_integer_prop_by_name            (xmlNode       *parent,
						     const xmlChar *prop_name,
						     int            value);

gboolean  e_xml_get_bool_prop_by_name               (const xmlNode *parent,
						     const xmlChar *prop_name);
void      e_xml_set_bool_prop_by_name               (xmlNode       *parent,
						     const xmlChar *prop_name,
						     gboolean       value);

double    e_xml_get_double_prop_by_name             (const xmlNode *parent,
						     const xmlChar *prop_name);
void      e_xml_set_double_prop_by_name             (xmlNode       *parent,
						     const xmlChar *prop_name,
						     double         value);

char     *e_xml_get_string_prop_by_name             (const xmlNode *parent,
						     const xmlChar *prop_name);
void      e_xml_set_string_prop_by_name             (xmlNode       *parent,
						     const xmlChar *prop_name,
						     char          *value);

char     *e_xml_get_translated_string_prop_by_name  (const xmlNode *parent,
						     const xmlChar *prop_name);

#endif /* __E_XML_UTILS__ */
