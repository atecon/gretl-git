/*
 *  Copyright (c) by Ramu Ramanathan and Allin Cottrell
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

/*
    compare.c - gretl model comparison procedures
*/

#include "libgretl.h"
#include "internal.h"

#ifdef OS_WIN32
# include <windows.h>
#endif

static int _justreplaced (const int i, const DATAINFO *pdinfo, 
			  const int *list);

/* ........................................................... */

int _addtolist (const int *oldlist, const int *addvars, int **pnewlist,
		const DATAINFO *pdinfo, const int model_count)
/* Adds specified independent variables to a specified
   list, forming newlist.  The first element of addvars
   is the number of variables to be added; the remaining
   elements are the ID numbers of the variables to be added.
*/
{
    int i, j, k, nadd = addvars[0], match; 

    *pnewlist = malloc((oldlist[0] + nadd + 1) * sizeof(int));
    if (*pnewlist == NULL) return E_ALLOC;

    for (i=0; i<=oldlist[0]; i++) (*pnewlist)[i] = oldlist[i];
    k = oldlist[0];

    for (i=1; i<=addvars[0]; i++) {
	match = 0;
	for (j=1; j<=oldlist[0]; j++) {
	    if (addvars[i] == oldlist[j]) {
		/* a "new" var was already present */
		free(*pnewlist);
		return E_ADDDUP;
	    }
	}
	if (!match) {
	    (*pnewlist)[0] += 1;
	    k++;
	    (*pnewlist)[k] = addvars[i];
	}
    }

    if ((*pnewlist)[0] == oldlist[0]) 
	return E_NOADD;
    if (_justreplaced(model_count, pdinfo, oldlist)) 
	return E_VARCHANGE;
    return 0;
}

/* ........................................................... */

int _omitfromlist (int *list, const int *omitvars, int newlist[],
		   const DATAINFO *pdinfo, const int model_count)
/* Drops specified independent variables from a specified
   list, forming newlist.  The first element of omitvars
   is the number of variables to be omitted; the remaining
   elements are the ID numbers of the variables to be dropped.
*/
{
    int i, j, k, nomit = omitvars[0], l0 = list[0], match; 

    /* attempting to omit all vars or more ? */
    if (nomit >= l0-1) return E_NOVARS;

    newlist[0] = 1;
    newlist[1] = list[1];
    k = 1;
    for (i=2; i<=l0; i++) {
        match = 0;
        for (j=1; j<=nomit; j++) {
            if (list[i] == omitvars[j]) {
                match = 1; /* matching var: omit it */
		break;
            }
        }
        if (!match) { /* var is not in omit list: keep it */
	    k++;
            newlist[k] = list[i];
        }
    }
    newlist[0] = k;
    if (newlist[0] == list[0]) /* no vars were omitted */
	return E_NOOMIT; 
    if (_justreplaced(model_count, pdinfo, newlist)) 
	return E_VARCHANGE; /* values of one or more vars to be
				omitted have changed */
    return 0;
}

/* ........................................................... */

static void _difflist (int *biglist, int *smalist, int *targ)
{
    int i, j, k, match;

    targ[0] = biglist[0] - smalist[0];
    k = 1;
    for (i=2; i<=biglist[0]; i++) {
	match = 0;
	for (j=2; j<=smalist[0]; j++) {
	    if (smalist[j] == biglist[i]) {
		match = 1;
		break;
	    }
	}
	if (!match) {
	    targ[k] = biglist[i];
	    k++;
	}
    }
}

/* ........................................................... */

static int _justreplaced (const int i, const DATAINFO *pdinfo, 
			  const int *list)
     /* check if any var in list has been replaced via genr since a
	previous model (model_count i) was estimated.  Expects
	the "label" in datainfo to be of the form "Replaced
	after model <count>" */
{
    int j, repl = 0;

    for (j=1; j<=list[0]; j++) {
	if (strncmp(pdinfo->label[list[j]], _("Replaced"), 8) == 0 &&
	    sscanf(pdinfo->label[list[j]], "%*s %*s %*s %d", &repl) == 1)
	if (repl >= i) return 1;
    }
    return 0; 
}

/* ........................................................... */

