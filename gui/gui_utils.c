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
 *   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 *
 */

/* gui_utils.c for gretl */

#include "gretl.h"
#include "var.h"

#include <sys/stat.h>
#include <unistd.h>
#include <dlfcn.h>

#include "guiprint.h"
#include "model_table.h"
#include "series_view.h"
#include "console.h"
#include "session.h"

char *storelist = NULL;

extern int session_saved;
extern GtkWidget *mysheet;
extern GdkColor red, blue;

#define SCRIPT_CHANGED(w) (w->active_var = 1)
#define SCRIPT_SAVED(w) (w->active_var = 0)
#define SCRIPT_IS_CHANGED(w) (w->active_var == 1)

#define MULTI_COPY_ENABLED(c) (c == SUMMARY || c == VAR_SUMMARY \
	                      || c == CORR || c == FCASTERR \
	                      || c == FCAST || c == COEFFINT \
	                      || c == COVAR || c == VIEW_MODEL \
                              || c == VIEW_MODELTABLE || c == VAR)

static void set_up_viewer_menu (GtkWidget *window, windata_t *vwin, 
				GtkItemFactoryEntry items[]);
static void add_vars_to_plot_menu (windata_t *vwin);
static void add_dummies_to_plot_menu (windata_t *vwin);
static void add_var_menu_items (windata_t *vwin);
static void check_model_menu (GtkWidget *w, GdkEventButton *eb, 
			      gpointer data);
static void file_viewer_save (GtkWidget *widget, windata_t *vwin);
static gint query_save_script (GtkWidget *w, GdkEvent *event, windata_t *vwin);
static void buf_edit_save (GtkWidget *widget, gpointer data);

extern void do_coeff_intervals (gpointer data, guint i, GtkWidget *w);
extern void save_plot (char *fname, GPT_SPEC *plot);
extern void do_panel_diagnostics (gpointer data, guint u, GtkWidget *w);
extern void do_leverage (gpointer data, guint u, GtkWidget *w);

GtkItemFactoryEntry model_items[] = {
    { N_("/_File"), NULL, NULL, 0, "<Branch>" },
    { N_("/File/_Save as text..."), NULL, file_save, SAVE_MODEL, NULL },
    { N_("/File/Save to session as icon"), NULL, remember_model, 0, NULL },
    { N_("/File/Save as icon and close"), NULL, remember_model, 1, NULL },
#if defined(USE_GNOME)
    { N_("/File/_Print..."), NULL, window_print, 0, NULL },
#endif
    { N_("/_Edit"), NULL, NULL, 0, "<Branch>" },
    { N_("/Edit/_Copy selection"), NULL, text_copy, COPY_SELECTION, NULL },
    { N_("/Edit/Copy _all"), NULL, NULL, 0, "<Branch>" },
    { N_("/Edit/Copy all/as plain _text"), NULL, text_copy, COPY_TEXT, NULL },
    { N_("/Edit/Copy all/as _LaTeX"), NULL, text_copy, COPY_LATEX, NULL },
    { N_("/Edit/Copy all/as _RTF"), NULL, text_copy, COPY_RTF, NULL },
    { N_("/_Tests"), NULL, NULL, 0, "<Branch>" },    
    { N_("/Tests/omit variables"), NULL, selector_callback, OMIT, NULL },
    { N_("/Tests/add variables"), NULL, selector_callback, ADD, NULL },
    { N_("/Tests/sum of coefficients"), NULL, selector_callback, COEFFSUM, NULL },
    { N_("/Tests/sep1"), NULL, NULL, 0, "<Separator>" },
    { N_("/Tests/non-linearity (squares)"), NULL, do_lmtest, AUX_SQ, NULL },
    { N_("/Tests/non-linearity (logs)"), NULL, do_lmtest, AUX_LOG, NULL },
    { N_("/Tests/Ramsey's RESET"), NULL, do_reset, 0, NULL },
    { N_("/Tests/sep2"), NULL, NULL, 0, "<Separator>" },
    { N_("/Tests/autocorrelation"), NULL, model_test_callback, LMTEST, NULL },
    { N_("/Tests/heteroskedasticity"), NULL, do_lmtest, AUX_WHITE, NULL },
    { N_("/Tests/influential observations"), NULL, do_leverage, 0, NULL },
    { N_("/Tests/Chow test"), NULL, model_test_callback, CHOW, NULL },
    { N_("/Tests/CUSUM test"), NULL, do_cusum, 0, NULL },
    { N_("/Tests/ARCH"), NULL, model_test_callback, ARCH, NULL },
    { N_("/Tests/normality of residual"), NULL, do_resid_freq, 0, NULL },
    { N_("/Tests/panel diagnostics"), NULL, do_panel_diagnostics, 0, NULL },
    { N_("/_Graphs"), NULL, NULL, 0, "<Branch>" }, 
    { N_("/Graphs/residual plot"), NULL, NULL, 0, "<Branch>" },
    { N_("/Graphs/fitted, actual plot"), NULL, NULL, 0, "<Branch>" },
    { N_("/_Model data"), NULL, NULL, 0, "<Branch>" },
    { N_("/Model data/Display actual, fitted, residual"), NULL, 
      display_fit_resid, 0, NULL },
    { N_("/Model data/Forecasts with standard errors"), NULL, 
      model_test_callback, FCASTERR, NULL },
    { N_("/Model data/Confidence intervals for coefficients"), NULL, 
      do_coeff_intervals, 0, NULL },
    { N_("/Model data/coefficient covariance matrix"), NULL, 
      do_outcovmx, 0, NULL },
    { N_("/Model data/Add to data set"), NULL, NULL, 0, "<Branch>" },
    { N_("/Model data/Add to data set/fitted values"), NULL, 
      fit_resid_callback, 1, NULL },
    { N_("/Model data/Add to data set/residuals"), NULL, 
      fit_resid_callback, 0, NULL },
    { N_("/Model data/Add to data set/squared residuals"), NULL, 
      fit_resid_callback, 2, NULL },
    { N_("/Model data/Add to data set/error sum of squares"), NULL, 
      model_stat_callback, ESS, NULL },
    { N_("/Model data/Add to data set/standard error of residuals"), NULL, 
      model_stat_callback, SIGMA, NULL },
    { N_("/Model data/Add to data set/R-squared"), NULL, 
      model_stat_callback, R2, NULL },
    { N_("/Model data/Add to data set/T*R-squared"), NULL, 
      model_stat_callback, TR2, NULL },
    { N_("/Model data/Add to data set/log likelihood"), NULL, 
      model_stat_callback, LNL, NULL },
    { N_("/Model data/Add to data set/degrees of freedom"), NULL, 
      model_stat_callback, DF, NULL },
    { N_("/Model data/sep1"), NULL, NULL, 0, "<Separator>" },
    { N_("/Model data/Define new variable..."), NULL, model_test_callback, 
      MODEL_GENR, NULL },
    { N_("/_LaTeX"), NULL, NULL, 0, "<Branch>" },
    { N_("/LaTeX/_View"), NULL, NULL, 0, "<Branch>" },
    { N_("/LaTeX/View/_Tabular"), NULL, view_latex, LATEX_VIEW_TABULAR, NULL },
    { N_("/LaTeX/View/_Equation"), NULL, view_latex, LATEX_VIEW_EQUATION, NULL },
    { N_("/LaTeX/_Save"), NULL, NULL, 0, "<Branch>" },
    { N_("/LaTeX/Save/_Tabular"), NULL, NULL, 0, "<Branch>" },
    { N_("/LaTeX/Save/Tabular/as _document"), NULL, file_save, 
      SAVE_TEX_TAB, NULL },
    { N_("/LaTeX/Save/Tabular/as _fragment"), NULL, file_save, 
      SAVE_TEX_TAB_FRAG, NULL },
    { N_("/LaTeX/Save/_Equation"), NULL, NULL, 0, "<Branch>" },
    { N_("/LaTeX/Save/Equation/as _document"), NULL, file_save, 
      SAVE_TEX_EQ, NULL },
    { N_("/LaTeX/Save/Equation/as _fragment"), NULL, file_save, 
      SAVE_TEX_EQ_FRAG, NULL },
    { N_("/LaTeX/_Copy"), NULL, NULL, 0, "<Branch>" },
    { N_("/LaTeX/Copy/_Tabular"), NULL, text_copy, COPY_LATEX, NULL },
    { N_("/LaTeX/Copy/_Equation"), NULL, text_copy, COPY_LATEX_EQUATION, NULL },
    { NULL, NULL, NULL, 0, NULL}
};

GtkItemFactoryEntry var_items[] = {
    { N_("/_File"), NULL, NULL, 0, "<Branch>" },
    { N_("/File/_Save as text..."), NULL, file_save, SAVE_MODEL, NULL },
    { N_("/File/Save to session as icon"), NULL, remember_var, 0, NULL },
    { N_("/File/Save as icon and close"), NULL, remember_var, 1, NULL },
#if defined(USE_GNOME)
    { N_("/File/_Print..."), NULL, window_print, 0, NULL },
#endif
    { N_("/_Edit"), NULL, NULL, 0, "<Branch>" },
    { N_("/Edit/Copy _all"), NULL, NULL, 0, "<Branch>" },
    { N_("/Edit/Copy all/as plain _text"), NULL, text_copy, COPY_TEXT, NULL },
    { N_("/Edit/Copy all/as _LaTeX"), NULL, text_copy, COPY_LATEX, NULL },
    { NULL, NULL, NULL, 0, NULL}
};

#ifdef ENABLE_NLS
gchar *menu_translate (const gchar *path, gpointer p)
{
    return (_(path));
}
#endif

/* ........................................................... */

int copyfile (const char *src, const char *dest) 
{
    FILE *srcfd, *destfd;
    char buf[8192];
    size_t n;

    if (!strcmp(src, dest)) return 1;
   
    if ((srcfd = fopen(src, "rb")) == NULL) {
	return 1; 
    }
    if ((destfd = fopen(dest, "wb")) == NULL) {
	fclose(srcfd);
	return 1;
    }
    while ((n = fread(buf, 1, sizeof buf, srcfd)) > 0) {
	fwrite(buf, 1, n, destfd);
    }

    fclose(srcfd);
    fclose(destfd);

    return 0;
}

/* ........................................................... */

int isdir (const char *path)
{
    struct stat buf;

    if (stat(path, &buf) == 0 && S_ISDIR(buf.st_mode)) 
	return 1;
    else 
	return 0;
}

/* ........................................................... */

int getbufline (char *buf, char *line, int init)
{
    static int pos;
    int i = 0;

    if (init) pos = 0;
    else {
	while (buf[i+pos] != '\n') {
	    line[i] = buf[i+pos];
	    if (buf[i+pos] == 0)
		return 0;
	    i++;
	}
	pos += i + 1;
	line[i] = 0;
    }
    return i;
}

/* ........................................................... */

/* Below: Keep a record of (most) windows that are open, so they 
   can be destroyed en masse when a new data file is opened, to
   prevent weirdness that could arise if (e.g.) a model window
   that pertains to a previously opened data file remains open
   after the data set has been changed.  Script windows are
   exempt, otherwise they are likely to disappear when their
   "run" control is activated, which we don't want.
*/

enum winstack_codes {
    STACK_INIT,
    STACK_ADD,
    STACK_REMOVE,
    STACK_DESTROY,
    STACK_QUERY
};

