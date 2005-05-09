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

/* datafiles.c : for gretl */

#undef COLL_DEBUG 

#include "gretl.h"
#include "datafiles.h"
#include "database.h"
#include "filelists.h"
#include "webget.h"
#include "menustate.h"

#if !GLIB_CHECK_VERSION(2,0,0)
# define OLD_GTK
#else
# include "treeutils.h"
#endif

#include <sys/stat.h>
#include <unistd.h>
#include <dirent.h>
#include <errno.h>
#include <time.h>

#define N_BROWSER_TYPES 5
#define BROWSER_BUSY    1
#define BROWSER_OK      0

#ifdef OLD_GTK
extern GdkColor gray;
#endif

static GtkWidget *browsers[N_BROWSER_TYPES];

static GtkWidget *files_window (windata_t *fdata);
static GtkWidget *files_notebook (windata_t *fdata, int code);
static int populate_notebook_filelists (windata_t *fdata, 
					GtkWidget *notebook,
					int code);

typedef struct _file_collection file_collection;

struct _file_collection {
    char *path;
    char *descfile;
    char *title;
    int which;
    GtkWidget *page;
};

enum {
    STACK_PUSH,
    STACK_POP_DATA,
    STACK_POP_PS,
    STACK_RESET_DATA,
    STACK_RESET_PS,
    STACK_DESTROY
};

enum {
    RECOG_OK,
    RECOG_ERROR,
    RECOG_NOT
};

enum {
    COLL_NONE,
    COLL_DATA,
    COLL_PS
};

static char *full_path (char *s1, const char *s2);

static char *unslash (const char *s)
{
    char *dest = g_strdup(s);

    if (dest != NULL) {
	size_t n = strlen(dest);

	if (dest[n-1] == '\\' || dest[n-1] == '/') {
	    dest[n-1] = '\0';
	}
    }

    return dest;
}

static int recognized_collection (file_collection *coll)
{
    const file_collection recognized_data[] = {
	{ "wooldridge", "jw_descriptions", "Wooldridge", COLL_DATA, NULL },
	{ "gujarati", "dg_descriptions", "Gujarati", COLL_DATA, NULL },
	{ "pwt56", "descriptions", "Penn World Table", COLL_DATA, NULL },
	{ NULL, NULL, NULL, COLL_DATA, NULL }
    }; 
    const file_collection recognized_ps[] = {
	{ "pwt56", "ps_descriptions", "Penn World Table", COLL_PS, NULL },
	{ NULL, NULL, NULL, COLL_PS, NULL }
    }; 
    int i;

    for (i=0; recognized_data[i].path != NULL; i++) {
	if (strstr(coll->path, recognized_data[i].path) &&
	    !strcmp(coll->descfile, recognized_data[i].descfile)) {
	    coll->title = malloc(strlen(recognized_data[i].title) + 1);
	    if (coll->title == NULL) {
		return RECOG_ERROR;
	    }
	    strcpy(coll->title, recognized_data[i].title);
	    coll->which = COLL_DATA;
	    return RECOG_OK;
	}
    }

    for (i=0; recognized_ps[i].path != NULL; i++) {
	if (strstr(coll->path, recognized_ps[i].path) &&
	    !strcmp(coll->descfile, recognized_ps[i].descfile)) {
	    coll->title = malloc(strlen(recognized_ps[i].title) + 1);
	    if (coll->title == NULL) {
		return RECOG_ERROR;
	    }
	    strcpy(coll->title, recognized_ps[i].title);
	    coll->which = COLL_PS;
	    return RECOG_OK;
	}
    }

    return RECOG_NOT;
} 

static int get_title_from_descfile (file_collection *coll)
{
    FILE *fp;
    char *test;
    char line[64], title[24];
    int err = 0;

    test = full_path(coll->path, coll->descfile);

    fp = gretl_fopen(test, "r");

    if (fp == NULL) {
	err = 1;
    } else if (fgets(line, sizeof line, fp) == NULL) {
	err = 1;
    } else {
	chopstr(line);

	if (sscanf(line, "# %23[^:]", title) != 1) {
	    err = 1;
	} else {
	    coll->title = g_strdup(title);
	    if (coll->title == NULL) {
		err = 1;
	    }
	}
    } 

    if (fp != NULL) {
	fclose(fp);
    }

    return err;
}

static void free_file_collection (file_collection *coll)
{
    free(coll->path);
    free(coll->descfile);
    if (coll->title != NULL) {
	free(coll->title);
    }
    free(coll);
}  

