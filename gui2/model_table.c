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

/* model_table.c for gretl */

#include "gretl.h"
#include "model_table.h"

static const MODEL **model_list;
static int model_list_len;
static int *grand_list;

static int model_already_listed (const MODEL *pmod)
{
    int i;

    for (i=0; i<model_list_len; i++) {
	if (pmod == model_list[i]) return 1;
    }

    return 0;
}

int start_model_list (const MODEL *pmod, int add_mode)
{
    model_list = mymalloc(sizeof *model_list);
    if (model_list == NULL) return 1;

    model_list_len = 1;
    model_list[0] = pmod;

    if (add_mode == MODEL_ADD_FROM_MENU) {
	infobox(_("Model added to table")); 
    }   

    return 0;
}

void remove_from_model_list (const MODEL *pmod)
{
    int i;

    if (model_list_len == 0 || model_list == NULL) 
	return;

    for (i=0; i<model_list_len; i++) {
	if (pmod == model_list[i]) {
	    model_list[i] = NULL;
	}
    }
}

int add_to_model_list (const MODEL *pmod, int add_mode)
{
    const MODEL **tmp;

    /* check that list is really started */
    if (model_list_len == 0) {
	return start_model_list(pmod, add_mode);
    }

    /* check that the dependent variable is in common */
    if (pmod->list[1] != (model_list[0])->list[1]) {
	errbox(_("Can't add model to table -- this model has a "
		 "different dependent variable"));
	return 1;
    }

    /* check that model is not already on the list */
    if (model_already_listed(pmod)) {
	errbox(_("Model is already included in the table"));
	return 0;
    }

    model_list_len++;
    tmp = myrealloc(model_list, model_list_len * sizeof *model_list);
    if (tmp == NULL) {
	free(model_list);
	return 1;
    }

    model_list = tmp;
    model_list[model_list_len - 1] = pmod;

    if (add_mode == MODEL_ADD_FROM_MENU) {
	infobox(_("Model added to table"));
    }

    return 0;
}

void free_model_list (void)
{
    free(model_list);
    model_list = NULL;
    free(grand_list);
    grand_list = NULL;
    model_list_len = 0;
#if 0
    infobox(_("Model table cleared"));
#endif
}

static int var_is_in_model (int v, const MODEL *pmod)
{
    int i;

    for (i=2; i<=pmod->list[0]; i++) {
	if (v == pmod->list[i]) return i;
    }

    return 0;    
}

static int on_grand_list (int v)
{
    int i;

    for (i=2; i<=grand_list[0]; i++) {
	if (v == grand_list[i]) return 1;
    }

    return 0;
}

static void add_to_grand_list (const int *list)
{
    int i, j = grand_list[0] + 1;

    for (i=2; i<=list[0]; i++) {
	if (!on_grand_list(list[i])) {
	    grand_list[0] += 1;
	    grand_list[j++] = list[i];
	}
    }
}

static int make_grand_varlist (void)
{
    int i, j;
    int l0 = 0;
    const MODEL *pmod;

    free(grand_list);

    for (i=0; i<model_list_len; i++) {
	if (model_list[i] == NULL) continue;
	l0 += (model_list[i])->list[0];
    }

    grand_list = mymalloc((l0 + 1) * sizeof *grand_list);
    if (grand_list == NULL) return 1;

    for (i=0; i<model_list_len; i++) {
	pmod = model_list[i];
	if (pmod == NULL) continue;
	if (i == 0) {
	    for (j=0; j<=pmod->list[0]; j++) {
		grand_list[j] = pmod->list[j];
	    }
	} else {
	    add_to_grand_list(pmod->list);
	}
    }

    return 0;
}

static int model_list_empty (void)
{
    int i, real_n_models = 0;

    if (model_list_len == 0 || model_list == NULL) 
	return 1;

    for (i=0; i<model_list_len; i++) {
	if (model_list[i] != NULL) 
	    real_n_models++;
    }

    return (real_n_models == 0);
}

int display_model_table (void)
{
    int i, j, gl0;
    const MODEL *pmod;
    PRN *prn;
    char se[16];

    if (model_list_empty()) {
	errbox(_("The model table is empty"));
	return 1;
    }

    if (make_grand_varlist()) return 1;

    if (bufopen(&prn)) {
	free_model_list();
	return 1;
    }

    gl0 = grand_list[0];

    pprintf(prn, _("Dependent variable: %s\n\n"),
	    datainfo->varname[grand_list[1]]);

    pputs(prn, "          ");
    for (j=0; j<model_list_len; j++) {
	char modhd[16];

	if (model_list[j] == NULL) continue;
	sprintf(modhd, _("Model %d "), (model_list[j])->ID);
	pprintf(prn, "%12s", modhd);
    }
    pputs(prn, "\n\n");

    /* print coefficients, standard errors */
    for (i=2; i<=gl0; i++) {
	int k, v = grand_list[i];

	pprintf(prn, "%8s ", datainfo->varname[v]);
	for (j=0; j<model_list_len; j++) {
	    pmod = model_list[j];
	    if (pmod == NULL) continue;
	    if ((k = var_is_in_model(v, pmod))) {
		pprintf(prn, "%#12.5g", pmod->coeff[k-1]);
	    } else {
		pputs(prn, "            ");
	    }
	}
	pputs(prn, "\n          ");
	for (j=0; j<model_list_len; j++) {
	    pmod = model_list[j];
	    if (pmod == NULL) continue;
	    if ((k = var_is_in_model(v, pmod))) {
		sprintf(se, "(%#.5g)", pmod->sderr[k-1]);
		pprintf(prn, "%12s", se);
	    } else {
		pputs(prn, "            ");
	    }
	}
	pputs(prn, "\n\n");
    }

    /* print sample sizes, R-bar-squared */
    pprintf(prn, "%8s ", _("n"));
    for (j=0; j<model_list_len; j++) {
	pmod = model_list[j];
	if (pmod == NULL) continue;
	pprintf(prn, "%12d", pmod->nobs);
    }
    pputs(prn, "\n");
    pprintf(prn, "%s", _("Adj. R**2"));
    for (j=0; j<model_list_len; j++) {
	pmod = model_list[j];
	if (pmod == NULL) continue;
	pprintf(prn, "%#12.4g", pmod->adjrsq);
    }
    pputs(prn, "\n");

    view_buffer(prn, 78, 350, _("gretl: model table"), PRINT, NULL);

    return 0;
}