static COMPARE add_compare (const MODEL *pmodA, const MODEL *pmodB) 
/* Generate comparison statistics between an initial model, A,
   and a new model, B, arrived at by adding variables to A. */
{
    COMPARE add;
    int i;	

    add.m1 = pmodA->ID;
    add.m2 = pmodB->ID;
    add.F = 0.0;
    add.ols = add.discrete = 0;

    if (pmodA->ci == OLS) add.ols = 1;
    if (pmodA->ci == LOGIT || pmodA->ci == PROBIT)
	add.discrete = 1;
    add.score = 0;
    add.dfn = pmodB->ncoeff - pmodA->ncoeff;
    add.dfd = pmodB->dfd;
    if (add.ols && pmodB->aux == AUX_ADD) {
	add.F = ((pmodA->ess - pmodB->ess)/pmodB->ess)
	    * add.dfd/add.dfn;
    }
    else if (add.discrete) {
	add.chisq = 2.0 * (pmodB->lnL - pmodA->lnL);
	return add;
    }
    for(i=0; i<8; i++) 
	if (pmodB->criterion[i] < pmodA->criterion[i]) 
	    add.score++;
    return add;
}	    

/* ........................................................... */

static COMPARE omit_compare (const MODEL *pmodA, const MODEL *pmodB)
/* Generate comparison statistics between a general model, A,
   and a restricted model B, arrived at via one or more
   zero restrictions on the parameters of A.
*/
{
    COMPARE omit;
    int i;	

    omit.m1 = pmodA->ID;
    omit.m2 = pmodB->ID;
    omit.ols = omit.discrete = 0;

    if (pmodA->ci == OLS) omit.ols = 1;
    if (pmodA->ci == LOGIT || pmodA->ci == PROBIT)
	omit.discrete = 1;
    omit.score = 0;
    if (omit.ols || omit.discrete) {
	omit.dfn = pmodA->dfn - pmodB->dfn;
	omit.dfd = pmodA->dfd;
	if (pmodA->ifc && !pmodB->ifc) omit.dfn += 1;
	if (omit.ols) {
	    omit.F = ((pmodB->ess - pmodA->ess)/pmodA->ess)
		* omit.dfd/omit.dfn;
	} else {
	    omit.chisq = 2.0 * (pmodA->lnL - pmodB->lnL);
	    return omit;
	}
    }
    for (i=0; i<8; i++) 
	if (pmodB->criterion[i] < pmodA->criterion[i]) omit.score++;
    return omit;
}	    

/**
 * auxreg:
 * @addvars: list of variables to add to original model (or NULL)
 * @orig: pointer to original model.
 * @new: pointer to new (modified) model.
 * @model_count: count of models estimated so far.
 * @pZ: pointer to data matrix.
 * @pdinfo: information on the data set.
 * @aux_code: code indicating what sort of aux regression to run.
 * @prn: gretl printing struct.
 * @test: hypothesis test results struct.
 *
 * Run an auxiliary regression, in order to test a given set of added
 * variables, or to test for non-linearity (squares, logs).
 * 
 * Returns: 0 on successful completion, error code on error.
 */

