/*
 *   Copyright (c) by Allin Cottrell
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the Free Software
 *   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 */

/* progress.c for gretl */

#include <gtk/gtk.h>
#ifdef G_OS_WIN32
# include "winconfig.h"
#else
# include "../config.h"
#endif

#include "libgretl.h"

typedef struct _ProgressData {
    GtkWidget *window;
    GtkWidget *label;
    GtkWidget *pbar;
} ProgressData;

/* ........................................................... */

static void destroy_progress (GtkWidget *widget, ProgressData *pdata)
{
    pdata->window = NULL;
    g_free(pdata);
    pdata = NULL;
}

/* ........................................................... */

static ProgressData *progress_window (int flag)
{
    ProgressData *pdata;
    GtkWidget *align;
    GtkWidget *separator;
    GtkWidget *button;
    GtkWidget *vbox;

    pdata = malloc(sizeof *pdata);
    if (pdata == NULL) return NULL;

    pdata->window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_resizable(GTK_WINDOW(pdata->window), FALSE);

    g_signal_connect(G_OBJECT(pdata->window), "destroy",
		     G_CALLBACK(destroy_progress),
		     pdata);

    if (flag == SP_LOAD_INIT) 
	gtk_window_set_title(GTK_WINDOW(pdata->window), _("gretl: loading data"));
    else if (flag == SP_SAVE_INIT)
	gtk_window_set_title(GTK_WINDOW(pdata->window), _("gretl: storing data"));
    else if (flag == SP_FONT_INIT)
	gtk_window_set_title(GTK_WINDOW(pdata->window), _("gretl: scanning fonts"));
	
    gtk_container_set_border_width(GTK_CONTAINER(pdata->window), 0);

    vbox = gtk_vbox_new(FALSE, 5);
    gtk_container_set_border_width(GTK_CONTAINER(vbox), 10);
    gtk_container_add(GTK_CONTAINER(pdata->window), vbox);
    gtk_widget_show(vbox);

    /* Add a label */
    pdata->label = gtk_label_new("");
    gtk_widget_show(pdata->label);
    gtk_box_pack_start(GTK_BOX(vbox), pdata->label, FALSE, FALSE, 0);
        
    /* Create a centering alignment object */
    align = gtk_alignment_new(0.5, 0.5, 0, 0);
    gtk_box_pack_start(GTK_BOX(vbox), align, FALSE, FALSE, 5);
    gtk_widget_show(align);

    /* Create the GtkProgressBar */
    pdata->pbar = gtk_progress_bar_new();

    gtk_container_add(GTK_CONTAINER(align), pdata->pbar);
    gtk_widget_show(pdata->pbar);

    separator = gtk_hseparator_new();
    gtk_box_pack_start(GTK_BOX(vbox), separator, FALSE, FALSE, 0);
    gtk_widget_show(separator);

    /* Add button to close progress bar window */
    button = gtk_button_new_with_label(_("Cancel"));
    g_signal_connect_swapped(G_OBJECT(button), "clicked",
			     G_CALLBACK(gtk_widget_destroy),
			     pdata->window);
    gtk_box_pack_start(GTK_BOX(vbox), button, FALSE, FALSE, 0);

    GTK_WIDGET_SET_FLAGS(button, GTK_CAN_DEFAULT);
    gtk_widget_grab_default(button);
    gtk_widget_show(button);

    gtk_widget_show(pdata->window);

    return pdata;
}

/* ........................................................... */

int show_progress (long res, long expected, int flag)
{
    static long offs;
    static ProgressData *pdata;

    if (expected == 0) return 0;

    if (flag == SP_FINISH) {
	if (pdata != NULL)
	    gtk_widget_destroy(GTK_WIDGET(pdata->window)); 
	return 0;
    }

    if (flag == SP_LOAD_INIT || flag == SP_SAVE_INIT || flag == SP_FONT_INIT) {
	char bytestr[48];

	offs = 0L;
	if ((pdata = progress_window(flag)) == NULL) return 0;
	gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(pdata->pbar), (gdouble) 0);
	if (flag == SP_LOAD_INIT) {
	    sprintf(bytestr, "%s %ld Kbytes", _("Retrieving"),
		    expected / 1024);
	}
	else if (flag == SP_SAVE_INIT) {
	    sprintf(bytestr, "%s %ld Kbytes", _("Storing"),
		    expected / 1024);
	}
	else if (flag == SP_FONT_INIT) {
	    sprintf(bytestr, _("Scanning %ld fonts"), expected);
	}
	gtk_label_set_text(GTK_LABEL(pdata->label), bytestr);
	while (gtk_events_pending()) gtk_main_iteration();
    }

    offs += res;

    if (offs > expected && pdata != NULL) {
	gtk_widget_destroy(GTK_WIDGET(pdata->window)); 
	return 0;
    }

    if (pdata != NULL) {
	gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(pdata->pbar), 
				      (gdouble) ((double) offs / expected));
	while (gtk_events_pending()) gtk_main_iteration();
    } else
	return -1;
	
    return 0;
}

