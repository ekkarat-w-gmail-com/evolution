/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*- */
/* 
 * e-table-memory-store.c
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

#include <config.h>
#include <string.h>
#include "e-table-memory-store.h"

#define PARENT_TYPE e_table_memory_get_type ()

#define STORE_LOCATOR(etms, col, row) (*((etms)->priv->store + (row) * (etms)->priv->col_count + (col)))

struct _ETableMemoryStorePrivate {
	int col_count;
	ETableMemoryStoreColumnInfo *columns;
	void **store;
};

static void *
duplicate_value (ETableMemoryStore *etms, int col, const void *val)
{
	switch (etms->priv->columns[col].type) {
	case E_TABLE_MEMORY_STORE_COLUMN_TYPE_STRING:
		return g_strdup (val);
	case E_TABLE_MEMORY_STORE_COLUMN_TYPE_PIXBUF:
		if (val)
			gdk_pixbuf_ref ((void *) val);
		return (void *) val;
	case E_TABLE_MEMORY_STORE_COLUMN_TYPE_OBJECT:
		if (val)
			gtk_object_ref ((void *) val);
		return (void *) val;
	case E_TABLE_MEMORY_STORE_COLUMN_TYPE_CUSTOM:
		if (etms->priv->columns[col].custom.duplicate_value)
			return etms->priv->columns[col].custom.duplicate_value (E_TABLE_MODEL (etms), col, val, NULL);
		break;
	default:
		break;
	}
	return (void *) val;
}

static int
etms_column_count (ETableModel *etm)
{
	ETableMemoryStore *etms = E_TABLE_MEMORY_STORE(etm);

	return etms->priv->col_count;
}

static void *
etms_value_at (ETableModel *etm, int col, int row)
{
	ETableMemoryStore *etms = E_TABLE_MEMORY_STORE(etm);

	return STORE_LOCATOR (etms, col, row);
}

static void
etms_set_value_at (ETableModel *etm, int col, int row, const void *val)
{
	ETableMemoryStore *etms = E_TABLE_MEMORY_STORE(etm);

	STORE_LOCATOR (etms, col, row) = duplicate_value (etms, col, val);

	e_table_model_cell_changed (etm, col, row);
}

static gboolean
etms_is_cell_editable (ETableModel *etm, int col, int row)
{
	ETableMemoryStore *etms = E_TABLE_MEMORY_STORE(etm);

	return etms->priv->columns[col].editable;
}

/* The default for etms_duplicate_value is to return the raw value. */
static void *
etms_duplicate_value (ETableModel *etm, int col, const void *value)
{
	ETableMemoryStore *etms = E_TABLE_MEMORY_STORE(etm);

	return duplicate_value (etms, col, value);
}

static void
etms_free_value (ETableModel *etm, int col, void *value)
{
	ETableMemoryStore *etms = E_TABLE_MEMORY_STORE(etm);

	switch (etms->priv->columns[col].type) {
	case E_TABLE_MEMORY_STORE_COLUMN_TYPE_STRING:
		g_free (value);
		break;
	case E_TABLE_MEMORY_STORE_COLUMN_TYPE_PIXBUF:
		if (value)
			gdk_pixbuf_unref (value);
		break;
	case E_TABLE_MEMORY_STORE_COLUMN_TYPE_OBJECT:
		if (value)
			gtk_object_unref (value);
		break;
	case E_TABLE_MEMORY_STORE_COLUMN_TYPE_CUSTOM:
		if (etms->priv->columns[col].custom.free_value)
			etms->priv->columns[col].custom.free_value (E_TABLE_MODEL (etms), col, value, NULL);
		break;
	default:
		break;
	}
}

static void *
etms_initialize_value (ETableModel *etm, int col)
{
	ETableMemoryStore *etms = E_TABLE_MEMORY_STORE(etm);
	
	switch (etms->priv->columns[col].type) {
	case E_TABLE_MEMORY_STORE_COLUMN_TYPE_STRING:
		return g_strdup ("");
	case E_TABLE_MEMORY_STORE_COLUMN_TYPE_PIXBUF:
		return NULL;
	case E_TABLE_MEMORY_STORE_COLUMN_TYPE_CUSTOM:
	case E_TABLE_MEMORY_STORE_COLUMN_TYPE_OBJECT:
		if (etms->priv->columns[col].custom.initialize_value)
			return etms->priv->columns[col].custom.initialize_value (E_TABLE_MODEL (etms), col, NULL);
		break;
	default:
		break;
	}
	return 0;
}