int auxreg (LIST addvars, MODEL *orig, MODEL *new, int *model_count, 
	    double ***pZ, DATAINFO *pdinfo, const int aux_code, 
	    PRN *prn, GRETLTEST *test)
{
    COMPARE add;             
    int *newlist, *tmplist = NULL;
    MODEL aux;
    int i, t, n = pdinfo->n, orig_nvar = pdinfo->v; 
    int m = *model_count, pos = 0, listlen; 
    double trsq = 0.0, rho = 0.0; 
    int newvars = 0, err = 0;

    if (orig->ci == TSLS) return E_NOTIMP;

    /* temporarily impose the sample that was in force when the
       original model was estimated */
    exchange_smpl(orig, pdinfo);

    _init_model(&aux, pdinfo);

    /* was a specific list of vars to add passed in, or should we
       concoct one? (e.g. "lmtest") */
    if (addvars != NULL) {
	err = _addtolist(orig->list, addvars, &newlist, pdinfo, m);
    } else {
	/* does the original list contain a constant? */
	if (orig->ifc) listlen = orig->list[0] - 1;
	else listlen = orig->list[0];
	tmplist = malloc(listlen * sizeof *tmplist);
	if (tmplist == NULL) {
	    err = E_ALLOC;
	} else {
	    tmplist[0] = listlen - 1;
	    for (i=1; i<=tmplist[0]; i++) tmplist[i] = orig->list[i+1];
	    /* no cross-products yet */
	    if (aux_code == AUX_SQ) { /* add squares of original variables */
		newvars = xpxgenr(tmplist, pZ, pdinfo, 0, 0);
		if (newvars < 0) {
		    fprintf(stderr, "gretl: generation of squares failed\n");
		    free(tmplist);
		    err = E_SQUARES;
		}
	    }
	    else if (aux_code == AUX_LOG) { /* add logs of orig vars */
		newvars = logs(tmplist, pZ, pdinfo);
		if (newvars < 0) {
		    fprintf(stderr, "gretl: generation of logs failed\n");
		    free(tmplist);
		    err = E_LOGS;
		}
	    }	    
	    /* now construct an "addvars" list including all
	       the vars that were just generated.  Use tmplist again. */
	    if (!err) {
		tmplist = realloc(tmplist, (newvars + 2) * sizeof *tmplist);
		if (tmplist == NULL) {
		    err = E_ALLOC;
		} else {
		    tmplist[0] = pdinfo->v - orig_nvar;
		    for (i=1; i<=tmplist[0]; i++) 
			tmplist[i] = i + orig_nvar - 1;
		    err = _addtolist(orig->list, tmplist, &newlist,
				     pdinfo, m);
		}
	    }
	} /* tmplist != NULL */
    }

    /* ADD: run an augmented regression, matching the original
       estimation method */
    if (!err && aux_code == AUX_ADD) {
	if (orig->ci == CORC || orig->ci == HILU) {
	    err = hilu_corc(&rho, newlist, pZ, pdinfo, 
			    orig->ci, prn);
	}
	else if (orig->ci == WLS || orig->ci == AR) {
	    pos = _full_model_list(orig, &newlist);
	    if (pos < 0) err = E_ALLOC;
	}

	if (!err) {
	    /* select sort of model to estimate */
	    if (orig->ci == AR) {
		*new = ar_func(newlist, pos, pZ, pdinfo, model_count, prn);
		*model_count -= 1;
	    }
	    else if (orig->ci == ARCH) {
		*new = arch(orig->archp, newlist, pZ, pdinfo, model_count, 
			    prn, NULL);
		*model_count -= 1;
	    } 
	    else if (orig->ci == LOGIT || orig->ci == PROBIT) {
		*new = logit_probit(newlist, pZ, pdinfo, orig->ci);
	    }
	    else *new = lsq(newlist, pZ, pdinfo, orig->ci, 1, rho);

	    if (new->nobs < orig->nobs) 
		new->errcode = E_MISS;
	    if (new->errcode) {
		err = new->errcode;
		free(newlist);
		if (addvars == NULL) free(tmplist); 
		clear_model(new, NULL, NULL, pdinfo);
	    } else {
		++m;
		new->ID = m;
	    }
	}
    } /* end if AUX_ADD */

    /* non-linearity test? Run auxiliary regression here -- 
       Replace depvar with uhat from orig */
    else if (!err && (aux_code == AUX_SQ || aux_code == AUX_LOG)) {
	int df = 0;

	/* grow data set to accommodate new dependent var */
	if (dataset_add_vars(1, pZ, pdinfo)) {
	    err = E_ALLOC;
	} else {
	    for (t=0; t<n; t++)
		(*pZ)[pdinfo->v - 1][t] = NADBL;
	    for (t=orig->t1; t<=orig->t2; t++)
		(*pZ)[pdinfo->v - 1][t] = orig->uhat[t];
	    newlist[1] = pdinfo->v - 1;
	    pdinfo->extra = 1;

	    aux = lsq(newlist, pZ, pdinfo, OLS, 1, rho);
	    if (aux.errcode) {
		err = aux.errcode;
		fprintf(stderr, "auxiliary regression failed\n");
		free(newlist);
		if (addvars == NULL) free(tmplist); 
	    } else {
		aux.aux = aux_code;
		printmodel(&aux, pdinfo, prn);
		trsq = aux.rsq * aux.nobs;

		if (test) {
		    df = newlist[0] - orig->list[0];
		    sprintf(test->type, _("Non-linearity test (%s)"),
			    (aux_code == AUX_SQ)? _("squares") : _("logs"));
		    strcpy(test->h_0, _("relationship is linear"));
		    sprintf(test->teststat, "TR^2 = %f", trsq);
		    sprintf(test->pvalue, _("prob(Chi-square(%d) > %f) = %f"), 
			    df, trsq, chisq(trsq, df));
		}
	    } /* ! aux.errcode */
	    clear_model(&aux, NULL, NULL, pdinfo);
	    /* shrink for uhat */
	    dataset_drop_vars(1, pZ, pdinfo);
	    pdinfo->extra = 0;
	}
    }

    if (!err) {
	if (aux_code == AUX_ADD) new->aux = aux_code;
	add = add_compare(orig, new);
	add.trsq = trsq;

	if (aux_code == AUX_ADD && new->ci != AR && new->ci != ARCH)
	    printmodel(new, pdinfo, prn);

	if (addvars != NULL) {
	    _difflist(new->list, orig->list, addvars);
	    _print_add(&add, addvars, pdinfo, aux_code, prn);
	} else {
	    add.dfn = newlist[0] - orig->list[0];
	    _print_add(&add, tmplist, pdinfo, aux_code, prn);
	}

	*model_count += 1;
	free(newlist);
	if (addvars == NULL) free(tmplist); 

	/* trash any extra variables generated (squares, logs) */
	if (pdinfo->v > orig_nvar)
	    dataset_drop_vars(pdinfo->v - orig_nvar, pZ, pdinfo);
    }

    /* put back into pdinfo what was there on input */
    exchange_smpl(orig, pdinfo);
    return err;
}