static file_collection *file_collection_new (const char *path,
					     const char *descfile,
					     int *err)
{
    file_collection *coll;
    int chk;

    *err = 0;

    coll = malloc(sizeof *coll);
    if (coll == NULL) {
	*err = 1;
	return NULL;
    }

    coll->path = malloc(strlen(path) + 1);
    if (coll->path == NULL) {
	free(coll);
	*err = 1;
	return NULL;
    }

    coll->descfile = malloc(strlen(descfile) + 1);
    if (coll->descfile == NULL) {
	free(coll->path);
	free(coll);
	*err = 1;
	return NULL;
    }  

    strcpy(coll->path, path);
    strcpy(coll->descfile, descfile);

    coll->title = NULL;

    if (strstr(coll->descfile, "ps_")) {
	coll->which = COLL_PS;
    } else {
	coll->which = COLL_DATA;
    }

    chk = recognized_collection(coll);

    if (chk == RECOG_ERROR) {
	free_file_collection(coll);
	*err = 1;
	coll = NULL;
    } else if (chk == RECOG_NOT) {
	chk = get_title_from_descfile(coll);
	if (chk) {
	    free_file_collection(coll);
	    coll = NULL;
	}
    }
    
    return coll;
}

static file_collection *collection_stack (file_collection *coll, int op)
{
    static file_collection **datacoll;
    static file_collection **pscoll;
    static int n_data;
    static int n_data_popped;
    static int n_ps;
    static int n_ps_popped;
    file_collection *ret = NULL;
    int j;

    if (op == STACK_PUSH && coll != NULL) {
	if (coll->which == COLL_DATA) {
	    datacoll = realloc(datacoll, (n_data + 1) * sizeof *datacoll);
	    if (datacoll != NULL) {
		datacoll[n_data++] = coll;
		ret = coll;
	    }
	} else if (coll->which == COLL_PS) {
	    pscoll = realloc(pscoll, (n_ps + 1) * sizeof *pscoll);
	    if (pscoll != NULL) {
		pscoll[n_ps++] = coll;
		ret = coll;
	    }
	}
    } else if (op == STACK_POP_DATA && n_data_popped < n_data) {
	ret = datacoll[n_data_popped++];
    } else if (op == STACK_POP_PS && n_ps_popped < n_ps) {
	ret = pscoll[n_ps_popped++];
    } else if (op == STACK_RESET_DATA) {
	n_data_popped = 0;
    } else if (op == STACK_RESET_PS) {
	n_ps_popped = 0;
    } else if (op == STACK_DESTROY) {

        for (j=0; j<n_data; j++) {
	    free_file_collection(datacoll[j]);
	}
        free(datacoll);
        datacoll = NULL;
        n_data = 0;
        n_data_popped = 0;

        for (j=0; j<n_ps; j++) {
	    free_file_collection(pscoll[j]);
	}
        free(pscoll);
        pscoll = NULL;
        n_ps = 0;
        n_ps_popped = 0;
    } 

    return ret;
}

static int push_collection (file_collection *collection)
{
    return (collection_stack(collection, STACK_PUSH) == NULL);
}

static file_collection *pop_data_collection (void)
{
    return collection_stack(NULL, STACK_POP_DATA);
}

static file_collection *pop_ps_collection (void)
{
    return collection_stack(NULL, STACK_POP_PS);
}

void destroy_file_collections (void)
{
    collection_stack(NULL, STACK_DESTROY);
}

static void reset_data_stack (void)
{
    collection_stack(NULL, STACK_RESET_DATA);
}

static void reset_ps_stack (void)
{
    collection_stack(NULL, STACK_RESET_PS);
}

static char *full_path (char *s1, const char *s2)
{
    static char fpath[FILENAME_MAX];

    if (s1[strlen(s1) - 1] == '.') {
	s1[strlen(s1) - 1] = '\0';
    }
    
    if (s1[strlen(s1) - 1] == SLASH) {
	sprintf(fpath, "%s%s", s1, s2);
    } else {
	sprintf(fpath, "%s%c%s", s1, SLASH, s2);
    }

#ifdef COLL_DEBUG
    fprintf(stderr, "full_path: got '%s' from '%s' + '%s'\n",
	    fpath, s1, s2);
#endif

    return fpath;
}

static int test_dir_for_file_collections (const char *dname, DIR *dir)
{
    file_collection *coll;
    struct dirent *dirent;
    size_t n;
    int err = 0;

    while (!err && (dirent = readdir(dir))) { 
	if (strstr(dirent->d_name, "descriptions")) {
#ifdef COLL_DEBUG
	    fprintf(stderr, "   %s: looking at '%s'\n", dname, dirent->d_name);
#endif
	    n = strlen(dirent->d_name);
	    if (!strcmp(dirent->d_name + n - 12, "descriptions")) {
		coll = file_collection_new(dname, dirent->d_name, &err);
		if (coll != NULL && !err) {
		    err = push_collection(coll);
		}
	    }
	}
    }

    return err;
}

