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
 *   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 *
 */

/* panel data plugin for gretl */

#include "libgretl.h"
#include "f2c.h"
#include "clapack_double.h"
#include "gretl_model.h"

#define PDEBUG 0

enum vcv_ops {
    VCV_INIT,
    VCV_SUBTRACT
};

typedef struct hausman_t_ hausman_t;
typedef struct diagnostics_t_ diagnostics_t;

struct hausman_t_ {
    int nbeta;
    double sigma_e;
    double H;
    double *bdiff;
    double *sigma;
};

struct diagnostics_t_ {
    int nunits;
    int effn;
    int T;
    int effT;
    int *unit_obs;
    char *varying;
    int *vlist;
    gretlopt opt;
};

struct {
    int ts;
    int n;
    int T;
    int offset;
} panidx;

static void 
panel_index_init (const DATAINFO *pdinfo, int nunits, int T)
{
    panidx.ts = (pdinfo->structure == STACKED_TIME_SERIES);
    panidx.n = nunits;
    panidx.T = T;
    panidx.offset = pdinfo->t1;
}

static int 
varying_vars_list (const MODEL *pmod, const double **Z, const DATAINFO *pdinfo,
		   diagnostics_t *diag);

#define panel_index(i,t) ((panidx.ts)? (i * panidx.T + t + panidx.offset) : \
                                       (t * panidx.n + i + panidx.offset))

/* .................................................................. */

static void haus_init (hausman_t *haus)
{
    haus->nbeta = 0;
    haus->sigma_e = NADBL;
    haus->H = NADBL;
    haus->bdiff = NULL;
    haus->sigma = NULL;
}

static void haus_free (hausman_t *haus)
{
    free(haus->bdiff);
    free(haus->sigma);
}

static void diagnostics_init (diagnostics_t *diag, gretlopt opt)
{
    diag->nunits = 0;
    diag->effn = 0;
    diag->T = 0;
    diag->effT = 0;
    diag->unit_obs = NULL;
    diag->varying = NULL;
    diag->vlist = NULL;
    diag->opt = opt;
}

static void diag_free (diagnostics_t *diag)
{
    free(diag->unit_obs);
    free(diag->varying);
    free(diag->vlist);
}

/* test variable number v against the (possibly reduced) regression
   list which contains only time-varying regressors (or perhaps
   unit-varying regressors if we're doing time effects) */

static int var_is_varying (const int *list, int v)
{
    int ret = 0;

    if (v != 0) {
	int i;

	for (i=2; i<=list[0]; i++) {
	    if (list[i] == v) {
		ret = 1;
		break;
	    }
	}
    }

    return ret;
}

static int 
haus_alloc (hausman_t *haus, const int *vlist)
{
    int nbeta = vlist[0] - 2;
    int nsigma = (nbeta * nbeta + nbeta) / 2;

    haus->nbeta = nbeta;

    /* array to hold differences between coefficient estimates */
    haus->bdiff = malloc(nbeta * sizeof *haus->bdiff);
    if (haus->bdiff == NULL) {
	return E_ALLOC;
    }

    /* array to hold covariance matrix */
    haus->sigma = malloc(nsigma * sizeof *haus->sigma);
    if (haus->sigma == NULL) {
	free(haus->bdiff);
	haus->bdiff = NULL;
	return E_ALLOC; 
    }

#if PDEBUG
    fprintf(stderr, "haus_alloc: allocated %d terms for bdiff, "
	    "%d for sigma\n", nbeta, nsigma);
#endif

    return 0;
}   

static void print_panel_coeff (const MODEL *pmod, 
			       const char *vname,
			       int i, PRN *prn)
{
    double tstat = pmod->coeff[i] / pmod->sderr[i];
    char errstr[18];
    char pvstr[18];

    sprintf(errstr, "(%.5g)", pmod->sderr[i]);
    sprintf(pvstr, "[%.5g]", tprob(tstat, pmod->dfd));
    pprintf(prn, "%*s: %14.5g %15s %15s\n", VNAMELEN, vname,
	    pmod->coeff[i], errstr, pvstr);
}

/* construct a version of the dataset from which the group means
   are subtracted */

static DATAINFO *
within_groups_dataset (const MODEL *pmod, const double **Z, 
		       double ***wZ, diagnostics_t *diag)
{
    DATAINFO *winfo;
    int i, j, k;
    int t, bigt;

#if PDEBUG
    fprintf(stderr, "within_groups_dataset: nvar=%d, nobs=%d, *wZ=%p\n", 
	    diag->vlist[0], pmod->nobs, (void *) *wZ);
#endif

    winfo = create_new_dataset(wZ, diag->vlist[0], pmod->nobs, 0);
    if (winfo == NULL) {
	return NULL;
    }

    k = 0;
    for (j=1; j<=diag->vlist[0]; j++) { 

	if (diag->vlist[j] == 0) {
	    continue;
	} else {
	    k++;
	}

#if PDEBUG
	fprintf(stderr, "working on list[%d] (original var %d, var %d in wZ)\n",
		j, list[j], k);
#endif

	for (i=0; i<diag->nunits; i++) { 
	    int Ti = diag->unit_obs[i];
	    double xbar = 0.0;

	    if (Ti == 0) {
		continue;
	    }

	    for (t=0; t<diag->T; t++) {
		bigt = panel_index(i, t);
		if (!na(pmod->uhat[bigt])) {
		    xbar += Z[diag->vlist[j]][bigt];
		}
	    }

	    xbar /= (double) Ti;
#if PDEBUG > 1
	    fprintf(stderr, "xbar for var %d, unit %d = %g\n", 
		    diag->vlist[j], i, xbar);
#endif
	    for (t=0; t<diag->T; t++) {
		bigt = panel_index(i, t);
		if (!na(pmod->uhat[bigt])) {
		    (*wZ)[k][bigt] = Z[diag->vlist[j]][bigt] - xbar;
		}
#if PDEBUG > 1
		fprintf(stderr, "Set wZ[%d][%d] = %g\n", k, bigt, (*wZ)[k][bigt]);
#endif
	    }
	}
    }

    return winfo;
}

/* construct a mini-dataset containing the group means */

static DATAINFO *
group_means_dataset (const MODEL *pmod, 
		     const double **Z, const DATAINFO *pdinfo,
		     double ***gZ, diagnostics_t *diag)
{
    DATAINFO *ginfo;
    int i, j, k;
    int t, bigt;

#if PDEBUG
    fprintf(stderr, "group_means_dataset: nvar=%d, nobs=%d, *gZ=%p\n", 
	    pmod->list[0], diag->effn, (void *) *gZ);
#endif

    ginfo = create_new_dataset(gZ, pmod->list[0], diag->effn, 0);
    if (ginfo == NULL) {
	return NULL;
    }

    k = 1;
    for (j=1; j<=pmod->list[0]; j++) { 
	int s = 0;

	if (pmod->list[j] == 0) {
	    continue;
	}

	for (i=0; i<diag->nunits; i++) { 
	    int Ti = diag->unit_obs[i];
	    double xx = 0.0;

	    if (Ti == 0) {
		continue;
	    }
	    for (t=0; t<diag->T; t++) {
		bigt = panel_index(i, t);
		if (!na(pmod->uhat[bigt])) {
		    xx += Z[pmod->list[j]][bigt];
		}
	    }
	    (*gZ)[k][s] = xx / (double) Ti;
#if PDEBUG > 1
	    fprintf(stderr, "Set gZ[%d][%d] = %g\n", k, i, (*gZ)[k][i]);
#endif
	    s++;
	}
	k++;
    }

    return ginfo;
}