/**
 * omit_test:
 * @omitvars: list of variables to omit from original model.
 * @orig: pointer to original model.
 * @new: pointer to new (modified) model.
 * @model_count: count of models estimated so far.
 * @pZ: pointer to data matrix.
 * @pdinfo: information on the data set.
 * @prn: gretl printing struct.
 *
 * Re-estimate a given model after removing a list of 
 * specified variables.
 * 
 * Returns: 0 on successful completion, error code on error.
 */

int omit_test (LIST omitvars, MODEL *orig, MODEL *new, 
	       int *model_count, double ***pZ, DATAINFO *pdinfo, 
	       PRN *prn)
{
    COMPARE omit;             /* Comparison struct for two models */
    int *tmplist, m = *model_count, pos = 0;
    int maxlag = 0, t1 = pdinfo->t1;
    double rho = 0.0;
    int err = 0;

    if (orig->ci == TSLS) return E_NOTIMP;

    /* temporarily impose the sample that was in force when the
       original model was estimated */
    exchange_smpl(orig, pdinfo);

    if (orig->ci == AR) 
	maxlag = orig->arlist[orig->arlist[0]];
    else if (orig->ci == ARCH) 
	maxlag = orig->archp;
    pdinfo->t1 = orig->t1 - maxlag;

    tmplist = malloc((orig->ncoeff + 2) * sizeof(int));
    if (tmplist == NULL) { 
	pdinfo->t1 = t1;
	err = E_ALLOC; 
    } else {
	err = _omitfromlist(orig->list, omitvars, tmplist, pdinfo, m);
	if (err) {
	    free(tmplist);
	}
    }

    if (!err) {
	if (orig->ci == CORC || orig->ci == HILU) {
	    err = hilu_corc(&rho, tmplist, pZ, pdinfo, 
			    orig->ci, prn);
	    if (err) {
		free(tmplist);
	    }
	}
	else if (orig->ci == WLS || orig->ci == AR) {
	    pos = _full_model_list(orig, &tmplist);
	    if (pos < 0) {
		free(tmplist);
		err = E_ALLOC;
	    }
	}
    }

    if (!err) {
	if (orig->ci == AR) {
	    *new = ar_func(tmplist, pos, pZ, pdinfo, model_count, prn);
	    *model_count -= 1;
	}
	else if (orig->ci == ARCH) {
	    *new = arch(orig->archp, tmplist, pZ, pdinfo, model_count, 
			prn, NULL);
	    *model_count -= 1;
	} 
	else if (orig->ci == LOGIT || orig->ci == PROBIT) {
	    *new = logit_probit(tmplist, pZ, pdinfo, orig->ci);
	    new->aux = AUX_OMIT;
	}
	else 
	    *new = lsq(tmplist, pZ, pdinfo, orig->ci, 1, rho);

	if (new->errcode) {
	    pprintf(prn, "%s\n", gretl_errmsg);
	    free(tmplist);
	    err = new->errcode; 
	}
    }

    if (!err) {
	++m;
	new->ID = m;
	omit = omit_compare(orig, new);
	if (orig->ci != AR && orig->ci != ARCH) 
	    printmodel(new, pdinfo, prn); 
	_difflist(orig->list, new->list, omitvars);
	_print_omit(&omit, omitvars, pdinfo, prn);     

	*model_count += 1;
	free(tmplist);
	if (orig->ci == LOGIT || orig->ci == PROBIT)
	    new->aux = NONE;
    }

    pdinfo->t1 = t1;

    /* put back into pdinfo what was there on input */
    exchange_smpl(orig, pdinfo);

    return err;
}