static int dont_go_there (const char *s)
{
    int ret = 0;

    if (!strcmp(s, "..") || strstr(s, ".inp") || strstr(s, ".gdt") || 
	strstr(s, ".gretl") || strstr(s, ".hdr")) {
	ret = 1;
    }

    return ret;
}

static int seek_file_collections (const char *topdir)
{
    DIR *dir, *try;  
    struct dirent *dirent;
    char *subdir;
    int err = 0;
    char *tmp = unslash(topdir);

    dir = opendir(tmp);
    if (dir == NULL) {
	return 1;
    }

#ifdef COLL_DEBUG
    fprintf(stderr, "seeking file collections in '%s'\n", topdir);
#endif

    while (!err && (dirent = readdir(dir))) {
	if (!dont_go_there(dirent->d_name)) {
	    if (strcmp(dirent->d_name, ".")) {
		subdir = full_path(tmp, dirent->d_name);
	    } else {
		subdir = tmp;
	    }
	    try = opendir(subdir);
	    if (try != NULL) {
#ifdef COLL_DEBUG
		fprintf(stderr, " trying in subdir '%s'\n", subdir);
#endif
		err = test_dir_for_file_collections(subdir, try);
#ifdef COLL_DEBUG
		fprintf(stderr, " result: err = %d\n", err);
#endif
		closedir(try);
	    }
	}
    }

    closedir(dir);

    free(tmp);

    return err;
}

#ifdef COLL_DEBUG
static void print_collection (const file_collection *coll)
{
    printf("path = '%s'\n", coll->path);
    printf("descfile = '%s'\n", coll->descfile);
    if (coll->title != NULL && *coll->title != '\0') {
	printf("title = '%s'\n", coll->title);
    }
}

static void print_data_collections (void)
{
    file_collection *coll;

    printf("\n*** Data collections:\n");
    while ((coll = pop_data_collection())) {
	print_collection(coll);
    }
    reset_data_stack();
}

static void print_script_collections (void)
{
    file_collection *coll;

    printf("\n*** Script collections:\n");

    while ((coll = pop_ps_collection())) {
	print_collection(coll);
    }
    reset_ps_stack();
}
#endif

static int build_file_collections (void)
{
    static int built;
    static int err;

    if (!built) {
	err = seek_file_collections(paths.datadir);
	if (!err) {
	    err = seek_file_collections(paths.scriptdir);
	}
	if (!err) {
	    err = seek_file_collections(paths.userdir);
	}
	built = 1;
    }

#ifdef COLL_DEBUG
    print_data_collections();
    print_script_collections();
#endif

    return err;
}

char *strip_extension (char *s)
{
    char *p = strrchr(s, '.');
    
    if (p != NULL && 
	(!strcmp(p, ".gdt") || !strcmp(p, ".inp") ||
	 !strcmp(p, ".bin"))) {
	*p = '\0';
    }

    return s;
}

static int read_file_descriptions (windata_t *win, gpointer p)
{
    FILE *fp;
#ifndef OLD_GTK
    GtkListStore *store;
    GtkTreeSelection *selection;
    GtkTreeIter iter;
#else
    gint i;    
#endif
    char line[MAXLEN], *index;
    file_collection *coll = (file_collection *) p;

    index = full_path(coll->path, coll->descfile);

    fp = gretl_fopen(index, "r");
    if (fp == NULL) {
	return 1;
    }

#ifndef OLD_GTK
    store = GTK_LIST_STORE(gtk_tree_view_get_model(GTK_TREE_VIEW(win->listbox)));
    gtk_tree_model_get_iter_first(GTK_TREE_MODEL(store), &iter);
#else
    i = 0;
#endif
    
    while (fgets(line, sizeof line, fp)) {
	char fname[24], descrip[80], data[64];

	if (*line == '#') continue;

	if (win->role == TEXTBOOK_DATA) {
	    if (sscanf(line, " \"%23[^\"]\",\"%79[^\"]\"", 
		       fname, descrip) == 2) {
#ifdef OLD_GTK
		gchar *row[2];	

		row[0] = strip_extension(fname);
		row[1] = descrip;
		gtk_clist_append(GTK_CLIST(win->listbox), row);
#else
		gtk_list_store_append(store, &iter);
		gtk_list_store_set(store, &iter, 
				   0, strip_extension(fname), 
				   1, descrip, -1);
#endif
	    }
	} else { /* script files */
	    if (sscanf(line, " \"%23[^\"]\",\"%79[^\"]\",\"%63[^\"]\"", 
		       fname, descrip, data) == 3) {
#ifdef OLD_GTK
		gchar *row[3];

		row[0] = strip_extension(fname);
		row[1] = descrip;
		row[2] = data;
		gtk_clist_append(GTK_CLIST(win->listbox), row);
#else
		gtk_list_store_append(store, &iter);
		gtk_list_store_set(store, &iter, 
				   0, strip_extension(fname), 
				   1, descrip, 
				   2, data, -1);
#endif
	    }
	}
#ifdef OLD_GTK
	if (i % 2) {
	    gtk_clist_set_background(GTK_CLIST(win->listbox), i, &gray);
	}
	i++;
#endif	
    }

    fclose(fp);

    /* select the first row */
#ifndef OLD_GTK
    gtk_tree_model_get_iter_first(GTK_TREE_MODEL(store), &iter);
    selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(win->listbox));
    gtk_tree_selection_select_iter(selection, &iter);