/* calculate the group means regression and return its error variance */

static double 
group_means_variance (const MODEL *pmod, double ***gZ, DATAINFO *ginfo)
{
    MODEL gmmod;
    double gmvar = NADBL;
    int *gmlist;
    int i, j;

    gmlist = malloc((pmod->list[0] + 1) * sizeof *gmlist);
    if (gmlist == NULL) {
	return NADBL;
    }

    gmlist[0] = pmod->list[0];
    j = 1;
    for (i=1; i<=gmlist[0]; i++) { 
	if (pmod->list[i] == 0) {
	    gmlist[i] = 0;
	} else {
	    gmlist[i] = j++;
	}
    }

    gmmod = lsq(gmlist, gZ, ginfo, OLS, OPT_A, 0.0);

    if (gmmod.errcode == 0) {
	gmvar = gmmod.sigma * gmmod.sigma;
    }

    clear_model(&gmmod);
    free(gmlist);

    return gmvar;
}

/* op is VCV_SUBTRACT for the random effects model.  With the fixed
   effects model we don't have to worry about excluding vcv entries
   for time-invariant variables, since none of these are included.
*/

static int 
vcv_skip (const MODEL *pmod, int i, const diagnostics_t *diag, int op)
{
    int skip = 0;

    if (pmod->list[i+2] == 0) {
	/* always skip the constant */
	skip = 1;
    } else if (op == VCV_SUBTRACT && !diag->varying[i]) {
	/* random effects, time-invariant var */
	skip = 1;
    }

    return skip;
}

/* fill out the covariance matrix for use with the Hausman test:
   the entries that get transcribed here are only those for
   slopes with respect time-varying variables */

static void 
vcv_slopes (hausman_t *haus, const diagnostics_t *diag, 
	    const MODEL *pmod, int op)
{
    int idx, i, j;
    int mj, mi = 0;
    int k = 0;

    for (i=0; i<haus->nbeta; i++) {
	if (vcv_skip(pmod, mi, diag, op)) {
	    i--;
	    mi++;
	    continue;
	}
	mj = mi;
	for (j=i; j<haus->nbeta; j++) {
	    if (vcv_skip(pmod, mj, diag, op)) {
		j--;
		mj++;
		continue;
	    }

	    idx = ijton(mi, mj, pmod->ncoeff);
#if PDEBUG
	    fprintf(stderr, "setting sigma[%d] using vcv[%d] (%d,%d) = %g\n",
		    k, idx, mi, mj, pmod->vcv[idx]);
#endif
	    if (op == VCV_SUBTRACT) {
		haus->sigma[k++] -= pmod->vcv[idx];
	    } else {
		haus->sigma[k++] = pmod->vcv[idx];
	    }
	    mj++;
	}
	mi++;
    }
}

/* calculate Hausman test statistic */

static int bXb (hausman_t *haus)
{
    char uplo = 'L'; 
    integer nrhs = 1;
    integer n = haus->nbeta;
    integer ldb = n;
    integer info = 0;
    integer *ipiv;
    double *x;
    int i;

    /* make a copy of haus->bdiff first */
    x = malloc(n * sizeof *x);
    if (x == NULL) {
	return E_ALLOC;
    }

    ipiv = malloc(n * sizeof *ipiv);
    if (ipiv == NULL) {
	free(x);
	return E_ALLOC;
    }

    for (i=0; i<haus->nbeta; i++) {
	x[i] = haus->bdiff[i];
    }

    /* solve for X-inverse * b */
    dspsv_(&uplo, &n, &nrhs, haus->sigma, ipiv, x, &ldb, &info);

    if (info > 0) {
	fprintf(stderr, "Hausman sigma matrix is singular\n");
    } else if (info < 0) {
	fprintf(stderr, "Illegal entry in Hausman sigma matrix\n");
    } else {
	haus->H = 0.0;
	for (i=0; i<haus->nbeta; i++) {
	    haus->H += x[i] * haus->bdiff[i];
	}
    }

    free(x);
    free(ipiv);

    return info;
}

#define MINOBS 1

/* calculate the fixed effects regression, either using dummy variables
   or by subtracting means from the data
*/