static int winstack (int code, GtkWidget *w, gpointer ptest)
{
    static int n_windows;
    static GtkWidget **wstack;
    int found = 0;
    int i;

    switch (code) {
    case STACK_DESTROY:	
	for (i=0; i<n_windows; i++) 
	    if (wstack[i] != NULL) 
		gtk_widget_destroy(wstack[i]);
	free(wstack);
    case STACK_INIT:
	wstack = NULL;
	n_windows = 0;
	break;
    case STACK_ADD:
	for (i=0; i<n_windows; i++) {
	    if (wstack[i] == NULL) {
		wstack[i] = w;
		break;
	    }
	}
	if (i == n_windows) {
	    n_windows++;
	    wstack = myrealloc(wstack, n_windows * sizeof *wstack);
	    if (wstack != NULL) 
		wstack[n_windows-1] = w;
	}
	break;
    case STACK_REMOVE:
	for (i=0; i<n_windows; i++) {
	    if (wstack[i] == w) {
		wstack[i] = NULL;
		break;
	    }
	}
	break;
    case STACK_QUERY:
	for (i=0; i<n_windows; i++) {
	    if (wstack[i] != NULL) {
		gpointer p = gtk_object_get_data(GTK_OBJECT(wstack[i]), 
						 "object");

		if (p == ptest) {
		    found = 1;
		    break;
		}
	    }
	}
	break;
    default:
	break;
    }

    return found;
}

void winstack_init (void)
{
    winstack(STACK_INIT, NULL, NULL);
}
    
void winstack_destroy (void)
{
    winstack(STACK_DESTROY, NULL, NULL);
}

int winstack_match_data (gpointer p)
{
    return winstack(STACK_QUERY, NULL, p);
}

static void winstack_add (GtkWidget *w)
{
    winstack(STACK_ADD, w, NULL);
}

static void winstack_remove (GtkWidget *w)
{
    winstack(STACK_REMOVE, w, NULL);
}

/* ........................................................... */

static void delete_file (GtkWidget *widget, char *fle) 
{
    remove(fle);
    g_free(fle);
}

/* ........................................................... */

static void delete_file_viewer (GtkWidget *widget, gpointer data) 
{
    windata_t *vwin = (windata_t *) data;

    if (vwin->role == EDIT_SCRIPT && SCRIPT_IS_CHANGED(vwin)) {
	gint resp;

	resp = query_save_script(NULL, NULL, vwin);
	if (!resp) gtk_widget_destroy(vwin->dialog);
    } else 
	gtk_widget_destroy(vwin->dialog);
}

/* ........................................................... */

static void delete_unnamed_model (GtkWidget *widget, gpointer data) 
{
    MODEL *pmod = (MODEL *) data;

    if (pmod->dataset != NULL) {
	free_model_dataset(pmod);
    }

    if (pmod->name == NULL) {
	free_model(pmod);
	pmod = NULL;
    }
}

/* ........................................................... */

void delete_widget (GtkWidget *widget, gpointer data)
{
    gtk_widget_destroy(data);
}

/* ........................................................... */

static
void catch_edit_key (GtkWidget *w, GdkEventKey *key, windata_t *vwin)
{
    GdkModifierType mods;

    gdk_window_get_pointer(w->window, NULL, NULL, &mods);

    if (key->keyval == GDK_F1 && vwin->role == EDIT_SCRIPT) { 
	vwin->help_active = 1;
	edit_script_help(NULL, NULL, vwin);
    }

    else if (mods & GDK_CONTROL_MASK) {
	if (gdk_keyval_to_upper(key->keyval) == GDK_S) { 
	    if (vwin->role == EDIT_HEADER || vwin->role == EDIT_NOTES) {
		buf_edit_save(NULL, vwin);
	    } else {
		file_viewer_save(NULL, vwin);
	    }
	} else if (gdk_keyval_to_upper(key->keyval) == GDK_Q) {
	    if (vwin->role == EDIT_SCRIPT && SCRIPT_IS_CHANGED(vwin)) {
		gint resp;

		resp = query_save_script(NULL, NULL, vwin);
		if (!resp) gtk_widget_destroy(vwin->dialog);
	    } else 
		gtk_widget_destroy(w);
	}
    }

#ifdef notyet
    /* pick out some stuff that shouldn't be captured */
    if (mods > GDK_SHIFT_MASK || 
	key->keyval < GDK_space || 
	key->keyval > GDK_asciitilde) {
	return;
    } else {  /* colorize comments */
	int cw = gdk_char_width(fixed_font, 'x'); 
	int currpos, xpos;
	gchar *starter, out[2];

	currpos = GTK_EDITABLE(vwin->w)->current_pos;
	xpos = GTK_TEXT(vwin->w)->cursor_pos_x / cw; 
	starter = gtk_editable_get_chars(GTK_EDITABLE(vwin->w),
					 currpos - xpos, currpos - xpos + 1);
	sprintf(out, "%c", key->keyval);
	key->keyval = GDK_VoidSymbol;

	if ((starter != NULL && starter[0] == '#') || out[0] == '#') {
	    gtk_text_insert(GTK_TEXT(vwin->w), fixed_font, 
			    &blue, NULL, out, 1);
	} else {
	    gtk_text_insert(GTK_TEXT(vwin->w), fixed_font, 
			    NULL, NULL, out, 1);
	}
	gtk_signal_emit_stop_by_name(GTK_OBJECT(w), "key-press-event");
	if (starter != NULL) g_free(starter);
    }
#endif
}

/* ........................................................... */

static 
void catch_viewer_key (GtkWidget *w, GdkEventKey *key, windata_t *vwin)
{
    /* FIXME: see if text window is editable, and if so return
       catch_edit_key()
    */
    if (key->keyval == GDK_q) { 
        gtk_widget_destroy(w);
    }
    else if (key->keyval == GDK_s) {
	if (Z != NULL && vwin != NULL && vwin->role == VIEW_MODEL) {
	    remember_model(vwin, 1, NULL);
	}
    }
}

/* ........................................................... */

void *mymalloc (size_t size) 
{
    void *mem;
   
    if ((mem = malloc(size)) == NULL) 
	errbox(_("Out of memory!"));
    return mem;
}

/* ........................................................... */

void *myrealloc (void *ptr, size_t size) 
{
    void *mem;
   
    if ((mem = realloc(ptr, size)) == NULL) 
	errbox(_("Out of memory!"));
    return mem;
}

/* ........................................................... */

void mark_dataset_as_modified (void)
{
    data_status |= MODIFIED_DATA;
    set_sample_label(datainfo);
}

/* ........................................................... */

void register_data (const char *fname, const char *user_fname,
		    int record)
{    
    char datacmd[MAXLEN];

    /* basic accounting */
    data_status |= HAVE_DATA;
    orig_vars = datainfo->v;

    /* set appropriate data_status bits */
    if (fname == NULL)
	data_status |= (GUI_DATA|MODIFIED_DATA);
    else if (!(data_status & IMPORT_DATA)) {
	if (strstr(paths.datfile, paths.datadir) != NULL) 
	    data_status |= BOOK_DATA;
	else
	    data_status |= USER_DATA; 
    }

    /* sync main window with datafile */
    populate_varlist();
    set_sample_label(datainfo);
    main_menubar_state(TRUE);
    session_menu_state(TRUE);

    /* record opening of data file in command log */
    if (record && fname != NULL) {
	mkfilelist(FILE_LIST_DATA, fname);
	sprintf(datacmd, "open %s", user_fname ? user_fname : fname);
	check_cmd(datacmd);
	cmd_init(datacmd); 
    } 
}

#define APPENDING(action) (action == APPEND_CSV || \
                           action == APPEND_GNUMERIC || \
                           action == APPEND_EXCEL || \
                           action == APPEND_ASCII)

/* ........................................................... */

int get_worksheet_data (const char *fname, int datatype, int append)
{
    int err;
    void *handle;
    PRN *errprn;
    int (*sheet_get_data)(const char*, double ***, DATAINFO *, PRN *prn);

    if (datatype == GRETL_GNUMERIC) {
	sheet_get_data = gui_get_plugin_function("wbook_get_data",
						 &handle);
    }
    else if (datatype == GRETL_EXCEL) {
	sheet_get_data = gui_get_plugin_function("excel_get_data",
						 &handle);
    }
    else {
	errbox(_("Unrecognized data type"));
	return 1;
    }

    if (sheet_get_data == NULL) {
        return 1;
    }

    if (bufopen(&errprn)) {
	close_plugin(handle);
	return 1;
    }

    err = (*sheet_get_data)(fname, &Z, datainfo, errprn);
    close_plugin(handle);

    if (err == -1) /* the user canceled the import */
	return 0;

    if (err) {
	if (*errprn->buf != '\0') {
	    errbox(errprn->buf);
	} else {
	    errbox(_("Failed to import spreadsheet data"));
	}
	return 1;
    } else {
	if (*errprn->buf != '\0') infobox(errprn->buf);
    }

    gretl_print_destroy(errprn);

    if (append) {
	infobox(_("Data appended OK"));
	register_data(NULL, NULL, 0);
    } else {
	data_status |= IMPORT_DATA;
	strcpy(paths.datfile, fname);
	if (mdata != NULL) register_data(fname, NULL, 1);
    }

    return 0;
}

/* ........................................................... */

void do_open_data (GtkWidget *w, gpointer data, int code)
     /* cases: 
	- called from dialog: user has said Yes to opening data file,
	although a data file is already open
	- reached without dialog, in expert mode or when no datafile
	is open yet
     */
{
    gint datatype, err;
    dialog_t *d = NULL;
    windata_t *fwin = NULL;
    int append = APPENDING(code);

    if (data != NULL) {    
	if (w == NULL) { /* not coming from edit_dialog */
	    fwin = (windata_t *) data;
	} else {
	    d = (dialog_t *) data;
	    fwin = (windata_t *) d->data;
	}
    }

    if (code == OPEN_CSV || code == APPEND_CSV || code == OPEN_ASCII ||
	code == APPEND_ASCII)
	datatype = GRETL_CSV_DATA;
    if (code == OPEN_GNUMERIC || code == APPEND_GNUMERIC)
	datatype = GRETL_GNUMERIC;
    else if (code == OPEN_EXCEL || code == APPEND_EXCEL)
	datatype = GRETL_EXCEL;
    else if (code == OPEN_BOX)
	datatype = GRETL_BOX_DATA;
    else if (code == OPEN_DES)
	datatype = GRETL_DES_DATA;
    else {
	PRN *prn;	

	if (bufopen(&prn)) return;
	datatype = detect_filetype(trydatfile, &paths, prn);
	gretl_print_destroy(prn);
    }

    /* destroy the current data set, etc., unless we're explicitly appending */
    if (!append) close_session();

    if (datatype == GRETL_GNUMERIC || datatype == GRETL_EXCEL) {
	get_worksheet_data(trydatfile, datatype, append);
	return;
    }
    else if (datatype == GRETL_CSV_DATA) {
	do_open_csv_box(trydatfile, OPEN_CSV, append);
	return;
    }
    else if (datatype == GRETL_BOX_DATA) {
	do_open_csv_box(trydatfile, OPEN_BOX, 0);
	return;
    }
    else if (datatype == GRETL_DES_DATA) {
	get_worksheet_data(trydatfile, datatype, 0);
	return;
    }    
    else { /* native data */
	PRN prn;

	gretl_print_attach_file(&prn, stderr);
	if (datatype == GRETL_XML_DATA)
	    err = get_xmldata(&Z, datainfo, trydatfile, &paths, 
			      data_status, &prn, 1);
	else
	    err = get_data(&Z, datainfo, trydatfile, &paths, data_status, &prn);
    }

    if (err) {
	gui_errmsg(err);
	delete_from_filelist(FILE_LIST_DATA, trydatfile);
	return;
    }	

    /* trash the practice files window that launched the query? */
    if (fwin != NULL) gtk_widget_destroy(fwin->w); 

    if (append) {
	register_data(NULL, NULL, 0);
    } else {
	strcpy(paths.datfile, trydatfile);
	register_data(paths.datfile, NULL, 1);
    }
}