#else
    gtk_clist_select_row(GTK_CLIST(win->listbox), 0, 0);
#endif
    
    return 0;
}

static void display_datafile_info (GtkWidget *w, gpointer data)
{
    char hdrname[MAXLEN];
    windata_t *vwin = (windata_t *) data;
    PRN *prn;
    gchar *fname;
    file_collection *coll;

#ifndef OLD_GTK
    tree_view_get_string(GTK_TREE_VIEW(vwin->listbox), vwin->active_var,
			 0, &fname);
    coll = g_object_get_data(G_OBJECT(vwin->listbox), "coll");
#else
    gtk_clist_get_text(GTK_CLIST(vwin->listbox), vwin->active_var, 
		       0, &fname);
    coll = gtk_object_get_data(GTK_OBJECT(vwin->listbox), "coll");
#endif

    build_path(coll->path, fname, hdrname, ".gdt");

#ifndef OLD_GTK
    g_free(fname);
#endif

    prn = gretl_print_new(GRETL_PRINT_NULL, NULL);

    prn->buf = get_xml_description(hdrname);

    if (prn->buf != NULL) {
	view_buffer(prn, 80, 320, _("gretl: data header"), INFO, NULL);
    } else {
	errbox(_("Failed to retrieve description of data"));
	fprintf(stderr, I_("didn't get description from %s\n"), hdrname);
    }
}

void browser_open_data (GtkWidget *w, gpointer data)
{
    windata_t *vwin = (windata_t *) data;
    gchar *datname;
    file_collection *coll;

#ifndef OLD_GTK
    tree_view_get_string(GTK_TREE_VIEW(vwin->listbox), vwin->active_var, 
			 0, &datname);
    coll = g_object_get_data(G_OBJECT(vwin->listbox), "coll");
#else
    gtk_clist_get_text(GTK_CLIST(vwin->listbox), vwin->active_var, 
		       0, &datname);
    coll = gtk_object_get_data(GTK_OBJECT(vwin->listbox), "coll");
#endif

    build_path(coll->path, datname, trydatfile, ".gdt");

#ifndef OLD_GTK
    g_free(datname);
#endif

    set_datapage(coll->title);

    verify_open_data(vwin, OPEN_DATA);
} 

void browser_open_ps (GtkWidget *w, gpointer data)
{
    windata_t *vwin = (windata_t *) data;
    gchar *fname;
    file_collection *coll;

#ifndef OLD_GTK
    tree_view_get_string(GTK_TREE_VIEW(vwin->listbox), vwin->active_var, 
			 0, &fname);
    coll = g_object_get_data(G_OBJECT(vwin->listbox), "coll");
#else
    gtk_clist_get_text(GTK_CLIST(vwin->listbox), vwin->active_var, 
		       0, &fname);
    coll = gtk_object_get_data(GTK_OBJECT(vwin->listbox), "coll");
#endif

    build_path(coll->path, fname, scriptfile, ".inp");

#ifndef OLD_GTK
    g_free(fname);
#endif

    gtk_widget_destroy(GTK_WIDGET(vwin->w));

    mkfilelist(FILE_LIST_SCRIPT, scriptfile);

    set_scriptpage(coll->title);

    view_file(scriptfile, 0, 0, 78, 370, VIEW_SCRIPT);
} 

/* ........................................................... */

static void set_browser_status (windata_t *vwin, int status)
{
    if (status == BROWSER_BUSY) {
	browsers[vwin->role - TEXTBOOK_DATA] = vwin->w;
    } else {
	browsers[vwin->role - TEXTBOOK_DATA] = NULL;
    }
} 

static void free_browser (GtkWidget *w, gpointer data)
{
    windata_t *vwin = (windata_t *) data;

    set_browser_status(vwin, BROWSER_OK);
    free_windata(NULL, data);
}

static gpointer get_browser_ptr (int role)
{
    return (gpointer) &(browsers[role - TEXTBOOK_DATA]);
}

/* ........................................................... */