static MODEL
fixed_effects_model (const MODEL *pmod, 
		     double ***pZ, DATAINFO *pdinfo,
		     diagnostics_t *diag, int *usedum)
{
    int i, t, oldv = pdinfo->v;
    int dvlen, ndum = 0;
    int *felist = NULL;
    MODEL lsdv;
#if PDEBUG
    PRN *prn = gretl_print_new(GRETL_PRINT_STDERR, NULL);
#endif

    gretl_model_init(&lsdv);

    for (i=0; i<diag->nunits; i++) {
	if (diag->unit_obs[i] >= MINOBS) {
	    ndum++;
	}
    }

    dvlen = diag->vlist[0] + ndum;
    ndum--;

#if PDEBUG
    fprintf(stderr, "ndum = %d, dvlen = %d\n", ndum, dvlen);
#endif

    /* should we use a set of dummy variables, or subtract
       the group means? */

    if (ndum <= 20 || ndum < diag->vlist[0]) {
	*usedum = 1;
    } else {
	*usedum = 0;
    }

    if (*usedum == 0) {
	double **wZ;
	DATAINFO *winfo;

	felist = malloc((diag->vlist[0]) * sizeof *felist);
	if (felist == NULL) {
	    lsdv.errcode = E_ALLOC;
	    return lsdv;
	}	

	winfo = within_groups_dataset(pmod, (const double **) *pZ, &wZ, diag);
	if (winfo == NULL) {
	    free(felist);
	    lsdv.errcode = E_ALLOC;
	    return lsdv;
	} 

	felist[0] = diag->vlist[0] - 1;
	for (i=1; i<=felist[0]; i++) {
	    felist[i] = i;
	}

	/* OPT_Z: do _not_ automatically eliminate perfectly
	   collinear variables */
	lsdv = lsq(felist, &wZ, winfo, OLS, OPT_A | OPT_Z, 0.0);

	if (lsdv.errcode) {
	    ; /* pass on */
	} else if (lsdv.list[0] < felist[0]) {
	    /* one or more variables were dropped, because they were
	       all zero -- this is a symptom of collinearity */
	    lsdv.errcode = E_SINGULAR;
	} else {
	    double sdcorr = sqrt((double) lsdv.dfd / (lsdv.dfd - diag->nunits));

	    /* we estimated a bunch of group means, and have to
	       subtract that many degrees of freedom */
	    lsdv.dfd -= diag->nunits;
	    lsdv.dfn += diag->nunits; 

	    lsdv.sigma *= sdcorr;

	    for (i=0; i<lsdv.ncoeff; i++) {
		lsdv.sderr[i] *= sdcorr;
	    }
#if PDEBUG
	    printmodel(&lsdv, winfo, OPT_NONE, prn);
#endif
	}

#if PDEBUG
	gretl_print_destroy(prn);
#endif

	free_Z(wZ, winfo);
	clear_datainfo(winfo, CLEAR_FULL);
	free(winfo);
	    
    } else {
	int j = 0;

	/* We can be assured there's an intercept in the original
	   regression */

	felist = malloc(dvlen * sizeof *felist);
	if (felist == NULL) {
	    lsdv.errcode = E_ALLOC;
	    return lsdv;
	}

	if (dataset_add_vars(ndum, pZ, pdinfo)) {
	    free(felist);
	    lsdv.errcode = E_ALLOC;
	    return lsdv;
	}

	/* create the per-unit dummy variables */

	for (i=0; i<diag->nunits; i++) {
	    int dv = oldv + j;

	    if (diag->unit_obs[i] < MINOBS) {
		continue;
	    }

	    sprintf(pdinfo->varname[dv], "unit_%d", i + 1);

	    for (t=0; t<pdinfo->n; t++) {
		(*pZ)[dv][t] = 0.0;
	    }

	    for (t=0; t<diag->T; t++) {
		(*pZ)[dv][panel_index(i, t)] = 1.0;
	    }

	    if (++j == ndum) {
		break;
	    }
	}

	/* construct the regression list */

	felist[0] = dvlen - 1;

	for (i=1; i<=diag->vlist[0]; i++) {
	    felist[i] = diag->vlist[i];
	}

	for (i=0; i<ndum; i++) {
	    felist[diag->vlist[0] + i + 1] = oldv + i;
	}

	lsdv = lsq(felist, pZ, pdinfo, OLS, OPT_A | OPT_Z, 0.0);

#if PDEBUG
	if (lsdv.errcode == 0) {
	    printmodel(&lsdv, pdinfo, OPT_NONE, prn);
	}
	gretl_print_destroy(prn);
#endif

	dataset_drop_vars(pdinfo->v - oldv, pZ, pdinfo);
    }

    free(felist);

    return lsdv;
}

/* drive the calculation of the fixed effects regression, print the
   results, and return the error variance */

static double 
fixed_effects_variance (const MODEL *pmod, 
			double ***pZ, DATAINFO *pdinfo,
			diagnostics_t *diag, hausman_t *haus, 
			PRN *prn)
{
    MODEL lsdv;
    double var = NADBL;
    int usedum = 0;

    lsdv = fixed_effects_model(pmod, pZ, pdinfo, diag, &usedum);

    if (lsdv.errcode) {
	pputs(prn, _("Error estimating fixed effects model\n"));
	errmsg(lsdv.errcode, prn);
    } else {
	int i, j, k;
	int ndum;
	double F;

	var = lsdv.sigma * lsdv.sigma;

#if PDEBUG
	fprintf(stderr, "f.e. variance, lsdv.ncoeff=%d, var=%g, "
		"list[0]=%d, haus=%p\n", lsdv.ncoeff, var,
		list[0], (void *) haus);
	if (haus != NULL) {
	    fprintf(stderr, "haus->bdiff = %p, haus->sigma = %p\n",
		    haus->bdiff, haus->sigma);
	}
#endif

	ndum = lsdv.list[0] - diag->vlist[0];

	pputs(prn, 
	      _("Fixed effects estimator\n"
		"allows for differing intercepts by cross-sectional unit\n"
		"slope standard errors in parentheses, p-values in brackets\n"));

	if (ndum > 0) {
	    pputs(prn, _("a_i = intercepts"));
	    pputs(prn, "\n\n");
	} else {
	    pputc(prn, '\n');
	}

	/* print and record the slope coefficients, for varying
	   regressors */

	k = 0;
	for (i=1; i<diag->vlist[0] - 1; i++) {
	    int vi = diag->vlist[i+2];

	    j = (usedum)? i : i - 1;
	    print_panel_coeff(&lsdv, pdinfo->varname[vi], j, prn);
	    if (haus != NULL) {
		haus->bdiff[k++] = lsdv.coeff[j];
	    }
	}

	pputc(prn, '\n');   

	/* if we used dummy variables, print the per-unit intercept 
	   estimates */

	if (ndum > 0) {
	    j = 0;
	    for (i=0; i<diag->nunits; i++) {
		char dumstr[VNAMELEN];
		double b;

		if (diag->unit_obs[i] < MINOBS) {
		    continue;
		}

		if (j == ndum) {
		    b = lsdv.coeff[0];
		} else {
		    b = lsdv.coeff[j + diag->vlist[0] - 1] + lsdv.coeff[0];
		}
		sprintf(dumstr, "a_%d", i + 1);
		pprintf(prn, "%*s: %14.4g\n", VNAMELEN, dumstr, b);
		j++;
	    }
	} else {
	    pprintf(prn, _("%d group means were subtracted from the data"), diag->effn);
	    pputc(prn, '\n');
	    ndum = diag->effn - 1;
	}

	pprintf(prn, _("\nResidual variance: %g/(%d - %d) = %g\n"), 
		lsdv.ess, lsdv.nobs, diag->vlist[0] - 1 + ndum, var);

	if (usedum) {
	    pputs(prn, _("Joint significance of unit dummy variables:\n"));
	} else {
	    pprintf(prn, _("Joint significance of differing group means:\n"));
	}

	F = (pmod->ess - lsdv.ess) * lsdv.dfd / (lsdv.ess * ndum);
	pprintf(prn, " F(%d, %d) = %g %s %g\n", ndum, lsdv.dfd, F, 
		_("with p-value"), fdist(F, ndum, lsdv.dfd));

	pputs(prn, _("(A low p-value counts against the null hypothesis that "
		     "the pooled OLS model\nis adequate, in favor of the fixed "
		     "effects alternative.)\n\n"));

	if (haus != NULL) {
	    makevcv(&lsdv);
	    haus->sigma_e = lsdv.sigma;
	    vcv_slopes(haus, diag, &lsdv, VCV_INIT);
	}
    }

    clear_model(&lsdv);

    return var;
}

/* calculate the random effects regression and print the results */

