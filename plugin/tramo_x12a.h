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

enum tx_objects {
    D11,      /* seasonally adjusted series */
    D12,      /* trend/cycle */
    D13,      /* irregular component */
    TRIGRAPH, /* graph showing all of the above */
    XAXIS     /* x-axis (time) variable for graphing */
};

typedef struct _common_opt_info common_opt_info;
typedef struct _tx_request tx_request;

struct _common_opt_info {
    GtkWidget *check;
    char save;
    unsigned short v;
};

struct _tx_request {
    int code;          /* tramo vs x12arima */
    GtkWidget *dialog;
    common_opt_info opt[4];
    void *opts;
    int savevars;
#if GTK_MAJOR_VERSION == 1
    int ret;
#endif
};

int show_tramo_options (tx_request *request, GtkWidget *vbox);

void print_tramo_options (tx_request *request, FILE *fp);