static void get_local_status (char *fname, char *status, time_t remtime)
{
    char fullname[MAXLEN];
    struct stat fbuf;
    int err;

    build_path(paths.binbase, fname, fullname, NULL);

    if ((err = stat(fullname, &fbuf)) == -1) {
	if (errno == ENOENT) {
#ifdef G_OS_WIN32
	    strcpy(status, _("Not installed"));
#else
	    /* try user dir too if not on Windows */
	    build_path(paths.userdir, fname, fullname, NULL);
	    if ((err = stat(fullname, &fbuf)) == -1) {
		strcpy(status, _("Not installed"));
	    } 
#endif
	} else {
	    strcpy(status, _("Unknown: access error"));
	}
    }

    if (!err) {
	if (difftime(remtime, fbuf.st_ctime) > 360) {
	    strcpy(status, _("Not up to date"));
	} else {
	    strcpy(status, _("Up to date"));
	}
    }
}

/* ........................................................... */

static int parse_db_list_line (char *line, char *fname, time_t *date)
{
    char mon[4], hrs[9];
    int day, yr;
    struct tm mytime;
    const char *months[] = {
	"Jan", "Feb", "Mar", "Apr",
	"May", "Jun", "Jul", "Aug",
	"Sep", "Oct", "Nov", "Dec"
    };
    int i;

    if (sscanf(line, "%*s%*s%3s%2d%8s%4d%16s", 
	       mon, &day, hrs, &yr, fname) != 5) {
	return 1;
    }

    hrs[2] = 0;

    mytime.tm_sec = 0;
    mytime.tm_min = 0;   
    mytime.tm_wday = 0;   
    mytime.tm_yday = 0;   
    mytime.tm_isdst = -1; 
    mytime.tm_hour = atoi(hrs);
    mytime.tm_year = yr - 1900;
    mytime.tm_mday = day;
    mytime.tm_mon = 0;

    for (i=0; i<12; i++) {
	if (strcmp(mon, months[i]) == 0) {
	    mytime.tm_mon = i;
	}
    }

    *date = mktime(&mytime);

    return 0;
}

/* ........................................................... */

static gint populate_remote_db_list (windata_t *win)
{
#ifndef OLD_GTK
    GtkListStore *store;
    GtkTreeIter iter;  
#endif  
    int err;
    char *buf;
    char fname[16], line[80], errbuf[80], status[20];
    gchar *row[3];
    gint i;
    time_t remtime;

    buf = mymalloc(GRETL_BUFSIZE);
    if (buf == NULL) return 1;

    memset(buf, 0, GRETL_BUFSIZE);

    *errbuf = '\0';

    err = list_remote_dbs(&buf, errbuf);

    if (err) {
	display_db_error(NULL, errbuf);
	free(buf);
	return err;
    }

#ifndef OLD_GTK
    store = GTK_LIST_STORE(gtk_tree_view_get_model 
			   (GTK_TREE_VIEW(win->listbox)));
    gtk_list_store_clear (store);
    gtk_tree_model_get_iter_first (GTK_TREE_MODEL(store), &iter);
#else
    gtk_clist_clear(GTK_CLIST(win->listbox));
    gtk_clist_freeze(GTK_CLIST(win->listbox));
#endif

    bufgets(NULL, 0, buf);
    i = 0;

    while (bufgets(line, sizeof line, buf)) {
	if (strstr(line, "idx")) continue;
	if (parse_db_list_line(line, fname, &remtime))
	    continue;
	get_local_status(fname, status, remtime);
	row[0] = strip_extension(fname);

	if (bufgets(line, sizeof line, buf)) {
	    row[1] = line + 2;
	} else {
	    row[1] = NULL;
	}

	row[2] = status;
#ifndef OLD_GTK
	gtk_list_store_append(store, &iter);
	gtk_list_store_set (store, &iter, 0, row[0], 1, row[1],
			    2, row[2], -1);
#else
	gtk_clist_append(GTK_CLIST(win->listbox), row);
	if (i % 2) {
	    gtk_clist_set_background(GTK_CLIST(win->listbox), i, &gray);
	}
#endif
	i++;
    }

#ifdef OLD_GTK
    gtk_clist_thaw(GTK_CLIST(win->listbox));
#endif

    free(buf);

    if (i == 0) errbox(_("No database files found"));
#ifdef OLD_GTK
    else {
	gtk_clist_select_row(GTK_CLIST(win->listbox), 0, 0);
    }
#endif

    return 0;
}

static void build_datafiles_popup (windata_t *vwin)
{
    if (vwin->popup == NULL) {
	vwin->popup = gtk_menu_new();

	add_popup_item(_("Info"), vwin->popup, 
		       G_CALLBACK(display_datafile_info), 
		       vwin);

	add_popup_item(_("Open"), vwin->popup, 
		       G_CALLBACK(browser_open_data), 
		       vwin);
    }
}

static int browser_busy (guint code)
{
    int ret = 0;

    if (code >= TEXTBOOK_DATA && code <= REMOTE_DB) {
	GtkWidget *w;

	w = browsers[code - TEXTBOOK_DATA];
	if (w != NULL) {
	    gdk_window_raise(w->window);
	    ret = 1;
	}
    }

    return ret;
}