/**
 * autocorr_test:
 * @pmod: pointer to model to be tested.
 * @pZ: pointer to data matrix.
 * @pdinfo: information on the data set.
 * @prn: gretl printing struct.
 * @test: hypothesis test results struct.
 *
 * Tests the given model for autocorrelation of order equal to
 * the frequency of the data. Gives TR^2 and LMF test statistics.
 * 
 * Returns: 0 on successful completion, error code on error.
 */

int autocorr_test (MODEL *pmod, double ***pZ, DATAINFO *pdinfo, 
		   PRN *prn, GRETLTEST *test)
{
    int *newlist;
    MODEL aux;
    int i, k, t, n = pdinfo->n, v = pdinfo->v; 
    double trsq, LMF;
    int err = 0;

    exchange_smpl(pmod, pdinfo);
    _init_model(&aux, pdinfo);

    k = pdinfo->pd + 1;
    newlist = malloc((pmod->list[0] + k) * sizeof *newlist);
    if (newlist == NULL) {
	err = E_ALLOC;
    } else {
	newlist[0] = pmod->list[0] + pdinfo->pd;
	for (i=2; i<=pmod->list[0]; i++) newlist[i] = pmod->list[i];

	if (dataset_add_vars(1, pZ, pdinfo)) {
	    k = 0;
	    err = E_ALLOC;
	}
    }

    if (!err) {
	/* add uhat to data set */
	for (t=0; t<n; t++)
	    (*pZ)[v][t] = NADBL;
	for (t = pmod->t1; t<= pmod->t2; t++)
	    (*pZ)[v][t] = pmod->uhat[t];
	strcpy(pdinfo->varname[v], "uhat");
	strcpy(pdinfo->label[v], _("residual"));
	/* then lags of same */
	for (i=1; i<=pdinfo->pd; i++) {
	    if (_laggenr(v, i, 1, pZ, pdinfo)) {
		sprintf(gretl_errmsg, _("lagging uhat failed"));
		err = E_LAGS;
	    } else 
		newlist[pmod->list[0] + i] = v+i;
	}
    }

    if (!err) {
	newlist[1] = v;
	/*  printlist(newlist); */
	aux = lsq(newlist, pZ, pdinfo, OLS, 1, 0.0);
	err = aux.errcode;
	if (err) {
	    errmsg(aux.errcode, prn);
	}
    } 

    if (!err) {
	aux.aux = AUX_AR;
	printmodel(&aux, pdinfo, prn);
	trsq = aux.rsq * aux.nobs;
	LMF = (aux.rsq/(1.0 - aux.rsq)) * 
	    (aux.nobs - pmod->ncoeff - pdinfo->pd)/pdinfo->pd; 

	pprintf(prn, _("\nTest statistic: LMF = %f,\n"), LMF);
	pprintf(prn, _("with p-value = prob(F(%d,%d) > %f) = %f\n"), 
		pdinfo->pd, aux.nobs - pmod->ncoeff - pdinfo->pd, LMF,
		fdist(LMF, pdinfo->pd, aux.nobs - pmod->ncoeff - pdinfo->pd));

	pprintf(prn, _("\nAlternative statistic: TR^2 = %f,\n"), trsq);
	pprintf(prn, _("with p-value = prob(Chi-square(%d) > %f) = %f\n\n"), 
		pdinfo->pd, trsq, chisq(trsq, pdinfo->pd));

	if (test != NULL) {
	    strcpy(test->type, _("LM test for autocorrelation"));
	    sprintf(test->h_0, _("no autocorrelation up to order %d"), pdinfo->pd);
	    /* sprintf(test->teststat, "TR^2 = %f", trsq); */
	    /* sprintf(test->pvalue, "prob(Chi-square(%d) > %f) = %f", 
	       pdinfo->pd, trsq, chisq(trsq, pdinfo->pd)); */
	    sprintf(test->teststat, "LMF = %f", trsq);
	    sprintf(test->pvalue, _("prob(F(%d,%d) > %f) = %f"), pdinfo->pd, 
		    aux.nobs - pmod->ncoeff - pdinfo->pd, LMF,
		    fdist(LMF, pdinfo->pd, 
			  aux.nobs - pmod->ncoeff - pdinfo->pd));	
	}
    }

    free(newlist);
    dataset_drop_vars(k, pZ, pdinfo); 
    clear_model(&aux, NULL, NULL, pdinfo); 
    exchange_smpl(pmod, pdinfo);

    return err;
}

