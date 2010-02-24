/* 
 *  gretl -- Gnu Regression, Econometrics and Time-series Library
 *  Copyright (C) 2001 Allin Cottrell and Riccardo "Jack" Lucchetti
 * 
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 * 
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 * 
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 * 
 */

/* helpfiles.c for gretl */

#include "gretl.h"
#include "textbuf.h"
#include "gretl_www.h"
#include "treeutils.h"
#include "dlgutils.h"
#include "menustate.h"

#ifdef G_OS_WIN32
# include "gretlwin32.h"
#else
# include <sys/stat.h>
# include <sys/types.h>
# include <fcntl.h>
# include <unistd.h>
# include <dirent.h>
#endif

#define HDEBUG 0

static int translated_helpfile = -1;
static char *en_gui_helpfile;
static char *en_cli_helpfile;

static void real_do_help (int idx, int pos, int role);
static void en_help_callback (GtkAction *action, windata_t *hwin);
static void delete_help_viewer (GtkAction *a, windata_t *hwin);
static char *funcs_helpfile (void);
static void helpwin_set_topic_index (windata_t *hwin, int idx);

/* searching stuff */
static void find_in_text (GtkWidget *button, GtkWidget *dialog);
static void find_in_listbox (GtkWidget *button, GtkWidget *dialog);
static void find_string_dialog (void (*findfunc)(), windata_t *vwin);
static gboolean real_find_in_text (GtkTextView *view, const gchar *s, 
				   gboolean from_cursor, 
				   gboolean search_all);
static gboolean real_find_in_listbox (windata_t *vwin, gchar *s, 
				      gboolean vnames);

static GtkWidget *find_dialog = NULL;
static GtkWidget *find_entry;
static char *needle;

const gchar *help_tools_ui = 
    "<ui>"
    "  <toolbar/>"
    "</ui>";

GtkActionEntry help_tools[] = {
    { "ZoomIn", GTK_STOCK_ZOOM_IN, NULL, NULL, N_("Larger"),
      G_CALLBACK(text_zoom) },
    { "ZoomOut", GTK_STOCK_ZOOM_OUT, NULL, NULL, N_("Smaller"),
      G_CALLBACK(text_zoom) },
    { "EnHelp", GRETL_STOCK_EN, NULL, NULL, N_("Show English help"), 
      G_CALLBACK(en_help_callback) },
    { "Close", GTK_STOCK_CLOSE, NULL, NULL, N_("Close"),
      G_CALLBACK(delete_help_viewer) }
};

static gint n_help_tools = G_N_ELEMENTS(help_tools);

struct gui_help_item {
    int code;
    char *string;
};

/* codes and strings for GUI help items other than
   regular gretl commands */

static struct gui_help_item gui_help_items[] = {
    { 0,              "nothing" },
    { GR_PLOT,        "graphing" },
    { GR_XY,          "graphing" },
    { GR_DUMMY,       "factorized" },
    { GR_XYZ,         "controlled" },
    { BXPLOT,         "boxplots" },
    { GR_BOX,         "boxplots" },
    { GR_NBOX,        "boxplots" },
    { GR_3D,          "3-D" },
    { ONLINE,         "online" },
    { MARKERS,        "markers" },
    { EXPORT,         "export" },
    { SMPLBOOL,       "sampling" },
    { SMPLDUM,        "sampling" },
    { COMPACT,        "compact" },
    { EXPAND,         "expand" },
    { VSETMISS,       "missing" },
    { GSETMISS,       "missing" },
    { GUI_HELP,       "dialog" },
    { GENR_RANDOM,    "genrand" },
    { SEED_RANDOM,    "genseed" },
    { KERNEL_DENSITY, "density" },
    { HCCME,          "hccme" },
    { IRF_BOOT,       "irfboot" },
    { HTEST,          "gui-htest" },
    { HTESTNP,        "gui-htest-np" },
    { MODEL_RESTR,    "restrict-model" },
    { SYS_RESTR,      "restrict-system" },
    { VECM_RESTR,     "restrict-vecm" },
    { LAGS_DIALOG,    "lags-dialog" },
    { MINIBUF,        "minibuffer" },
    { VLAGSEL,        "VAR-lagselect" },
    { VAROMIT,        "VAR-omit" },
    { PANEL_MODE,     "panel-mode" },
    { PANEL_WLS,      "panel-wls" },
    { PANEL_B,        "panel-between" },
    { BOOTSTRAP,      "bootstrap" },
    { TRANSPOS,       "transpos" },
    { DATASORT,       "datasort" },
    { WORKDIR,        "working-dir" },
    { DFGLS,          "dfgls" },
    { GPT_ADDLINE,    "addline" }, 
    { GPT_CURVE,      "curve" }, 
    { SAVE_SESSION,   "save-session" },
    { SAVE_CMD_LOG,   "save-script" },
    { BFGS_CONFIG,    "bfgs-config" },
    { COUNTMOD,       "count-model" },
    { -1,             NULL },
};

enum {
    COMPAT_CORC = GUI_CMD_MAX + 1,
    COMPAT_FCASTERR,
    COMPAT_HILU,
    COMPAT_PWE
};

static struct gui_help_item compat_help_items[] = {
    { GUI_CMD_MAX,     "nothing" },
    { COMPAT_CORC,     "corc" },
    { COMPAT_FCASTERR, "fcasterr" },
    { COMPAT_HILU,     "hilu" },
    { COMPAT_PWE,      "pwe" },
    { -1,              NULL }
};

/* state the topic headings from the script help files so they 
   can be translated */

const char *intl_topics[] = {
    N_("Dataset"),
    N_("Estimation"),
    N_("Graphs"),
    N_("Prediction"),
    N_("Printing"),
    N_("Programming"),
    N_("Statistics"),
    N_("Tests"),
    N_("Transformations"),
    N_("Utilities")
};

/* Handle non-uniqueness of map from 'extra' command words to codes
   (e.g. both GR_PLOT and GR_XY correspond to "graphing").  We want
   the first code, to find the right place in the help file
   when responding to a "context help" request.
*/

static int gui_ci_to_index (int ci)
{
    if (ci < NC) {
	/* regular gretl command, no problem */
	return ci;
    } else {
	int i, k, ret = ci;

	for (i=1; gui_help_items[i].code > 0; i++) {
	    if (ci == gui_help_items[i].code) {
		for (k=i-1; k>0; k--) {
		    /* back up the list so long as the word above
		       is the same as the current one */
		    if (!strcmp(gui_help_items[k].string,
				gui_help_items[i].string)) {
			ret = gui_help_items[k].code;
		    } else {
			break;
		    }
		}
		return ret;
	    }
	}
    }

    return -1;
}

static int extra_command_number (const char *s)
{
    int i;

    for (i=1; gui_help_items[i].code > 0; i++) {
	if (!strcmp(s, gui_help_items[i].string)) {
	    return gui_help_items[i].code;
	}
    }

    return -1;
}

static int compat_command_number (const char *s)
{
    int i;

    for (i=1; compat_help_items[i].code > 0; i++) {
	if (!strcmp(s, compat_help_items[i].string)) {
	    return compat_help_items[i].code;
	}
    }

    return -1;
}