/* ........................................................... */

void verify_open_data (gpointer userdata, int code)
     /* give user choice of not opening selected datafile,
	if there's already a datafile open and we're not
	in "expert" mode */
{
    if (data_status && !expert) {
	int resp = 
	    yes_no_dialog (_("gretl: open data"), 
			   _("Opening a new data file will automatically\n"
			     "close the current one.  Any unsaved work\n"
			     "will be lost.  Proceed to open data file?"), 0);
	if (resp == GRETL_NO) return;
    } 

    do_open_data(NULL, userdata, code);
}

/* ........................................................... */

void verify_open_session (gpointer userdata)
     /* give user choice of not opening session file,
	if there's already a datafile open and we're not
	in "expert" mode */
{
    if (data_status && !expert) {
	int resp = 
	    yes_no_dialog (_("gretl: open session"), 
			   _("Opening a new session file will automatically\n"
			     "close the current session.  Any unsaved work\n"
			     "will be lost.  Proceed to open session file?"), 0);

	if (resp == GRETL_NO) return;
    }

    do_open_session(NULL, userdata);
}

/* ........................................................... */

void save_session (char *fname) 
{
    int spos;
    char msg[MAXLEN], savedir[MAXLEN], fname2[MAXLEN];
    char session_base[MAXLEN];
    FILE *fp;
    PRN *prn;

    spos = slashpos(fname);
    if (spos) 
	safecpy(savedir, fname, spos);
    else *savedir = 0;

#ifdef CMD_DEBUG
    dump_cmd_stack("stderr", 0);
#endif

    /* save commands, by dumping the command stack */
    if (haschar('.', fname) < 0)
	strcat(fname, ".gretl");
    if (dump_cmd_stack(fname, 1)) return;

    get_base(session_base, fname, '.');

    /* get ready to save "session" */
    fp = fopen(fname, "a");
    if (fp == NULL) {
	sprintf(errtext, _("Couldn't open session file %s"), fname);
	errbox(errtext);
	return;
    }

    print_saved_object_specs(session_base, fp);

    fclose(fp);

    /* delete any extraneous graph files */
    session_file_manager(REALLY_DELETE_ALL, NULL);

    switch_ext(fname2, fname, "Notes");
    if (print_session_notes(fname2)) {
	errbox(_("Couldn't write session notes file"));
    }    

    /* save output */
    switch_ext(fname2, fname, "txt");
    prn = gretl_print_new(GRETL_PRINT_FILE, fname2);
    if (prn == NULL) {
	errbox(_("Couldn't open output file for writing"));
	return;
    }

    gui_logo(prn->fp);
    session_time(prn->fp);
    pprintf(prn, _("Output from %s\n"), fname);
    execute_script(fname, NULL, prn, SAVE_SESSION_EXEC); 
    gretl_print_destroy(prn);

    sprintf(msg, _("session saved to %s -\n"), savedir);
    strcat(msg, _("commands: "));
    strcat(msg, (spos)? fname + spos + 1 : fname);
    strcat(msg, _("\noutput: "));
    spos = slashpos(fname2);
    strcat(msg, (spos)? fname2 + spos + 1 : fname2);
    infobox(msg);

    mkfilelist(FILE_LIST_SESSION, fname);
    session_saved = 1;
    session_changed(0);

    return;
}

/* ........................................................... */

static void buf_edit_save (GtkWidget *widget, gpointer data)
{
    windata_t *vwin = (windata_t *) data;
    gchar *text;
    char **pbuf = (char **) vwin->data;

    text = gtk_editable_get_chars(GTK_EDITABLE(vwin->w), 0, -1);
    if (text == NULL || !strlen(text)) {
	errbox(_("Buffer is empty"));
	g_free(text);
	return;
    }

    /* swap the edited text into the buffer */
    free(*pbuf); 
    *pbuf = text;

    if (vwin->role == EDIT_HEADER) {
	infobox(_("Data info saved"));
	data_status |= MODIFIED_DATA;
    } 
    else if (vwin->role == EDIT_NOTES) {
	infobox(_("Notes saved"));
	session_changed(1);
    }
}

/* ........................................................... */

static void file_viewer_save (GtkWidget *widget, windata_t *vwin)
{
    /* special case: a newly created script */
    if (strstr(vwin->fname, "script_tmp") || !strlen(vwin->fname)) {
	file_save(vwin, SAVE_SCRIPT, NULL);
	strcpy(vwin->fname, scriptfile);
    } else {
	char buf[MAXLEN];
	FILE *fp;
	gchar *text;

	if ((fp = fopen(vwin->fname, "w")) == NULL) {
	    errbox(_("Can't open file for writing"));
	    return;
	} else {
	    text = gtk_editable_get_chars(GTK_EDITABLE(vwin->w), 0, -1);
	    fprintf(fp, "%s", text);
	    fclose(fp);
	    g_free(text);
	    sprintf(buf, _("Saved %s\n"), vwin->fname);
	    infobox(buf);
	    if (vwin->role == EDIT_SCRIPT) 
		SCRIPT_SAVED(vwin);
	}
    }
} 

/* .................................................................. */

void windata_init (windata_t *vwin)
{
    vwin->dialog = NULL;
    vwin->vbox = NULL;
    vwin->listbox = NULL;
    vwin->mbar = NULL;
    vwin->w = NULL;
    vwin->status = NULL;
    vwin->popup = NULL;
    vwin->ifac = NULL;
    vwin->data = NULL;
    vwin->fname[0] = '\0';
    vwin->role = 0;
    vwin->active_var = 0;
    vwin->help_active = 0;
}

/* .................................................................. */

void free_windata (GtkWidget *w, gpointer data)
{
    windata_t *vwin = (windata_t *) data;

    if (vwin != NULL) {
	if (vwin->w) {
	    gchar *undo = 
		gtk_object_get_data(GTK_OBJECT(vwin->w), "undo");
	    
	    if (undo) g_free(undo);
	}
	if (vwin->ifac) 
	    gtk_object_unref(GTK_OBJECT(vwin->ifac));
	if (vwin->popup) 
	    gtk_object_unref(GTK_OBJECT(vwin->popup));
	if (vwin->role == SUMMARY || vwin->role == VAR_SUMMARY)
	    free_summary(vwin->data); 
	if (vwin->role == CORR || vwin->role == PCA)
	    free_corrmat(vwin->data);
	else if (vwin->role == FCASTERR || vwin->role == FCAST)
	    free_fit_resid(vwin->data);
	else if (vwin->role == COEFFINT)
	    free_confint(vwin->data);
	else if (vwin->role == COVAR)
	    free_vcv(vwin->data);
	else if (vwin->role == VIEW_SERIES)
	    free_series_view(vwin->data);
	else if (vwin->role == VAR) 
	    gretl_var_free_unnamed(vwin->data);
	else if (vwin->role == LEVERAGE) 
	    gretl_matrix_free(vwin->data);

	if (vwin->dialog)
	    winstack_remove(vwin->dialog);
	free(vwin);
    }
}

static void modeltable_tex_view (void)
{
    tex_print_model_table (NULL, 1, NULL);
}

#if defined(USE_GNOME) 
static void window_print_callback (GtkWidget *w, windata_t *vwin)
{
    window_print(vwin, 0, w);
}
#endif

/* ........................................................... */

static void activate_script_help (GtkWidget *widget, windata_t *vwin)
{
    GdkCursor *cursor = gdk_cursor_new(GDK_QUESTION_ARROW);

    gdk_window_set_cursor(GTK_TEXT(vwin->w)->text_area, cursor);
    gdk_cursor_destroy(cursor);
    vwin->help_active = 1;
}

/* ........................................................... */

static void script_changed (GtkWidget *w, windata_t *vwin)
{
    SCRIPT_CHANGED(vwin);
}

/* ........................................................... */

#include "../pixmaps/stock_save_16.xpm"
#include "../pixmaps/stock_save_as_16.xpm"
#include "../pixmaps/stock_exec_16.xpm"
#include "../pixmaps/stock_copy_16.xpm"
#include "../pixmaps/stock_paste_16.xpm"
#include "../pixmaps/stock_search_16.xpm"
#include "../pixmaps/stock_search_replace_16.xpm"
#include "../pixmaps/stock_undo_16.xpm"
#include "../pixmaps/stock_help_16.xpm"
#include "../pixmaps/stock_add_16.xpm"
#include "../pixmaps/stock_close_16.xpm"
#include "../pixmaps/mini.tex.xpm"
#if defined(USE_GNOME)
# include "../pixmaps/stock_print_16.xpm"
#endif

/* ........................................................... */

static void choose_copy_format_callback (GtkWidget *w, windata_t *vwin)
{
    copy_format_dialog(vwin);
}

/* ........................................................... */

static void add_pca_data (windata_t *vwin)
{
    int err, oldv = datainfo->v;
    unsigned char oflag = 'd';
    CORRMAT *corrmat = (CORRMAT *) vwin->data;

    err = call_pca_plugin(corrmat, &Z, datainfo, &oflag, NULL);

    if (err) {
	gui_errmsg(err);
	return;
    }

    if (datainfo->v > oldv) {
	/* if data were added, register the command */
	if (oflag == 'o' || oflag == 'a') {
	    char listbuf[MAXLEN - 8];
	    
	    err = print_list_to_buffer(corrmat->list, listbuf, sizeof listbuf);
	    if (!err) {
		sprintf(line, "pca %s-%c", listbuf, oflag);
		verify_and_record_command(line);
	    }
	}
    }
}

static void add_leverage_data (windata_t *vwin)
{
    void *handle;
    int (*leverage_data_dialog) (void);
    gretl_matrix *m = (gretl_matrix *) vwin->data;
    int opt, err;

    if (m == NULL) return;

    leverage_data_dialog = gui_get_plugin_function("leverage_data_dialog",
						   &handle);
    if (leverage_data_dialog == NULL) return;

    opt = leverage_data_dialog();
    close_plugin(handle);

    if (opt == 0) return;

    err = add_leverage_values_to_dataset(&Z, datainfo, m, opt);
    if (err) {
	gui_errmsg(err);
    }
}

static void add_data_callback (GtkWidget *w, windata_t *vwin)
{
    int oldv = datainfo->v;

    if (vwin->role == PCA) {
	add_pca_data(vwin);
    }
    else if (vwin->role == LEVERAGE) {
	add_leverage_data(vwin);
    }

    if (datainfo->v > oldv) {
	infobox(_("data added"));
	populate_varlist();
	mark_dataset_as_modified();
    }	
}

/* ........................................................... */

struct viewbar_item {
    const char *str;
    gchar **toolxpm;
    void (*toolfunc)();
    int flag;
};

enum {
    SAVE_ITEM = 1,
    SAVE_AS_ITEM,
    EDIT_ITEM,
    GP_ITEM,
    RUN_ITEM,
    COPY_ITEM,
    MODELTABLE_ITEM,
    ADD_ITEM
} viewbar_codes;

