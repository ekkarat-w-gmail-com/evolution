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
 *		Jon Trowbridge <trow@ximian.com>
 *      Jeffrey Stedfast <fejj@ximian.com>
 *
 * Copyright (C) 1999-2008 Novell, Inc. (www.novell.com)
 *
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <string.h>

#include "em-filter-source-element.h"

#include <gtk/gtk.h>
#include <camel/camel.h>
#include <libedataserver/e-sexp.h>

#include <e-util/e-account-utils.h>

#include "filter/e-filter-part.h"

#define EM_FILTER_SOURCE_ELEMENT_GET_PRIVATE(obj) \
	(G_TYPE_INSTANCE_GET_PRIVATE \
	((obj), EM_TYPE_FILTER_SOURCE_ELEMENT, EMFilterSourceElementPrivate))

typedef struct _SourceInfo {
	gchar *account_name;
	gchar *name;
	gchar *address;
	gchar *url;
} SourceInfo;

struct _EMFilterSourceElementPrivate {
	GList *sources;
	gchar *current_url;
};

G_DEFINE_TYPE (
	EMFilterSourceElement,
	em_filter_source_element,
	E_TYPE_FILTER_ELEMENT)

static void
source_info_free (SourceInfo *info)
{
	g_free (info->account_name);
	g_free (info->name);
	g_free (info->address);
	g_free (info->url);
	g_free (info);
}

static void
filter_source_element_source_changed (GtkComboBox *combobox,
                                      EMFilterSourceElement *fs)
{
	SourceInfo *info;
	gint idx;

	idx = gtk_combo_box_get_active (combobox);
	g_return_if_fail (idx >= 0 && idx < g_list_length (fs->priv->sources));

	info = (SourceInfo *) g_list_nth_data (fs->priv->sources, idx);
	g_return_if_fail (info != NULL);

	g_free (fs->priv->current_url);
	fs->priv->current_url = g_strdup (info->url);
}

static void
filter_source_element_add_source (EMFilterSourceElement *fs,
                                  const gchar *account_name,
                                  const gchar *name,
                                  const gchar *addr,
                                  const gchar *url)
{
	SourceInfo *info;

	g_return_if_fail (EM_IS_FILTER_SOURCE_ELEMENT (fs));

	info = g_new0 (SourceInfo, 1);
	info->account_name = g_strdup (account_name);
	info->name = g_strdup (name);
	info->address = g_strdup (addr);
	info->url = g_strdup (url);

	fs->priv->sources = g_list_append (fs->priv->sources, info);
}

static void
filter_source_element_get_sources (EMFilterSourceElement *fs)
{
	EAccountList *accounts;
	const EAccount *account;
	EIterator *it;
	gchar *uri;
	CamelURL *url;

	/* should this get the global object from mail? */
	accounts = e_get_account_list ();

	for (it = e_list_get_iterator ((EList *) accounts);
	     e_iterator_is_valid (it);
	     e_iterator_next (it)) {
		account = (const EAccount *) e_iterator_get (it);

		if (account->source == NULL)
			continue;

		if (account->source->url == NULL)
			continue;

		if (*account->source->url == '\0')
			continue;

		url = camel_url_new (account->source->url, NULL);
		if (url) {
			/* hide secret stuff */
			uri = camel_url_to_string (url, CAMEL_URL_HIDE_ALL);
			camel_url_free (url);
			filter_source_element_add_source (
				fs, account->name, account->id->name,
				account->id->address, uri);
			g_free (uri);
		}
	}

	g_object_unref (it);
}
static void
filter_source_element_finalize (GObject *object)
{
	EMFilterSourceElementPrivate *priv;

	priv = EM_FILTER_SOURCE_ELEMENT_GET_PRIVATE (object);

	g_list_foreach (priv->sources, (GFunc) source_info_free, NULL);
	g_list_free (priv->sources);
	g_free (priv->current_url);

	/* Chain up to parent's finalize() method. */
	G_OBJECT_CLASS (em_filter_source_element_parent_class)->finalize (object);
}

static gint
filter_source_element_eq (EFilterElement *fe,
                          EFilterElement *cm)
{
	EMFilterSourceElement *fs = (EMFilterSourceElement *) fe;
	EMFilterSourceElement *cs = (EMFilterSourceElement *) cm;

	return E_FILTER_ELEMENT_CLASS (em_filter_source_element_parent_class)->eq (fe, cm)
		&& ((fs->priv->current_url && cs->priv->current_url
		     && strcmp (fs->priv->current_url, cs->priv->current_url)== 0)
		    ||(fs->priv->current_url == NULL && cs->priv->current_url == NULL));
}

static xmlNodePtr
filter_source_element_xml_encode (EFilterElement *fe)
{
	xmlNodePtr value;

	EMFilterSourceElement *fs = (EMFilterSourceElement *) fe;

	value = xmlNewNode (NULL, (const guchar *) "value");
	xmlSetProp (value, (const guchar *) "name", (guchar *)fe->name);
	xmlSetProp (value, (const guchar *) "type", (const guchar *) "uri");

	if (fs->priv->current_url)
		xmlNewTextChild (
			value, NULL, (const guchar *) "uri",
			(guchar *) fs->priv->current_url);

	return value;
}