static void set_en_help_file (int gui)
{
    const char *helpfile;
    char *tmp, *p;

    if (gui) {
	helpfile = helpfile_path(GRETL_HELPFILE);
    } else {
	helpfile = helpfile_path(GRETL_CMD_HELPFILE);
    }

    tmp = malloc(strlen(helpfile) + 1);

    if (tmp != NULL) {
	strcpy(tmp, helpfile);
#ifdef G_OS_WIN32
	p = strrchr(tmp, '_');
	if (p != NULL) strcpy(p, ".txt");
#else
	p = strrchr(tmp, '.');
	if (p != NULL) *p = 0;
#endif
	if (gui) {
	    en_gui_helpfile = tmp;
	} else {
	    en_cli_helpfile = tmp;
	}
    }
}

void helpfile_init (void)
{
    const char *hpath = helpfile_path(GRETL_HELPFILE);
    char *p;

#ifdef G_OS_WIN32
    p = strrchr(hpath, '_');
    if (p != NULL && strncmp(p, "_hlp", 4)) { 
	translated_helpfile = 1;
    } else {
	translated_helpfile = 0;
    }
#else
    p = strrchr(hpath, '.');
    if (p != NULL && strcmp(p, ".hlp")) { 
	translated_helpfile = 1;
    } else {
	translated_helpfile = 0;
    }
#endif

    if (translated_helpfile == 1) {
	set_en_help_file(0);
	set_en_help_file(1);
    }
}

char *quoted_help_string (const char *s)
{
    const char *p, *q;

    p = strchr(s, '"');
    q = strrchr(s, '"');

    if (p != NULL && q != NULL && q - p > 1) {
	p++;
	return g_strndup(p, q - p);
    }

    return g_strdup("Missing string");
}

int command_help_index (const char *word)
{
    int h = gretl_command_number(word);

    if (h == 0) {
	h = compat_command_number(word);
    }

    return h;
}

static int gui_help_topic_index (const char *word)
{
    int h = gretl_command_number(word);

    if (h == 0) {
	h = extra_command_number(word);
	if (h <= 0) {
	    h = compat_command_number(word);
	}
    }

    return h;
}

enum {
    STRING_COL,
    POSITION_COL,
    INDEX_COL,
    NUM_COLS
};

static void help_tree_select_row (GtkTreeSelection *selection, 
				  windata_t *hwin)
{
    GtkTreeIter iter;
    GtkTreeModel *model;

    if (!gtk_tree_selection_get_selected(selection, &model, &iter)) {
	return;
    }

    if (!gtk_tree_model_iter_has_child(model, &iter)) {
	int pos, idx;

	gtk_tree_model_get(model, &iter, 
			   POSITION_COL, &pos, 
			   INDEX_COL, &idx,
			   -1);

	if (idx != hwin->active_var) {
	    /* not already in position */
	    real_do_help(idx, pos, hwin->role);
	}
    }    
}

static int get_section_iter (GtkTreeModel *model, const char *s, 
			     GtkTreeIter *iter)
{
    gchar *sect;
    int found = 0;

    if (!gtk_tree_model_get_iter_first(model, iter)) {
	return 0;
    }

    while (!found && gtk_tree_model_iter_next(model, iter)) {
	if (gtk_tree_model_iter_has_child(model, iter)) {
	    gtk_tree_model_get(model, iter, STRING_COL, &sect, -1);
	    if (!strcmp(s, sect)) {
		found = 1;
	    }
	    g_free(sect);
	}
    }

    return found;
}

static const char *real_funcs_heading (const char *s)
{
    if (!strcmp(s, "access")) {
	return _("Accessors");
    } else if (!strcmp(s, "math")) {
	return _("Mathematical");
    } else if (!strcmp(s, "numerical")) {
	return _("Numerical methods");
    } else if (!strcmp(s, "filters")) {
	return _("Filters");
    } else if (!strcmp(s, "stats")) {
	return _("Statistical");
    } else if (!strcmp(s, "probdist")) {
	return _("Probability");
    } else if (!strcmp(s, "linalg")) {
	return _("Linear algebra");
    } else if (!strcmp(s, "matbuild")) {
	return _("Matrix building");
    } else if (!strcmp(s, "matshape")) {
	return _("Matrix shaping");
    } else if (!strcmp(s, "transforms")) {
	return _("Transformations");
    } else if (!strcmp(s, "data-utils")) {
	return _("Data utilities");
    } else if (!strcmp(s, "strings")) {
	return _("Strings");
    } else {
	return "??";
    }
}

static GtkTreeStore *make_help_topics_tree (int role)
{
    const char *fname;
    GtkTreeStore *store;
    GtkTreeIter iter, parent;
    gchar *s, *buf = NULL;
    char word[16], sect[32];
    const char *heading;
    int pos = 0, idx = 0;
    int err; 

    if (role == CLI_HELP) {
	fname = helpfile_path(GRETL_CMD_HELPFILE);
    } else if (role == GUI_HELP) {
	fname = helpfile_path(GRETL_HELPFILE);
    } else if (role == FUNCS_HELP) {
	fname = funcs_helpfile();
    } else if (role == CLI_HELP_EN) {
	fname = en_cli_helpfile;
    } else if (role == GUI_HELP_EN) {
	fname = en_gui_helpfile;
    } else {
	return NULL;
    } 

    err = gretl_file_get_contents(fname, &buf);
    if (err) {
	return NULL;
    }

    store = gtk_tree_store_new(NUM_COLS, G_TYPE_STRING,
			       G_TYPE_INT, G_TYPE_INT);

    gtk_tree_store_append(store, &iter, NULL);
    gtk_tree_store_set(store, &iter, STRING_COL, _("Index"),
		       POSITION_COL, 0, -1);

    s = buf;

    while (*s) {
	if (*s == '\n' && *(s+1) == '#' && 
	    *(s+2) != '#' && *(s+2) != '\0') {
	    if (sscanf(s+2, "%15s %31s", word, sect) == 2) {
		if (role == FUNCS_HELP) {
		    heading = real_funcs_heading(sect);
		} else {
		    heading = _(sect);
		}
		if (!get_section_iter(GTK_TREE_MODEL(store), heading, &parent)) {
		    gtk_tree_store_append(store, &parent, NULL);
		    gtk_tree_store_set(store, &parent, 
				       STRING_COL, heading,
				       POSITION_COL, 0, 
				       INDEX_COL, 0,
				       -1);
		} 
		gtk_tree_store_append(store, &iter, &parent);
		if (role == FUNCS_HELP) {
		    ++idx;
		} else if (role == GUI_HELP || role == GUI_HELP_EN) {
		    idx = gui_help_topic_index(word);
		} else {
		    idx = command_help_index(word);
		} 
		gtk_tree_store_set(store, &iter, 
				   STRING_COL, word,
				   POSITION_COL, pos + 1, 
				   INDEX_COL, idx,
				   -1);
	    }
	}
	s++;
	pos++;
    }
    
    g_free(buf);

    return store;
}

static GtkTreeStore *get_help_topics_tree (int role)
{
    static GtkTreeStore *cli_tree;
    static GtkTreeStore *gui_tree;
    static GtkTreeStore *funcs_tree;
    static GtkTreeStore *en_cli_tree;
    static GtkTreeStore *en_gui_tree;
    GtkTreeStore **ptree = NULL;

    if (role == CLI_HELP) {
	ptree = &cli_tree;
    } else if (role == GUI_HELP) {
	ptree = &gui_tree;
    } else if (role == FUNCS_HELP) {
	ptree = &funcs_tree;
    } else if (role == CLI_HELP_EN) {
	ptree = &en_cli_tree;
    } else if (role == GUI_HELP_EN) {
	ptree = &en_gui_tree;
    } else {
	return NULL;
    }

    if (*ptree == NULL) {
	*ptree = make_help_topics_tree(role);
    }

    return *ptree;
}