static int random_effects (const MODEL *pmod, 
			   const double **Z, DATAINFO *pdinfo, 
			   const double **gZ, double theta, 
			   diagnostics_t *diag, hausman_t *haus, 
			   PRN *prn)
{
    double **reZ;
    DATAINFO *reinfo;
    MODEL remod;
    int *relist;
    int re_n = diag->T * diag->effn;
    int i, j, k, t;
    int err = 0;

    /* regression list */
    relist = malloc((pmod->list[0] + 1) * sizeof *relist);
    if (relist == NULL) {
	return E_ALLOC;
    }

    /* special transformed dataset */
    reinfo = create_new_dataset(&reZ, pmod->list[0], re_n, 0);
    if (reinfo == NULL) {
	free(relist);
	return E_ALLOC;
    }

#if PDEBUG
    fprintf(stderr, "reZ: series length = T * effn = %d * %d = %d\n",
	    diag->T, diag->effn, diag->T * diag->effn);
#endif

    relist[0] = pmod->list[0];

    /* create transformed variables: original data minus theta
       times the appropriate group mean */

    k = 1;
    for (j=1; j<=relist[0]; j++) {
	const double *xj = Z[pmod->list[j]];
	const double *gm = gZ[k];
	int bigt, rt, u = 0;

	if (pmod->list[j] == 0) {
	    relist[j] = 0;
	    continue;
	}

	relist[j] = k;

	for (i=0; i<diag->nunits; i++) {
	    if (diag->unit_obs[i] == 0) {
		continue;
	    }
	    for (t=0; t<diag->T; t++) {
		bigt = panel_index(i, t);
		if (pdinfo->structure == STACKED_TIME_SERIES) {
		    rt = u * diag->T + t;
		} else {
		    rt = t * diag->effn + u;
		}
		if (na(pmod->uhat[bigt])) {
		    reZ[k][rt] = NADBL;
		} else {
		    reZ[k][rt] = xj[bigt] - theta * gm[u];
		}
	    }
	    u++;
	}
	k++;
    }

    for (t=0; t<re_n; t++) {
	reZ[0][t] -= theta;
    }

    remod = lsq(relist, &reZ, reinfo, OLS, OPT_A | OPT_Z, 0.0);

    if ((err = remod.errcode)) {
	pputs(prn, _("Error estimating random effects model\n"));
	errmsg(err, prn);
    } else {
	pputs(prn,
	      _("                         Random effects estimator\n"
		"           allows for a unit-specific component to the "
		"error term\n"
		"           (standard errors in parentheses, p-values in brackets)\n\n"));

	k = 0;
	for (i=0; i<remod.ncoeff; i++) {
	    int vi = pmod->list[i+2];

	    print_panel_coeff(&remod, pdinfo->varname[vi], i, prn);
	    if (var_is_varying(diag->vlist, vi)) {
		haus->bdiff[k++] -= remod.coeff[i];
	    }
	}
	makevcv(&remod);
	vcv_slopes(haus, diag, &remod, VCV_SUBTRACT);
    }

    clear_model(&remod);

    free_Z(reZ, reinfo);
    clear_datainfo(reinfo, CLEAR_FULL);
    free(reinfo);

    free(relist);    

    return err;
}

/* .................................................................. */

static void 
unit_error_variances (double *uvar, const MODEL *pmod, const DATAINFO *pdinfo,
		      int nunits, int T, const int *unit_obs)
{
    int i, t;
    double x;

    for (i=0; i<nunits; i++) {
	uvar[i] = 0.0;
	for (t=0; t<T; t++) {
	    x = pmod->uhat[panel_index(i, t)];
	    if (!na(x)) {
		uvar[i] += x * x;
	    }
	}
	if (unit_obs[i] > 1) {
	    uvar[i] /= (double) unit_obs[i]; 
	}	
    }
}

static int breusch_pagan_LM (const MODEL *pmod, const DATAINFO *pdinfo, 
			     diagnostics_t *diag, PRN *prn)
{
    double LM, eprime = 0.0;
    int i, t;

    pputs(prn, _("\nMeans of pooled OLS residuals for cross-sectional "
		 "units:\n\n"));

    for (i=0; i<diag->nunits; i++) {
	int Ti = diag->unit_obs[i];
	double u, ubar = 0.0;

	if (Ti == 0) {
	    continue;
	}

	for (t=0; t<diag->T; t++) {
	    u = pmod->uhat[panel_index(i, t)];
	    if (!na(u)) {
		ubar += u;
	    }
	}
	ubar /= (double) Ti; 
	pprintf(prn, _(" unit %2d: %13.5g\n"), i + 1, ubar);
	eprime += ubar * ubar;
    }

    /* FIXME unbalanced panels */

    LM = (double) pmod->nobs / (2.0 * (diag->effT - 1.0)) * 
	pow((diag->effT * diag->effT * eprime / pmod->ess) - 1.0, 2);

    pprintf(prn, _("\nBreusch-Pagan test statistic:\n"
		   " LM = %g with p-value = prob(chi-square(1) > %g) = %g\n"), 
	    LM, LM, chisq(LM, 1));

    pputs(prn, _("(A low p-value counts against the null hypothesis that "
		 "the pooled OLS model\nis adequate, in favor of the random "
		 "effects alternative.)\n\n"));

    return 0;
}

/* .................................................................. */

static int do_hausman_test (hausman_t *haus, PRN *prn)
{
#if PDEBUG
    int i, ns = haus->nbeta;
    int nterms = (ns * ns + ns) / 2;

    for (i=0; i<ns; i++) {
 	fprintf(stderr, "b%d_FE - beta%d_RE = %g\n", i, i, haus->bdiff[i]);
    }
    fputc('\n', stderr);

    for (i=0; i<nterms; i++) {
 	fprintf(stderr, "vcv_diff[%d] = %g\n", i, haus->sigma[i]);
    }
#endif

    if (bXb(haus)) { 
	pputs(prn, _("Error attempting to invert vcv difference matrix\n"));
	return 1;
    }

    if (na(haus->H)) {
	pputs(prn, _("\nHausman test matrix is not positive definite (this "
		     "result may be treated as\n\"fail to reject\" the random effects "
		     "specification).\n"));
    } else {
	pprintf(prn, _("\nHausman test statistic:\n"
		       " H = %g with p-value = prob(chi-square(%d) > %g) = %g\n"),
		haus->H, haus->nbeta, haus->H, chisq(haus->H, haus->nbeta));
	pputs(prn, _("(A low p-value counts against the null hypothesis that "
		     "the random effects\nmodel is consistent, in favor of the fixed "
		     "effects model.)\n"));
    }

    return 0;
}

int get_maj_min (const DATAINFO *pdinfo, int *maj, int *min)
{
    int startmaj, startmin;
    int endmaj, endmin;

    if (sscanf(pdinfo->stobs, "%d:%d", &startmaj, &startmin) != 2) {
	return 1;
    }

    if (sscanf(pdinfo->endobs, "%d:%d", &endmaj, &endmin) != 2) {
	return 1;
    } 

    *maj = endmaj - startmaj + 1;
    *min = endmin - startmin + 1;

    return 0;
}

int n_included_units (const MODEL *pmod, const DATAINFO *pdinfo,
		      int *unit_obs)
{
    int nmaj, nmin;
    int nunits, T;
    int i, t;
    int ninc = 0;

    if (get_maj_min(pdinfo, &nmaj, &nmin)) {
	return -1;
    }

    if (pdinfo->structure == STACKED_TIME_SERIES) {
	nunits = nmaj;
	T = nmin;
    } else {
	nunits = nmin;
	T = nmaj;
    }

    for (i=0; i<nunits; i++) {
	unit_obs[i] = 0;
	for (t=0; t<T; t++) {
	    if (!na(pmod->uhat[panel_index(i, t)])) {
		unit_obs[i] += 1;
	    }
	}
	if (unit_obs[i] > 0) {
	    ninc++;
	}
    }

    return ninc;
}