static gint
filter_source_element_xml_decode (EFilterElement *fe,
                                  xmlNodePtr node)
{
	EMFilterSourceElement *fs = (EMFilterSourceElement *) fe;
	CamelURL *url;
	gchar *uri;

	node = node->children;
	while (node != NULL) {
		if (!strcmp((gchar *)node->name, "uri")) {
			uri = (gchar *) xmlNodeGetContent (node);
			url = camel_url_new (uri, NULL);
			xmlFree (uri);

			g_free (fs->priv->current_url);
			fs->priv->current_url = camel_url_to_string (url, CAMEL_URL_HIDE_ALL);
			camel_url_free (url);
			break;
		}

		node = node->next;
	}

	return 0;
}

static EFilterElement *
filter_source_element_clone (EFilterElement *fe)
{
	EMFilterSourceElement *fs = (EMFilterSourceElement *) fe;
	EMFilterSourceElement *cpy;
	GList *i;

	cpy = (EMFilterSourceElement *) em_filter_source_element_new ();
	((EFilterElement *) cpy)->name = (gchar *) xmlStrdup ((guchar *) fe->name);

	cpy->priv->current_url = g_strdup (fs->priv->current_url);

	for (i = fs->priv->sources; i != NULL; i = g_list_next (i)) {
		SourceInfo *info = (SourceInfo *) i->data;
		filter_source_element_add_source (
			cpy, info->account_name, info->name,
			info->address, info->url);
	}

	return (EFilterElement *) cpy;
}

static GtkWidget *
filter_source_element_get_widget (EFilterElement *fe)
{
	EMFilterSourceElement *fs = (EMFilterSourceElement *) fe;
	GtkWidget *combobox;
	GList *i;
	SourceInfo *first = NULL;
	gint index, current_index;

	if (fs->priv->sources == NULL)
		filter_source_element_get_sources (fs);

	combobox = gtk_combo_box_text_new ();

	index = 0;
	current_index = -1;

	for (i = fs->priv->sources; i != NULL; i = g_list_next (i)) {
		SourceInfo *info = (SourceInfo *) i->data;
		gchar *label;

		if (info->url != NULL) {
			if (first == NULL)
				first = info;

			if (info->account_name && strcmp (info->account_name, info->address))
				label = g_strdup_printf (
					"%s <%s> (%s)", info->name,
					info->address, info->account_name);
			else
				label = g_strdup_printf (
					"%s <%s>", info->name, info->address);

			gtk_combo_box_text_append_text (
				GTK_COMBO_BOX_TEXT (combobox), label);

			g_free (label);

			if (fs->priv->current_url && !strcmp (info->url, fs->priv->current_url))
				current_index = index;

			index++;
		}
	}

	if (current_index >= 0) {
		gtk_combo_box_set_active (
			GTK_COMBO_BOX (combobox), current_index);
	} else {
		gtk_combo_box_set_active (GTK_COMBO_BOX (combobox), 0);
		g_free (fs->priv->current_url);

		if (first)
			fs->priv->current_url = g_strdup (first->url);
		else
			fs->priv->current_url = NULL;
	}

	g_signal_connect (
		combobox, "changed",
		G_CALLBACK (filter_source_element_source_changed), fs);

	return combobox;
}

static void
filter_source_element_build_code (EFilterElement *fe,
                                  GString *out,
                                  EFilterPart *ff)
{
	/* We are doing nothing on purpose. */
}

static void
filter_source_element_format_sexp (EFilterElement *fe,
                                   GString *out)
{
	EMFilterSourceElement *fs = (EMFilterSourceElement *) fe;

	e_sexp_encode_string (out, fs->priv->current_url);
}

static void
em_filter_source_element_class_init (EMFilterSourceElementClass *class)
{
	GObjectClass *object_class;
	EFilterElementClass *filter_element_class;

	g_type_class_add_private (class, sizeof (EMFilterSourceElementPrivate));

	object_class = G_OBJECT_CLASS (class);
	object_class->finalize = filter_source_element_finalize;

	filter_element_class = E_FILTER_ELEMENT_CLASS (class);
	filter_element_class->eq = filter_source_element_eq;
	filter_element_class->xml_encode = filter_source_element_xml_encode;
	filter_element_class->xml_decode = filter_source_element_xml_decode;
	filter_element_class->clone = filter_source_element_clone;
	filter_element_class->get_widget = filter_source_element_get_widget;
	filter_element_class->build_code = filter_source_element_build_code;
	filter_element_class->format_sexp = filter_source_element_format_sexp;
}

static void
em_filter_source_element_init (EMFilterSourceElement *fs)
{
	fs->priv = EM_FILTER_SOURCE_ELEMENT_GET_PRIVATE (fs);
}

EFilterElement *
em_filter_source_element_new (void)
{
	return g_object_new (EM_TYPE_FILTER_SOURCE_ELEMENT, NULL);
}

