/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*- */

/*
 * E-shell-view.c: Implements a Shell View of Evolution
 *
 * Authors:
 *   Miguel de Icaza (miguel@helixcode.com)
 *
 * (C) 2000 Helix Code, Inc.
 */
#include <config.h>
#include <gnome.h>
#include "shortcut-bar/e-shortcut-bar.h"
#include "e-util/e-util.h"
#include "e-shell-view.h"
#include "e-shell-view-menu.h"
#include "e-shell-shortcut.h"
#include "Evolution.h"

#include <bonobo.h>
#include <libgnorba/gnorba.h>

#define PARENT_TYPE gnome_app_get_type ()

static GtkObjectClass *parent_class;

struct _EShellViewPrivate 
{
	/* a hashtable of e-folders -> widgets */
	GHashTable *folder_views;
	GtkWidget *notebook;
};

static void
destroy_folder_view (EFolder *unused, GtkWidget *folder_view, gpointer unused2)
{
	BonoboWidget *bonobo_widget;
	BonoboObject *bonobo_object;	
	CORBA_Object corba_control;
	CORBA_Environment ev;

	g_print ("%s: %s entered\n",
		 __FILE__, __FUNCTION__);
	
	g_return_if_fail (BONOBO_IS_WIDGET (folder_view));

	bonobo_widget = BONOBO_WIDGET (folder_view);
	
	bonobo_object = BONOBO_OBJECT (
		bonobo_widget_get_server (bonobo_widget));

	corba_control = bonobo_object_corba_objref (bonobo_object);
	
	g_return_if_fail (corba_control != NULL);
	
	CORBA_exception_init (&ev);

	/* hangs on this! */
	Bonobo_Unknown_unref (corba_control, &ev);
	CORBA_exception_free (&ev);

	g_print ("%s: %s exited\n",
		 __FILE__, __FUNCTION__);	
}


static void
esv_destroy (GtkObject *object)
{
	EShellView *eshell_view = E_SHELL_VIEW (object);

	e_shell_unregister_view (eshell_view->eshell, eshell_view);

	g_hash_table_foreach (eshell_view->priv->folder_views,
			      destroy_folder_view, NULL);
	
	g_hash_table_destroy (eshell_view->priv->folder_views);	
	g_free (eshell_view->priv);	
	parent_class->destroy (object);
}

static void
e_shell_view_class_init (GtkObjectClass *object_class)
{
	object_class->destroy = esv_destroy;

	parent_class = gtk_type_class (PARENT_TYPE);
}

static void
e_shell_view_setup (EShellView *eshell_view)
{
	/*
	 * FIXME, should load the config if (load_config)....
	 */
	gtk_window_set_default_size (GTK_WINDOW (eshell_view), 600, 600);
}

static void
e_shell_view_setup_shortcut_display (EShellView *eshell_view)
{
	eshell_view->shortcut_bar = e_shortcut_bar_view_new (eshell_view->eshell->shortcut_bar);
	
	eshell_view->shortcut_hpaned = gtk_hpaned_new ();
	gtk_widget_show (eshell_view->shortcut_hpaned);
	gtk_paned_set_position (GTK_PANED (eshell_view->shortcut_hpaned), 100);

	gtk_paned_pack1 (GTK_PANED (eshell_view->shortcut_hpaned),
			 eshell_view->shortcut_bar, FALSE, FALSE);
	gtk_widget_show (eshell_view->shortcut_bar);

	gnome_app_set_contents (GNOME_APP (eshell_view), eshell_view->shortcut_hpaned);

	gtk_signal_connect (
		GTK_OBJECT (eshell_view->shortcut_bar), "item_selected",
		GTK_SIGNAL_FUNC (shortcut_bar_item_selected), eshell_view);
}

