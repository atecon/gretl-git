/*
 *  Copyright (c) by Allin Cottrell
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

/* dialogs.c for gretl */

#include "gretl.h"
#include "ssheet.h"
#ifdef G_OS_WIN32 
# include "../lib/src/version.h"
# include "build.h"
#else 
extern const char *version_string;
#endif

#include "selector.h"

extern GtkWidget *active_edit_id;
extern GtkWidget *active_edit_name;

GtkWidget *open_dialog;
int session_saved;

/* ........................................................... */

int make_default_storelist (void)
{
    int i;
    char numstr[5];

    if (storelist != NULL) free(storelist);
    storelist = NULL;

    /* if there are very many variables, we won't offer
       a selection, but just save them all */
    if (datainfo->v < 50) {
	storelist = mymalloc(datainfo->v * 4);
	if (storelist == NULL) return 1;

	strcpy(storelist, "1 ");
	for (i=2; i<datainfo->v; i++) {
	    if (hidden_var(i, datainfo)) continue;
	    if (!datainfo->vector[i]) continue;
	    sprintf(numstr, "%d ", i);
	    strcat(storelist, numstr);
	}
	storelist[strlen(storelist) - 1] = '\0';
    }

    return 0;
}

/* ........................................................... */

void random_dialog (gpointer data, guint code, GtkWidget *widget) 
{
    if (code == GENR_UNIFORM) {
	edit_dialog (_("gretl: uniform variable"), 
		     _("Enter name for variable, and\n"
		       "minimum and maximum values:"), 
		     "unif 0 100",  
		     do_random, NULL, 
		     GENR_UNIFORM, GENR);
    } else if (code == GENR_NORMAL) {
	edit_dialog (_("gretl: normal variable"), 
		     _("Enter name, mean and standard deviation:"), 
		     "norm 0 1", 
		     do_random, NULL, 
		     GENR_NORMAL, GENR);
    }
}

/* ........................................................... */