static struct viewbar_item viewbar_items[] = {
    { N_("Save"), stock_save_16_xpm, file_viewer_save, SAVE_ITEM },
    { N_("Save as..."), stock_save_as_16_xpm, file_save_callback, SAVE_AS_ITEM },
    { N_("Send to gnuplot"), stock_exec_16_xpm, gp_send_callback, GP_ITEM },
#ifdef USE_GNOME
    { N_("Print..."), stock_print_16_xpm, window_print_callback, 0 },
#endif
    { N_("Run"), stock_exec_16_xpm, run_script_callback, RUN_ITEM },
    { N_("Copy"), stock_copy_16_xpm, text_copy_callback, COPY_ITEM }, 
    { N_("Paste"), stock_paste_16_xpm, text_paste_callback, EDIT_ITEM },
    { N_("Find..."), stock_search_16_xpm, text_find_callback, 0 },
    { N_("Replace..."), stock_search_replace_16_xpm, text_replace_callback, EDIT_ITEM },
    { N_("Undo"), stock_undo_16_xpm, text_undo_callback, EDIT_ITEM },
    { N_("Help on command"), stock_help_16_xpm, activate_script_help, RUN_ITEM },
    { N_("LaTeX"), mini_tex_xpm, modeltable_tex_view, MODELTABLE_ITEM },
    { N_("Add to dataset..."), stock_add_16_xpm, add_data_callback, ADD_ITEM },
    { N_("Close"), stock_close_16_xpm, delete_file_viewer, 0 },
    { NULL, NULL, NULL, 0 }};

static void make_viewbar (windata_t *vwin, int text_out)
{
    GtkWidget *iconw, *hbox;
    GdkPixmap *icon;
    GdkBitmap *mask;
    GdkColormap *cmap;
    int i;
    void (*toolfunc)() = NULL;

    int run_ok = (vwin->role == EDIT_SCRIPT ||
		  vwin->role == VIEW_SCRIPT ||
		  vwin->role == VIEW_LOG);

    int edit_ok = (vwin->role == EDIT_SCRIPT ||
		   vwin->role == EDIT_HEADER ||
		   vwin->role == EDIT_NOTES ||
		   vwin->role == GR_PLOT || 
		   vwin->role == GR_BOX ||
		   vwin->role == SCRIPT_OUT);

    int save_as_ok = (vwin->role != EDIT_HEADER && 
		      vwin->role != EDIT_NOTES);

    if (text_out || vwin->role == SCRIPT_OUT) {
	gtk_object_set_data(GTK_OBJECT(vwin->dialog), "text_out", GINT_TO_POINTER(1));
    }

    hbox = gtk_hbox_new(FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vwin->vbox), hbox, FALSE, FALSE, 0);

    vwin->mbar = gtk_toolbar_new(GTK_ORIENTATION_HORIZONTAL, GTK_TOOLBAR_ICONS);
    gtk_toolbar_set_button_relief(GTK_TOOLBAR(vwin->mbar), GTK_RELIEF_NONE);
    gtk_toolbar_set_space_size(GTK_TOOLBAR(vwin->mbar), 3);

    gtk_box_pack_start(GTK_BOX(hbox), vwin->mbar, FALSE, FALSE, 0);

    cmap = gdk_colormap_get_system();

    colorize_tooltips(GTK_TOOLBAR(vwin->mbar)->tooltips);

    for (i=0; viewbar_items[i].str != NULL; i++) {

	toolfunc = viewbar_items[i].toolfunc;

	if (!edit_ok && viewbar_items[i].flag == EDIT_ITEM) {
	    continue;
	}

	if (!save_as_ok && viewbar_items[i].flag == SAVE_AS_ITEM) {
	    continue;
	}

	if (!run_ok && viewbar_items[i].flag == RUN_ITEM) {
	    continue;
	}

	if (vwin->role != GR_PLOT && viewbar_items[i].flag == GP_ITEM) {
	    continue;
	}

	if (vwin->role != VIEW_MODELTABLE && 
	    viewbar_items[i].flag == MODELTABLE_ITEM) {
	    continue;
	}

	if (vwin->role != PCA && vwin->role != LEVERAGE &&
	    viewbar_items[i].flag == ADD_ITEM) {
	    continue;
	}

	if (viewbar_items[i].flag == COPY_ITEM && 
	    MULTI_COPY_ENABLED(vwin->role)) {
	    toolfunc = choose_copy_format_callback;
	}

	if (viewbar_items[i].flag == SAVE_ITEM) { 
	    if (!edit_ok || vwin->role == SCRIPT_OUT) {
		/* script output doesn't already have a filename */
		continue;
	    }
	    if (vwin->role == EDIT_HEADER || vwin->role == EDIT_NOTES) {
		toolfunc = buf_edit_save;
	    } 
	}

	icon = gdk_pixmap_colormap_create_from_xpm_d(NULL, cmap, &mask, NULL, 
						     viewbar_items[i].toolxpm);
	iconw = gtk_pixmap_new(icon, mask);
	gtk_toolbar_append_item(GTK_TOOLBAR(vwin->mbar),
				NULL, _(viewbar_items[i].str), NULL,
				iconw, toolfunc, vwin);
	gtk_toolbar_append_space(GTK_TOOLBAR(vwin->mbar));
    }

    gtk_widget_show(vwin->mbar);
    gtk_widget_show(hbox);
}

static void add_edit_items_to_viewbar (windata_t *vwin)
{
    GtkWidget *iconw;
    GdkPixmap *icon;
    GdkBitmap *mask;
    GdkColormap *cmap;
    int i, pos = 0;

    cmap = gdk_colormap_get_system();

    for (i=0; viewbar_items[i].str != NULL; i++) {
	if (viewbar_items[i].flag == SAVE_ITEM ||
	    viewbar_items[i].flag == EDIT_ITEM) {

	    icon = gdk_pixmap_colormap_create_from_xpm_d(NULL, cmap, &mask, NULL, 
							 viewbar_items[i].toolxpm);
	    iconw = gtk_pixmap_new(icon, mask);
	    gtk_toolbar_insert_item(GTK_TOOLBAR(vwin->mbar),
				    NULL, _(viewbar_items[i].str), NULL,
				    iconw, viewbar_items[i].toolfunc, 
				    vwin, pos);
	    gtk_toolbar_insert_space(GTK_TOOLBAR(vwin->mbar), pos + 1);
	}
	if (viewbar_items[i].flag != GP_ITEM) pos += 2;
    }
}

/* ........................................................... */

static gchar *make_viewer_title (int role, const char *fname)
{
    gchar *title = NULL;

    switch (role) {
    case GUI_HELP: 
	title = g_strdup(_("gretl: help")); break;
    case CLI_HELP:
	title = g_strdup(_("gretl: command syntax")); break;
    case GUI_HELP_ENGLISH: 
	title = g_strdup("gretl: help"); break;
    case CLI_HELP_ENGLISH:
	title = g_strdup("gretl: command syntax"); break;
    case VIEW_LOG:
	title = g_strdup(_("gretl: command log")); break;
    case CONSOLE:
	title = g_strdup(_("gretl console")); break;
    case EDIT_SCRIPT:
    case VIEW_SCRIPT:	
    case VIEW_FILE:
	if (strstr(fname, "script_tmp") || strstr(fname, "session.inp"))
	    title = g_strdup(_("gretl: command script"));
	else {
	    gchar *p = strrchr(fname, SLASH);
	    title = g_strdup_printf("gretl: %s", p? p + 1 : fname);
	} 
	break;
    case EDIT_NOTES:
	title = g_strdup(_("gretl: session notes")); break;
    case GR_PLOT:
	title = g_strdup(_("gretl: edit plot commands")); break;
    case SCRIPT_OUT:
	title = g_strdup(_("gretl: script output")); break;
    case VIEW_DATA:
	title = g_strdup(_("gretl: display data")); break;
    case TRAMO: 
	title = g_strdup(_("gretl: TRAMO analysis")); break;
    case X12A: 
	title = g_strdup(_("gretl: X-12-ARIMA analysis")); break;
    default:
	break;
    }
    return title;
}

/* ........................................................... */

static windata_t *common_viewer_new (int role, const char *title, 
                                     gpointer data, int record)
{
    windata_t *vwin;

    if ((vwin = mymalloc(sizeof *vwin)) == NULL) return NULL;

    windata_init(vwin);
    vwin->role = role;
    vwin->data = data;
    vwin->dialog = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(vwin->dialog), title);
    gtk_window_set_policy(GTK_WINDOW(vwin->dialog), TRUE, TRUE, TRUE);

    if (record) {
	gtk_object_set_data(GTK_OBJECT(vwin->dialog), "object", data);
	winstack_add(vwin->dialog);
    }

    return vwin;
}

/* ........................................................... */

static void create_text (windata_t *vwin, int hsize, int vsize, 
			 gboolean editable)
{
    vwin->w = gtk_text_new(NULL, NULL);
    gtk_text_set_word_wrap(GTK_TEXT(vwin->w), TRUE);
    gtk_text_set_editable(GTK_TEXT(vwin->w), editable);

    hsize *= gdk_char_width(fixed_font, 'W');
    hsize += 48;
    gtk_widget_set_usize (vwin->dialog, hsize, vsize);
}

/* ........................................................... */

static void viewer_box_config (windata_t *vwin)
{
    vwin->vbox = gtk_vbox_new(FALSE, 1);
    gtk_container_border_width (GTK_CONTAINER(vwin->vbox), 5);
    gtk_box_set_spacing (GTK_BOX(vwin->vbox), 5);
    gtk_signal_connect_after(GTK_OBJECT(vwin->dialog), "realize", 
                             GTK_SIGNAL_FUNC(set_wm_icon), 
                             NULL);
    gtk_container_add (GTK_CONTAINER(vwin->dialog), vwin->vbox);
}

/* ........................................................... */

static void dialog_table_setup (windata_t *vwin)
{
    GtkWidget *table, *vscroll;

    table = gtk_table_new(1, 2, FALSE);
    gtk_widget_set_usize(table, 500, 400);
    gtk_box_pack_start(GTK_BOX(vwin->vbox), 
		       table, TRUE, TRUE, FALSE);

    gtk_table_attach(GTK_TABLE(table), vwin->w, 0, 1, 0, 1,
		     GTK_FILL | GTK_EXPAND, GTK_FILL | GTK_EXPAND | 
		     GTK_SHRINK, 0, 0);
    gtk_widget_show(vwin->w);

    vscroll = gtk_vscrollbar_new(GTK_TEXT(vwin->w)->vadj);
    gtk_table_attach (GTK_TABLE (table), 
		      vscroll, 1, 2, 0, 1,
		      GTK_FILL, GTK_EXPAND | GTK_SHRINK | GTK_FILL, 0, 0);
    gtk_widget_show (vscroll);

    gtk_widget_show(table);
}

/* ........................................................... */

windata_t *view_buffer (PRN *prn, int hsize, int vsize, 
			const char *title, int role,
			gpointer data) 
{
    GtkWidget *dialog, *close;
    windata_t *vwin;

    vwin = common_viewer_new(role, title, data, 1);
    if (vwin == NULL) return NULL;   

    create_text(vwin, hsize, vsize, FALSE);

    dialog = vwin->dialog;
    viewer_box_config(vwin);

    /* in a few special cases, add a text-based menu bar */
    if (role == VAR || role == VIEW_SERIES || role == VIEW_SCALAR) {
	GtkItemFactoryEntry *menu_items;

	if (role == VAR) {
	    menu_items = var_items;
	} else {
	    menu_items = get_series_view_menu_items(role);
	}
	set_up_viewer_menu(vwin->dialog, vwin, menu_items);
	gtk_box_pack_start(GTK_BOX(vwin->vbox), 
			   vwin->mbar, FALSE, TRUE, 0);
	gtk_widget_show(vwin->mbar);
    } else if (role != IMPORT) {
	make_viewbar(vwin, 1);
    }    

    if (role == VAR) {
	/* model-specific additions to menus */
	add_var_menu_items(vwin);
    }    

    dialog_table_setup(vwin);

    /* arrange for clean-up when dialog is destroyed */
    gtk_signal_connect(GTK_OBJECT(dialog), "destroy", 
		       GTK_SIGNAL_FUNC(free_windata), vwin);

    /* close button */
    close = gtk_button_new_with_label(_("Close"));
    gtk_box_pack_start(GTK_BOX(vwin->vbox), 
		       close, FALSE, TRUE, 0);
    gtk_signal_connect(GTK_OBJECT(close), "clicked", 
		       GTK_SIGNAL_FUNC(delete_file_viewer), vwin);
    gtk_widget_show(close);

    /* insert and then free the text buffer */
    gtk_text_insert(GTK_TEXT(vwin->w), fixed_font, 
		    NULL, NULL, prn->buf, 
		    strlen(prn->buf));
    gretl_print_destroy(prn);
    
    gtk_signal_connect(GTK_OBJECT(dialog), "key_press_event", 
		       GTK_SIGNAL_FUNC(catch_viewer_key), vwin);

    gtk_widget_show(vwin->vbox);
    gtk_widget_show(dialog);

    return vwin;
}