/**
 * chow_test:
 * @line: command line for parsing.
 * @pmod: pointer to model to be tested.
 * @pZ: pointer to data matrix.
 * @pdinfo: information on the data set.
 * @prn: gretl printing struct.
 * @test: hypothesis test results struct.
 *
 * Tests the given model for structural stability (Chow test).
 * 
 * Returns: 0 on successful completion, error code on error.
 */

int chow_test (const char *line, MODEL *pmod, double ***pZ,
	       DATAINFO *pdinfo, PRN *prn, GRETLTEST *test)
{
    int *chowlist = NULL;
    int newvars = pmod->list[0] - 1;
    int i, t, v = pdinfo->v, n = pdinfo->n;
    char chowdate[9], s[9];
    MODEL chow_mod;
    double F;
    int split = 0, err = 0;

    if (pmod->ci != OLS) return E_OLSONLY;

    /* temporarily impose the sample that was in force when the
       original model was estimated */
    exchange_smpl(pmod, pdinfo);

    _init_model(&chow_mod, pdinfo);

    if (sscanf(line, "%*s %8s", chowdate) != 1) 
	err = E_PARSE;
    else {
	split = dateton(chowdate, pdinfo) - 1;
	if (split <= 0 || split >= pdinfo->n) 
	    err = E_SPLIT;
    }

    if (!err) {
	/* take the original regression list, add a split dummy
	   and interaction terms. */
	if (pmod->ifc == 0) newvars += 1;

	if (dataset_add_vars(newvars, pZ, pdinfo)) {
	    newvars = 0;
	    err = E_ALLOC;
	} else {
	    chowlist = malloc((pmod->list[0] + newvars + 1) * sizeof *chowlist);
	    if (chowlist == NULL) 
		err = E_ALLOC;
	}
    }

    if (!err) {
	chowlist[0] = pmod->list[0] + newvars;
	for (i=1; i<=pmod->list[0]; i++) 
	    chowlist[i] = pmod->list[i];

	/* generate the split variable */
	for (t=0; t<n; t++) 
	    (*pZ)[v][t] = (double) (t > split); 
	strcpy(pdinfo->varname[v], "splitdum");
	strcpy(pdinfo->label[v], _("dummy variable for Chow test"));
	chowlist[pmod->list[0] + 1] = v;

	/* and the interaction terms */
	for (i=1; i<newvars; i++) {
	    for (t=0; t<n; t++)
		(*pZ)[v+i][t] = 
		    (*pZ)[v][t] * (*pZ)[pmod->list[1+i]][t];
	    strcpy(s, pdinfo->varname[pmod->list[1+i]]); 
	    _esl_trunc(s, 5);
	    strcpy(pdinfo->varname[v+i], "sd_");
	    strcat(pdinfo->varname[v+i], s);
	    sprintf(pdinfo->label[v+i], "splitdum * %s", 
		    pdinfo->varname[pmod->list[1+i]]);
	    chowlist[pmod->list[0]+1+i] = v+i;
	}

	chow_mod = lsq(chowlist, pZ, pdinfo, OLS, 1, 0.0);
	if (chow_mod.errcode) {
	    err = chow_mod.errcode;
	    errmsg(err, prn);
	} else {
	    chow_mod.aux = AUX_CHOW;
	    printmodel(&chow_mod, pdinfo, prn);
	    F = (pmod->ess - chow_mod.ess) * chow_mod.dfd / 
		(chow_mod.ess * newvars);
	    pprintf(prn, _("\nChow test for structural break at observation %s:\n"
		    "  F(%d, %d) = %f with p-value %f\n\n"), chowdate,
		    newvars, chow_mod.dfd, F, 
		    fdist(F, newvars, chow_mod.dfd)); 

	    if (test != NULL) {
		sprintf(test->type, _("Chow test for structural break at "
			"observation %s"), chowdate);
		strcpy(test->h_0, _("no structural break"));
		sprintf(test->teststat, "F(%d, %d) = %f", 
			newvars, chow_mod.dfd, F);
		sprintf(test->pvalue, "%f", fdist(F, newvars, chow_mod.dfd));
	    }
	}
	clear_model(&chow_mod, NULL, NULL, pdinfo);
    }

    /* clean up extra variables */
    dataset_drop_vars(newvars, pZ, pdinfo);
    free(chowlist);

    exchange_smpl(pmod, pdinfo);    

    return err;
}