static void prep_spreadsheet (GtkWidget *widget, dialog_t *data)
{
    const gchar *edttext;
    char dataspec[32];
    char *test, stobs[9], endobs[9], firstvar[9];
    double sd0, ed0;

    edttext = gtk_entry_get_text (GTK_ENTRY (data->edit));
    strncpy(dataspec, edttext, 31);
    if (dataspec[0] == '\0') return;

    /* check validity of dataspec */
    if (sscanf(dataspec, "%8s %8s %8s", stobs, endobs, firstvar) != 3) {
	errbox(_("Insufficient dataset information supplied"));
	return;
    }

    /* daily data: special */
    if (datainfo->pd == 5 || datainfo->pd == 7) {
	int err = 0;
	sd0 = (double) get_epoch_day(stobs); 
	ed0 = (double) get_epoch_day(endobs);

	if (sd0 < 0) {
	    err = 1;
	    sprintf(errtext, _("Invalid starting observation '%s'"), stobs);
	}
	if (!err && ed0 < 0) {
	    err = 1;
	    sprintf(errtext, _("Invalid ending observation '%s'"), endobs);
	}
	if (err) {
	    errbox(errtext);
	    return;
	}
    } else { /* not daily data */
	sd0 = strtod(stobs, &test);
	if (strcmp(stobs, test) == 0 || test[0] != '\0' || sd0 < 0) {
	    sprintf(errtext, _("Invalid starting observation '%s'"), stobs);
	    errbox(errtext);
	    return;
	}
	ed0 = strtod(endobs, &test);
	if (strcmp(endobs, test) == 0 || test[0] != '\0' || ed0 < 0) {
	    sprintf(errtext, _("Invalid ending observation '%s'"), endobs);
	    errbox(errtext);
	    return;
	}
    }

    if (sd0 > ed0) {
	sprintf(errtext, _("Empty data range '%s - %s'"), stobs, endobs);
	errbox(errtext);
	return;
    }

    if (datainfo->pd == 999) { /* panel */
	char unit[8], period[8];

	/* try to infer structure from ending obs */
	if (sscanf(endobs, "%[^.].%s", unit, period) == 2) { 
	    datainfo->pd = atoi(period);
	    fprintf(stderr, _("Setting data frequency = %d\n"), datainfo->pd);
	} else {
	    sprintf(errtext, _("Invalid ending observation '%s'"), endobs);
	    errbox(errtext);
	    return;	    
	}
    }    

    if (datainfo->pd == 1) {
	size_t i, n;
	
	n = strlen(stobs);
	for (i=0; i<n; i++) {
	    if (!isdigit((unsigned char) stobs[i])) {
		sprintf(errtext, _("Invalid starting observation '%s'\n"
				   "for data frequency 1"), stobs);
		errbox(errtext);
		return;
	    }
	}
	n = strlen(endobs);
	for (i=0; i<n; i++) {
	    if (!isdigit((unsigned char) endobs[i])) {
		sprintf(errtext, _("Invalid ending observation '%s'\n"
				   "for data frequency 1"), endobs);
		errbox(errtext);
		return;
	    }
	}	
    } 
    else if (datainfo->pd != 5 && datainfo->pd != 7) { 
	char year[8], subper[8];

	if (sscanf(stobs, "%[^.].%s", year, subper) != 2 ||
	    strlen(year) > 4 || atoi(subper) > datainfo->pd ||
	    (datainfo->pd < 10 && strlen(subper) != 1) ||
	    (datainfo->pd >= 10 && strlen(subper) != 2)) {
	    sprintf(errtext, _("Invalid starting observation '%s'\n"
			       "for data frequency %d"), stobs, datainfo->pd);
	    errbox(errtext);
	    return;
	}
	if (sscanf(endobs, "%[^.].%s", year, subper) != 2 ||
	    strlen(year) > 4 || atoi(subper) > datainfo->pd ||
	    (datainfo->pd < 10 && strlen(subper) != 1) ||
	    (datainfo->pd >= 10 && strlen(subper) != 2)) {
	    sprintf(errtext, _("Invalid ending observation '%s'\n"
			       "for data frequency %d"), endobs, datainfo->pd);
	    errbox(errtext);
	    return;
	}	    
    }

    gtk_widget_destroy(data->dialog); 

    strcpy(datainfo->stobs, stobs);
    strcpy(datainfo->endobs, endobs);
    datainfo->sd0 = sd0;
    datainfo->n = -1;
    datainfo->n = dateton(datainfo->endobs, datainfo) + 1; 

    if (datainfo->n <= 0) {
	errbox("Got zero-length data series");
	return;
    }

    datainfo->v = 2;
    start_new_Z(&Z, datainfo, 0);
    datainfo->markers = 0;

    strcpy(datainfo->varname[1], firstvar);

    show_spreadsheet(datainfo);
}

/* ........................................................... */

void newdata_dialog (gpointer data, guint pd_code, GtkWidget *widget) 
{
    windata_t *wdata = NULL;
    gchar *obsstr = NULL;

    if (pd_code == 0) {
	datainfo->time_series = 0;
	datainfo->pd = 1;
    } else {
	datainfo->time_series = TIME_SERIES;
	datainfo->pd = pd_code;
    }

    switch (pd_code) {
    case 0:
	datainfo->pd = 1;
	obsstr = g_strdup_printf("1 50 %s", _("newvar"));
	break;
    case 1:
	obsstr = g_strdup_printf("1950 2001 %s", _("newvar"));
	break;
    case 4:
	obsstr = g_strdup_printf("1950.1 2001.4 %s", _("newvar"));
	break;
    case 5:
	obsstr = g_strdup_printf("99/01/18 01/03/31 %s", _("newvar"));
	break;
    case 7:
	obsstr = g_strdup_printf("99/01/18 01/03/31 %s", _("newvar"));
	break;
    case 12:
	obsstr = g_strdup_printf("1950.01 2001.12 %s", _("newvar"));
	break;
    case 24:
	obsstr = g_strdup_printf("0.01 0.24 %s", _("newvar"));
	break;
    case 52:
	obsstr = g_strdup_printf("1950.01 2001.52 %s", _("newvar"));
	break;
    }
    edit_dialog (_("gretl: create data set"), 
		 _("Enter start and end obs for new data set\n"
		   "and name of first var to add:"), 
		 obsstr, 
		 prep_spreadsheet, wdata, 
		 0, 0);
    g_free(obsstr);
}

/* ........................................................... */

void start_panel_dialog (gpointer data, guint u, GtkWidget *widget) 
{
    windata_t *wdata = NULL;

    datainfo->pd = 999;

    edit_dialog (_("gretl: create panel data set"), 
		 _("Enter starting and ending observations and\n"
		   "the name of the first variable to add.\n"
		   "The example below is suitable for 20 units\n"
		   "observed over 10 periods"), 
		 "1.01 10.20 newvar", 
		 prep_spreadsheet, wdata, 
		 0, 0);
}