static int effective_T (const int *unit_obs, int nunits)
{
    int i, effT = 0;

    for (i=0; i<nunits; i++) {
	if (unit_obs[i] > effT) {
	    effT = unit_obs[i];
	}
    }

    return effT;
}

static int diag_set_varying (diagnostics_t *diag, const MODEL *pmod)
{
    int i;

    /* char array to record which params in the full model
       correspond to time-varying regressors */
    diag->varying = calloc(pmod->ncoeff, 1);
    if (diag->varying == NULL) {
	return E_ALLOC; 
    }

    for (i=0; i<pmod->ncoeff; i++) {
	if (var_is_varying(diag->vlist, pmod->list[i+2])) {
	    diag->varying[i] = 1;
	} 
    }

    return 0;
}

/* .................................................................. */

int panel_diagnostics (MODEL *pmod, double ***pZ, DATAINFO *pdinfo, 
		       gretlopt opt, PRN *prn)
{
    int unbal = gretl_model_get_int(pmod, "unbalanced");
    int xdf;
    double fe_var;
    diagnostics_t diag;
    hausman_t haus;
    hausman_t *hptr;
    int err = 0;

#if PDEBUG
    fputs("\n*** Starting panel_diagnostics ***\n", stderr);
#endif

    if (pmod->ifc == 0) {
	/* at many points in these functions we assume the base
	   regression has an intercept included */
	return 1;
    }

    diagnostics_init(&diag, opt);

    if (get_panel_structure(pdinfo, &diag.nunits, &diag.T)) {
	return E_DATA;
    }

    panel_index_init(pdinfo, diag.nunits, diag.T);

    haus_init(&haus);

    diag.unit_obs = malloc(diag.nunits * sizeof *diag.unit_obs);
    if (diag.unit_obs == NULL) {
	return E_ALLOC;
    }

    diag.effn = n_included_units(pmod, pdinfo, diag.unit_obs);
    if (diag.effn < diag.nunits) {
	fprintf(stderr, "number of units included = %d\n", diag.effn);
    }

    diag.effT = effective_T(diag.unit_obs, diag.nunits);

    /* figure out which of the original regressors are time-varying,
       or unit-varying as the case may be 
    */
    err = varying_vars_list(pmod, (const double **) *pZ, pdinfo, &diag);
    if (err) {
	goto bailout;
    }

    err = diag_set_varying(&diag, pmod);
    if (err) {
	goto bailout;
    }    

    /* degrees of freedom relative to # of x-sectional units */
    xdf = diag.effn - pmod->ncoeff;

#if PDEBUG
    fprintf(stderr, "nunits=%d, T=%d, effn=%d, effT=%d, unbal=%d, xdf=%d\n",
	    diag.nunits, diag.T, diag.effn, diag.effT, unbal, xdf);
#endif

    /* can we do the Hausman test or not? */
    if (!unbal && xdf > 0) {
	err = haus_alloc(&haus, diag.vlist);
	if (err) {
	    goto bailout;
	}
	hptr = &haus;
    } else {
	/* not if the panel is unbalanced, or if we can't run
	   a group means regression */
	hptr = NULL;
    }

    if (!unbal) {
	pprintf(prn, _("      Diagnostics: assuming a balanced panel with %d "
		       "cross-sectional units\n "
		       "                        observed over %d periods\n\n"), 
		diag.effn, diag.effT);
    }

    fe_var = fixed_effects_variance(pmod, pZ, pdinfo, &diag, hptr, prn);

    if (unbal) {
	pprintf(prn, "Omitting random effects model since "
		"panel is unbalanced\n");
	goto bailout;
    }

    breusch_pagan_LM(pmod, pdinfo, &diag, prn);

    if (xdf <= 0) {
	pprintf(prn, "Omitting group means regression: "
		"insufficient degrees of freedom\n");
	goto bailout;
    }
    
    if (xdf > 0 && !na(fe_var)) {
	double **gZ = NULL;
	DATAINFO *ginfo;
	double gm_var = NADBL;

	ginfo = group_means_dataset(pmod, (const double **) *pZ,
				    pdinfo, &gZ, &diag);

	if (ginfo != NULL) {
	    gm_var = group_means_variance(pmod, &gZ, ginfo);
	}

	if (na(gm_var)) { 
	    pputs(prn, _("Couldn't estimate group means regression\n"));
	} else {
	    double theta = 1.0 - sqrt(fe_var / (diag.effT * gm_var));

	    pprintf(prn, _("Residual variance for group means "
			   "regression: %g\n\n"), gm_var);    
	    random_effects(pmod, (const double **) *pZ, pdinfo, 
			   (const double **) gZ, theta, &diag,
			   &haus, prn);
	    do_hausman_test(&haus, prn);
	}

	if (ginfo != NULL) {
	    free_Z(gZ, ginfo);
	    clear_datainfo(ginfo, CLEAR_FULL);
	    free(ginfo);
	}
    }

 bailout:

    haus_free(&haus);
    diag_free(&diag);

    return err;
}

/* .................................................................. */

static int 
write_uvar_to_dataset (double *uvar, int nunits, int T,
		       double **Z, DATAINFO *pdinfo)
{
    int i, t;
    int uv = pdinfo->v - 1;

    for (i=0; i<nunits; i++) {
	for (t=0; t<T; t++) {
	    if (uvar[i] <= 0.0) {
		Z[uv][panel_index(i, t)] = 0.0;
	    } else {
		Z[uv][panel_index(i, t)] = 1.0 / sqrt(uvar[i]);
	    }
	}
    }

    return 0;
}

static int
allocate_weight_var (double ***pZ, DATAINFO *pdinfo)
{
    if (dataset_add_vars(1, pZ, pdinfo)) {
	return E_ALLOC;
    }

    strcpy(pdinfo->varname[pdinfo->v - 1], "unit_wt");

    return 0;
}

static double max_coeff_diff (const MODEL *pmod, const double *bvec)
{
    double diff, maxdiff = 0.0;
    int i;

    for (i=0; i<pmod->ncoeff; i++) {
	diff = fabs(pmod->coeff[i] - bvec[i]);
	if (diff > maxdiff) {
	    maxdiff = diff;
	}
    }

    return maxdiff;
}

static void
print_wald_test (double W, int nunits, const int *unit_obs, PRN *prn)
{
    int i, df = 0;

    for (i=0; i<nunits; i++) {
	if (unit_obs[i] > 1) df++;
    }

    pprintf(prn, "\n%s\n%s:\n",
	    _("Distribution free Wald test for heteroskedasticity"),
	    _("based on the FGLS residuals"));
    pprintf(prn, "%s(%d) = %g, ",  _("Chi-square"), df, W);
    pprintf(prn, _("with p-value = %g\n\n"), chisq(W, df));
}