static GtkWidget *
get_view (EShellView *eshell_view, EFolder *efolder, Bonobo_UIHandler uih)
{
  	GtkWidget *w = NULL;
	Evolution_Shell corba_shell = CORBA_OBJECT_NIL;
	EShell *shell_model = eshell_view->eshell;

	/* This type could be E_FOLDER_MAIL, E_FOLDER_CONTACTS, etc */
	EFolderType e_folder_type;

	g_assert (efolder);
	g_assert (eshell_view);
	
	e_folder_type = e_folder_get_folder_type (efolder);
	
	if (shell_model)
		corba_shell = bonobo_object_corba_objref (
			BONOBO_OBJECT (shell_model));
	else 
		g_warning ("The shell Bonobo object does not have "
			   "an associated CORBA object\n");
	
	/* depending on the type of folder, 
	 * we launch a different bonobo component */
	switch (e_folder_type) {

	case E_FOLDER_MAIL :
		w = bonobo_widget_new_control ("control:evolution-mail", uih);
		break;

	case E_FOLDER_CONTACTS :
		w = bonobo_widget_new_control ("control:addressbook", uih);
		break;

	case E_FOLDER_CALENDAR : {
		gchar *user_cal_file;
		BonoboPropertyBagClient *pbc;
		BonoboControlFrame *cf;

		w = bonobo_widget_new_control ("control:calendar", uih);
		cf = bonobo_widget_get_control_frame (BONOBO_WIDGET (w));
		pbc = bonobo_control_frame_get_control_property_bag (cf);
		/*pbc = bonobo_control_get_property_bag (w);*/

		user_cal_file =
			g_concat_dir_and_file (gnome_util_user_home (),
					       ".gnome/user-cal.vcf");

		bonobo_property_bag_client_set_value_string (pbc,
							     "calendar_uri",
							     user_cal_file);
		break;
	}

	case E_FOLDER_TASKS :
	case E_FOLDER_OTHER :		
	default : 
		printf ("%s: %s: No bonobo component associated with %s\n",
			__FILE__,
			__FUNCTION__,
			e_folder_get_description (efolder));
		return NULL;
	}

	if (w)
	{
		Evolution_ServiceRepository corba_sr;
		BonoboObjectClient *server =
			bonobo_widget_get_server (BONOBO_WIDGET (w));
		CORBA_Environment ev;
		CORBA_exception_init (&ev);

		/* Does this control have the "ServiceRepository" interface? */
		corba_sr = (Evolution_ServiceRepository) 
			bonobo_object_client_query_interface (
				server,
				"IDL:Evolution/ServiceRepository:1.0",
				NULL);

		/* If it does, pass our shell interface to it */
		if (corba_sr != CORBA_OBJECT_NIL) {

			Evolution_ServiceRepository_set_shell (corba_sr,
							       corba_shell,
							       &ev);
			/* We're done with the service repository interface,
			   so now let's get rid of it */
			Bonobo_Unknown_unref (corba_sr, &ev);
			
		} else {
			
			g_print ("The bonobo component for \"%s\" doesn't "
				 "seem to implement the "
				 "Evolution::ServiceRepository interface\n",
				 e_folder_get_description (efolder));
		}

		CORBA_exception_free (&ev);

		gtk_widget_show (w);
	}
	
	return w;
}