/* ........................................................... */

void destroy_dialog_data (GtkWidget *w, gpointer data) 
{
    dialog_t *ddata = (dialog_t *) data;

    gtk_main_quit();
    /* FIXME? */
    g_free (ddata);
    open_dialog = NULL;
    if (active_edit_id) active_edit_id = NULL;
    if (active_edit_name) active_edit_name = NULL;
}

/* ........................................................... */

void edit_dialog (const char *diagtxt, const char *infotxt, const char *deftext, 
		  void (*okfunc)(), void *okptr,
		  guint cmdcode, guint varclick)
{
    dialog_t *d;
    GtkWidget *tempwid;

    if (open_dialog != NULL) {
	gdk_window_raise(open_dialog->window);
	return;
    }

    d = mymalloc(sizeof *d);
    if (d == NULL) return;

    d->data = okptr;
    d->code = cmdcode;

    d->dialog = gtk_dialog_new();
    open_dialog = d->dialog;

    gtk_window_set_title (GTK_WINDOW (d->dialog), diagtxt);
    gtk_window_set_resizable (GTK_WINDOW (d->dialog), FALSE);

    gtk_box_set_homogeneous (GTK_BOX 
			     (GTK_DIALOG (d->dialog)->action_area), TRUE);
    gtk_window_set_position (GTK_WINDOW (d->dialog), GTK_WIN_POS_MOUSE);

    g_signal_connect (G_OBJECT (d->dialog), "destroy", 
		      G_CALLBACK (destroy_dialog_data), 
		      d);

    tempwid = gtk_label_new (infotxt);
    gtk_box_pack_start (GTK_BOX (GTK_DIALOG (d->dialog)->vbox), 
			tempwid, TRUE, TRUE, 10);

    gtk_widget_show (tempwid);
   
    d->edit = gtk_entry_new ();
    gtk_box_pack_start (GTK_BOX (GTK_DIALOG (d->dialog)->vbox), 
			d->edit, TRUE, TRUE, 0);

    /* make the Enter key do the business */
    if (okfunc) 
	g_signal_connect (G_OBJECT (d->edit), "activate", 
			  G_CALLBACK (okfunc), (gpointer) d);
    g_signal_connect (G_OBJECT (d->edit), "activate", 
		      G_CALLBACK (delete_widget), 
		      d->dialog);

    if (deftext) {
	gtk_entry_set_text (GTK_ENTRY (d->edit), deftext);
	gtk_editable_select_region (GTK_EDITABLE(d->edit), 0, strlen (deftext));
    }

    gtk_widget_show (d->edit);
    if (varclick == 1) active_edit_id = d->edit; 
    if (varclick == 2) active_edit_name = d->edit;
    gtk_widget_grab_focus (d->edit);

    /* Create the "OK" button */
    tempwid = standard_button (GTK_STOCK_OK);
    GTK_WIDGET_SET_FLAGS (tempwid, GTK_CAN_DEFAULT);
    gtk_box_pack_start (GTK_BOX (GTK_DIALOG (d->dialog)->action_area), 
			tempwid, TRUE, TRUE, 0);
    if (okfunc) 
	g_signal_connect (G_OBJECT (tempwid), "clicked", 
			  G_CALLBACK (okfunc), (gpointer) d);
    g_signal_connect (G_OBJECT (tempwid), "clicked", 
		      G_CALLBACK (delete_widget), 
		      d->dialog);
    gtk_widget_grab_default (tempwid);
    gtk_widget_show (tempwid);

    /* Create the "Cancel" button */
    tempwid = standard_button (GTK_STOCK_CANCEL);
    GTK_WIDGET_SET_FLAGS (tempwid, GTK_CAN_DEFAULT);
    gtk_box_pack_start (GTK_BOX (GTK_DIALOG (d->dialog)->action_area), 
			tempwid, TRUE, TRUE, 0);
    g_signal_connect (G_OBJECT (tempwid), "clicked", 
		      G_CALLBACK (delete_widget), 
		      d->dialog);
    gtk_widget_show (tempwid);

    /* Create a "Help" button if wanted */
    if (cmdcode && cmdcode != PRINT) {
	tempwid = standard_button (GTK_STOCK_HELP);
	gtk_box_pack_start (GTK_BOX (GTK_DIALOG (d->dialog)->action_area), 
			    tempwid, TRUE, TRUE, 0);
	g_signal_connect (G_OBJECT (tempwid), "clicked", 
			  G_CALLBACK (context_help), 
			  GINT_TO_POINTER (cmdcode));
	gtk_widget_show (tempwid);
    }

    gtk_widget_show (d->dialog); 
    gtk_main();
} 