/* add a tree-style navigation pane to the left of the help index or
   text */

int add_help_navigator (windata_t *vwin, GtkWidget *hp)
{
    GtkTreeStore *store;
    GtkWidget *view, *sw;
    GtkCellRenderer *renderer;
    GtkTreeViewColumn *column;
    GtkTreeSelection *select;

    store = get_help_topics_tree(vwin->role);
    if (store == NULL) {
	return 1;
    }

    view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(store));
    gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(view), FALSE);
    g_object_set(view, "enable-tree-lines", TRUE, NULL);

    renderer = gtk_cell_renderer_text_new();
    
    column = gtk_tree_view_column_new_with_attributes("",
						      renderer,
						      "text", 0,
						      NULL);
    gtk_tree_view_append_column(GTK_TREE_VIEW(view), column);

    select = gtk_tree_view_get_selection(GTK_TREE_VIEW(view));
    gtk_tree_selection_set_mode(select, GTK_SELECTION_SINGLE);
    g_signal_connect(G_OBJECT(select), "changed",
		     G_CALLBACK(help_tree_select_row),
		     vwin);

    sw = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw),
				   GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(sw),
					GTK_SHADOW_IN);
    gtk_container_add(GTK_CONTAINER(sw), view);
    gtk_paned_pack1(GTK_PANED(hp), sw, FALSE, TRUE);
    gtk_widget_set_size_request(sw, 150, -1);
    gtk_tree_view_columns_autosize(GTK_TREE_VIEW(view));

    g_object_set_data(G_OBJECT(vwin->text), "tview", view);

    return 0;
}

static int help_attr_from_word (const char *word, 
				int role, int col)
{
    GtkTreeModel *model;
    GtkTreeIter iter, child;
    gchar *s;
    int attr = 0;

    model = GTK_TREE_MODEL(get_help_topics_tree(role));

    if (!model || !gtk_tree_model_get_iter_first(model, &iter)) {
	return 0;
    }

    while (gtk_tree_model_iter_next(model, &iter)) {
	if (gtk_tree_model_iter_children(model, &child, &iter)) {
	    while (1) {
		gtk_tree_model_get(model, &child, STRING_COL, &s, -1);
		if (!strcmp(s, word)) {
		    gtk_tree_model_get(model, &child, col, &attr, -1);
		    g_free(s);
		    return attr;
		} 
		g_free(s);
		if (!gtk_tree_model_iter_next(model, &child)) {
		    break;
		}
	    }
	}
    }

    return 0;
}

int function_help_index_from_word (const char *word)
{
    return help_attr_from_word(word, FUNCS_HELP, INDEX_COL);
}

static int function_help_pos_from_word (const char *word)
{
    return help_attr_from_word(word, FUNCS_HELP, POSITION_COL);
}

static int help_pos_from_index (int idx, int role)
{
    GtkTreeModel *model;
    GtkTreeIter iter, child;
    int pos, tidx;

    model = GTK_TREE_MODEL(get_help_topics_tree(role));

    if (!model || !gtk_tree_model_get_iter_first(model, &iter)) {
	return 0;
    }

    while (gtk_tree_model_iter_next(model, &iter)) {
	if (gtk_tree_model_iter_children(model, &child, &iter)) {
	    while (1) {
		gtk_tree_model_get(model, &child, INDEX_COL, &tidx, -1);
		if (tidx == idx) {
		    gtk_tree_model_get(model, &child, POSITION_COL, &pos, -1);
		    return pos;
		} 
		if (!gtk_tree_model_iter_next(model, &child)) {
		    break;
		}
	    }
	}
    }

    return 0;
}

static int help_index_from_pos (int pos, int role)
{
    GtkTreeModel *model;
    GtkTreeIter iter, child;
    int idx, tpos;

    model = GTK_TREE_MODEL(get_help_topics_tree(role));

    if (!model || !gtk_tree_model_get_iter_first(model, &iter)) {
	return 0;
    }

    while (gtk_tree_model_iter_next(model, &iter)) {
	if (gtk_tree_model_iter_children(model, &child, &iter)) {
	    while (1) {
		gtk_tree_model_get(model, &child, POSITION_COL, &tpos, -1);
		if (tpos == pos) {
		    gtk_tree_model_get(model, &child, INDEX_COL, &idx, -1);
		    return idx;
		} 
		if (!gtk_tree_model_iter_next(model, &child)) {
		    break;
		}
	    }
	}
    }

    return 0;
}

static gboolean help_iter_from_index (int idx, int role,
				      GtkTreeIter *iter,
				      GtkTreeIter *parent)
{
    GtkTreeModel *model;
    int fnum;

    model = GTK_TREE_MODEL(get_help_topics_tree(role));

    if (!model || !gtk_tree_model_get_iter_first(model, parent)) {
	return 0;
    }

    while (gtk_tree_model_iter_next(model, parent)) {
	if (gtk_tree_model_iter_children(model, iter, parent)) {
	    while (1) {
		gtk_tree_model_get(model, iter, INDEX_COL, &fnum, -1);
		if (idx == fnum) {
		    return TRUE;
		} 
		if (!gtk_tree_model_iter_next(model, iter)) {
		    break;
		}
	    }
	}
    }

    return FALSE;
}

static void en_help_callback (GtkAction *action, windata_t *hwin)
{
    int pos, idx = hwin->active_var;
    int role = (hwin->role == CLI_HELP)? CLI_HELP_EN :
	GUI_HELP_EN;

    pos = help_pos_from_index(idx, role);

    if (pos < 0 && role == CLI_HELP_EN) {
	/* missed, but we can at least show the index */
	pos = 0;
    }

    real_do_help(idx, pos, role);
}

static void delete_help_viewer (GtkAction *a, windata_t *hwin) 
{
    gtk_widget_destroy(hwin->main); 
}

static void normalize_base (GtkWidget *w, gpointer p)
{
    gtk_widget_modify_base(w, GTK_STATE_SELECTED, NULL);
}

static void notify_not_found (GtkWidget *entry)
{
    GdkColor color;

    gdk_color_parse("red", &color);
    gtk_widget_grab_focus(entry);
    gtk_editable_select_region(GTK_EDITABLE(entry), 0, -1);
    gtk_widget_modify_base(entry, GTK_STATE_SELECTED, &color);
    g_signal_connect(G_OBJECT(entry), "changed",
		     G_CALLBACK(normalize_base), NULL);
}

#define help_index_ok(r) (r == CLI_HELP || \
                          r == CLI_HELP_EN || \
                          r == FUNCS_HELP)

static gboolean finder_key_handler (GtkEntry *entry, GdkEventKey *key,
				    windata_t *vwin)
{
    if (key->keyval == GDK_Tab && help_index_ok(vwin->role) &&
	vwin->active_var == 0) {
	/* tab-completion in help index mode */
	const gchar *s = gtk_entry_get_text(entry);

	if (s != NULL && *s != '\0') {
	    const char *comp = NULL;

	    if (vwin->role == CLI_HELP) {
		comp = gretl_command_complete(s);
	    } else if (vwin->role == FUNCS_HELP) {
		comp = gretl_function_complete(s);
	    }

	    if (comp != NULL) {
		gtk_entry_set_text(entry, comp);
		gtk_editable_set_position(GTK_EDITABLE(entry), -1);
	    }

	    return TRUE;
	}
    }

    return FALSE;
}