#define help_role(r) (r == CLI_HELP || \
                      r == GUI_HELP || \
                      r == CLI_HELP_ENGLISH || \
                      r == GUI_HELP_ENGLISH)

windata_t *view_file (char *filename, int editable, int del_file, 
		      int hsize, int vsize, int role) 
{
    GtkWidget *dialog, *close; 
    void *colptr = NULL, *nextcolor = NULL;
    char tempstr[MAXSTR];
    FILE *fd = NULL;
    windata_t *vwin;
    gchar *title = NULL;
    static GtkStyle *style;
    int doing_script = (role == EDIT_SCRIPT ||
			role == VIEW_SCRIPT ||
			role == VIEW_LOG);

    /* first check that we can open the specified file */
    fd = fopen(filename, "r");
    if (fd == NULL) {
	sprintf(errtext, _("Can't open %s for reading"), filename);
	errbox(errtext);
	return NULL;
    }

    /* then start building the file viewer */
    title = make_viewer_title(role, filename);
    vwin = common_viewer_new(role, (title != NULL)? title : filename, 
			     NULL, !doing_script && role != CONSOLE);
    g_free(title);
    if (vwin == NULL) return NULL; 

    create_text(vwin, hsize, vsize, editable);

    dialog = vwin->dialog;
    viewer_box_config(vwin);
   
    strcpy(vwin->fname, filename);

    if (help_role(role)) {
	GtkItemFactoryEntry *menu_items;

	menu_items = get_help_menu_items(role);
	set_up_viewer_menu(vwin->dialog, vwin, menu_items);
	gtk_box_pack_start(GTK_BOX(vwin->vbox), 
			   vwin->mbar, FALSE, TRUE, 0);
	gtk_widget_show(vwin->mbar);
    } else if (role != VIEW_FILE) { 
	make_viewbar(vwin, (role == VIEW_DATA || role == CONSOLE));
    }

    dialog_table_setup(vwin);

    if (style == NULL) {
	style = gtk_style_new();
	gdk_font_unref(style->font);
	style->font = fixed_font;
    }
    gtk_widget_set_style(GTK_WIDGET(vwin->w), style);

    /* special case: the gretl console */
    if (role == CONSOLE) {
	gtk_signal_connect(GTK_OBJECT(vwin->w), "button_release_event",
			   GTK_SIGNAL_FUNC(console_mouse_handler), NULL);
	gtk_signal_connect(GTK_OBJECT(vwin->w), "key_press_event",
			   GTK_SIGNAL_FUNC(console_key_handler), NULL);
    } 

    if (doing_script) {
	gtk_signal_connect_after(GTK_OBJECT(vwin->w), "button_press_event",
				 (GtkSignalFunc) edit_script_help, vwin);
    } 

    /* Close button */
    close = gtk_button_new_with_label(_("Close"));
    gtk_box_pack_start(GTK_BOX(vwin->vbox), 
		       close, FALSE, TRUE, 0);
    gtk_signal_connect(GTK_OBJECT(close), "clicked", 
		       GTK_SIGNAL_FUNC(delete_file_viewer), vwin);
    gtk_widget_show(close);

    /* insert the file text */
    memset(tempstr, 0, sizeof tempstr);
    while (fgets(tempstr, sizeof tempstr - 1, fd)) {
	if (tempstr[0] == '@') continue;
	if (tempstr[0] == '?') 
	    colptr = (role == CONSOLE)? &red : &blue;
	if (tempstr[0] == '#') {
	    if (help_role(role)) {
		tempstr[0] = ' ';
		nextcolor = &red;
	    } else {
		colptr = &blue;
	    }
	} else {
	    nextcolor = NULL;
	}
	gtk_text_insert(GTK_TEXT(vwin->w), fixed_font, 
			colptr, NULL, tempstr, 
			strlen(tempstr));
	colptr = nextcolor;
	memset(tempstr, 0, sizeof tempstr);
    }
    fclose(fd);

    /* grab the "changed" signal when editing a script */
    if (role == EDIT_SCRIPT) {
	gtk_signal_connect(GTK_OBJECT(vwin->w), "changed", 
			   GTK_SIGNAL_FUNC(script_changed), vwin);
    }

    /* catch some keystrokes */
    if (!editable) {
	gtk_signal_connect(GTK_OBJECT(dialog), "key_press_event", 
			   GTK_SIGNAL_FUNC(catch_viewer_key), vwin);
    } else {
	gtk_object_set_data(GTK_OBJECT(dialog), "vwin", vwin);
	gtk_signal_connect(GTK_OBJECT(dialog), "key_press_event", 
			   GTK_SIGNAL_FUNC(catch_edit_key), vwin);	
    }  

    /* offer chance to save script on exit */
    if (role == EDIT_SCRIPT)
	gtk_signal_connect(GTK_OBJECT(dialog), "delete_event", 
			   GTK_SIGNAL_FUNC(query_save_script), vwin);

    /* clean up when dialog is destroyed */
    if (del_file) {
	gchar *fname = g_strdup(filename);

	gtk_signal_connect(GTK_OBJECT(dialog), "destroy", 
			   GTK_SIGNAL_FUNC(delete_file), (gpointer) fname);
    }
    gtk_signal_connect(GTK_OBJECT(dialog), "destroy", 
		       GTK_SIGNAL_FUNC(free_windata), vwin);

    gtk_widget_show(vwin->vbox);
    gtk_widget_show(dialog);

    if (role == EDIT_SCRIPT) {
	gtk_widget_grab_focus(vwin->w);
    }

    return vwin;
}

/* ........................................................... */

void file_view_set_editable (windata_t *vwin)
{
    gtk_text_set_editable(GTK_TEXT(vwin->w), TRUE);
    gtk_object_set_data(GTK_OBJECT(vwin->dialog), "vwin", vwin);
    vwin->role = EDIT_SCRIPT;
    gtk_signal_connect(GTK_OBJECT(vwin->w), "changed", 
		       GTK_SIGNAL_FUNC(script_changed), vwin);

    /* redo viewer toolbar */
    add_edit_items_to_viewbar(vwin);
}

/* ........................................................... */

windata_t *edit_buffer (char **pbuf, int hsize, int vsize, 
			char *title, int role) 
{
    GtkWidget *dialog, *close;
    windata_t *vwin;

    vwin = common_viewer_new(role, title, pbuf, 1);
    if (vwin == NULL) return NULL;

    create_text(vwin, hsize, vsize, TRUE);

    dialog = vwin->dialog;
    viewer_box_config(vwin);

    /* add a menu bar */
    make_viewbar(vwin, 0);

    dialog_table_setup(vwin);

    /* insert the buffer text */
    if (*pbuf) {
	gtk_text_insert(GTK_TEXT(vwin->w), fixed_font, 
			NULL, NULL, *pbuf, strlen(*pbuf));
    } else {
	gtk_text_insert(GTK_TEXT(vwin->w), fixed_font, 
			NULL, NULL, "A", 1);
	gtk_editable_delete_text(GTK_EDITABLE(vwin->w), 0, -1);
    }

    gtk_signal_connect(GTK_OBJECT(dialog), "key_press_event", 
		       GTK_SIGNAL_FUNC(catch_edit_key), vwin);	

    /* clean up when dialog is destroyed */
    gtk_signal_connect(GTK_OBJECT(dialog), "destroy", 
		       GTK_SIGNAL_FUNC(free_windata), vwin);

    /* close button */
    close = gtk_button_new_with_label(_("Close"));
    gtk_box_pack_start(GTK_BOX(vwin->vbox), 
		       close, FALSE, TRUE, 0);
    gtk_signal_connect(GTK_OBJECT(close), "clicked", 
		       GTK_SIGNAL_FUNC(delete_file_viewer), vwin);
    gtk_widget_show(close);

    gtk_widget_show(vwin->vbox);
    gtk_widget_show(dialog);

    gtk_widget_grab_focus(GTK_WIDGET(vwin->w));

    return vwin;
}

/* ........................................................... */

int view_model (PRN *prn, MODEL *pmod, int hsize, int vsize, 
		char *title) 
{
    windata_t *vwin;
    GtkWidget *dialog, *close;

    vwin = common_viewer_new(VIEW_MODEL, title, pmod, 1);
    if (vwin == NULL) return 1;    

    create_text(vwin, hsize, vsize, FALSE);

    dialog = vwin->dialog;
    viewer_box_config(vwin);

    set_up_viewer_menu(dialog, vwin, model_items);

    /* add menu of indep vars, against which to plot resid */
    add_vars_to_plot_menu(vwin);
    add_dummies_to_plot_menu(vwin);
    gtk_signal_connect(GTK_OBJECT(vwin->mbar), "button_press_event", 
		       GTK_SIGNAL_FUNC(check_model_menu), vwin);

    gtk_box_pack_start(GTK_BOX(vwin->vbox), 
		       vwin->mbar, FALSE, TRUE, 0);
    gtk_widget_show(vwin->mbar);

    dialog_table_setup(vwin);

    /* close button */
    close = gtk_button_new_with_label(_("Close"));
    gtk_box_pack_start(GTK_BOX(vwin->vbox), 
		       close, FALSE, TRUE, 0);
    gtk_signal_connect(GTK_OBJECT(close), "clicked", 
		       GTK_SIGNAL_FUNC(delete_file_viewer), vwin);
    gtk_widget_show(close);

    /* insert and then free the model buffer */
    gtk_text_insert(GTK_TEXT(vwin->w), fixed_font, 
		    NULL, NULL, prn->buf, 
		    strlen(prn->buf));
    gretl_print_destroy(prn);

    if (pmod->ci != NLS) copylist(&default_list, pmod->list);

    /* attach shortcuts */
    gtk_object_set_data(GTK_OBJECT(dialog), "ddata", vwin);
    gtk_signal_connect(GTK_OBJECT(dialog), "key_press_event", 
		       GTK_SIGNAL_FUNC(catch_viewer_key), 
		       vwin);

    /* clean up when dialog is destroyed */
    gtk_signal_connect(GTK_OBJECT(dialog), "destroy", 
		       GTK_SIGNAL_FUNC(delete_unnamed_model), 
		       vwin->data);
    gtk_signal_connect(GTK_OBJECT(dialog), "destroy", 
		       GTK_SIGNAL_FUNC(free_windata), 
		       vwin);

    gtk_widget_show(vwin->vbox);
    gtk_widget_show_all(dialog);

    return 0;
}

/* ........................................................... */