#ifdef USE_GNOME

void about_dialog (gpointer data)
{
    static GtkWidget *about = NULL;
    gchar *pixfile;
    GdkPixbuf* pbuf = NULL;
	
    gchar *authors[] = {
	"Allin Cottrell <cottrell@wfu.edu>",
	NULL
    };
    gchar *documenters[] = {
	"Allin Cottrell <cottrell@wfu.edu>",
	NULL
    };
    gchar *translator_credits = _("translator_credits");

    if (about != NULL) {
	gdk_window_show (about->window);
	gdk_window_raise (about->window);
	return;
    }

    pixfile = gnome_program_locate_file(NULL,
					GNOME_FILE_DOMAIN_PIXMAP,
					"gretl-logo.xpm",
					TRUE,
					NULL);

    if (pixfile != NULL) {
	pbuf = gdk_pixbuf_new_from_file(pixfile, NULL);
    } else {
	fprintf(stderr, "Couldn't find gretl-logo.xpm\n");
    }

    about = gnome_about_new ("gretl", version_string,
			     "(C) 2000-2002 Allin Cottrell",
			     _("An econometrics program for the gnome desktop "
			       "issued under the GNU General Public License.  "
			       "http://gretl.sourceforge.net/"),
			     (const char **)authors,
			     (const char **)documenters,
			     strcmp (translator_credits, "translator_credits") != 0 ?
			     (const char *)translator_credits : NULL,
			     pbuf);

    gtk_window_set_transient_for (GTK_WINDOW (about),
				  GTK_WINDOW (mdata->w));

    gtk_window_set_destroy_with_parent (GTK_WINDOW (about), TRUE);

    if (pbuf != NULL)
	g_object_unref(pbuf);
	
    g_signal_connect (G_OBJECT (about), "destroy",
		      G_CALLBACK (gtk_widget_destroyed), &about);
	
    gtk_widget_show (about);
}

#else /* plain GTK version of About dialog follows */

static GtkWidget *open_logo (const char *pngname)
{
    char fullname[MAXLEN];
    GdkPixbuf *pbuf;
    GError *error = NULL;
    GtkWidget *image;

    build_path(paths.gretldir, pngname, fullname, NULL);

    pbuf = gdk_pixbuf_new_from_file (fullname, &error);

    if (pbuf == NULL) {
	errbox(error->message);
	g_error_free(error);
	return NULL;
    } else {
	image = gtk_image_new_from_pixbuf (pbuf);
	return image;
    }
}

static void about_table_setup (GtkWidget *vbox, GtkWidget *view)
{
    GtkWidget *sw;

    sw = gtk_scrolled_window_new (NULL, NULL);
    gtk_box_pack_start(GTK_BOX(vbox), 
                       sw, TRUE, TRUE, FALSE);
    gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (sw),
                                    GTK_POLICY_AUTOMATIC,
                                    GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_shadow_type (GTK_SCROLLED_WINDOW (sw),
                                         GTK_SHADOW_IN);
    gtk_container_add (GTK_CONTAINER(sw), view); 
    gtk_widget_show(view);
    gtk_widget_show(sw);
}