static double 
wald_hetero_test (const MODEL *pmod, const DATAINFO *pdinfo, 
		  double s2, const double *uvar,
		  int T, int nunits, const int *unit_obs)
{
    double x, W = 0.0;
    int i, t, Ti;

    for (i=0; i<nunits; i++) {
	double fii = 0.0;

	Ti = unit_obs[i];
	if (Ti == 1) {
	    W = NADBL;
	    break;
	}
	for (t=0; t<T; t++) {
	    x = pmod->uhat[panel_index(i, t)];
	    if (!na(x)) {
		x = x * x - uvar[i];
		fii += x * x;
	    }
	}
	if (fii <= 0) {
	    W = NADBL;
	    break;
	}	    
	fii *= (1.0 / Ti) * (1.0 / (Ti - 1.0));
	x = uvar[i] - s2;
	W += x * x / fii;
    }

    return W;
}

static int
ml_hetero_test (MODEL *pmod, double s2, const double *uvar, 
		int nunits, const int *unit_obs)
{
    GRETLTEST test;
    double x2, s2h = 0.0;
    int i, Ti, df = 0;

    for (i=0; i<nunits; i++) {
	Ti = unit_obs[i];
	if (Ti > 0) {
	    s2h += Ti * log(uvar[i]);
	    df++;
	}
    }

    x2 = pmod->nobs * log(s2) - s2h;
    df--;

    gretl_test_init(&test);
    strcpy(test.type, 
	   N_("Likelihood ratio test for groupwise heteroskedasticity"));
    strcpy(test.h_0, N_("the units have a common error variance"));
    test.teststat = GRETL_TEST_LR;
    test.dfn = df;
    test.value = x2;
    test.pvalue = chisq(x2, df);

    return add_test_to_model(pmod, &test);
}

static double pooled_ll (const MODEL *pmod)
{
    double n = pmod->nobs;

    return -(n / 2.0) * (1.0 + LN_2_PI - log(n) + log(pmod->ess));
}

static double real_ll (const MODEL *pmod, const double *uvar, 
		       int nunits, const int *unit_obs)
{
    double ll = -(pmod->nobs / 2.0) * LN_2_PI;
    int i, Ti;

    for (i=0; i<nunits; i++) {
	Ti = unit_obs[i];
	if (Ti > 0) {
	    ll -= (Ti / 2.0) * (1.0 + log(uvar[i]));
	}
    }

    return ll;
}

/* we can't estimate a group-specific variance based on just one
   observation? */

static int singleton_check (const int *unit_obs, int nunits)
{
    int i;

    for (i=0; i<nunits; i++) {
	if (unit_obs[i] == 1) {
	    return 1;
	}
    }

    return 0;
}

#define SMALLDIFF 0.0001
#define WLS_MAX   20

MODEL panel_wls_by_unit (int *list, double ***pZ, DATAINFO *pdinfo,
			 gretlopt opt, PRN *prn)
{
    MODEL mdl;
    gretlopt wlsopt = OPT_A;
    double *uvar = NULL;
    double *bvec = NULL;
    double s2, diff = 1.0;
    double W = NADBL;
    int *unit_obs = NULL;
    int *wlist = NULL;
    int nunits, effn, T, effT;
    int orig_v = pdinfo->v;
    int i, iter = 0;

    gretl_errmsg_clear();

    if (opt & OPT_T) {
	/* iterating: no degrees-of-freedom correction */
	wlsopt |= OPT_N; 
    } 

    if (opt & OPT_A) {
	wlsopt |= OPT_A;
    }

    gretl_model_init(&mdl);

    if (get_panel_structure(pdinfo, &nunits, &T)) {
	mdl.errcode = E_DATA;
	return mdl;
    }

    panel_index_init(pdinfo, nunits, T);

    unit_obs = malloc(nunits * sizeof *unit_obs);
    if (unit_obs == NULL) {
	mdl.errcode = E_ALLOC;
	return mdl;
    }

    uvar = malloc(nunits * sizeof *uvar);
    if (unit_obs == NULL) {
	free(unit_obs);
	mdl.errcode = E_ALLOC;
	return mdl;
    }    
    
    mdl = lsq(list, pZ, pdinfo, OLS, OPT_A, 0.0);
    if (mdl.errcode) {
	goto bailout;
    }

    effn = n_included_units(&mdl, pdinfo, unit_obs);
    effT = effective_T(unit_obs, nunits);  

    if (opt & OPT_T) {
	if (singleton_check(unit_obs, nunits)) {
	    pprintf(prn, _("Can't produce ML estimates: "
			   "some units have only one observation"));
	    pputc(prn, '\n');
	    opt ^= OPT_T;
	}
    }

    s2 = mdl.ess / mdl.nobs;

    if ((opt & OPT_V) && (opt & OPT_T)) {
	pprintf(prn, "\nOLS error variance = %g\n", s2);
	pprintf(prn, "log-likelihood = %g\n", pooled_ll(&mdl));
    }

    if (allocate_weight_var(pZ, pdinfo)) {
	mdl.errcode = E_ALLOC;
	goto bailout;
    }

    if (opt & OPT_T) {
	bvec = malloc(mdl.ncoeff * sizeof *bvec);
	if (bvec == NULL) {
	    mdl.errcode = E_ALLOC;
	    goto bailout;
	}
    }

    /* allocate and construct WLS regression list */
    wlist = malloc((mdl.list[0] + 2) * sizeof *wlist);
    if (wlist == NULL) {
	mdl.errcode = E_ALLOC;
	goto bailout;
    }
    wlist[0] = mdl.list[0] + 1;
    wlist[1] = pdinfo->v - 1; /* weight variable: the last var added */
    for (i=2; i<=wlist[0]; i++) {
	wlist[i] = mdl.list[i-1];
    }

    /* if wanted, iterate to ML solution; otherwise just do
       one-step FGLS estimation 
    */

    while (diff > SMALLDIFF) {

	iter++;

	unit_error_variances(uvar, &mdl, pdinfo, nunits, T, unit_obs);

	if (opt & OPT_V) {
	    if (opt & OPT_T) {
		pprintf(prn, "\n*** %s %d ***\n", _("iteration"), 
			iter);
	    } else {
		pputc(prn, '\n');
	    }
	    pputs(prn, " unit    variance\n");
	    for (i=0; i<nunits; i++) {
		if (unit_obs[i] > 0) {
		    pprintf(prn, "%5d%12g\n", i + 1, uvar[i]);
		}
	    }
	}

	if ((opt & OPT_T) && iter == 2) {
	    W = wald_hetero_test(&mdl, pdinfo, s2, uvar, T, 
				 nunits, unit_obs);
	}

	write_uvar_to_dataset(uvar, nunits, T, *pZ, pdinfo);

	if (opt & OPT_T) {
	    /* save coefficients for comparison */
	    for (i=0; i<mdl.ncoeff; i++) {
		bvec[i] = mdl.coeff[i];
	    }
	}

	clear_model(&mdl);

	mdl = lsq(wlist, pZ, pdinfo, WLS, wlsopt, 0.0);

	if (mdl.errcode || iter > WLS_MAX) {
	    mdl.errcode = E_NOCONV;
	    break;
	}

	if (opt & OPT_T) {
	    diff = max_coeff_diff(&mdl, bvec);
	    if (opt & OPT_V && iter == 1) {
		pprintf(prn, "\nFGLS pooled error variance = %g\n",
			mdl.ess / mdl.nobs);
	    }
	} else {
	    /* one-step FGLS */
	    break;
	} 
    }

    if (!mdl.errcode) {
	if (!(opt & OPT_A)) {
	    set_model_id(&mdl);
	}
	gretl_model_set_int(&mdl, "n_included_units", effn);
	gretl_model_set_int(&mdl, "unit_weights", 1);
	mdl.nwt = 0;

	if (opt & OPT_T) {
	    gretl_model_set_int(&mdl, "iters", iter);
	    ml_hetero_test(&mdl, s2, uvar, nunits, unit_obs);
	    unit_error_variances(uvar, &mdl, pdinfo, nunits, T, unit_obs);
	    mdl.lnL = real_ll(&mdl, uvar, nunits, unit_obs);
	    if (opt & OPT_V) {
		pputc(prn, '\n');
	    }
	} else {
	    unit_error_variances(uvar, &mdl, pdinfo, nunits, T, unit_obs);
	    W = wald_hetero_test(&mdl, pdinfo, s2, uvar, T, 
				 nunits, unit_obs);
	}

	if (!na(W)) {
	    print_wald_test(W, nunits, unit_obs, prn);
	}
    }    

 bailout:

    free(unit_obs);
    free(uvar);
    free(wlist);
    free(bvec);

    dataset_drop_vars(pdinfo->v - orig_v, pZ, pdinfo);
    
    return mdl;
}