static gboolean
etms_value_is_empty (ETableModel *etm, int col, const void *value)
{
	ETableMemoryStore *etms = E_TABLE_MEMORY_STORE(etm);
	
	switch (etms->priv->columns[col].type) {
	case E_TABLE_MEMORY_STORE_COLUMN_TYPE_STRING:
		return !(value && *(char *) value);
	case E_TABLE_MEMORY_STORE_COLUMN_TYPE_PIXBUF:
		return value == NULL;
	case E_TABLE_MEMORY_STORE_COLUMN_TYPE_CUSTOM:
	case E_TABLE_MEMORY_STORE_COLUMN_TYPE_OBJECT:
		if (etms->priv->columns[col].custom.value_is_empty)
			return etms->priv->columns[col].custom.value_is_empty (E_TABLE_MODEL (etms), col, value, NULL);
		break;
	default:
		break;
	}
	return value == 0;
}

static char *
etms_value_to_string (ETableModel *etm, int col, const void *value)
{
	ETableMemoryStore *etms = E_TABLE_MEMORY_STORE(etm);
	
	switch (etms->priv->columns[col].type) {
	case E_TABLE_MEMORY_STORE_COLUMN_TYPE_STRING:
		return g_strdup (value);
	case E_TABLE_MEMORY_STORE_COLUMN_TYPE_PIXBUF:
		return g_strdup ("");
	case E_TABLE_MEMORY_STORE_COLUMN_TYPE_CUSTOM:
	case E_TABLE_MEMORY_STORE_COLUMN_TYPE_OBJECT:
		if (etms->priv->columns[col].custom.value_is_empty)
			return etms->priv->columns[col].custom.value_to_string (E_TABLE_MODEL (etms), col, value, NULL);
		break;
	default:
		break;
	}
	return g_strdup_printf ("%d", GPOINTER_TO_INT (value));
}

static void
etms_append_row (ETableModel *etm, ETableModel *source, int row)
{
	ETableMemoryStore *etms = E_TABLE_MEMORY_STORE(etm);
	void **new_data;
	int i;
	int row_count;

	new_data = g_new (void *, etms->priv->col_count);

	for (i = 0; i < etms->priv->col_count; i++) {
		new_data[i] = e_table_model_value_at (source, i, row);
	}

	row_count = e_table_model_row_count (E_TABLE_MODEL (etms));

	e_table_memory_store_insert (etms, row_count, new_data, NULL);
}

static void
e_table_memory_store_init (ETableMemoryStore *etms)
{
	etms->priv            = g_new (ETableMemoryStorePrivate, 1);

	etms->priv->col_count = 0;
	etms->priv->columns   = NULL;
	etms->priv->store     = NULL;
}

static void
e_table_memory_store_class_init (GtkObjectClass *object_class)
{
	ETableModelClass *model_class = (ETableModelClass *) object_class;

	model_class->column_count     = etms_column_count;
	model_class->value_at         = etms_value_at;
	model_class->set_value_at     = etms_set_value_at;
	model_class->is_cell_editable = etms_is_cell_editable;
	model_class->duplicate_value  = etms_duplicate_value;
	model_class->free_value       = etms_free_value;
	model_class->initialize_value = etms_initialize_value;
	model_class->value_is_empty   = etms_value_is_empty;
	model_class->value_to_string  = etms_value_to_string;
	model_class->append_row       = etms_append_row;
}

GtkType
e_table_memory_store_get_type (void)
{
	static GtkType type = 0;

	if (!type){
		GtkTypeInfo info = {
			"ETableMemoryStore",
			sizeof (ETableMemoryStore),
			sizeof (ETableMemoryStoreClass),
			(GtkClassInitFunc) e_table_memory_store_class_init,
			(GtkObjectInitFunc) e_table_memory_store_init,
			NULL, /* reserved 1 */
			NULL, /* reserved 2 */
			(GtkClassInitFunc) NULL
		};

		type = gtk_type_unique (PARENT_TYPE, &info);
	}

	return type;
}

/**
 * e_table_memory_store_new:
 * @col_count:
 * @value_at:
 * @set_value_at:
 * @is_cell_editable:
 * @duplicate_value:
 * @free_value:
 * @initialize_value:
 * @value_is_empty:
 * @value_to_string:
 * @data: closure pointer.
 *
 * This initializes a new ETableMemoryStoreModel object.  ETableMemoryStoreModel is
 * an implementaiton of the abstract class ETableModel.  The ETableMemoryStoreModel
 * is designed to allow people to easily create ETableModels without having
 * to create a new GtkType derived from ETableModel every time they need one.
 *
 * Instead, ETableMemoryStoreModel uses a setup based in callback functions, every
 * callback function signature mimics the signature of each ETableModel method
 * and passes the extra @data pointer to each one of the method to provide them
 * with any context they might want to use. 
 *
 * Returns: An ETableMemoryStoreModel object (which is also an ETableModel
 * object).
 */