void about_dialog (gpointer data) 
{
    GtkWidget *notebook, *box, *label, *tempwid;
    GtkWidget *view, *dialog;
    GtkTextBuffer *tbuf;
    GtkTextIter iter;
    char *tempstr, *no_gpl, buf[MAXSTR];
    const gchar *tr_credit = "";
    FILE *fd;

    no_gpl = 
	g_strdup_printf (_("Cannot find the license agreement file COPYING. "
			   "Please make sure it's in %s"), 
			 paths.gretldir);
    dialog = gtk_dialog_new ();
    gtk_window_set_title(GTK_WINDOW(dialog),_("About gretl")); 
    gtk_container_set_border_width (GTK_CONTAINER 
				(GTK_DIALOG (dialog)->vbox), 10);
    gtk_container_set_border_width (GTK_CONTAINER 
				(GTK_DIALOG (dialog)->action_area), 5);
    gtk_box_set_spacing (GTK_BOX (GTK_DIALOG (dialog)->vbox), 5);
    gtk_window_set_position (GTK_WINDOW (dialog), GTK_WIN_POS_MOUSE);
      
    notebook = gtk_notebook_new ();
    gtk_box_pack_start (GTK_BOX (GTK_DIALOG (dialog)->vbox), 
			notebook, TRUE, TRUE, 0);

    /* construct the first page */
    box = gtk_vbox_new (FALSE, 5);
    gtk_container_set_border_width (GTK_CONTAINER (box), 10);
    gtk_widget_show (box);

    if ((tempwid = open_logo("gretl-logo.xpm"))) {
	gtk_box_pack_start (GTK_BOX (box), tempwid, FALSE, FALSE, 30);
	gtk_widget_show (tempwid);
    }

#ifdef ENABLE_NLS
    if (strcmp(_("translator_credits"), "translator_credits")) {
	tr_credit = _("translator_credits");
    }
#endif    
    
    tempstr = g_strdup_printf ("gretl, version %s\n"
#ifdef G_OS_WIN32
			       BUILD_DATE
#endif
			       "Copyright (C) 2000-2001 Allin Cottrell "
			       "<cottrell@wfu.edu>\nHomepage: "
			       "http://gretl.sourceforge.net/\n"
			       "%s", version_string, tr_credit);
    tempwid = gtk_label_new (tempstr);
    g_free (tempstr);

    gtk_label_set_justify(GTK_LABEL(tempwid), GTK_JUSTIFY_CENTER);
    gtk_box_pack_start (GTK_BOX (box), tempwid, FALSE, FALSE, 0);
    gtk_widget_show (tempwid);

    gtk_widget_show(box);

    label = gtk_label_new (_("About"));
    gtk_widget_show (label);
    gtk_notebook_append_page (GTK_NOTEBOOK (notebook), box, label);

    /* now the second page */
    box = gtk_vbox_new (FALSE, 5);
    gtk_container_set_border_width (GTK_CONTAINER (box), 10);

    view = gtk_text_view_new ();
    gtk_text_view_set_editable (GTK_TEXT_VIEW (view), FALSE);
    gtk_text_view_set_wrap_mode (GTK_TEXT_VIEW (view), GTK_WRAP_NONE);
    gtk_widget_modify_font(GTK_WIDGET(view), fixed_font);

    about_table_setup(box, view);

    gtk_widget_show (box);

    label = gtk_label_new (_("License Agreement"));
    gtk_widget_show (label);
    gtk_notebook_append_page (GTK_NOTEBOOK (notebook), box, label);

    tempwid = standard_button(GTK_STOCK_OK);
    GTK_WIDGET_SET_FLAGS (tempwid, GTK_CAN_DEFAULT);
    gtk_box_pack_start (GTK_BOX (GTK_DIALOG (dialog)->action_area), 
			tempwid, FALSE, FALSE, 0);
    g_signal_connect (G_OBJECT (tempwid), "clicked", 
		      G_CALLBACK (delete_widget), 
		      dialog);
    gtk_widget_grab_default (tempwid);
    gtk_widget_show (tempwid);

    tbuf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(view));
    gtk_text_buffer_get_iter_at_offset (tbuf, &iter, 0);

    tempstr = g_strdup_printf("%s/COPYING", paths.gretldir);
    if ((fd = fopen (tempstr, "r")) == NULL) {
	gtk_text_buffer_insert (tbuf, &iter, no_gpl, -1);
	gtk_widget_show (dialog);
	g_free (tempstr);
	return;
    }
    g_free(tempstr);
   
    memset (buf, 0, sizeof (buf));
    while (fread (buf, 1, sizeof (buf) - 1, fd)) {
	gtk_text_buffer_insert (tbuf, &iter, buf, strlen (buf));
	memset (buf, 0, sizeof (buf));
    }
    fclose (fd);

    gtk_widget_show(notebook);
    gtk_widget_set_size_request(dialog, 520, 420);
    gtk_widget_show(dialog);
    g_free(no_gpl);
}         
#endif /* not GNOME */

/* ........................................................... */