#define starts_topic(s) (s[0]=='\n' && s[1]=='#' && s[2]==' ')

/* apparatus for permitting the user to search across all the
   "pages" in a help file 
*/

static int maybe_switch_page (const char *s, windata_t *hwin)
{
    const gchar *src, *hbuf;
    int currpos, newpos = 0;
    int wrapped = 0;
    int k, n = strlen(s);
    int ok = 0;

    /* where are we in the help file right now? */
    currpos = help_pos_from_index(hwin->active_var, hwin->role);
    hbuf = (const gchar *) hwin->data;
    k = currpos;
    src = hbuf + k;

    /* skip to start of next page */
    while (*src != '\0') {
	if (starts_topic(src)) {
	    break;
	}
	k++;
	src++;
    }

 retry:

    /* see if the search text can be found on a page other than
       the current one; if so, we'll switch to it */

    while (!ok && *src != '\0') {
	if (starts_topic(src)) {
	    /* record page position */
	    newpos = k + 1;
	} else if (wrapped && newpos == currpos) {
	    /* we got back to where we started */
	    break;
	} else if (newpos != currpos && !strncmp(s, src, n)) {
	    /* found the text on a different page */
	    ok = 1;
	} 
	k++;
	src++;
    }

    if (!ok && !wrapped) {
	/* start from the top */
	src = hbuf;
	newpos = k = 0;
	wrapped = 1;
	goto retry;
    }

    if (ok) {
	/* text found: move to new position */
	int idx = help_index_from_pos(newpos, hwin->role);
	int en = (hwin->role == CLI_HELP_EN);

	set_help_topic_buffer(hwin, newpos, en);
	helpwin_set_topic_index(hwin, idx);

	if (wrapped) {
	    infobox(_("Search wrapped"));
	}
    }

    return ok;
}

/* respond to Enter key in the 'finder' entry */

static void vwin_finder_callback (GtkEntry *entry, windata_t *vwin)
{
    gboolean search_all = FALSE;
    gboolean found;

    needle = gtk_editable_get_chars(GTK_EDITABLE(entry), 0, -1);
    if (needle == NULL || *needle == '\0') {
	return;
    }

    if (g_object_get_data(G_OBJECT(entry), "search-all")) {
	search_all = TRUE;
    } 

    if (vwin->text != NULL) {
	found = real_find_in_text(GTK_TEXT_VIEW(vwin->text), needle, TRUE, search_all);
    } else {
	found = real_find_in_listbox(vwin, needle, FALSE);
    }

    if (!found) {
	if (vwin->role == CLI_HELP || vwin->role == CLI_HELP_EN) {
	    /* are we looking for a command? */
	    int idx = command_help_index(needle);

	    if (idx > 0) {
		int pos = help_pos_from_index(idx, vwin->role);

		if (pos > 0) {
		    real_do_help(idx, pos, vwin->role);
		    found = TRUE;
		}
	    }
	} else if (vwin->role == FUNCS_HELP) {
	    /* are we looking for a function? */
	    int idx = function_help_index_from_word(needle);

	    if (idx > 0) {
		int pos = help_pos_from_index(idx, vwin->role);

		if (pos > 0) {
		    real_do_help(idx, pos, vwin->role);
		    found = TRUE;
		}
	    }
	}
    }

    if (!found && search_all) {
	if (maybe_switch_page(needle, vwin)) {
	    found = real_find_in_text(GTK_TEXT_VIEW(vwin->text), needle, 
				      TRUE, TRUE);
	}
    }

    if (!found) {
	notify_not_found(GTK_WIDGET(entry));
    }
}

static void toggle_search_all_help (GtkComboBox *box, GtkWidget *entry)
{
    gint i = gtk_combo_box_get_active(box);

    if (i > 0) {
	g_object_set_data(G_OBJECT(entry), "search-all", GINT_TO_POINTER(1));
    } else {
	g_object_steal_data(G_OBJECT(entry), "search-all");
    }
}

static void finder_add_options (GtkWidget *hbox, GtkWidget *entry)
{
    GtkWidget *combo = gtk_combo_box_new_text();

    gtk_combo_box_append_text(GTK_COMBO_BOX(combo), _("this page"));
    gtk_combo_box_append_text(GTK_COMBO_BOX(combo), _("all pages"));
    gtk_box_pack_end(GTK_BOX(hbox), combo, FALSE, FALSE, 5);
    gtk_combo_box_set_active(GTK_COMBO_BOX(combo), 0);

    g_signal_connect(G_OBJECT(combo), "changed",
		     G_CALLBACK(toggle_search_all_help), 
		     entry);
}

#if (GTK_MAJOR_VERSION > 2 || GTK_MINOR_VERSION >= 16)
# define USE_ENTRY_ICON
#endif

/* add a "search box" to the right of a viewer window's toolbar */

void vwin_add_finder (windata_t *vwin)
{
#ifndef USE_ENTRY_ICON
    GtkWidget *label;
#endif
    GtkWidget *entry;
    GtkWidget *hbox;

    hbox = gtk_widget_get_parent(vwin->mbar);

    vwin->finder = entry = gtk_entry_new();

    if (vwin->role == FUNCS_HELP || vwin->role == CLI_HELP) {
	finder_add_options(hbox, entry);
    }

    gtk_entry_set_width_chars(GTK_ENTRY(entry), 16);
    gtk_box_pack_end(GTK_BOX(hbox), entry, FALSE, FALSE, 5);

#ifdef USE_ENTRY_ICON
    gtk_entry_set_icon_from_stock(GTK_ENTRY(entry), 
				  GTK_ENTRY_ICON_SECONDARY,
				  GTK_STOCK_FIND);
#else
    label = gtk_label_new(_("Find:"));
    gtk_box_pack_end(GTK_BOX(hbox), label, FALSE, FALSE, 5);
#endif  

    g_signal_connect(G_OBJECT(entry), "key-press-event",
		     G_CALLBACK(finder_key_handler), vwin);
    g_signal_connect(G_OBJECT(entry), "activate",
		     G_CALLBACK(vwin_finder_callback),
		     vwin);
}

static void add_help_tool_by_name (windata_t *hwin, 
				   const char *aname)
{
    GtkActionEntry *item = NULL;
    int i;

    for (i=0; i<n_help_tools; i++) {
	if (!strcmp(help_tools[i].name, aname)) {
	    item = &help_tools[i];
	    break;
	}
    }

    if (item != NULL) {
	guint id = gtk_ui_manager_new_merge_id(hwin->ui);
	GtkActionGroup *actions;

	actions = gtk_action_group_new("AdHoc");
	gtk_action_group_set_translation_domain(actions, "gretl");
	gtk_action_group_add_actions(actions, item, 1, hwin);
	gtk_ui_manager_add_ui(hwin->ui, id, "/toolbar", 
			      item->name, item->name,
			      GTK_UI_MANAGER_TOOLITEM, FALSE);
	gtk_ui_manager_insert_action_group(hwin->ui, actions, 0);
	g_object_unref(actions);
    }
}

#define SHOW_FINDER(r)    (r != GUI_HELP && r != GUI_HELP_EN)
#define SHOW_EN_BUTTON(r) (translated_helpfile && \
                           (r == CLI_HELP || r == GUI_HELP))

