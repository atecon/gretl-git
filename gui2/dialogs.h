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

#ifndef DIALOGS_H
#define DIALOGS_H

enum {
    GRETL_YES,
    GRETL_NO,
    GRETL_CANCEL,
    HELP_BUTTON
} buttons;

typedef struct dialog_t_ dialog_t;

/* functions follow */

void errbox (const char *msg);

void infobox (const char *msg);

gint yes_no_dialog (char *title, char *msg, int cancel);

int make_default_storelist (void);

gint exit_check (GtkWidget *widget, GdkEvent *event, gpointer data);

void menu_exit_check (GtkWidget *w, gpointer data);

void delimiter_dialog (void);

void copy_format_dialog (windata_t *vwin, int multicopy);

void varinfo_dialog (int varnum, int full);

int select_var_from_list (const int *list, const char *query);

void sample_range_dialog (gpointer p, guint u, GtkWidget *w);

void arma_options_dialog (gpointer p, guint u, GtkWidget *w);

void panel_structure_dialog (DATAINFO *pdinfo);

void data_compact_dialog (GtkWidget *w, int spd, int *target_pd, 
			  int *mon_start, gint *compact_method);

int density_dialog (int vnum, double *bw);

int radio_dialog (const char *title, const char **opts, 
		  int nopts, int deflt, int helpcode);

int checks_dialog (const char *title, const char **opts, 
		   int nopts, int *active, int *spinval,
		   const char *spintext, int spinmax,
		   int helpcode);

void compute_default_ts_info (DATAINFO *dwinfo, int newdata);

void data_structure_wizard (gpointer p, guint u, GtkWidget *w);

void panel_restructure_dialog (gpointer data, guint u, GtkWidget *w);

#endif /* DIALOGS_H */