void menu_exit_check (GtkWidget *w, gpointer data)
{
    int ret = exit_check(w, NULL, data);

    if (ret == FALSE) gtk_main_quit();
}

/* ........................................................... */

int work_done (void)
     /* See whether user has done any work, to determine whether or
	not to offer the option of saving commands/output.  Merely
	running a script, or opening a data file, or a few other
	trivial actions, do not count as "work done". */
{
    FILE *fp;
    char line[MAXLEN];
    int work = 0;
    
    fp = fopen(cmdfile, "r");
    if (fp == NULL) return -1;
    while (fgets(line, MAXLEN-1, fp)) {
	if (strlen(line) > 2 && 
	    strncmp(line, "run ", 4) &&
	    strncmp(line, "open", 4) &&
	    strncmp(line, "help", 4) &&
	    strncmp(line, "impo", 4) &&
	    strncmp(line, "info", 4) &&
	    strncmp(line, "labe", 4) &&
	    strncmp(line, "list", 4) &&
	    strncmp(line, "quit", 4)) {
	    work = 1;
	    break;
	}
    }
    fclose(fp);
    return work;
}

/* ......................................................... */

static void save_data_callback (void)
{
    file_save(NULL, SAVE_DATA, NULL);
    if (data_status & MODIFIED_DATA)
	data_status ^= MODIFIED_DATA;
    /* FIXME: need to do more here? */
}

gint yes_no_dialog (char *title, char *msg, int cancel)
{
    GtkWidget *dialog, *label, *hbox;
    int ret;

    if (cancel) {
	dialog = gtk_dialog_new_with_buttons (title,
					      NULL,
					      GTK_DIALOG_MODAL | 
					      GTK_DIALOG_DESTROY_WITH_PARENT,
					      GTK_STOCK_YES,
					      GTK_RESPONSE_ACCEPT,
					      GTK_STOCK_NO,
					      GTK_RESPONSE_NO,
					      GTK_STOCK_CANCEL,
					      GTK_RESPONSE_REJECT,
					      NULL);
    } else {
	dialog = gtk_dialog_new_with_buttons (title,
					      NULL,
					      GTK_DIALOG_MODAL | 
					      GTK_DIALOG_DESTROY_WITH_PARENT,
					      GTK_STOCK_YES,
					      GTK_RESPONSE_ACCEPT,
					      GTK_STOCK_NO,
					      GTK_RESPONSE_NO,
					      NULL);
    }

    label = gtk_label_new (msg);
    gtk_widget_show(label);
    hbox = gtk_hbox_new(FALSE, 0);
    gtk_box_pack_start(GTK_BOX(hbox), label, FALSE, FALSE, 10);
    gtk_widget_show(hbox);
    gtk_box_pack_start(GTK_BOX(GTK_DIALOG(dialog)->vbox),
		       hbox, FALSE, FALSE, 10);
					  
    ret = gtk_dialog_run (GTK_DIALOG(dialog));
    gtk_widget_destroy (dialog);

    switch (ret) {
    case GTK_RESPONSE_ACCEPT: return YES_BUTTON;
    case GTK_RESPONSE_NO: return NO_BUTTON;	
    default: return -1;
    }
}

/* ........................................................... */

gint exit_check (GtkWidget *widget, GdkEvent *event, gpointer data) 
{
    char fname[MAXLEN];
    int button;
    extern int replay; /* lib.c */
    const char regular_save_msg[] = {
	N_("Do you want to save the commands and\n"
	  "output from this gretl session?")
    };
    const char session_save_msg[] = {
	N_("Do you want to save the changes you made\n"
	  "to this session?")
    };
	
    strcpy(fname, paths.userdir);
    strcat(fname, "session.inp");
    dump_cmd_stack(fname);

    /* FIXME: should make both save_session_callback() and
       save_data_callback() blocking functions */

    if (!expert && !replay && 
	(session_changed(0) || (work_done() && !session_saved))) {

	button = yes_no_dialog ("gretl", 
				(session_file_is_open()) ?
				_(session_save_msg) : _(regular_save_msg), 
				1);		      

	if (button == YES_BUTTON) {
	    save_session_callback(NULL, SAVE_RENAME, NULL);
	    return TRUE; /* bodge */
	}
	/* button -1 = wm close */
	else if (button == CANCEL_BUTTON || button == -1) return TRUE;
	/* else button = 1, NO: so fall through */
    }

    if (data_status & MODIFIED_DATA) {
	button = yes_no_dialog ("gretl", 
				_("Do you want to save changes you have\n"
				  "made to the current data set?"), 1);
	if (button == YES_BUTTON) {
	    save_data_callback();
	    return TRUE; 
	}
	else if (button == CANCEL_BUTTON || button == -1) return TRUE;
    }    

    write_rc();
    return FALSE;
}