void set_up_helpview_menu (windata_t *hwin) 
{
    GtkWidget *hbox, *tbar;
    GError *err = NULL;

    hwin->ui = gtk_ui_manager_new();

    /* add skeleton toolbar ui */
    gtk_ui_manager_add_ui_from_string(hwin->ui, help_tools_ui, -1, &err);

    if (err != NULL) {
	fprintf(stderr, "building helpview ui failed: %s", err->message);
	g_error_free(err);
	return;
    }

    /* add buttons to toolbar depending on context */
    add_help_tool_by_name(hwin, "ZoomIn");
    add_help_tool_by_name(hwin, "ZoomOut");
    if (SHOW_EN_BUTTON(hwin->role)) {
	add_help_tool_by_name(hwin, "EnHelp");
    }
    add_help_tool_by_name(hwin, "Close");

    /* hbox holding toolbar */
    hbox = gtk_hbox_new(FALSE, 0);
    gtk_box_pack_start(GTK_BOX(hwin->vbox), hbox, FALSE, FALSE, 0);
    tbar = gtk_ui_manager_get_widget(hwin->ui, "/toolbar");
    gtk_box_pack_start(GTK_BOX(hbox), tbar, FALSE, FALSE, 0);

    gtk_toolbar_set_icon_size(GTK_TOOLBAR(tbar), GTK_ICON_SIZE_MENU);
    gtk_toolbar_set_style(GTK_TOOLBAR(tbar), GTK_TOOLBAR_ICONS);
    gtk_toolbar_set_show_arrow(GTK_TOOLBAR(tbar), FALSE);
    hwin->mbar = tbar;

    if (SHOW_FINDER(hwin->role)) {
	vwin_add_finder(hwin);
    }

    gtk_widget_show_all(hbox);

    gtk_window_add_accel_group(GTK_WINDOW(hwin->main), 
			       gtk_ui_manager_get_accel_group(hwin->ui));
}

static char *funcs_helpfile (void)
{
    static char fname[MAXLEN];

    if (*fname == '\0') {
	sprintf(fname, "%s%s", gretl_home(), _("genrgui.hlp"));
    }

    return fname;
}

void context_help (GtkWidget *widget, gpointer data)
{
    int pos, idx = GPOINTER_TO_INT(data);
    int role = GUI_HELP;

    idx = gui_ci_to_index(idx);

    /* try for GUI help first */
    pos = help_pos_from_index(idx, role);

    if (pos <= 0 && translated_helpfile) {
	/* English GUI help? */
	role = GUI_HELP_EN;
	pos = help_pos_from_index(idx, role);
    }

    if (pos <= 0) {
	/* CLI help? */
	role = CLI_HELP;
	pos = help_pos_from_index(idx, role);
    }

    if (pos <= 0 && translated_helpfile) {
	/* English CLI help? */
	role = CLI_HELP_EN;
	pos = help_pos_from_index(idx, role);
    }

    if (pos <= 0) {
	warnbox(_("Sorry, no help is available"));
	return;
    }

#if HDEBUG
    fprintf(stderr, "context_help: idx=%d, pos=%d, role=%d\n", 
	    idx, pos, role);
#endif

    real_do_help(idx, pos, role);
}

static gboolean nullify_hwin (GtkWidget *w, windata_t **phwin)
{
    *phwin = NULL;
    return FALSE;
}

/* sync the tree index view with the currently selected topic, if it's
   not already in sync */

static void helpwin_set_topic_index (windata_t *hwin, int idx)
{
    GtkWidget *w = 
	g_object_get_data(G_OBJECT(hwin->text), "tview");
    GtkTreeView *view = GTK_TREE_VIEW(w);

    hwin->active_var = idx;

    if (view != NULL) {
	GtkTreeModel *model = gtk_tree_view_get_model(view);
	GtkTreeIter iter, parent;
	gboolean ok;

	if (idx == 0) {
	    ok = gtk_tree_model_get_iter_first(model, &iter);
	} else {
	    ok = help_iter_from_index(idx, hwin->role, &iter, 
				      &parent);
	}

	if (ok) {
	    GtkTreeSelection *sel;

	    sel = gtk_tree_view_get_selection(view);
	    if (!gtk_tree_selection_iter_is_selected(sel, &iter)) {
		GtkTreePath *path;

		/* gtk_tree_view_collapse_all(view); should we? */
		gtk_tree_selection_select_iter(sel, &iter);
		path = gtk_tree_model_get_path(model, &iter);
		gtk_tree_view_expand_to_path(view, path);
		gtk_tree_view_set_cursor(view, path, NULL, FALSE);
		gtk_tree_path_free(path);
	    }
	}
    }
}

static void real_do_help (int idx, int pos, int role)
{
    static windata_t *gui_hwin;
    static windata_t *cli_hwin;
    static windata_t *funcs_hwin;
    static windata_t *en_gui_hwin;
    static windata_t *en_cli_hwin;
    windata_t *hwin = NULL;
    const char *fname = NULL;
    int en = 0;

    if (pos < 0) {
	dummy_call();
	return;
    }

#if HDEBUG
    fprintf(stderr, "real_do_help: idx=%d, pos=%d, role=%d\n",
	    idx, pos, role);
    fprintf(stderr, "gui_hwin = %p\n", (void *) gui_hwin);
    fprintf(stderr, "cli_hwin = %p\n", (void *) cli_hwin);
    fprintf(stderr, "funcs_hwin = %p\n", (void *) funcs_hwin);
    fprintf(stderr, "en_gui_hwin = %p\n", (void *) en_gui_hwin);
    fprintf(stderr, "en_cli_hwin = %p\n", (void *) en_cli_hwin);
#endif

    if (role == CLI_HELP) {
	hwin = cli_hwin;
	fname = helpfile_path(GRETL_CMD_HELPFILE);
    } else if (role == GUI_HELP) {
	hwin = gui_hwin;
	fname = helpfile_path(GRETL_HELPFILE);
    } else if (role == FUNCS_HELP) {
	hwin = funcs_hwin;
	fname = funcs_helpfile();
    } else if (role == CLI_HELP_EN) {
	hwin = en_cli_hwin;
	fname = en_cli_helpfile;
	en = 1;
    } else if (role == GUI_HELP_EN) {
	hwin = en_gui_hwin;
	fname = en_gui_helpfile;
	en = 1;
    }

    if (hwin != NULL) {
	gtk_window_present(GTK_WINDOW(hwin->main));
    } else {
	hwin = view_help_file(fname, role);
	if (hwin != NULL) {
	    windata_t **phwin = NULL;

	    if (role == CLI_HELP) {
		cli_hwin = hwin;
		phwin = &cli_hwin;
	    } else if (role == GUI_HELP) {
		gui_hwin = hwin;
		phwin = &gui_hwin;
	    } else if (role == FUNCS_HELP) {
		funcs_hwin = hwin;
		phwin = &funcs_hwin;
	    } else if (role == CLI_HELP_EN) {
		en_cli_hwin = hwin;
		phwin = &en_cli_hwin;
	    } else if (role == GUI_HELP_EN) {
		en_gui_hwin = hwin;
		phwin = &en_gui_hwin;
	    }

	    g_signal_connect(G_OBJECT(hwin->main), "destroy",
			     G_CALLBACK(nullify_hwin), phwin);
	}
    }

#if HDEBUG
    fprintf(stderr, "real_do_help: doing set_help_topic_buffer:\n"
	    " hwin=%p, hcode=%d, pos=%d, role=%d\n",
	    (void *) hwin, hcode, pos, role);
#endif

    if (hwin != NULL) {
	int ret = set_help_topic_buffer(hwin, pos, en);

	if (ret >= 0) {
	    helpwin_set_topic_index(hwin, idx);
	}
    }
}