ETableModel *
e_table_memory_store_new (ETableMemoryStoreColumnInfo *columns)
{
	ETableMemoryStore *et;

	et = gtk_type_new (e_table_memory_store_get_type ());

	if (e_table_memory_store_construct (et, columns)) {
		return (ETableModel *) et;
	} else {
		gtk_object_unref (GTK_OBJECT (et));
		return NULL;
	}
}

ETableModel *
e_table_memory_store_construct (ETableMemoryStore *etms, ETableMemoryStoreColumnInfo *columns)
{
	int i;
	for (i = 0; columns[i].type != E_TABLE_MEMORY_STORE_COLUMN_TYPE_TERMINATOR; i++)
		/* Intentionally blank */;
	etms->priv->col_count = i;

	etms->priv->columns = g_new (ETableMemoryStoreColumnInfo, etms->priv->col_count + 1);

	memcpy (etms->priv->columns, columns, (etms->priv->col_count + 1) * sizeof (ETableMemoryStoreColumnInfo));

	return E_TABLE_MODEL (etms);
}
				

void
e_table_memory_store_adopt_value_at (ETableMemoryStore *etms, int col, int row, void *value)
{
	STORE_LOCATOR (etms, col, row) = value;

	e_table_model_cell_changed (E_TABLE_MODEL (etms), col, row);
}

/* The size of these arrays is the number of columns. */
void
e_table_memory_store_insert (ETableMemoryStore *etms, int row, void **store, gpointer data)
{
	int row_count;
	int i;

	e_table_memory_insert (E_TABLE_MEMORY (etms), row, data);

	row_count = e_table_model_row_count (E_TABLE_MODEL (etms));
	if (row == -1)
		row = row_count - 1;
	etms->priv->store = g_realloc (etms->priv->store, etms->priv->col_count * row_count * sizeof (void *));
	memmove (etms->priv->store + etms->priv->col_count * (row + 1),
		 etms->priv->store + etms->priv->col_count * row,
		 etms->priv->col_count * (row_count - row - 1) * sizeof (void *));

	for (i = 0; i < etms->priv->col_count; i++) {
		STORE_LOCATOR(etms, i, row) = duplicate_value(etms, i, store[i]);
	}
}

void
e_table_memory_store_insert_list (ETableMemoryStore *etms, int row, gpointer data, ...)
{
	void **store;
	va_list args;
	int i;

	store = g_new (void *, etms->priv->col_count + 1);

	va_start (args, data);
	for (i = 0; i < etms->priv->col_count; i++) {
		store[i] = va_arg (args, void *);
	}
	va_end (args);

	e_table_memory_store_insert (etms, row, store, data);

	g_free (store);
}

void
e_table_memory_store_insert_adopt (ETableMemoryStore *etms, int row, void **store, gpointer data)
{
	int row_count;
	int i;

	e_table_memory_insert (E_TABLE_MEMORY (etms), row, data);

	row_count = e_table_model_row_count (E_TABLE_MODEL (etms));
	if (row == -1)
		row = row_count - 1;
	etms->priv->store = g_realloc (etms->priv->store, etms->priv->col_count * row_count * sizeof (void *));
	memmove (etms->priv->store + etms->priv->col_count * (row + 1),
		 etms->priv->store + etms->priv->col_count * row,
		 etms->priv->col_count * (row_count - row - 1) * sizeof (void *));

	for (i = 0; i < etms->priv->col_count; i++) {
		STORE_LOCATOR(etms, i, row) = store[i];
	}
}

void
e_table_memory_store_remove (ETableMemoryStore *etms, int row)
{
	int row_count;

	e_table_memory_remove (E_TABLE_MEMORY (etms), row);

	row_count = e_table_model_row_count (E_TABLE_MODEL (etms));
	memmove (etms->priv->store + etms->priv->col_count * row,
		 etms->priv->store + etms->priv->col_count * (row + 1),
		 etms->priv->col_count * (row_count - row) * sizeof (void *));
	etms->priv->store = g_realloc (etms->priv->store, etms->priv->col_count * row_count * sizeof (void *));
}

void
e_table_memory_store_clear (ETableMemoryStore *etms)
{
	e_table_memory_clear (E_TABLE_MEMORY (etms));

	g_free (etms->priv->store);
	etms->priv->store = NULL;
}