static void auto_save_script (windata_t *vwin)
{
    FILE *fp;
    char msg[MAXLEN];
    gchar *savestuff;

    if (strstr(vwin->fname, "script_tmp") || !strlen(vwin->fname)) {
	file_save(vwin, SAVE_SCRIPT, NULL);
	strcpy(vwin->fname, scriptfile);
    }

    if ((fp = fopen(vwin->fname, "w")) == NULL) {
	sprintf(msg, _("Couldn't write to %s"), vwin->fname);
	errbox(msg); 
	return;
    }
    savestuff = 
	gtk_editable_get_chars(GTK_EDITABLE(vwin->w), 0, -1);
    fprintf(fp, "%s", savestuff);
    g_free(savestuff); 
    fclose(fp);
    infobox(_("script saved"));
    SCRIPT_SAVED(vwin);
}

/* ........................................................... */

static gint query_save_script (GtkWidget *w, GdkEvent *event, windata_t *vwin)
{
    if (SCRIPT_IS_CHANGED(vwin)) {
	int resp = 
	    yes_no_dialog(_("gretl: script"), 
			  _("Save changes?"), 1);

	if (resp == GRETL_CANCEL) {
	    return TRUE;
	}
	if (resp == GRETL_YES) {
	    auto_save_script(vwin);
	}
    }
    return FALSE;
}

/* ........................................................... */

void flip (GtkItemFactory *ifac, char *path, gboolean s)
{
    if (ifac != NULL) {
	GtkWidget *w = gtk_item_factory_get_item(ifac, path);

	if (w != NULL) {
	    gtk_widget_set_sensitive(w, s);
	} else {
	    fprintf(stderr, _("Failed to flip state of \"%s\"\n"), path);
	}
    }
}

/* ........................................................... */

static void model_rtf_copy_state (GtkItemFactory *ifac, gboolean s)
{
    flip(ifac, "/Edit/Copy all/as RTF", s);
}

/* ........................................................... */

static void model_latex_copy_state (GtkItemFactory *ifac, gboolean s)
{
    flip(ifac, "/Edit/Copy all/as LaTeX", s);
}

/* ........................................................... */

static void model_equation_copy_state (GtkItemFactory *ifac, gboolean s)
{
    flip(ifac, "/LaTeX/View/Equation", s);
    flip(ifac, "/LaTeX/Save/Equation", s);
    flip(ifac, "/LaTeX/Copy/Equation", s);
}

/* ........................................................... */

static void model_arch_menu_state (GtkItemFactory *ifac, gboolean s)
{
    flip(ifac, "/Tests/ARCH", s);
}

/* ........................................................... */

static void model_panel_menu_state (GtkItemFactory *ifac, gboolean s)
{
    flip(ifac, "/Tests/panel diagnostics", s);
}

/* ........................................................... */

static void model_ml_menu_state (GtkItemFactory *ifac, gboolean s)
{
    flip(ifac, "/Model data/Add to data set/log likelihood", s);
}

/* ........................................................... */

static void model_menu_state (GtkItemFactory *ifac, gboolean s)
{
    flip(ifac, "/Tests/non-linearity (squares)", s);
    flip(ifac, "/Tests/non-linearity (logs)", s);
    flip(ifac, "/Tests/autocorrelation", s);
    flip(ifac, "/Tests/heteroskedasticity", s);
    flip(ifac, "/Tests/Chow test", s);
    flip(ifac, "/Tests/CUSUM test", s);
    flip(ifac, "/Tests/ARCH", s);
    flip(ifac, "/Tests/normality of residual", s);
    flip(ifac, "/Graphs", s);
    flip(ifac, "/Model data/Display actual, fitted, residual", s);
    flip(ifac, "/Model data/Forecasts with standard errors", s);
    flip(ifac, "/Model data/Add to data set/residuals", s);
    flip(ifac, "/Model data/Add to data set/squared residuals", s);
    flip(ifac, "/Model data/Add to data set/error sum of squares", s);
    flip(ifac, "/Model data/Add to data set/standard error of residuals", s);
    flip(ifac, "/Model data/Add to data set/R-squared", s);
    flip(ifac, "/Model data/Add to data set/T*R-squared", s);    
}

/* ........................................................... */

static void ols_menu_state (GtkItemFactory *ifac, gboolean s)
{
    flip(ifac, "/Tests/non-linearity (squares)", s);
    flip(ifac, "/Tests/non-linearity (logs)", s);
    flip(ifac, "/Tests/autocorrelation", s);
    flip(ifac, "/Tests/heteroskedasticity", s);
    flip(ifac, "/Tests/Chow test", s);
    flip(ifac, "/Tests/CUSUM test", s);
    flip(ifac, "/Tests/ARCH", s);
    flip(ifac, "/Tests/influential observations", s);
}

/* ........................................................... */

static void lad_menu_mod (GtkItemFactory *ifac)
{
    flip(ifac, "/Tests", FALSE);
    flip(ifac, "/Model data/Forecasts with standard errors", FALSE);
    flip(ifac, "/Model data/coefficient covariance matrix", FALSE);
    flip(ifac, "/Model data/Add to data set/R-squared", FALSE);
    flip(ifac, "/Model data/Add to data set/T*R-squared", FALSE);
}

/* ........................................................... */

static void nls_menu_mod (GtkItemFactory *ifac)
{
    flip(ifac, "/Tests/omit variables", FALSE);
    flip(ifac, "/Tests/add variables", FALSE);
    flip(ifac, "/Tests/sum of coefficients", FALSE);
    flip(ifac, "/Tests/Ramsey's RESET", FALSE);
    flip(ifac, "/Model data/Forecasts with standard errors", FALSE);
    flip(ifac, "/Model data/Confidence intervals for coefficients", FALSE);
}

/* ........................................................... */

static void latex_menu_state (GtkItemFactory *ifac, gboolean s)
{
    flip(ifac, "/LaTeX", s);
}

/* ........................................................... */

static void model_save_state (GtkItemFactory *ifac, gboolean s)
{
    flip(ifac, "/File/Save to session as icon", s);
    flip(ifac, "/File/Save as icon and close", s);
}

/* ........................................................... */

static void set_up_viewer_menu (GtkWidget *window, windata_t *vwin, 
				GtkItemFactoryEntry items[])
{
    GtkAccelGroup *accel;
    gint n_items = 0;

    while (items[n_items].path != NULL) n_items++;

    accel = gtk_accel_group_new();
    vwin->ifac = gtk_item_factory_new(GTK_TYPE_MENU_BAR, "<main>", 
				      accel);
#ifdef ENABLE_NLS
    gtk_item_factory_set_translate_func(vwin->ifac, menu_translate, NULL, NULL);
#endif
    gtk_item_factory_create_items(vwin->ifac, n_items, items, vwin);
    vwin->mbar = gtk_item_factory_get_widget(vwin->ifac, "<main>");
    gtk_accel_group_attach(accel, GTK_OBJECT (window));

    if (vwin->role == SUMMARY || vwin->role == VAR_SUMMARY
	|| vwin->role == CORR || vwin->role == FCASTERR
	|| vwin->role == FCAST || vwin->role == COEFFINT
	|| vwin->role == COVAR) {
	augment_copy_menu(vwin);
	return;
    }

    if (vwin->role == VIEW_MODEL && vwin->data != NULL) { 
	MODEL *pmod = (MODEL *) vwin->data;

	model_rtf_copy_state(vwin->ifac, !pmod->errcode);
	model_latex_copy_state(vwin->ifac, !pmod->errcode);
	latex_menu_state(vwin->ifac, !pmod->errcode);

	model_equation_copy_state(vwin->ifac, 
				  !pmod->errcode && pmod->ci != NLS);

	model_panel_menu_state(vwin->ifac, pmod->ci == POOLED);

	ols_menu_state(vwin->ifac, pmod->ci == OLS || pmod->ci == POOLED);

	if (pmod->ci == LOGIT || pmod->ci == PROBIT) {
	    model_menu_state(vwin->ifac, FALSE);
	    model_ml_menu_state(vwin->ifac, TRUE);
	} else {
	    model_ml_menu_state(vwin->ifac, FALSE);
	}

	if (pmod->name) model_save_state(vwin->ifac, FALSE);

	if (pmod->ci == LAD) lad_menu_mod(vwin->ifac);
	else if (pmod->ci == NLS) nls_menu_mod(vwin->ifac);

	if (dataset_is_panel(datainfo)) {
	    model_arch_menu_state(vwin->ifac, FALSE);
	}
    } else if (vwin->role == VAR && vwin->data != NULL) {
	GRETL_VAR *var = (GRETL_VAR *) vwin->data;
	const char *name = gretl_var_get_name(var);

	if (name != NULL && *name != '\0') {
	    model_save_state(vwin->ifac, FALSE);
	}
    }
}

/* .................................................................. */

static void add_vars_to_plot_menu (windata_t *vwin)
{
    int i, j = 0, varstart;
    GtkItemFactoryEntry varitem;
    const gchar *mpath[] = {
	N_("/Graphs/residual plot"), 
	N_("/Graphs/fitted, actual plot")
    };
    MODEL *pmod = vwin->data;
    char tmp[16];

    for (i=0; i<2; i++) {
	varitem.accelerator = NULL;
	varitem.callback_action = 0; 
	varitem.item_type = NULL;
	if (dataset_is_time_series(datainfo)) {
	    varitem.path = 
		g_strdup_printf(_("%s/against time"), mpath[i]);
	} else {
	    varitem.path = 
		g_strdup_printf(_("%s/by observation number"), mpath[i]);
	}
	if (i == 0) {
	    varitem.callback = resid_plot; 
	} else {
	    varitem.callback = fit_actual_plot;
	}

	gtk_item_factory_create_item(vwin->ifac, &varitem, vwin, 1);
	g_free(varitem.path);

	varstart = (i == 0)? 1 : 2;

	/* put the indep vars on the menu list */
	for (j=varstart; pmod->ci != NLS && j<=pmod->list[0]; j++) {
	    if (pmod->list[j] == 0) continue;
	    if (!strcmp(datainfo->varname[pmod->list[j]], "time")) 
		continue;
	    varitem.accelerator = NULL;
	    varitem.callback_action = pmod->list[j]; 
	    varitem.item_type = NULL;
	    double_underscores(tmp, datainfo->varname[pmod->list[j]]);
	    varitem.path =
		g_strdup_printf(_("%s/against %s"), mpath[i], tmp);
	    if (i == 0) {
		varitem.callback = resid_plot; 
	    } else {
		varitem.callback = fit_actual_plot;
	    }
	    gtk_item_factory_create_item(vwin->ifac, &varitem, vwin, 1);
	    g_free(varitem.path);
	}

	/* if the model has two independent vars, offer a 3-D fitted
	   versus actual plot */
	if (i == 1 && pmod->ifc && pmod->ncoeff == 3) {
	    char tmp2[16];

	    varitem.accelerator = NULL;
	    varitem.callback_action = 0;
	    varitem.item_type = NULL;
	    double_underscores(tmp, datainfo->varname[pmod->list[3]]);
	    double_underscores(tmp2, datainfo->varname[pmod->list[4]]);
	    varitem.path =
		g_strdup_printf(_("%s/against %s and %s"), 
				mpath[i], tmp, tmp2);
	    varitem.callback = fit_actual_splot;
	    gtk_item_factory_create_item(vwin->ifac, &varitem, vwin, 1);
	    g_free(varitem.path);
	}
    }
}

/* .................................................................. */

static void plot_dummy_call (gpointer data, guint v, GtkWidget *widget)
{
    GtkCheckMenuItem *item = GTK_CHECK_MENU_ITEM(widget);
    windata_t *vwin = (windata_t *) data;

    if (item->active) vwin->active_var = v; 
}

/* .................................................................. */