/* .................................................................. */

static void panel_copy_var (double **targZ, DATAINFO *targinfo, int targv,
			    double *src, DATAINFO *srcinfo, int srcv,
			    int order)
{
    int t, j = 0;

    for (t=srcinfo->t1; t<=srcinfo->t2; t++) {
	if (t % srcinfo->pd >= order) {
	    targZ[targv][j++] = src[t];
	} 
    }

    if (srcv == -1) {
	strcpy(targinfo->varname[targv], "uhat");
	strcpy(VARLABEL(targinfo, targv), _("residual"));
    } else {
	strcpy(targinfo->varname[targv], srcinfo->varname[srcv]);
	strcpy(VARLABEL(targinfo, targv), VARLABEL(srcinfo, srcv));
    }
}

static void make_reduced_data_info (DATAINFO *targ, DATAINFO *src, int order)
{
    targ->pd = src->pd - order;
    ntodate(targ->stobs, src->t1 + order, src);
    targ->sd0 = obs_str_to_double(targ->stobs); 
    targ->structure = src->structure;
}

static void panel_lag (double **tmpZ, DATAINFO *tmpinfo, 
		       double *src, DATAINFO *srcinfo, 
		       int v, int order, int lag)
{
    int t, j = 0;

    for (t=srcinfo->t1; t<=srcinfo->t2; t++) {
	if (t % srcinfo->pd >= order) {
	    tmpZ[v][j++] = src[t - lag];
	}
    }

    sprintf(tmpinfo->varname[v], "uhat_%d", lag);
    *VARLABEL(tmpinfo, v) = 0;
}

/* - do some sanity checks
   - create a local copy of the required portion of the data set,
     skipping the obs that will be missing
   - copy in the lags of uhat
   - estimate the aux model
   - destroy the temporary data set
*/

int panel_autocorr_test (MODEL *pmod, int order, 
			 double **Z, DATAINFO *pdinfo, 
			 PRN *prn, GRETLTEST *test)
{
    int *aclist;
    double **tmpZ;
    DATAINFO *tmpinfo;
    MODEL aux;
    double trsq, LMF;
    int i, nv, nunits, nobs, err = 0;
    int sn = pdinfo->t2 - pdinfo->t1 + 1;

    /* basic checks */
    if (order <= 0) order = 1;
    if (order > pdinfo->pd - 1) return E_DF;
    if (pmod->ncoeff + order >= sn) return E_DF;

    if (pdinfo->structure != STACKED_TIME_SERIES ||
	!balanced_panel(pdinfo)) { 
        return E_DATA;
    }

    if (pmod->missmask != NULL) {
	return E_MISSDATA;
    }

    /* get number of cross-sectional units */
    nunits = sn / pdinfo->pd;

    /* we lose "order" observations for each unit */
    nobs = sn - nunits * order;

    /* the required number of variables */
    nv = pmod->list[0] + order;

    /* create temporary reduced dataset */
    tmpinfo = create_new_dataset(&tmpZ, nv, nobs, 0);
    if (tmpinfo == NULL) return E_ALLOC;

    make_reduced_data_info(tmpinfo, pdinfo, order);

#if PDEBUG
    fprintf(stderr, "Created data set, n=%d, pd=%d, vars=%d, stobs='%s'\n", 
	    tmpinfo->n, tmpinfo->pd, tmpinfo->v, tmpinfo->stobs);
#endif

    /* allocate the auxiliary regression list */
    aclist = malloc((nv + 1) * sizeof *aclist);
    if (aclist == NULL) {
	err = E_ALLOC;
    } 

    if (!err) {
	int k;

	aclist[0] = pmod->list[0] + order;
	/* copy model uhat to position 1 in temp data set */
	aclist[1] = 1;
	panel_copy_var(tmpZ, tmpinfo, 1,
		       &pmod->uhat[0], pdinfo, -1,
		       order);
	/* copy across the original indep vars, making
	   the new regression list while we're at it */
	k = 2;
	for (i=2; i<=pmod->list[0]; i++) {
	    if (pmod->list[i] == 0) { /* the constant */
		aclist[i] = 0;
	    } else {
		aclist[i] = k;
		panel_copy_var(tmpZ, tmpinfo, k, 
			       &Z[pmod->list[i]][0], pdinfo, pmod->list[i], 
			       order);
		k++;
	    }
	}
    }

    if (!err) {
	int v = pmod->list[0] - 1;

	/* add lags of uhat to temp data set */
	for (i=1; i<=order; i++) {
	    panel_lag(tmpZ, tmpinfo, &pmod->uhat[0], 
		      pdinfo, v + i, order, i);
	    aclist[v + i + 1] = v + i;
	}
    }

    if (!err) {
	aux = lsq(aclist, &tmpZ, tmpinfo, OLS, OPT_A, 0.0);
	err = aux.errcode;
	if (err) {
	    errmsg(aux.errcode, prn);
	}
    } 

    if (!err) {
	aux.aux = AUX_AR;
	aux.order = order;
	printmodel(&aux, tmpinfo, OPT_NONE, prn);
	trsq = aux.rsq * aux.nobs;
	LMF = (aux.rsq/(1.0 - aux.rsq)) * 
	    (aux.nobs - pmod->ncoeff - order)/order; 

	pprintf(prn, "\n%s: LMF = %f,\n", _("Test statistic"), LMF);
	pprintf(prn, "%s = P(F(%d,%d) > %g) = %.3g\n", _("with p-value"), 
		order, aux.nobs - pmod->ncoeff - order, LMF,
		fdist(LMF, order, aux.nobs - pmod->ncoeff - order));

	pprintf(prn, "\n%s: TR^2 = %f,\n", 
		_("Alternative statistic"), trsq);
	pprintf(prn, "%s = P(%s(%d) > %g) = %.3g\n\n", 	_("with p-value"), 
		_("Chi-square"), order, trsq, chisq(trsq, order));

	if (test != NULL) {
	    strcpy(test->type, N_("LM test for autocorrelation up to order %s"));
	    strcpy(test->h_0, N_("no autocorrelation"));
	    sprintf(test->param, "%d", order);
	    test->teststat = GRETL_TEST_LMF;
	    test->dfn = order;
	    test->dfd = aux.nobs - pmod->ncoeff - order;
	    test->value = LMF;
	    test->pvalue = fdist(LMF, test->dfn, test->dfd);
	}
    }

    free(aclist);
    clear_model(&aux); 

    free_Z(tmpZ, tmpinfo);
    clear_datainfo(tmpinfo, CLEAR_FULL);
    free(tmpinfo);

    return err;
}