typedef struct {
    GtkWidget *space_button;
    GtkWidget *point_button;
    gint delim;
    gint decpoint;
} csv_stuff;

#ifdef ENABLE_NLS
static void set_dec (GtkWidget *w, gpointer p)
{
    gint i;
    csv_stuff *csv = (csv_stuff *) p;

    if (GTK_TOGGLE_BUTTON(w)->active) {
	i = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(w), "action"));
	csv->decpoint = i;
	if (csv->decpoint == ',' && csv->delim == ',') {
	    csv->delim = ' ';
	    gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (csv->space_button), 
					  TRUE);
	}
    }
}
#endif

static void set_delim (GtkWidget *w, gpointer p)
{
    gint i;
    csv_stuff *csv = (csv_stuff *) p;

    if (GTK_TOGGLE_BUTTON(w)->active) {
	i = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(w), "action"));
	csv->delim = i;
	if (csv->point_button != NULL && 
	    csv->delim == ',' && csv->decpoint == ',') {
	    csv->decpoint = '.';
	    gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (csv->point_button), 
					  TRUE);
	}
    }
}

static void really_set_csv_stuff (GtkWidget *w, gpointer p)
{
    csv_stuff *stuff = (csv_stuff *) p;

    datainfo->delim = stuff->delim;
    datainfo->decpoint = stuff->decpoint;
}

static void destroy_delim_dialog (GtkWidget *w, gint *p)
{
    free(p);
    gtk_main_quit();
}

void delimiter_dialog (void)
{
    GtkWidget *dialog, *tempwid, *button, *hbox;
    GtkWidget *internal_vbox;
    GSList *group;
    csv_stuff *csvptr = NULL;

    csvptr = mymalloc(sizeof *csvptr);
    if (csvptr == NULL) return;
    csvptr->delim = datainfo->delim;
    csvptr->decpoint = '.';
    csvptr->point_button = NULL;

    dialog = gtk_dialog_new();

    gtk_window_set_title (GTK_WINDOW (dialog), _("gretl: data delimiter"));
    gtk_window_set_resizable (GTK_WINDOW (dialog), FALSE);
    gtk_container_set_border_width (GTK_CONTAINER 
				    (GTK_DIALOG (dialog)->vbox), 10);
    gtk_container_set_border_width (GTK_CONTAINER 
				    (GTK_DIALOG (dialog)->action_area), 5);
    gtk_box_set_spacing (GTK_BOX (GTK_DIALOG (dialog)->vbox), 5);

    gtk_window_set_position (GTK_WINDOW (dialog), GTK_WIN_POS_MOUSE);

    g_signal_connect (G_OBJECT(dialog), "destroy", 
		      G_CALLBACK(destroy_delim_dialog), csvptr);

    internal_vbox = gtk_vbox_new (FALSE, 5);

    hbox = gtk_hbox_new(FALSE, 5);
    tempwid = gtk_label_new (_("separator for data columns:"));
    gtk_box_pack_start (GTK_BOX(hbox), tempwid, TRUE, TRUE, 5);
    gtk_widget_show(tempwid);
    gtk_box_pack_start (GTK_BOX(internal_vbox), hbox, TRUE, TRUE, 5);
    gtk_widget_show(hbox);    

    /* comma separator */
    button = gtk_radio_button_new_with_label (NULL, _("comma (,)"));
    gtk_box_pack_start (GTK_BOX(internal_vbox), button, TRUE, TRUE, 0);
    if (csvptr->delim == ',')
	gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (button), TRUE);
    g_signal_connect(G_OBJECT(button), "clicked",
		     G_CALLBACK(set_delim), csvptr);
    g_object_set_data(G_OBJECT(button), "action", 
		      GINT_TO_POINTER(','));
    gtk_widget_show (button);

    /* space separator */
    group = gtk_radio_button_get_group (GTK_RADIO_BUTTON (button));
    button = gtk_radio_button_new_with_label(group, _("space"));
    csvptr->space_button = button;
    gtk_box_pack_start (GTK_BOX(internal_vbox), button, TRUE, TRUE, 0);
    if (csvptr->delim == ' ')
	gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (button), TRUE);
    g_signal_connect(G_OBJECT(button), "clicked",
		     G_CALLBACK(set_delim), csvptr);
    g_object_set_data(G_OBJECT(button), "action", 
		      GINT_TO_POINTER(' '));    
    gtk_widget_show (button);

    /* tab separator */
    group = gtk_radio_button_get_group (GTK_RADIO_BUTTON (button));
    button = gtk_radio_button_new_with_label(group, _("tab"));
    gtk_box_pack_start (GTK_BOX(internal_vbox), button, TRUE, TRUE, 0);
    if (csvptr->delim == '\t')
	gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (button), TRUE);
    g_signal_connect(G_OBJECT(button), "clicked",
		     G_CALLBACK(set_delim), csvptr);
    g_object_set_data(G_OBJECT(button), "action", 
		      GINT_TO_POINTER('\t'));    
    gtk_widget_show (button);