static void add_dummies_to_plot_menu (windata_t *vwin)
{
    int i, dums = 0;
    GtkItemFactoryEntry dumitem;
    MODEL *pmod = vwin->data;
    const gchar *mpath[] = {
	N_("/Graphs/dumsep"), 
	N_("/Graphs/Separation")
    };
    gchar *radiopath = NULL;
    char tmp[16];

    dumitem.path = NULL;
    dumitem.accelerator = NULL; 

    /* put the dummy independent vars on the menu list */
    for (i=2; i<pmod->list[0]; i++) {

	if (pmod->list[i] == 0) continue;
	if (pmod->list[i] == LISTSEP) break;

	if (!isdummy(Z[pmod->list[i]], datainfo->t1, datainfo->t2)) {
	    continue;
	}

	if (!dums) { /* add separator, branch and "none" */
	    dumitem.path = mymalloc(64);

	    /* separator */
	    dumitem.callback = NULL;
	    dumitem.callback_action = 0;
	    dumitem.item_type = "<Separator>";
	    sprintf(dumitem.path, _("%s"), mpath[0]);
	    gtk_item_factory_create_item(vwin->ifac, &dumitem, vwin, 1);

	    /* menu branch */
	    dumitem.item_type = "<Branch>";
	    sprintf(dumitem.path, _("%s"), mpath[1]);
	    gtk_item_factory_create_item(vwin->ifac, &dumitem, vwin, 1);

	    /* "none" option */
	    dumitem.callback = plot_dummy_call;
	    dumitem.item_type = "<RadioItem>";
	    sprintf(dumitem.path, _("%s/none"), mpath[1]);
	    radiopath = g_strdup(dumitem.path);
	    gtk_item_factory_create_item(vwin->ifac, &dumitem, vwin, 1);
	    dums = 1;
	} 

	dumitem.callback_action = pmod->list[i]; 
	double_underscores(tmp, datainfo->varname[pmod->list[i]]);
	sprintf(dumitem.path, _("%s/by %s"), mpath[1], tmp);
	dumitem.callback = plot_dummy_call;	    
	dumitem.item_type = radiopath;
	gtk_item_factory_create_item(vwin->ifac, &dumitem, vwin, 1);

    }

    free(dumitem.path);
    free(radiopath);
}

/* ........................................................... */

static void impulse_response_call (gpointer p, guint shock, GtkWidget *w)
{
    windata_t *vwin = (windata_t *) p;
    GRETL_VAR *var = (GRETL_VAR *) vwin->data;
    gint targ;
    int err;

    targ = GPOINTER_TO_INT(gtk_object_get_data(GTK_OBJECT(w), "targ"));

    err = gretl_var_plot_impulse_response(var, targ, shock, 0, datainfo, 
					  &paths);
    if (!err) {
	register_graph();
    }
}

static void add_var_menu_items (windata_t *vwin)
{
    int i, j;
    GtkItemFactoryEntry varitem;
    const gchar *gpath = N_("/Graphs");
    const gchar *dpath = N_("/Model data/Add to data set");
    GRETL_VAR *var = vwin->data;
    int neqns = gretl_var_get_n_equations(var);
    int vtarg, vshock;
    char tmp[16];

    varitem.accelerator = NULL;
    varitem.callback = NULL;
    varitem.callback_action = 0;
    varitem.item_type = "<Branch>";

    varitem.path = g_strdup(_("/_Graphs"));
    gtk_item_factory_create_item(vwin->ifac, &varitem, vwin, 1);
    g_free(varitem.path);

    varitem.path = g_strdup(_("/_Model data/Add to data set"));
    gtk_item_factory_create_item(vwin->ifac, &varitem, vwin, 1);
    g_free(varitem.path);

    for (i=0; i<neqns; i++) {
	char maj[32], min[16];

	/* save resids items */
	varitem.path = g_strdup_printf("%s/%s %d", _(dpath), 
				       _("residuals from equation"), i + 1);
	varitem.callback = var_resid_callback;
	varitem.callback_action = i;
	varitem.item_type = NULL;
	gtk_item_factory_create_item(vwin->ifac, &varitem, vwin, 1);
	g_free(varitem.path);	

	/* impulse responses: make branch for target */
	vtarg = gretl_var_get_variable_number(var, i);

	double_underscores(tmp, datainfo->varname[vtarg]);
	sprintf(maj, _("response of %s"), tmp);

	varitem.path = g_strdup_printf("%s/%s", _(gpath), maj);
	varitem.callback = NULL;
	varitem.callback_action = 0;
	varitem.item_type = "<Branch>";
	gtk_item_factory_create_item(vwin->ifac, &varitem, vwin, 1);
	g_free(varitem.path);

	varitem.item_type = NULL;
	
	for (j=0; j<neqns; j++) {
	    GtkWidget *w;

	    /* impulse responses: subitems for shocks */
	    vshock = gretl_var_get_variable_number(var, j);
	    varitem.callback_action = j;

	    double_underscores(tmp, datainfo->varname[vshock]);
	    sprintf(min, _("to %s"), tmp);

	    varitem.path = g_strdup_printf("%s/%s/%s", _(gpath), maj, min);
	    varitem.callback = impulse_response_call;
	    varitem.callback_action = j;
	    varitem.item_type = NULL;
	    gtk_item_factory_create_item(vwin->ifac, &varitem, vwin, 1);
	    w = gtk_item_factory_get_widget_by_action(vwin->ifac, j);
	    gtk_object_set_data(GTK_OBJECT(w), "targ", GINT_TO_POINTER(i));
	    g_free(varitem.path);
	}
    }
}

/* ........................................................... */

#define ALLOW_MODEL_DATASETS

static void check_model_menu (GtkWidget *w, GdkEventButton *eb, 
			      gpointer data)
{
    windata_t *mwin = (windata_t *) data;
    MODEL *pmod = mwin->data;
    extern int quiet_sample_check (MODEL *pmod); /* library.c */
    int s, ok = 1;

    if (Z == NULL) { 
	/* should be impossible, but... */
	flip(mwin->ifac, "/File/Save to session as icon", FALSE);
	flip(mwin->ifac, "/File/Save as icon and close", FALSE);
	flip(mwin->ifac, "/Edit/Copy all", FALSE);
	flip(mwin->ifac, "/Model data", FALSE);
	flip(mwin->ifac, "/Tests", FALSE);
	flip(mwin->ifac, "/Graphs", FALSE);
	flip(mwin->ifac, "/Model data", FALSE);
	flip(mwin->ifac, "/LaTeX", FALSE);
	return;
    }

    if (quiet_sample_check(pmod)) ok = 0;

    s = GTK_WIDGET_IS_SENSITIVE
	(gtk_item_factory_get_item(mwin->ifac, "/Tests"));
    if ((s && ok) || (!s && !ok)) return;
    s = !s;

#ifdef ALLOW_MODEL_DATASETS
    if (!ok && dataset_added_to_model(pmod)) {
	flip(mwin->ifac, "/Tests", s);
	flip(mwin->ifac, "/Model data/Display actual, fitted, residual", s);
	if (pmod->ci != LAD) {
	    flip(mwin->ifac, "/Model data/Forecasts with standard errors", s);
	}
	flip(mwin->ifac, "/Model data/Confidence intervals for coefficients", s);
	flip(mwin->ifac, "/Model data/Add to data set/fitted values", s);
	flip(mwin->ifac, "/Model data/Add to data set/residuals", s);
	flip(mwin->ifac, "/Model data/Add to data set/squared residuals", s);
	flip(mwin->ifac, "/Model data/Define new variable...", s);
	infobox(get_gretl_errmsg());
	return;
    }
#endif

    flip(mwin->ifac, "/Tests", s);
    flip(mwin->ifac, "/Graphs", s);
    flip(mwin->ifac, "/Model data/Display actual, fitted, residual", s);
    if (pmod->ci != LAD) {
	flip(mwin->ifac, "/Model data/Forecasts with standard errors", s);
    }
    flip(mwin->ifac, "/Model data/Confidence intervals for coefficients", s);
    flip(mwin->ifac, "/Model data/Add to data set/fitted values", s);
    flip(mwin->ifac, "/Model data/Add to data set/residuals", s);
    flip(mwin->ifac, "/Model data/Add to data set/squared residuals", s);
    flip(mwin->ifac, "/Model data/Define new variable...", s);

    if (!ok) {
	infobox(get_gretl_errmsg());
    } 
}

/* ......................................................... */

void setup_column (GtkWidget *listbox, int column, int width) 
{
    if (width == 0) {
	gtk_clist_set_column_auto_resize (GTK_CLIST (listbox), column, TRUE);
    } else if (width == -1) {
	gtk_clist_set_column_visibility (GTK_CLIST (listbox), column, FALSE);
    } else {
	gtk_clist_set_column_width (GTK_CLIST (listbox), column, width);
    }
}

/* ........................................................... */

#include "../pixmaps/stock_dialog_error_48.xpm"
#include "../pixmaps/stock_dialog_info_48.xpm"

static GtkWidget *get_msgbox_icon (int err)
{
    static GdkColormap *cmap;
    GtkWidget *iconw;
    GdkPixmap *icon;
    GdkBitmap *mask;
    gchar **msgxpm;

    if (err) {
	msgxpm = stock_dialog_error_48_xpm;
    } else {
	msgxpm = stock_dialog_info_48_xpm;
    }

    if (cmap == NULL) {
	cmap = gdk_colormap_get_system();
    }
    icon = gdk_pixmap_colormap_create_from_xpm_d(NULL, cmap, &mask, NULL, 
						 msgxpm);
    iconw = gtk_pixmap_new(icon, mask);

    return iconw;
}

static void msgbox (const char *msg, int err) 
{
    GtkWidget *w, *label, *button, *vbox, *hbox, *hsep, *iconw;

    w = gtk_window_new(GTK_WINDOW_DIALOG);
    gtk_container_border_width(GTK_CONTAINER(w), 5);
    gtk_window_position (GTK_WINDOW(w), GTK_WIN_POS_MOUSE);
    gtk_window_set_title (GTK_WINDOW (w), (err)? _("gretl error") : 
			  _("gretl info")); 

    vbox = gtk_vbox_new(FALSE, 5);
    gtk_container_add(GTK_CONTAINER(w), vbox);

    hbox = gtk_hbox_new(FALSE, 5);
    gtk_container_add(GTK_CONTAINER(vbox), hbox);

    /* icon */
    iconw = get_msgbox_icon(err);
    gtk_box_pack_start(GTK_BOX(hbox), iconw, FALSE, FALSE, 5);

    /* text of message */
    label = gtk_label_new(msg);
    gtk_box_pack_start(GTK_BOX(hbox), label, FALSE, FALSE, 5);

    hsep = gtk_hseparator_new();
    gtk_container_add(GTK_CONTAINER(vbox), hsep);

    /* button */
    hbox = gtk_hbox_new(FALSE, 5);
    gtk_container_add(GTK_CONTAINER(vbox), hbox);
    
    if (err) {
	button = gtk_button_new_with_label(_("Close"));
    } else {
	button = gtk_button_new_with_label(_("OK"));
    }

    gtk_box_pack_end(GTK_BOX(hbox), button, FALSE, FALSE, 5);

    gtk_signal_connect(GTK_OBJECT(button), "clicked",
		       GTK_SIGNAL_FUNC(delete_widget), w);

    gtk_widget_show_all(w);
}

/* ........................................................... */

void errbox (const char *msg) 
{
    msgbox(msg, 1);
}

/* ........................................................... */

void infobox (const char *msg) 
{
    msgbox(msg, 0);
}

/* ........................................................... */