void display_files (gpointer data, guint code, GtkWidget *widget)
{
    GtkWidget *filebox, *button;
    GtkWidget *main_vbox, *button_box;
    windata_t *vwin;
    void (*browse_func)() = NULL;
    int err = 0;

    if (browser_busy(code)) {
	return;
    }

    vwin = mymalloc(sizeof *vwin);
    if (vwin == NULL) {
	return;
    }

    windata_init(vwin);

    vwin->role = code;
    vwin->w = gtk_window_new(GTK_WINDOW_TOPLEVEL);

    g_signal_connect(G_OBJECT(vwin->w), "destroy",
		     G_CALLBACK(free_browser),
		     vwin);

    set_browser_status(vwin, BROWSER_BUSY);

    switch (code) {
    case PS_FILES:
	gtk_window_set_title(GTK_WINDOW(vwin->w), 
			     _("gretl: practice files"));
	browse_func = browser_open_ps;
	break;
    case TEXTBOOK_DATA:
	gtk_window_set_title(GTK_WINDOW(vwin->w), 
			     _("gretl: data files"));
	browse_func = browser_open_data;
	break;
    case NATIVE_DB:
    case RATS_DB:
	gtk_window_set_title(GTK_WINDOW(vwin->w), 
			     _("gretl: database files"));
	browse_func = open_db_list;
	break;
    case REMOTE_DB:
	gtk_window_set_title(GTK_WINDOW(vwin->w), 
			     _("gretl: databases on server"));
	browse_func = open_remote_db_list;
	break;
    }

    /* set up grids */
    main_vbox = gtk_vbox_new(FALSE, 5);
    gtk_container_set_border_width(GTK_CONTAINER(main_vbox), 10);
    gtk_container_add(GTK_CONTAINER(vwin->w), main_vbox);

    if (code == TEXTBOOK_DATA || code == PS_FILES) {
	filebox = files_notebook(vwin, code);
    } else {
	filebox = files_window(vwin);
    }

    gtk_box_pack_start(GTK_BOX(main_vbox), filebox, TRUE, TRUE, 0);

    if (code == TEXTBOOK_DATA) { 
	file_collection *coll;

	/* crate popup menu */
	build_datafiles_popup(vwin);

	while ((coll = pop_data_collection())) {
	    g_signal_connect(G_OBJECT(coll->page), "button_press_event",
			     G_CALLBACK(popup_menu_handler), 
			     vwin->popup);
	}
	reset_data_stack();
    } else if (code == REMOTE_DB) {
	GtkWidget *hbox;

	hbox = gtk_hbox_new(FALSE, 0);
	gtk_box_pack_start(GTK_BOX(main_vbox), hbox, FALSE, FALSE, 0);
	vwin->status = gtk_label_new(_("Network status: OK"));
	gtk_label_set_justify(GTK_LABEL(vwin->status), GTK_JUSTIFY_LEFT);
	gtk_box_pack_start(GTK_BOX(hbox), vwin->status, FALSE, FALSE, 0);
    }

    button_box = gtk_hbox_new(FALSE, 5);
    gtk_box_set_homogeneous(GTK_BOX(button_box), TRUE);
    gtk_box_pack_start(GTK_BOX(main_vbox), button_box, FALSE, FALSE, 0);

    button = gtk_button_new_with_label 
	((code == REMOTE_DB)? _("Get series listing") : _("Open"));
    gtk_box_pack_start(GTK_BOX(button_box), button, FALSE, TRUE, 0);

    g_signal_connect(G_OBJECT(button), "clicked",
		     G_CALLBACK(browse_func), vwin);

    if (code != NATIVE_DB && code != RATS_DB && code != REMOTE_DB) {
       	g_signal_connect(G_OBJECT(button), "clicked", 
			 G_CALLBACK(delete_widget), vwin->w); 
    }

    if (code == TEXTBOOK_DATA || code == REMOTE_DB) {
	button = gtk_button_new_with_label 
	    ((code == REMOTE_DB)? _("Install") : _("Info"));
	gtk_box_pack_start(GTK_BOX(button_box), button, FALSE, TRUE, 0);
	g_signal_connect(G_OBJECT(button), "clicked",
			 (code == REMOTE_DB)?
			 G_CALLBACK(grab_remote_db) :
			 G_CALLBACK(display_datafile_info), vwin);
    }

    if (code == TEXTBOOK_DATA) {
	button = gtk_button_new_with_label(_("Find"));
	gtk_box_pack_start(GTK_BOX (button_box), button, FALSE, TRUE, 0);
	g_signal_connect(G_OBJECT(button), "clicked",
			 G_CALLBACK(datafile_find), vwin);
    }

    button = gtk_button_new_with_label(_("Close"));
    gtk_box_pack_start(GTK_BOX(button_box), button, FALSE, TRUE, 0);
    g_signal_connect(G_OBJECT(button), "clicked",
		     G_CALLBACK(delete_widget), vwin->w);

    /* put stuff into list box(es) */
    if (code == TEXTBOOK_DATA || code == PS_FILES) {
	err = populate_notebook_filelists(vwin, filebox, code);
    } else {
	err = populate_filelist(vwin, NULL);
    }

    if (err) {
	errbox(_("Couldn't open database"));
	gtk_widget_destroy(vwin->w);
    } else {
	gtk_widget_show_all(vwin->w); 
    }
}