void
e_shell_view_set_view (EShellView *eshell_view, EFolder *efolder)
{
	GtkNotebook *notebook;
	GtkWidget *folder_view;
	int current_page;
	BonoboControlFrame *control_frame;

	g_assert (eshell_view);
	g_assert (efolder);

	notebook = GTK_NOTEBOOK (eshell_view->priv->notebook);
	current_page = gtk_notebook_get_current_page (notebook);

	if (current_page != -1) {
		GtkWidget *current;

		current = gtk_notebook_get_nth_page (notebook, current_page);
		control_frame = bonobo_widget_get_control_frame (
			BONOBO_WIDGET (current));
	} else
		control_frame = NULL;

	/* If there's a notebook page in our hash that represents this
	 * efolder, switch to it.
	 */
	folder_view = g_hash_table_lookup (eshell_view->priv->folder_views,
					   efolder);
	if (folder_view) {
		int notebook_page = gtk_notebook_page_num (notebook,
							   folder_view);
		g_assert (notebook_page != -1);

		gtk_notebook_set_page (notebook, notebook_page);
	} else {
		/* Get a new control that represents this efolder,
		 * append it to our notebook, and put it in our hash.
		 */
		Bonobo_UIHandler uih =
			bonobo_object_corba_objref (
				BONOBO_OBJECT (eshell_view->uih));
		int new_page_index;

		folder_view = get_view (eshell_view, efolder, uih);
		if (!folder_view)
			return;

		gtk_notebook_append_page (notebook, folder_view, NULL);
		new_page_index = gtk_notebook_page_num (notebook,
							folder_view);
		g_hash_table_insert (eshell_view->priv->folder_views,
				     efolder, folder_view);
		gtk_notebook_set_page (notebook, new_page_index);
	}

	if (control_frame)
		bonobo_control_frame_control_deactivate (control_frame);

	control_frame =
		bonobo_widget_get_control_frame (BONOBO_WIDGET (folder_view));
	bonobo_control_frame_control_activate (control_frame);
}

GtkWidget *
e_shell_view_new (EShell *eshell, EFolder *efolder, gboolean show_shortcut_bar)
{
	EShellView *eshell_view;

	g_return_val_if_fail (eshell != NULL, NULL);
	g_return_val_if_fail (efolder != NULL, NULL);	
	
	eshell_view = gtk_type_new (e_shell_view_get_type ());

	eshell_view->priv = g_new (EShellViewPrivate, 1);
	eshell_view->priv->folder_views =
		g_hash_table_new (g_direct_hash, g_direct_equal);
	eshell_view->priv->notebook = NULL;

	gnome_app_construct (GNOME_APP (eshell_view),
			     "Evolution", "Evolution");

	eshell_view->eshell = eshell;
	e_shell_view_setup (eshell_view);
	e_shell_view_setup_menus (eshell_view);

	e_shell_register_view (eshell, eshell_view);
	eshell_view->shortcut_displayed = show_shortcut_bar;
	e_shell_view_setup_shortcut_display (eshell_view);

	/* create our notebook, if it hasn't been created already */
	if (!eshell_view->priv->notebook) {
		eshell_view->priv->notebook = gtk_notebook_new();

		gtk_notebook_set_show_tabs (
			GTK_NOTEBOOK (eshell_view->priv->notebook),
			FALSE);

		gtk_widget_show (eshell_view->priv->notebook);
		
		if (eshell_view->shortcut_displayed){
			gtk_paned_pack2 (
				GTK_PANED (eshell_view->shortcut_hpaned),
				eshell_view->priv->notebook, TRUE, TRUE);
		}
		else {
			gnome_app_set_contents (GNOME_APP (eshell_view),
						eshell_view->priv->notebook);
		}
	}

	e_shell_view_set_view (eshell_view, efolder);
	
	return (GtkWidget *) eshell_view;
}

void
e_shell_view_display_shortcut_bar (EShellView *eshell_view, gboolean display)
{
	g_return_if_fail (eshell_view != NULL);
	g_return_if_fail (E_IS_SHELL_VIEW (eshell_view));

	g_error ("Switching code for the shortcut bar is not written yet");
}

E_MAKE_TYPE (e_shell_view, "EShellView", EShellView, e_shell_view_class_init, NULL, PARENT_TYPE);

void
e_shell_view_new_folder (EShellView *esv)
{
	g_return_if_fail (esv != NULL);
	g_return_if_fail (E_IS_SHELL_VIEW (esv));
}

void
e_shell_view_new_shortcut (EShellView *esv)
{
	g_return_if_fail (esv != NULL);
	g_return_if_fail (E_IS_SHELL_VIEW (esv));
}