/* called from main menu in gretl.c; also used as callback from
   help window topics menu
*/

void plain_text_cmdref (GtkAction *action)
{
    const gchar *aname = NULL;
    int idx = 0, pos = 0;
    int role = CLI_HELP;
 
    if (action != NULL) {
	aname = gtk_action_get_name(action);
	idx = atoi(aname);
    }

    if (idx > 0) {
	pos = help_pos_from_index(idx, role);
	if (pos < 0 && translated_helpfile == 1) {
	    /* no translated entry: fall back on English */
	    role = CLI_HELP_EN;
	    pos = help_pos_from_index(idx, role);
	}
    }

    /* note: pos = 0 gives index of commands */

    real_do_help(idx, pos, role);
} 

void command_help_callback (int idx, int en)
{
    int role = (en)? CLI_HELP_EN : CLI_HELP;
    int pos = 0;

    if (idx > 0) {
	pos = help_pos_from_index(idx, role);
	if (pos < 0 && !en && translated_helpfile == 1) {
	    /* no translated entry: fall back on English */
	    role = CLI_HELP_EN;
	    pos = help_pos_from_index(idx, role);
	}
    }

    real_do_help(idx, pos, role);
}

void function_help_callback (int idx)
{
    int pos = help_pos_from_index(idx, FUNCS_HELP);

    real_do_help(idx, pos, FUNCS_HELP); 
}

/* called from main menu in gretl.c; also used as callback from
   help window topics menu
*/

void genr_funcs_ref (GtkAction *action)
{
    int idx = atoi(gtk_action_get_name(action));
    int pos = help_pos_from_index(idx, FUNCS_HELP);

    real_do_help(idx, pos, FUNCS_HELP);    
}

/* below: must return > 0 to do anything useful */

static int help_pos_from_string (const char *s, int *idx, int *role)
{
    char word[16];
    int pos;

    *word = '\0';
    strncat(word, s, 15);

    *idx = gretl_command_number(word);
    pos = help_pos_from_index(*idx, *role);

    if (pos <= 0 && translated_helpfile) {
	pos = help_pos_from_index(*idx, CLI_HELP_EN);
	if (pos > 0) {
	    *role = CLI_HELP_EN;
	}
    }

    if (pos <= 0) {
	/* try function instead of command */
	pos = function_help_pos_from_word(word);
	if (pos > 0) {
	    *idx = function_help_index_from_word(word);
	    *role = FUNCS_HELP;
	}
    }

    return pos;
}

gint interactive_script_help (GtkWidget *widget, GdkEventButton *b,
			      windata_t *vwin)
{
    if (!window_help_is_active(vwin)) { 
	/* command help not activated */
	return FALSE;
    } else {
	gchar *text = NULL;
	int pos = -1;
	int idx = 0;
	int role = CLI_HELP;
	GtkTextBuffer *buf;
	GtkTextIter iter;

	buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(vwin->text));
	gtk_text_buffer_get_iter_at_mark(buf, &iter,
					 gtk_text_buffer_get_insert(buf));

	if (gtk_text_iter_inside_word(&iter)) {
	    GtkTextIter w_start, w_end;
	    int got_dollar = 0;

	    w_start = iter;
	    w_end = iter;

	    if (!gtk_text_iter_starts_word(&iter)) {
		gtk_text_iter_backward_word_start(&w_start);
	    }

	    if (!gtk_text_iter_ends_word(&iter)) {
		gtk_text_iter_forward_word_end(&w_end);
	    }

	    text = gtk_text_buffer_get_text(buf, &w_start, &w_end, FALSE);

	    /* dollar accessors */
	    if (text != NULL) {
		GtkTextIter dstart = w_start;
		gchar *dtest = NULL;

		if (gtk_text_iter_backward_char(&dstart)) {
		    dtest = gtk_text_buffer_get_text(buf, &dstart, 
						     &w_start, FALSE);
		    if (*dtest == '$') {
			gchar *s = g_strdup_printf("$%s", text);
			
			g_free(text);
			text = s;
			got_dollar = 1;
		    }
		    g_free(dtest);
		}
	    }

	    /* special: "coint2" command */
	    if (!got_dollar && text != NULL && !strcmp(text, "coint")) {
		if (gtk_text_iter_forward_char(&w_end)) {
		    gchar *s = gtk_text_buffer_get_text(buf, &w_start, 
							&w_end, FALSE);

		    if (s != NULL) {
			if (!strcmp(s, "coint2")) {
			    g_free(text);
			    text = s;
			} else {
			    g_free(s);
			}
		    }
		}
	    }
	} 

	if (text != NULL && *text != '\0') {
	    pos = help_pos_from_string(text, &idx, &role);
	} 

	g_free(text);
	unset_window_help_active(vwin);
	text_set_cursor(vwin->text, 0);

	if (pos <= 0) {
	    warnbox(_("Sorry, help not found"));
	} else {
	    real_do_help(idx, pos, role);
	}
    }

    return FALSE;
}

/* First response to "help <param>" in GUI console, when given with no
   option: if we got a command word or function name, pop open a
   nicely formatted help window.  If this function returns non-zero
   we'll fall back on the command-line help function.
*/

int gui_console_help (const char *param)
{
    int idx = 0, role = CLI_HELP;
    int pos, err = 0;

    pos = help_pos_from_string(param, &idx, &role);

    if (pos <= 0) {
	err = 1;
    } else {
	real_do_help(idx, pos, role);
    }

    return err;
}

void text_find (gpointer unused, gpointer data)
{
    windata_t *vwin = (windata_t *) data;

    if (vwin->finder != NULL) {
	gtk_widget_grab_focus(vwin->finder);
	gtk_editable_select_region(GTK_EDITABLE(vwin->finder), 
				   0, -1);
    } else {
	find_string_dialog(find_in_text, data);
    }
}

void listbox_find (gpointer unused, gpointer data)
{
    windata_t *vwin = (windata_t *) data;

    if (vwin->finder != NULL) {
	gtk_widget_grab_focus(vwin->finder);
	gtk_editable_select_region(GTK_EDITABLE(vwin->finder), 
				   0, -1);
    } else {
	find_string_dialog(find_in_listbox, data);
    }
}

static gint close_find_dialog (GtkWidget *widget, gpointer data)
{
    find_dialog = NULL;
    return FALSE;
}

static int string_match_pos (const char *haystack, const char *needle, 
			     int start)
{
    int hlen = strlen(haystack);
    int nlen = strlen(needle);
    int pos;

    for (pos = start; pos < hlen; pos++) {
        if (strncmp(&haystack[pos], needle, nlen) == 0) { 
             return pos;
	}
    }

    return -1;
}

/* case-insensitive search in text buffer */