gint populate_filelist (windata_t *vwin, gpointer p)
{
    if (vwin->role == NATIVE_DB || vwin->role == RATS_DB) {
	return populate_dbfilelist(vwin);
    } else if (vwin->role == REMOTE_DB) {
	return populate_remote_db_list(vwin);
    } else {
	return read_file_descriptions(vwin, p);
    }
}

#ifndef OLD_GTK

static GtkWidget *files_window (windata_t *vwin) 
{
    const char *data_titles[] = {
	_("File"), 
	_("Summary")
    };
    const char *ps_titles[] = {
	_("Script"), 
	_("Topic"), 
	_("Data")
    };
    const char *db_titles[] = {
	_("Database"), 
	_("Source")
    };
    const char *remote_titles[] = {
	_("Database"), 
	_("Source"), 
	_("Local status")
    };

    const char **titles = data_titles;

    int full_width = 500, file_height = 260;
    int hidden_col = 0;

    GtkWidget *box;
    int cols = 2;

    switch (vwin->role) {
    case NATIVE_DB:
	titles = db_titles;
	hidden_col = 1;
	break;
    case REMOTE_DB:
	titles = remote_titles;
	cols = 3;
	full_width = 560;
	break;
    case RATS_DB:
	titles = db_titles;
	cols = 1;
	full_width = 240;
	hidden_col = 1;
	break;
    case PS_FILES:
	titles = ps_titles;
	cols = 3;
	full_width = 480;
	break;
    default:
	break;
    }

    full_width *= gui_scale;
    file_height *= gui_scale;

    box = gtk_vbox_new(FALSE, 0);
    gtk_widget_set_size_request(box, full_width, file_height);

    vwin->listbox = list_box_create(vwin, GTK_BOX(box), cols, 
				    hidden_col, titles);

    gtk_widget_show(box);

    return box;
}

#else /* now the old gtk version */

static GtkWidget *files_window (windata_t *fdata) 
{
    char *data_titles[] = {
	_("File"), 
	_("Summary")
    };
    char *ps_titles[] = {
	_("Script"), 
	_("Topic"), 
	_("Data")
    };
    char *db_titles[] = {
	_("Database"), 
	_("Source")
    };
    char *remote_titles[] = {
	_("Database"), 
	_("Source"), 
	_("Local status")
    };

    char **titles = data_titles;

    int data_col_width[] = {128, 256}; 
    int ps_col_width[] = {68, 180, 160};
    int db_col_width[] = {80, 304};
    int remote_col_width[] = {80, 256, 180};
    int *col_width = data_col_width;
    int full_width = 500, file_height = 260;

    GtkWidget *box, *scroller;
    int i, cols = 2;

    switch (fdata->role) {
    case NATIVE_DB:
	titles = db_titles;
	col_width = db_col_width;
	break;
    case REMOTE_DB:
	titles = remote_titles;
	cols = 3;
	col_width = remote_col_width;
	full_width = 560;
	break;
    case RATS_DB:
	titles = db_titles;
	cols = 1;
	col_width = db_col_width;
	col_width[0] = 200;
	full_width = 240;
	break;
    case PS_FILES:
	titles = ps_titles;
	cols = 3;
	col_width = ps_col_width;
	full_width = 480;
	break;
    default:
	break;
    }

    fdata->active_var = 1; 

    box = gtk_vbox_new (FALSE, 0);

    full_width *= gui_scale;
    file_height *= gui_scale;

    gtk_widget_set_usize (box, full_width, file_height);
   
    scroller = gtk_scrolled_window_new (NULL, NULL);
    gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scroller),
				    GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    fdata->listbox = gtk_clist_new_with_titles(cols, titles);
    gtk_clist_column_titles_passive(GTK_CLIST(fdata->listbox));
    gtk_container_add (GTK_CONTAINER (scroller), fdata->listbox);
    gtk_clist_set_selection_mode (GTK_CLIST (fdata->listbox), 
				  GTK_SELECTION_BROWSE);
    for (i=0; i<cols; i++) {
	col_width[i] *= gui_scale;
	gtk_clist_set_column_width (GTK_CLIST (fdata->listbox), i,
				    col_width[i]);
	gtk_clist_set_column_justification (GTK_CLIST (fdata->listbox), i, 
					    GTK_JUSTIFY_LEFT);
    }

    gtk_box_pack_start (GTK_BOX (box), scroller, TRUE, TRUE, TRUE);
    gtk_signal_connect_after (GTK_OBJECT (fdata->listbox), "select_row", 
			      GTK_SIGNAL_FUNC (selectrow), fdata);
    gtk_widget_show (fdata->listbox);
    gtk_widget_show (scroller);

    gtk_widget_show (box);

    return box;
}