#ifdef ENABLE_NLS
    if (',' == get_local_decpoint()) {
	GSList *decgroup;

	tempwid = gtk_hseparator_new();
	gtk_box_pack_start (GTK_BOX(internal_vbox), 
			    tempwid, TRUE, TRUE, FALSE);
	gtk_widget_show(tempwid);

	hbox = gtk_hbox_new(FALSE, 5);
	tempwid = gtk_label_new (_("decimal point character:"));
	gtk_box_pack_start (GTK_BOX(hbox), tempwid, TRUE, TRUE, 5);
	gtk_widget_show(tempwid);
	gtk_box_pack_start (GTK_BOX(internal_vbox), hbox, TRUE, TRUE, 5);
	gtk_widget_show(hbox);    

	/* period decpoint */
	button = gtk_radio_button_new_with_label (NULL, _("period (.)"));
	csvptr->point_button = button;
	gtk_box_pack_start (GTK_BOX(internal_vbox), 
			    button, TRUE, TRUE, 0);
	if (csvptr->decpoint == '.')
	    gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (button), TRUE);
	g_signal_connect(G_OBJECT(button), "clicked",
			 G_CALLBACK(set_dec), csvptr);
	g_object_set_data(G_OBJECT(button), "action", 
			  GINT_TO_POINTER('.'));
	gtk_widget_show (button);

	/* comma decpoint */
	decgroup = gtk_radio_button_get_group (GTK_RADIO_BUTTON (button));
	button = gtk_radio_button_new_with_label(decgroup, _("comma (,)"));
	gtk_box_pack_start (GTK_BOX(internal_vbox), 
			    button, TRUE, TRUE, 0);
	if (csvptr->decpoint == ',')
	    gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (button), TRUE);
	g_signal_connect(G_OBJECT(button), "clicked",
			 G_CALLBACK(set_dec), csvptr);
	g_object_set_data(G_OBJECT(button), "action", 
			  GINT_TO_POINTER(','));    
	gtk_widget_show (button);
    }
#endif

    hbox = gtk_hbox_new(FALSE, 5);
    gtk_box_pack_start(GTK_BOX(hbox), internal_vbox, TRUE, TRUE, 5);
    gtk_widget_show (hbox);

    gtk_widget_show (internal_vbox);

    gtk_box_pack_start(GTK_BOX(GTK_DIALOG(dialog)->vbox), hbox, TRUE, TRUE, 5);
    gtk_widget_show (hbox);

    /* Create the "OK" button */
    tempwid = standard_button (GTK_STOCK_OK);
    GTK_WIDGET_SET_FLAGS (tempwid, GTK_CAN_DEFAULT);
    gtk_box_pack_start (GTK_BOX(GTK_DIALOG (dialog)->action_area), 
			tempwid, TRUE, TRUE, 0);
    g_signal_connect(G_OBJECT(tempwid), "clicked",
		     G_CALLBACK(really_set_csv_stuff), csvptr);
    g_signal_connect (G_OBJECT (tempwid), "clicked", 
		      G_CALLBACK (delete_widget), 
		      dialog);
    gtk_widget_grab_default (tempwid);
    gtk_widget_show (tempwid);

    gtk_widget_show (dialog);

    gtk_main();
}