/* ........................................................... */

static double vprime_M_v (double *v, double *M, int n)
     /* compute v'Mv, for symmetric M */
{
    int i, j, jmin, jmax, k = 1;
    double xx, val = 0.0;

    jmin = 0;
    for (i=0; i<n; i++) {
	xx = 0.0;
	for (j=jmin; j<n; j++) 
	    xx += v[j] * M[k++];
	jmin++;
	val += xx * v[i];
    }
    jmax = 1;
    for (i=1; i<n; i++) {
	k = i + 1;
	xx = 0.0;
	for (j=0; j<jmax; j++) {
	    xx += v[j] * M[k];
	    k += n - j - 1;
	}
	jmax++;
	val += xx * v[i];
    }
    return val;
}

/**
 * cusum_test:
 * @pmod: pointer to model to be tested.
 * @pZ: pointer to data matrix.
 * @pdinfo: information on the data set.
 * @prn: gretl printing struct.
 * @ppaths: path information struct.
 * @test: hypothesis test results struct.
 *
 * Tests the given model for parameter stability (CUSUM test).
 * 
 * Returns: 0 on successful completion, error code on error.
 */

int cusum_test (MODEL *pmod, double ***pZ, DATAINFO *pdinfo, PRN *prn, 
		PATHS *ppaths, GRETLTEST *test)
{
    int n_est, i, j, t;
    int t1 = pdinfo->t1, t2 = pdinfo->t2;
    int xno, yno = pmod->list[1];
    int T = pmod->t2 - pmod->t1 + 1, K = pmod->ncoeff;
    MODEL cum_mod;
    char cumdate[9];
    double wbar, xx, yy, sigma, hct;
    double *cresid = NULL, *W = NULL, *xvec = NULL;
    FILE *fq = NULL;
    int err = 0;

    if (pmod->ci != OLS) return E_OLSONLY;

    n_est = T - K;
    /* set sample based on model to be tested */
    pdinfo->t1 = pmod->t1;
    pdinfo->t2 = pmod->t1 + K - 1;    

    cresid = malloc(n_est * sizeof *cresid);
    W = malloc(n_est * sizeof *W);
    xvec = malloc(K * sizeof *xvec);
    if (cresid == NULL || W == NULL || xvec == NULL) 
	err = E_ALLOC;

    if (!err) {
	_init_model(&cum_mod, pdinfo);
	wbar = 0.0;
	for (j=0; j<n_est; j++) {
	    cum_mod = lsq(pmod->list, pZ, pdinfo, OLS, 1, 0.0);
	    err = cum_mod.errcode;
	    if (err) {
		errmsg(err, prn);
		clear_model(&cum_mod, NULL, NULL, pdinfo);
		break;
	    } else {
		t = pdinfo->t2 + 1;
		yy = 0.0;
		for (i=1; i<=K; i++) {
		    xno = cum_mod.list[i+1];
		    xvec[i-1] = (*pZ)[xno][t];
		    yy += cum_mod.coeff[i] * (*pZ)[xno][t];
		}
		cresid[j] = (*pZ)[yno][t] - yy;
		cum_mod.ci = CUSUM;
		makevcv(&cum_mod);
		xx = vprime_M_v(xvec, cum_mod.vcv, K);
		cresid[j] /= sqrt(1.0 + xx);
		/*  printf("w[%d] = %g\n", t, cresid[j]); */
		wbar += cresid[j];
		clear_model(&cum_mod, NULL, NULL, pdinfo);
		pdinfo->t2 += 1;
	    }
	}
    }

    if (!err) {
	wbar /= T - K;
	pprintf(prn, _("\nCUSUM test for stability of parameters\n\n"));
	pprintf(prn, _("mean of scaled residuals = %g\n"), wbar);
	sigma = 0;
	for (j=0; j<n_est; j++) {
	    xx = (cresid[j] - wbar);
	    sigma += xx * xx;
	}
	sigma /= T - K - 1;
	sigma = sqrt(sigma);
	pprintf(prn, _("sigmahat                 = %g\n\n"), sigma);

	xx = 0.948*sqrt((double) (T-K));
	yy = 2.0*xx/(T-K);

	pprintf(prn, _("Cumulated sum of scaled residuals\n"
		"('*' indicates a value outside of 95%% confidence band):\n\n"));
    
	for (j=0; j<n_est; j++) {
	    W[j] = 0.0;
	    for (i=0; i<=j; i++) W[j] += cresid[i];
	    W[j] /= sigma;
	    t = pmod->t1 + K + j;
	    ntodate(cumdate, t, pdinfo);
	    /* FIXME printing of number below? */
	    pprintf(prn, " %s %9.3f %s\n", cumdate, W[j],
		    (fabs(W[j]) > xx + (j+1)*yy)? "*" : "");
	}
	hct = (sqrt((double) (T-K)) * wbar) / sigma;
	pprintf(prn, _("\nHarvey-Collier t(%d) = %g with p-value %.4g\n\n"), 
		T-K-1, hct, tprob(hct, T-K-1));

	if (test != NULL) {
	    strcpy(test->type, _("CUSUM test for parameter stability"));
	    strcpy(test->h_0, _("no change in parameters"));
	    sprintf(test->teststat, "Harvey-Collier t(%d) = %g", T-K-1, hct);
	    sprintf(test->pvalue, "%f", tprob(hct, T-K-1));
	}

	/* plot with 95% confidence bands, if not batch mode */

	if (prn->fp == NULL && gnuplot_init(ppaths, &fq) == 0) {
	    fprintf(fq, "# CUSUM test\n");
	    fprintf(fq, "set xlabel \"observation\"\n");
	    fprintf(fq, "set xzeroaxis\n");
	    fprintf(fq, "set title \"CUSUM plot with 95%% confidence band\"\n");
	    fprintf(fq, "set nokey\n");
	    fprintf(fq, "plot %f+%f*x w l 1, \\\n", xx - K*yy, yy);
	    fprintf(fq, "%f-%f*x w l 1, \\\n", -xx + K*yy, yy);
	    fprintf(fq, "'-' using 1:2 w lp\n");
	    for (j=0; j<n_est; j++) { 
		t = pmod->t1 + K + j;
		fprintf(fq, "%d %f\n", t, W[j]);
	    }
	    fprintf(fq, "e\n");

#ifdef OS_WIN32
	    fprintf(fq, "pause -1\n");
#endif
	    fclose(fq);
	    err = gnuplot_display(ppaths);
	}
    }

    /* restore sample */
    pdinfo->t1 = t1;
    pdinfo->t2 = t2;
    
    free(cresid);
    free(W);
    free(xvec);

    return err;
}