static gboolean real_find_in_text (GtkTextView *view, const gchar *s, 
				   gboolean from_cursor,
				   gboolean search_all)
{
    GtkTextBuffer *buf;
    GtkTextIter iter, start, end;
    GtkTextMark *vis;
    int found = 0;
    int wrapped = 0;
    int n = strlen(s);
    gchar *got;

    buf = gtk_text_view_get_buffer(view);

 text_search_wrap:
	
    if (from_cursor) {
	GtkTextIter sel_bound;
		
	gtk_text_buffer_get_iter_at_mark(buf, &iter,
					 gtk_text_buffer_get_mark(buf,
								  "insert"));
	gtk_text_buffer_get_iter_at_mark(buf, &sel_bound,
					 gtk_text_buffer_get_mark(buf,
								  "selection_bound"));
	gtk_text_iter_order(&sel_bound, &iter);		
    } else {		
	gtk_text_buffer_get_iter_at_offset(buf, &iter, 0);
    }

    start = end = iter;

    if (!gtk_text_iter_forward_chars(&end, n)) {
	/* already at end of buffer */
	if (from_cursor && !wrapped && !search_all) {
	    from_cursor = FALSE;
	    wrapped = 1;
	    goto text_search_wrap;
	} else {
	    return 0;
	}
    }

    while (!found) {
	got = gtk_text_buffer_get_text(buf, &start, &end, FALSE);
	if (g_ascii_strcasecmp(got, s) == 0) {
	    found = 1;
	}
	g_free(got);
	if (found || !gtk_text_iter_forward_char(&start) ||
	    !gtk_text_iter_forward_char(&end)) {
	    break;
	}
    }

    if (found) {
	gtk_text_buffer_place_cursor(buf, &start);
	gtk_text_buffer_move_mark_by_name(buf, "selection_bound", &end);
	vis = gtk_text_buffer_create_mark(buf, "vis", &end, FALSE);
	gtk_text_view_scroll_to_mark(view, vis, 0.0, FALSE, 0, 0);
    } else if (from_cursor && !wrapped && !search_all) {
	/* try wrapping */
	from_cursor = FALSE;
	wrapped = 1;
	goto text_search_wrap;
    }

    if (found && wrapped) {
	infobox(_("Search wrapped"));
    }

    return found;
}

static void find_in_text (GtkWidget *button, GtkWidget *dialog)
{
    windata_t *vwin = g_object_get_data(G_OBJECT(dialog), "windat");
    gboolean found;

    needle = gtk_editable_get_chars(GTK_EDITABLE(find_entry), 0, -1);
    if (needle == NULL || *needle == '\0') {
	return;
    }

    found = real_find_in_text(GTK_TEXT_VIEW(vwin->text), 
			      needle, TRUE, FALSE);

    if (!found) {
	notify_not_found(find_entry);
    }
}

static void 
get_tree_model_haystack (GtkTreeModel *mod, GtkTreeIter *iter, int col,
			 char *haystack, int vnames)
{
    gchar *tmp;

    gtk_tree_model_get(mod, iter, col, &tmp, -1);
    if (tmp != NULL) {
	strcpy(haystack, tmp);
	if (!vnames) {
	    lower(haystack);
	}
	g_free(tmp);
    } else {
	*haystack = '\0';
    }
}

static gboolean real_find_in_listbox (windata_t *vwin, gchar *s, gboolean vnames)
{
    int minvar, wrapped = 0;
    char haystack[MAXLEN];
    char pstr[16];
    GtkTreeModel *model = NULL;
    GtkTreeIter iter;
    gboolean got_iter;
    int pos = -1;

    /* first check that there's something to search */
    if (vwin->listbox != NULL) {
	model = gtk_tree_view_get_model(GTK_TREE_VIEW(vwin->listbox));
    }

    if (model == NULL) {
	return FALSE;
    }

    /* if searching in the main gretl window, start on line 1 */
    minvar = (vwin == mdata)? 1 : 0;

    if (!vnames) {
	lower(s);
    }

    /* first try to get the current line plus one as starting point */
    sprintf(pstr, "%d", vwin->active_var);
    got_iter = gtk_tree_model_get_iter_from_string(model, &iter, pstr);
    if (got_iter) {
	got_iter = gtk_tree_model_iter_next(model, &iter);
    }

    if (!got_iter) {
	/* fallback: start from the top */
	got_iter = gtk_tree_model_get_iter_first(model, &iter);
    }

    if (!got_iter) {
	/* failed totally, get out */
	return FALSE;
    }

 search_wrap:

    while (pos < 0) {
	/* try looking in column 1 first */
	get_tree_model_haystack(model, &iter, 1, haystack, vnames);

	pos = string_match_pos(haystack, needle, 0);

	if (pos < 0 && !vnames) {
	    if (vwin == mdata) {
		/* then column 2 */
		get_tree_model_haystack(model, &iter, 2, haystack, vnames);
	    } else {
		/* then column 0 */
		get_tree_model_haystack(model, &iter, 0, haystack, vnames);
	    }
	    pos = string_match_pos(haystack, needle, 0);
	}

	if (pos >= 0 || !gtk_tree_model_iter_next(model, &iter)) {
	    break;
	}
    }

    if (pos < 0 && vwin->active_var > minvar && !wrapped) {
	/* try wrapping to start */
	gtk_tree_model_get_iter_first(model, &iter);
	if (minvar > 0 && !gtk_tree_model_iter_next(model, &iter)) {
	    ; /* do nothing: there's only one line in the box */
	} else {
	    wrapped = 1;
	    goto search_wrap;
	}
    }
    
    if (pos >= 0) {
	GtkTreePath *path = gtk_tree_model_get_path(model, &iter);

	gtk_tree_view_scroll_to_cell(GTK_TREE_VIEW(vwin->listbox),
				     path, NULL, FALSE, 0, 0);
	gtk_tree_view_set_cursor(GTK_TREE_VIEW(vwin->listbox),
				 path, NULL, FALSE);
	vwin->active_var = tree_path_get_row_number(path);
	gtk_tree_path_free(path);
	if (wrapped) {
	    infobox(_("Search wrapped"));
	}
    } 

    return (pos >= 0);
}

/* used for windows that do not have a built-in search entry,
   but which call the function find_string_dialog() */

static void find_in_listbox (GtkWidget *w, GtkWidget *dialog)
{
    windata_t *vwin = g_object_get_data(G_OBJECT(dialog), "windat");
    gpointer vp;
    gboolean vnames = FALSE;
    gboolean found;

    if (needle != NULL) {
	g_free(needle);
	needle = NULL;
    }

    needle = gtk_editable_get_chars(GTK_EDITABLE(find_entry), 0, -1);
    if (needle == NULL || *needle == '\0') {
	return;
    }

    /* are we confining the search to variable names? */
    vp = g_object_get_data(G_OBJECT(dialog), "vnames_only");
    if (vp != NULL) {
	vnames = GPOINTER_TO_INT(vp);
    }

    found = real_find_in_listbox(vwin, needle, vnames);

    if (!found) {
	notify_not_found(find_entry);
    }
}

static void cancel_find (GtkWidget *button, GtkWidget *dialog)
{
    if (find_dialog != NULL) {
	gtk_widget_destroy(dialog);
	find_dialog = NULL;
    }
}

static void parent_find (GtkWidget *finder, windata_t *caller)
{
    GtkWidget *w = caller->main;

    if (w != NULL) {
	gtk_window_set_transient_for(GTK_WINDOW(finder), GTK_WINDOW(w));
	gtk_window_set_destroy_with_parent(GTK_WINDOW(finder), TRUE);
    }
}

static void toggle_vname_search (GtkToggleButton *tb, GtkWidget *w)
{
    if (gtk_toggle_button_get_active(tb)) {
	g_object_set_data(G_OBJECT(w), "vnames_only",
			  GINT_TO_POINTER(1));
    } else {
	g_object_set_data(G_OBJECT(w), "vnames_only",
			  GINT_TO_POINTER(0));
    }
}