int validate_varname (const char *varname)
{
    int i, n = strlen(varname);
    char namebit[VNAMELEN];
    unsigned char c;

    *namebit = 0;
    
    if (n > VNAMELEN - 1) {
	strncat(namebit, varname, VNAMELEN - 1);
	sprintf(errtext, _("Variable name %s... is too long\n"
	       "(the max is 8 characters)"), namebit);
	errbox(errtext);
	return 1;
    }
    if (!(isalpha(varname[0]))) {
	sprintf(errtext, _("First char of name ('%c') is bad\n"
	       "(first must be alphabetical)"), varname[0]);
	errbox(errtext);
	return 1;
    }
    for (i=1; i<n; i++) {
	c = (unsigned char) varname[i];
	
	if ((!(isalpha(c)) && !(isdigit(c)) && c != '_') || c > 127) {
	    sprintf(errtext, _("Name contains an illegal char (in place %d)\n"
		    "Use only unaccented letters, digits and underscore"), i + 1);
	    errbox(errtext);
	    return 1;
	}
    }
    return 0;
}	

/* .................................................................. */

int prn_to_clipboard (PRN *prn, int copycode)
{
    size_t len;

    if (prn->buf == NULL) return 0;
    len = strlen(prn->buf);

    if (clipboard_buf) g_free(clipboard_buf);
    clipboard_buf = mymalloc(len + 1);
    if (clipboard_buf == NULL) return 1;

    memcpy(clipboard_buf, prn->buf, len + 1);
    gtk_selection_owner_set(mdata->w,
			    GDK_SELECTION_PRIMARY,
			    GDK_CURRENT_TIME);
    return 0;
}

/* .................................................................. */

#define SPECIAL_COPY(h) (h == COPY_LATEX || h == COPY_RTF)

void text_copy (gpointer data, guint how, GtkWidget *widget) 
{
    windata_t *vwin = (windata_t *) data;
    PRN *prn;
    gchar *msg;

    /* descriptive statistics */
    if ((vwin->role == SUMMARY || vwin->role == VAR_SUMMARY)
	&& SPECIAL_COPY(how)) {
	GRETLSUMMARY *summ = (GRETLSUMMARY *) vwin->data;
	
	if (bufopen(&prn)) return;
	if (how == COPY_LATEX) {
	    texprint_summary(summ, datainfo, prn);
	} else { /* RTF */
	    rtfprint_summary(summ, datainfo, prn);
	}

	prn_to_clipboard(prn, 0);
	gretl_print_destroy(prn);
    }

    /* correlation matrix */
    else if (vwin->role == CORR && SPECIAL_COPY(how)) {
	CORRMAT *corr = (CORRMAT *) vwin->data;

	if (bufopen(&prn)) return;
	if (how == COPY_LATEX) { 
	    texprint_corrmat(corr, datainfo, prn);
	} 
	else { /* RTF */
	    rtfprint_corrmat(corr, datainfo, prn);
	}

	prn_to_clipboard(prn, 0);
	gretl_print_destroy(prn);
    }

    /* display for fitted, actual, resid */
    else if (vwin->role == FCAST && SPECIAL_COPY(how)) {
	FITRESID *fr = (FITRESID *) vwin->data;

	if (bufopen(&prn)) return;

	if (how == COPY_LATEX) { 
	    texprint_fit_resid(fr, datainfo, prn);
	} 
	else { /* RTF */
	    rtfprint_fit_resid(fr, datainfo, prn);
	}

	prn_to_clipboard(prn, 0);
	gretl_print_destroy(prn);
    }   

    /* forecasts with standard errors */
    else if (vwin->role == FCASTERR && SPECIAL_COPY(how)) {
	FITRESID *fr = (FITRESID *) vwin->data;

	if (bufopen(&prn)) return;

	if (how == COPY_LATEX) { 
	    texprint_fcast_with_errs(fr, datainfo, prn);
	} 
	else { /* RTF */
	    rtfprint_fcast_with_errs(fr, datainfo, prn);
	}

	prn_to_clipboard(prn, 0);
	gretl_print_destroy(prn);
    } 

    /* coefficient confidence intervals */
    else if (vwin->role == COEFFINT && SPECIAL_COPY(how)) {
	CONFINT *cf = (CONFINT *) vwin->data;

	if (bufopen(&prn)) return;

	if (how == COPY_LATEX) { 
	    texprint_confints(cf, datainfo, prn);
	} 
	else if (how == COPY_RTF) { 
	    rtfprint_confints(cf, datainfo, prn);
	}

	prn_to_clipboard(prn, 0);
	gretl_print_destroy(prn);
    } 

    /* coefficient covariance matrix */
    else if (vwin->role == COVAR && SPECIAL_COPY(how)) {
	VCV *vcv = (VCV *) vwin->data;

	if (bufopen(&prn)) return;

	if (how == COPY_LATEX) { 
	    texprint_vcv(vcv, datainfo, prn);
	} 
	else if (how == COPY_RTF) { 
	    rtfprint_vcv(vcv, datainfo, prn);
	}

	prn_to_clipboard(prn, 0);
	gretl_print_destroy(prn);
    }     

    /* VAR system */
    else if (vwin->role == VAR && SPECIAL_COPY(how)) {
	GRETL_VAR *var = (GRETL_VAR *) vwin->data;

	if (bufopen(&prn)) return;

	if (how == COPY_LATEX) { 
	    prn->format = GRETL_PRINT_FORMAT_TEX;
	    gretl_var_print(var, datainfo, prn);
	} 
	else if (how == COPY_RTF) { 
	    prn->format = GRETL_PRINT_FORMAT_RTF;
	    gretl_var_print(var, datainfo, prn);
	}

	prn_to_clipboard(prn, how);
	gretl_print_destroy(prn);
    }    
  
    /* or it's a model window we're copying from? */
    else if (vwin->role == VIEW_MODEL &&
	(how == COPY_RTF || how == COPY_LATEX ||
	 how == COPY_LATEX_EQUATION)) {
	MODEL *pmod = (MODEL *) vwin->data;

	if (pmod->errcode) { 
	    errbox("Couldn't format model");
	    return;
	}

	if (how == COPY_RTF) {
	    model_to_rtf(pmod);
	    return;
	}

	if (bufopen(&prn)) return;

	if (how == COPY_LATEX) { 
	    tex_print_model(pmod, datainfo, 0, prn);
	} else if (how == COPY_LATEX_EQUATION) {
	    tex_print_equation(pmod, datainfo, 0, prn);
	}

	prn_to_clipboard(prn, 0);
	gretl_print_destroy(prn);
    }

    /* otherwise copying plain text from window */
    else if (how == COPY_TEXT) {
	PRN textprn;

	gretl_print_attach_buffer(&textprn, 
				  gtk_editable_get_chars(GTK_EDITABLE(vwin->w), 
							 0, -1));
	prn_to_clipboard(&textprn, 0);
	g_free(textprn.buf);
    } else { /* COPY_SELECTION */
	gtk_editable_copy_clipboard(GTK_EDITABLE(vwin->w));
	return;
    }

    msg = g_strdup_printf(_("Copied contents of window as %s"),
			  (how == COPY_LATEX)? "LaTeX" :
			  (how == COPY_RTF)? "RTF" : _("plain text"));
    infobox(msg);
    g_free(msg);
}

/* .................................................................. */

#if defined (USE_GNOME)

void window_print (windata_t *vwin, guint u, GtkWidget *widget) 
{
    char *buf, *selbuf = NULL;
    GtkEditable *gedit = GTK_EDITABLE(vwin->w);

    buf = gtk_editable_get_chars(gedit, 0, -1);
    if (gedit->has_selection)
	selbuf = gtk_editable_get_chars(gedit, 
					gedit->selection_start_pos,
					gedit->selection_end_pos);
    winprint(buf, selbuf);
}

#endif

/* .................................................................. */

void text_undo (windata_t *vwin, guint u, GtkWidget *widget)
{
    gchar *old =
	gtk_object_get_data(GTK_OBJECT(vwin->w), "undo");
    
    if (old == NULL) {
	errbox(_("No undo information available"));
    } else {
	guint len = 
	    gtk_text_get_length(GTK_TEXT(vwin->w));
	guint pt = gtk_text_get_point(GTK_TEXT(vwin->w));

	gtk_text_freeze(GTK_TEXT(vwin->w));
	gtk_editable_delete_text(GTK_EDITABLE(vwin->w), 0, len);
	len = 0;
	gtk_editable_insert_text(GTK_EDITABLE(vwin->w), 
				 old, strlen(old), &len);
	gtk_text_set_point(GTK_TEXT(vwin->w), 
			   (pt > len - 1)? len - 1 : pt);
	gtk_text_thaw(GTK_TEXT(vwin->w));
	g_free(old);
	gtk_object_remove_data(GTK_OBJECT(vwin->w), "undo");
    }
}

/* .................................................................. */

void text_paste (windata_t *vwin, guint u, GtkWidget *widget)
{
    gchar *old;
    gchar *undo_buf =
	gtk_editable_get_chars(GTK_EDITABLE(vwin->w), 0, -1);

    old = gtk_object_get_data(GTK_OBJECT(vwin->w), "undo");
    g_free(old);

    gtk_object_set_data(GTK_OBJECT(vwin->w), "undo", undo_buf);

    gtk_editable_paste_clipboard(GTK_EDITABLE(vwin->w));
}

/* ......................................................... */

gint popup_menu_handler (GtkWidget *widget, GdkEvent *event,
			 gpointer data)
{
    GdkModifierType mods;

    gdk_window_get_pointer(widget->window, NULL, NULL, &mods);

    if (mods & GDK_BUTTON3_MASK && event->type == GDK_BUTTON_PRESS) {
	GdkEventButton *bevent = (GdkEventButton *) event; 

	gtk_menu_popup (GTK_MENU(data), NULL, NULL, NULL, NULL,
			bevent->button, bevent->time);
	return TRUE;
    }
    return FALSE;
}

/* .................................................................. */

void add_popup_item (gchar *label, GtkWidget *menu,
		     GtkSignalFunc func, gpointer data)
{
    GtkWidget *item;

    item = gtk_menu_item_new_with_label(label);
    gtk_menu_append(GTK_MENU(menu), item);
    gtk_signal_connect(GTK_OBJECT(item), "activate",
		       GTK_SIGNAL_FUNC(func), data);
    gtk_widget_show(item);
}

/* .................................................................. */

void *gui_get_plugin_function (const char *funcname, 
			       void **phandle)
{
    void *func;

    func = get_plugin_function(funcname, phandle);
    if (func == NULL) {
	errbox(get_gretl_errmsg());
    }

    return func;
}

/* .................................................................. */

int build_path (const char *dir, const char *fname, char *path, const char *ext)
{
    size_t len;
    *path = 0;

    if (dir == NULL || fname == NULL || path == NULL) return 1;

    strcat(path, dir);
    len = strlen(path);
    if (len == 0) return 1;

    /* strip a trailing single dot */
    if (len > 1 && path[len-1] == '.' && 
	(path[len-2] == '/' || path[len-2] == '\\')) {
	    path[len-1] = '\0';
    }    

    if (path[len - 1] == '/' || path[len - 1] == '\\') {
	/* dir is already properly terminated */
	strcat(path, fname);
    } else {
	/* otherwise put a separator in */
	strcat(path, SLASHSTR);
	strcat(path, fname);
    }

    if (ext != NULL) strcat(path, ext);

    return 0;
}

char *double_underscores (char *targ, const char *src)
{
    char *p = targ;

    while (*src) {
	if (*src == '_') {
	    *p++ = '_';
	    *p++ = '_';
	} else {
	    *p++ = *src;
	}
	src++;
    }
    *p = '\0';

    return targ;
}