/**
 * hausman_test:
 * @pmod: pointer to model to be tested.
 * @pZ: pointer to data matrix.
 * @pdinfo: information on the data set.
 * @prn: gretl printing struct.
 *
 * Tests the given pooled model for fixed and random effects.
 * 
 * Returns: 0 on successful completion, error code on error.
 */

int hausman_test (MODEL *pmod, double ***pZ, DATAINFO *pdinfo, 
		  const PATHS *ppaths, PRN *prn) 
{
    if (pmod->ci != POOLED) {
	pprintf(prn, _("This test is only relevant for pooled models\n"));
	return 1;
    }

    if (!balanced_panel(pdinfo)) {
	pprintf(prn, _("Sorry, can't do this test on an unbalanced panel.\n"
		"You need to have the same number of observations\n"
		"for each cross-sectional unit"));
	return 1;
    } else {
	void *handle;
	void (*panel_diagnostics)(MODEL *, double ***, DATAINFO *, PRN *);

	if (open_plugin(ppaths, "panel_data", &handle)) {
	    pprintf(prn, _("Couldn't access panel plugin\n"));
	    return 1;
	}
	panel_diagnostics = get_plugin_function("panel_diagnostics", handle);
	if (panel_diagnostics == NULL) {
	    pprintf(prn, _("Couldn't load plugin function\n"));
	    close_plugin(handle);
	    return 1;
	}
	(*panel_diagnostics) (pmod, pZ, pdinfo, prn);
	close_plugin(handle);
    }
    return 0;
}