static void find_string_dialog (void (*findfunc)(), windata_t *vwin)
{
    GtkWidget *label;
    GtkWidget *button;
    GtkWidget *vbox;
    GtkWidget *hbox;

    if (find_dialog != NULL) {
	g_object_set_data(G_OBJECT(find_dialog), "windat", vwin);
	parent_find(find_dialog, vwin);
	gtk_window_present(GTK_WINDOW(find_dialog));
	return;
    }

    find_dialog = gretl_dialog_new(_("gretl: find"), NULL, 0);
    g_object_set_data(G_OBJECT(find_dialog), "windat", vwin);
    parent_find(find_dialog, vwin);

    g_signal_connect(G_OBJECT(find_dialog), "destroy",
		     G_CALLBACK(close_find_dialog),
		     find_dialog);

    hbox = gtk_hbox_new(FALSE, 5);
    label = gtk_label_new(_(" Find what:"));
    gtk_widget_show(label);
    find_entry = gtk_entry_new();

    if (needle != NULL) {
	gtk_entry_set_text(GTK_ENTRY(find_entry), needle);
	gtk_editable_select_region(GTK_EDITABLE(find_entry), 0, -1);
    }

    g_signal_connect(G_OBJECT(find_entry), "activate", 
		     G_CALLBACK(findfunc), find_dialog);

    gtk_widget_show(find_entry);

    gtk_box_pack_start(GTK_BOX(hbox), label, TRUE, TRUE, 5);
    gtk_box_pack_start(GTK_BOX(hbox), find_entry, TRUE, TRUE, 5);
    gtk_widget_show(hbox);

    vbox = gtk_dialog_get_content_area(GTK_DIALOG(find_dialog));

    gtk_box_pack_start(GTK_BOX(vbox), hbox, TRUE, TRUE, 5);

    if (vwin == mdata) {
	hbox = gtk_hbox_new(FALSE, 5);
	button = gtk_check_button_new_with_label(_("Variable names only (case sensitive)"));
	g_signal_connect(G_OBJECT(button), "toggled",
			 G_CALLBACK(toggle_vname_search), find_dialog);
	gtk_box_pack_start(GTK_BOX(hbox), button, FALSE, FALSE, 5);
	gtk_widget_show_all(hbox);
	gtk_box_pack_start(GTK_BOX(vbox), hbox, FALSE, FALSE, 0);
    }

    hbox = gtk_dialog_get_action_area(GTK_DIALOG(find_dialog));

    /* cancel button */
    button = cancel_button(hbox);
    g_signal_connect(G_OBJECT(button), "clicked",
		     G_CALLBACK(cancel_find), find_dialog);
    gtk_widget_show(button);

    /* find button */
    button = gtk_button_new_from_stock(GTK_STOCK_FIND);
    GTK_WIDGET_SET_FLAGS(button, GTK_CAN_DEFAULT);
    gtk_container_add(GTK_CONTAINER(hbox), button);
    g_signal_connect(G_OBJECT(button), "clicked",
		     G_CALLBACK(findfunc), find_dialog);
    gtk_widget_grab_default(button);
    gtk_widget_show(button);

    gtk_widget_grab_focus(find_entry);
    gtk_widget_show(find_dialog);
}

enum {
    EN_LETTER,
    EN_A4,
    ITALIAN,
    SPANISH
};

static int get_writable_path (char *path, const char *fname)
{
    static int sysdoc_writable = -1;
    static int userdoc_writable = -1;
    const char *gretldir = gretl_home();
    const char *dotdir = gretl_dotdir();
    FILE *fp;
    int err = 0;

    if (sysdoc_writable == 1) {
	sprintf(path, "%sdoc%c%s", gretldir, SLASH, fname);
	return 0;
    } else if (userdoc_writable == 1) {
	sprintf(path, "%sdoc%c%s", dotdir, SLASH, fname);
	return 0;
    }

    if (sysdoc_writable < 0) {
	sysdoc_writable = 0;
	sprintf(path, "%sdoc", gretldir);
	if (gretl_mkdir(path) == 0) {
	    sprintf(path, "%sdoc%c%s", gretldir, SLASH, fname);
	    fp = gretl_fopen(path, "w");
	    if (fp != NULL) {
		sysdoc_writable = 1;
		fclose(fp);
		gretl_remove(path);
	    } 
	} 
    }

    if (!sysdoc_writable && userdoc_writable < 0) {
	/* can't write to 'sys' dir, user dir not tested yet */
	userdoc_writable = 0;
	sprintf(path, "%sdoc", dotdir);
	if (gretl_mkdir(path) == 0) {
	    sprintf(path, "%sdoc%c%s", dotdir, SLASH, fname);
	    fp = gretl_fopen(path, "w");
	    if (fp != NULL) {
		userdoc_writable = 1;
		fclose(fp);
		gretl_remove(path);
	    } 
	} 
    }

    if (!sysdoc_writable && !userdoc_writable) {
	err = 1;
    }

    return err;
}

enum {
    GRETL_GUIDE = 1,
    GRETL_REF,
    GNUPLOT_REF
};

static int find_or_download_pdf (int code, int i, char *fullpath)
{
    const char *guide_files[] = {
	"gretl-guide.pdf",
	"gretl-guide-a4.pdf",
	"gretl-guide-it.pdf",
	"gretl-guide-es.pdf"
    };
    const char *ref_files[] = {
	"gretl-ref.pdf",
	"gretl-ref-a4.pdf",
	"gretl-ref-it.pdf",
	"gretl-ref-es.pdf"  
    };
    const char *fname;
    FILE *fp;
    int err = 0;

    if (i < 0 || i > 3) {
	i = 0;
    }

    if (code == GRETL_GUIDE) {
	fname = guide_files[i];
    } else if (code == GRETL_REF) {
	fname = ref_files[i];
    } else if (code == GNUPLOT_REF) {
	fname = "gnuplot.pdf";
    } else {
	return E_DATA;
    }

    /* is the file available in public dir? */
    sprintf(fullpath, "%sdoc%c%s", gretl_home(), SLASH, fname);
    fp = gretl_fopen(fullpath, "r");
    if (fp != NULL) {
	fclose(fp);
	return 0;
    }

    /* or maybe in user dir? */
    sprintf(fullpath, "%sdoc%c%s", gretl_dotdir(), SLASH, fname);
    fp = gretl_fopen(fullpath, "r");
    if (fp != NULL) {
	fclose(fp);
	return 0;
    }

    /* check for download location */
    err = get_writable_path(fullpath, fname);

    /* do actual download */
    if (!err) {
	err = retrieve_manfile(fname, fullpath);
    }

    if (err) {
	const char *buf = gretl_errmsg_get();

	if (*buf) {
	    errbox(buf);
	} else {
	    errbox(_("Failed to download file"));
	}
    }

    return err;
}

static void show_pdf (const char *fname)
{
#if defined(G_OS_WIN32)
    win32_open_file(fname);
#elif defined(OSX_BUILD)
    osx_open_file(fname);
#else
    gretl_fork("viewpdf", fname);
#endif
}

void display_pdf_help (GtkAction *action)
{
    char fname[FILENAME_MAX];
    int err, code = GRETL_GUIDE;

    if (action != NULL && strcmp(gtk_action_get_name(action), "UserGuide")) {
	/* PDF command ref wanted */
	code = GRETL_REF;
    }

    err = find_or_download_pdf(code, get_manpref(), fname);

    if (!err) {
	show_pdf(fname);
    }
}

void display_gnuplot_help (void)
{
    char fname[FILENAME_MAX];
    int err;

    err = find_or_download_pdf(GNUPLOT_REF, 0, fname);

    if (!err) {
	show_pdf(fname);
    }
}