int switch_panel_orientation (double **Z, DATAINFO *pdinfo)
{
    int sts = (pdinfo->structure == STACKED_TIME_SERIES);
    double **tmpZ;
    int i, j, k, t, nvec;
    int pd = pdinfo->pd;
    int nblocks = pdinfo->n / pd;
    double pdx;
    char **markers = NULL;

    tmpZ = malloc((pdinfo->v - 1) * sizeof *tmpZ);
    if (tmpZ == NULL) {
	return E_ALLOC;
    }

    /* allocate temporary data matrix */
    j = 0;
    for (i=1; i<pdinfo->v; i++) {
	if (pdinfo->vector[i]) {
	    tmpZ[j] = malloc(pdinfo->n * sizeof **tmpZ);
	    if (tmpZ[j] == NULL) {
		for (i=0; i<j; i++) {
		    free(tmpZ[i]);
		}
		free(tmpZ);
		return E_ALLOC;
	    }
	    j++;
	} 
    }
    nvec = j;

    /* allocate marker space if relevant */
    if (pdinfo->S != NULL) {
	markers = malloc(pdinfo->n * sizeof *markers);
	if (markers != NULL) {
	    for (t=0; t<pdinfo->n; t++) {
		markers[t] = malloc(OBSLEN);
		if (markers[t] == NULL) {
		    free(markers);
		    markers = NULL;
		    break;
		} else {
		    strcpy(markers[t], pdinfo->S[t]);
		}
	    }
	}
    }

    /* copy the data (vectors) across */
    j = 0;
    for (i=1; i<pdinfo->v; i++) {
	if (pdinfo->vector[i]) {
	    for (t=0; t<pdinfo->n; t++) {
		tmpZ[j][t] = Z[i][t];
	    }
	    j++;
	} 
    }

    /* copy the data back in transformed order; also do markers if
       present */
    for (k=0; k<pd; k++) {
	j = 0;
	for (i=1; i<pdinfo->v; i++) {
	    if (!pdinfo->vector[i]) continue;
	    for (t=0; t<nblocks; t++) {
		Z[i][k * nblocks + t] = tmpZ[j][k + pd * t];
	    }
	    j++;
	}
	if (markers != NULL) {
	    for (t=0; t<nblocks; t++) {
		strcpy(pdinfo->S[k * nblocks + t], markers[k + pd * t]);
	    }
	}
    }

    /* change the datainfo setup */
    pdinfo->structure = (sts)? STACKED_CROSS_SECTION : STACKED_TIME_SERIES;
    pdinfo->pd = nblocks;

    pdinfo->sd0 = 1.0;
    pdx = 0.1;
    while (nblocks /= 10) {
	pdx *= 0.1;
    }
    pdinfo->sd0 += pdx;

    ntodate(pdinfo->stobs, 0, pdinfo);
    ntodate(pdinfo->endobs, pdinfo->n - 1, pdinfo);

    /* clean up */

    for (i=0; i<nvec; i++) {
	free(tmpZ[i]);
    }
    free(tmpZ);

    if (markers != NULL) {
	for (t=0; t<pdinfo->n; t++) {
	    free(markers[t]);
	}
	free(markers);
    }

    return 0;
}

static int 
varying_vars_list (const MODEL *pmod, const double **Z, const DATAINFO *pdinfo,
		   diagnostics_t *diag)
{
    int i, j, k, t;
    int bigt;

    diag->vlist = malloc((pmod->list[0] + 1) * sizeof *diag->vlist);
    if (diag->vlist == NULL) {
	return E_ALLOC;
    }

    diag->vlist[0] = 1;
    diag->vlist[1] = pmod->list[1];

    k = 2;

    for (j=2; j<=pmod->list[0]; j++) {
	int vj = pmod->list[j];
	int varies = 0;

	if (vj == 0) {
	    diag->vlist[k++] = 0;
	    diag->vlist[0] += 1;
	    continue;
	}

	if (diag->opt & OPT_T) {
	    /* doing time effects */
	    for (t=0; t<diag->T; t++) { 
		int started = 0;
		double xval = NADBL;

		for (i=0; i<diag->nunits; i++) {
		    if (diag->unit_obs[i] == 0) {
			continue;
		    }
		    bigt = panel_index(i, t);
		    if (na(pmod->uhat[bigt])) {
			continue;
		    }
		    if (!started) {
			xval = Z[vj][bigt];
			started = 1;
		    } else if (Z[vj][bigt] != xval) {
			varies = 1;
			break;
		    }
		}
		if (varies) break;
	    }
	} else {
	    for (i=0; i<diag->nunits; i++) { 
		int started = 0;
		double xval = NADBL;

		if (diag->unit_obs[i] == 0) {
		    continue;
		}

		for (t=0; t<diag->T; t++) {
		    bigt = panel_index(i, t);
		    if (na(pmod->uhat[bigt])) {
			continue;
		    }
		    if (!started) {
			xval = Z[vj][bigt];
			started = 1;
		    } else if (Z[vj][bigt] != xval) {
			varies = 1;
			break;
		    }
		}
		if (varies) break;
	    }
	}

	if (varies) {
	    diag->vlist[k++] = vj;
	    diag->vlist[0] += 1;
	} else {
	    fprintf(stderr, "Variable %d '%s' is %s-invariant\n",
		    vj, pdinfo->varname[vj], (diag->opt & OPT_T)? 
		    "unit" : "time");
	} 
    }

#if PDEBUG
    printlist(pmod->list, "original regressors");
    printlist(diag->vlist, "varying regressors");
#endif

    return 0;
}