#endif /* old vs new gtk */

static void 
switch_file_page_callback (GtkNotebook *notebook, GtkNotebookPage *page,
			   guint page_num, windata_t *vwin)
{
    gpointer p = g_object_get_data(G_OBJECT(notebook), "browse_ptr");

    if (p != NULL) {
	GtkWidget *w = *(GtkWidget **) p;

	if (w != NULL) {
	    char wnum[4];

	    sprintf(wnum, "%d", (int) page_num);
	    vwin->listbox = g_object_get_data(G_OBJECT(notebook), wnum);
	}
    }
}

/* below: Construct a set of notebook pages for either data files
   (Ramanathan, Wooldridge, etc.) or practice scripts.  Creates the
   pages but does not yet fill them out.
*/

static GtkWidget *files_notebook (windata_t *vwin, int code)
{
    GtkWidget *notebook;
    GtkWidget *listpage;
    GtkWidget *label;
    char wnum[4];
    file_collection *coll;
    int j;

    if (code != TEXTBOOK_DATA && code != PS_FILES) {
	return NULL;
    }

    build_file_collections(); /* FIXME check for errors */

    notebook = gtk_notebook_new();

    j = 0;

    while (1) {
	if (code == TEXTBOOK_DATA) {
	    coll = pop_data_collection();
	} else {
	    coll = pop_ps_collection();
	}

	if (coll == NULL) break;

	listpage = files_window(vwin);
	label = gtk_label_new(coll->title);
	gtk_widget_show(label);
	gtk_notebook_append_page(GTK_NOTEBOOK(notebook), listpage, label);
	coll->page = vwin->listbox;
	sprintf(wnum, "%d", j);
	g_object_set_data(G_OBJECT(notebook), wnum, coll->page);
	g_object_set_data(G_OBJECT(coll->page), "coll", coll);
	j++;
    }
    
    if (code == TEXTBOOK_DATA) {
	reset_data_stack();
    } else {
	reset_ps_stack();
    }

    g_object_set_data(G_OBJECT(GTK_NOTEBOOK(notebook)), "browse_ptr",
		      get_browser_ptr(vwin->role));
    g_signal_connect(G_OBJECT(GTK_NOTEBOOK(notebook)), "switch-page",
		     G_CALLBACK(switch_file_page_callback),
		     vwin);

    gtk_widget_show(notebook);

    return notebook;
}

/* below: fill out a set of notebook pages (for data files
   or scripts files), entering the details into the page.
*/

static int populate_notebook_filelists (windata_t *vwin, 
					GtkWidget *notebook,
					int code)
{
    file_collection *coll;
    const char *title;
    int gotcol = 0;
    int j;

    if (vwin == NULL) {
	return 1;
    }

    while (1) {
	if (code == TEXTBOOK_DATA) {
	    coll = pop_data_collection();
	} else {
	    coll = pop_ps_collection();
	}

	if (coll != NULL) {
	    gotcol = 1;
	} else {
	    break;
	}

	vwin->listbox = coll->page;
	populate_filelist(vwin, coll);
    }

    if (!gotcol) {
	return 1;
    }

    if (code == TEXTBOOK_DATA) {
	reset_data_stack();
    } else {
	reset_ps_stack();
    }

    j = 0;

    if (code == TEXTBOOK_DATA) {
	title = get_datapage();
	if (*title != '\0') {
	    while ((coll = pop_data_collection())) {
		if (!strcmp(coll->title, title)) {
		    break;
		}
		j++;
	    }
	} else {
	    coll = pop_data_collection();
	}
    } else {
	title = get_scriptpage();
	if (*title != '\0') {
	    while ((coll = pop_ps_collection())) {
		if (!strcmp(coll->title, title)) {
		    break;
		}
		j++;
	    }
	} else {
	    coll = pop_ps_collection();
	}
    }

    vwin->listbox = coll->page;

    if (code == TEXTBOOK_DATA) {
	reset_data_stack();
    } else {
	reset_ps_stack();
    }

    gtk_notebook_set_current_page(GTK_NOTEBOOK(notebook), j);

    gtk_widget_grab_focus(vwin->listbox);

    return 0;
} 





