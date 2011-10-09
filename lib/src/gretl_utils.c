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

#include "libgretl.h"
#include "monte_carlo.h"
#include "gretl_func.h"
#include "objstack.h"
#include "cmd_private.h"
#include "libset.h"
#include "usermat.h"
#include "gretl_panel.h"
#include "gretl_scalar.h"
#include "gretl_bundle.h"
#include "gretl_string_table.h"
#include "gretl_xml.h"
#include "forecast.h"
#include "kalman.h"

#ifdef USE_RLIB
# include "gretl_foreign.h"
#endif

#include <errno.h>

#ifndef WIN32
# include <signal.h>
# include <glib.h>
#endif

static void gretl_tests_cleanup (void);

/**
 * date:
 * @nt: observation number (zero-based).
 * @pd: data periodicity or frequency.
 * @sd0: floating point representation of starting date.
 *
 * Returns: the date corresponding to @nt, as a double-precision number.
 */

double date (int nt, int pd, const double sd0)
{
    int ysd = (int) sd0, yy, pp, yp;
    int p10 = 10;

    if (pd == 1) {
	return (double) (ysd + nt);  
    } 

    pp = pd;
    while ((pp = pp / 10)) {
	p10 *= 10;
    }

    pp = nt % pd + p10 * (sd0 - ysd) + .5;
    if (pp != pd)  {
        yy = ysd + nt/pd  + pp/pd + .5;
        yp = pp % pd;
    }  else {
        yy = ysd + nt/pd + .5;
        yp = pp;
    }

    return yy + (double) yp / p10;
}

/**
 * ijton:
 * @i: row number (0-based)
 * @j: column number (0-based)
 * @nrows: number of rows (and columns) in symmetric matrix.
 *
 * Given a (row, column) reference into a symmetric 2-dimensional 
 * matrix A, finds the index into a 1-dimensional array x
 * composed of the non-redundant (lower) elements of A.
 *
 * E.g. for the 3 x 3 case with 6 non-redundant elements, 0 to 5,
 *
 *    A(0,0) = x[0]  A(0,1) = x[1]  A(0,2) = x[2]
 *    A(1,0) = x[1]  A(1,1) = x[3]  A(1,2) = x[4]
 *    A(2,0) = x[2]  A(2,1) = x[4]  A(2,1) = x[5]
 *
 * Returns: 0-based index into flat array.
 */

int ijton (int i, int j, int nrows)
{
    if (i > j) {
	int tmp = i;

	i = j;
	j = tmp;
    }

    return nrows * i + j - i - ((i - 1) * i / 2);
}

/**
 * transcribe_array:
 * @targ: arrat to which to write.
 * @src: array from which to read.
 * @dset: data information struct.
 * 
 * Copy from @src to @targ, skipping any missing values,
 * over the sample range defined in @dset.
 *
 * Returns: the number of valid observations put into @targ.
 */

int transcribe_array (double *targ, const double *src, 
		      const DATASET *dset) 
{
    int t, n = 0;

    for (t=dset->t1; t<=dset->t2; t++) {
	if (!na(src[t])) {
	    targ[n++] = src[t];
	}
    }

    return n;
}

/**
 * gretl_isdummy:
 * @t1: starting observation.
 * @t2: ending observation. 
 * @x: data series to examine.
 * 
 * Check whether series @x has only 0 or 1 values over the
 * given sample range (or possibly missing values).
 *
 * Returns: 0 if the variable is not a 0/1 dummy, otherwise the
 * number of 1s in the series.
 */

int gretl_isdummy (int t1, int t2, const double *x)
{
    int t, m = 0, goodobs = 0;

    for (t=t1; t<=t2; t++) {
	if (na(x[t])) {
	    continue;
	}
	if (x[t] != 0.0 && x[t] != 1.0) {
	    return 0;
	}
	if (x[t] == 1.0) {
	    m++;
	}
	goodobs++;
    }

    if (m < goodobs) {
	return m;
    }

    return 0;
} 

/**
 * gretl_iszero:
 * @x: data series to examine.
 * @t1: starting observation.
 * @t2: ending observation. 
 * 
 * Check whether series @x has only zero values over the
 * given sample range (or possibly missing values).
 *
 * Returns: 1 if the variable is all zeros, otherwise 0.
 */

int gretl_iszero (int t1, int t2, const double *x)
{
    double sum = 0.0;
    int t;

    for (t=t1; t<=t2; t++) {
	if (!na(x[t])) {
	    sum += x[t] * x[t];
	}
    }

    return floateq(sum, 0.0);
}

/**
 * gretl_isconst:
 * @x: data series to examine.
 * @t1: starting observation.
 * @t2: ending observation. 
 * 
 * Check whether series @x is constant over the
 * given sample range (aside from any missing values).
 *
 * Returns: 1 if the variable is constant, otherwise 0.
 */

int gretl_isconst (int t1, int t2, const double *x)
{
    int t, ret = 1;

    while (na(x[t1]) && t1 <= t2) {
	t1++;
    }

    for (t=t1+1; t<=t2; t++) {
	if (na(x[t])) {
	    continue;
	}
	if (floatneq(x[t], x[t1])) {
	    ret = 0;
	    break;
	}
    }

    return ret;
}

/**
 * gretl_isunits:
 * @x: data series to examine.
 * @t1: starting observation.
 * @t2: ending observation. 
 * 
 * Check whether series @x equals 1 over the
 * given sample range (aside from any missing values).
 *
 * Returns: 1 if so, otherwise 0.
 */

int gretl_isunits (int t1, int t2, const double *x)
{
    int t, ret = 1;

    for (t=t1; t<=t2; t++) {
	if (!na(x[t]) && x[t] != 1.0) {
	    ret = 0;
	    break;
	}
    }

    return ret;
}

/**
 * gretl_isint:
 * @x: data series to examine.
 * @t1: starting observation.
 * @t2: ending observation. 
 * 
 * Check whether series @x contains only integer values over
 * the given sample range (aside from any missing values).
 *
 * Returns: 1 if so, otherwise 0.
 */

int gretl_isint (int t1, int t2, const double *x)
{
    int t, ret = 1;

    for (t=t1; t<=t2; t++) {
	if (!na(x[t]) && x[t] != floor(x[t])) {
	    ret = 0;
	    break;
	}
    }

    return ret;
}

/**
 * gretl_iscount:
 * @x: data series to examine.
 * @t1: starting observation.
 * @t2: ending observation. 
 * 
 * Check whether series @x contains nothing but non-negative
 * integer values (some of which are > 1) over the 
 * given sample range.
 *
 * Returns: 1 if so, otherwise 0.
 */

int gretl_iscount (int t1, int t2, const double *x)
{
    int t, xi;
    int g1 = 0;
    int ret = 1;

    for (t=t1; t<=t2; t++) {
	if (na(x[t])) {
	    continue;
	}
	if (x[t] < 0.0) {
	    ret = 0;
	    break;
	}
	xi = x[t];
	if (x[t] != (double) xi) {
	    ret = 0;
	    break;
	}
	if (x[t] > 1.0) {
	    g1 = 1;
	}
    }

    if (g1 == 0) {
	ret = 0;
    }

    return ret;
}

#define FEWVALS 32

static int few_vals (int t1, int t2, const double *x, double *ratio)
{
    double test[FEWVALS];
    int match;
    int i, t, n = 0, nv = 0;

    for (t=t1; t<=t2; t++) {
	if (!na(x[t])) {
	    match = 0;
	    for (i=0; i<nv; i++) {
		if (x[t] == test[i]) {
		    match = 1;
		    break;
		}
	    }
	    if (!match) {
		if (nv == FEWVALS) {
		    nv++;
		    break;
		}
		test[nv++] = x[t];
	    }
	    n++;
	}
    }

    *ratio = (double) nv / n;

    return nv;
}

 /**
 * gretl_isdiscrete:
 * @x: data series to examine.
 * @t1: starting observation.
 * @t2: ending observation. 
 *
 * Checks the variable @x over the range @t1 to @t2 for discreteness.
 * This is a heuristic whose components are (a) whether the values
 * are "fairly round" (multiples of 0.25) or not, and, if test (a) is
 * passed, (b) whether the variable takes on only "few" distinct
 * values.
 * 
 * Returns: 0 if test (a) is not passed or the number of distinct values
 * is > 32; else 1 if the number of distinct values is <= 32; else 2 if 
 * the number of distinct values is <= 4.  A return of 1 is supposed
 * to indicate that it's "reasonable" to treat @x as discrete, while
 * a return of 2 indicates that it's probably ureasonable _not_ to
 * treat @x as discrete, for the purpose of drawing up a frequency
 * distribution.
 */

int gretl_isdiscrete (int t1, int t2, const double *x)
{
    int t, n = 0, d = 1;
    double r;

    for (t=t1; t<=t2; t++) {
	if (na(x[t])) {
	    continue;
	}
	n++;
	if (!ok_int(x[t])) {
	    d = 0;
	    break;
	}
	r = x[t] - floor(x[t]);
	if (r != 0.0 && r != 0.25 && r != 0.5 && r != 0.75) {
	    d = 0;
	    break;
	}	    
    }

    if (n == 0) {
	d = 0;
    }

    if (d) {
	n = few_vals(t1, t2, x, &r);
	if (n > FEWVALS) {
	    d = 0;
	} else if (r > 0.9 && n > 30) {
	    /* somewhat arbitrary: but if r (= ratio of distinct
	       values to number of cases) is "too high", and the
	       number of observations is not tiny, perhaps we should
	       not take the var as discrete
	    */
	    d = 0;
	} else if (n < 5) {
	    d = 2;
	}
    }

    return d;
}

 /**
 * gretl_is_oprobit_ok:
 * @x: data series to examine.
 * @t1: starting observation.
 * @t2: ending observation. 
 *
 * Checks the variable @x over the range @t1 to @t2 for its
 * suitability as the dependent variable in an ordered probit
 * analysis.  The criterion used is that the variable has 
 * only non-negative integer values.
 * 
 * Returns: 1 if the test succeeds, otherwise 0.
 */

int gretl_is_oprobit_ok (int t1, int t2, const double *x)
{
    int t, n = 0, d = 1;

    for (t=t1; t<=t2; t++) {
	if (na(x[t])) {
	    continue;
	}
	n++;
	if (x[t] != floor(x[t]) || x[t] < 0) {
	    d = 0;
	    break;
	}
    }

    return (d > 0 && n > 0);
}

/**
 * true_const:
 * @v: index number of variable to test.
 * @dset: dataset struct. 
 * 
 * Check whether variable Z[v] equals 1 over the sample
 * range given in @dset (aside from any missing values).
 *
 * Returns: 1 if so, otherwise 0.
 */

int true_const (int v, const DATASET *dset)
{
    if (v < 0 || v >= dset->v) {
	return 0;
    }

    return gretl_isunits(dset->t1, dset->t2, dset->Z[v]);
}

/**
 * gretl_compare_doubles:
 * @a: pointer to first element to compare.
 * @b: pointer to second element to compare.
 *
 * Comparison function for use with qsort.  Sorts doubles in
 * ascending order.
 * 
 * Returns: appropriate value for qsort.
 */

int gretl_compare_doubles (const void *a, const void *b)
{
    const double *da = (const double *) a;
    const double *db = (const double *) b;
     
    return (*da > *db) - (*da < *db);
}

/**
 * gretl_inverse_compare_doubles:
 * @a: pointer to first element to compare.
 * @b: pointer to second element to compare.
 * 
 * Comparison function for use with qsort.  Sorts doubles in
 * descending order.
 * 
 * Returns: appropriate value for qsort.
 */

int gretl_inverse_compare_doubles (const void *a, const void *b)
{
    const double *da = (const double *) a;
    const double *db = (const double *) b;

    return (*da < *db) - (*da > *db);
}

/**
 * gretl_compare_ints:
 * @a: pointer to first element to compare.
 * @b: pointer to second element to compare.
 *
 * Comparison function for use with qsort.  Sorts integers in
 * ascending order.
 * 
 * Returns: appropriate value for qsort.
 */

int gretl_compare_ints (const void *a, const void *b)
{
    const int *ia = (const int *) a;
    const int *ib = (const int *) b;
     
    return *ia - *ib;
}

/**
 * count_distinct_values:
 * @x: sorted array of doubles.
 * @n: number of elements in array.
 *
 * Returns: the number of distinct values in array @x,
 * provided that @x is already sorted.
 */

int count_distinct_values (const double *x, int n)
{
    int i, c = 1;

    for (i=1; i<n; i++) {
	if (x[i] != x[i-1]) {
	    c++;
	}
    }

    return c;
}

/**
 * count_distinct_int_values:
 * @x: sorted array of ints.
 * @n: number of elements in array.
 *
 * Returns: the number of distinct values in array @x,
 * provided that @x is already sorted.
 */

int count_distinct_int_values (const int *x, int n)
{
    int i, c = 1;

    for (i=1; i<n; i++) {
	if (x[i] != x[i-1]) c++;
    }

    return c;
}

/**
 * rearrange_id_array:
 * @x: sorted array of doubles.
 * @m: number of distinct values in array.
 * @n: number of elements in array.
 *
 * Rearranges the sorted array @x such that the first @m 
 * elements contain the @m distinct values in sorted order.
 *
 * Returns: 0 on success, 1 on error (in case @m is greater
 * than @n).
 */

int rearrange_id_array (double *x, int m, int n)
{
    int i, k = 1;

    if (m >= n || m == 1) {
	return 1;
    }

    for (i=1; i<n && k<m; i++) {
	if (x[i] != x[i-1]) {
	    x[k++] = x[i];
	}
    }

    return 0;
}

/**
 * printlist:
 * @list: array of integers.
 * @msg: message to print along with @list (or NULL).
 * 
 * Prints to stderr the given @list of integers along with a message.
 */

void printlist (const int *list, const char *msg)
{
    int i;

    if (msg) {
	fprintf(stderr, "%s:\n", msg);
    } else {
	fprintf(stderr, "list: ");
    }

    if (list == NULL) {
	fputs("list is NULL", stderr);
    } else {
	fprintf(stderr, "%d : ", list[0]);
	for (i=1; i<=list[0]; i++) {
	    if (list[i] == LISTSEP) {
		fputs("; ", stderr);
	    } else {
		fprintf(stderr, "%d ", list[i]);
	    }
	}
    }

    fputc('\n', stderr);
}

/**
 * gretl_calculate_criteria:
 * @ess: error sum of squares.
 * @n: number of observations.
 * @k: number of parameters estimated.
 * @ll: pointer to recieve loglikelihood.
 * @aic: pointer to recieve Akaike criterion.
 * @bic: pointer to recieve Schwarz Bayesian criterion.
 * @hqc: pointer to recieve Hannan-Quinn criterion.
 *
 * Calculates model selection criteria based on @ess, @nobs and
 * @k, for a model estimated via least squares.
 *
 * Returns: 0 on success, non-zero on error.
 */

int gretl_calculate_criteria (double ess, int n, int k,
			      double *ll, double *aic, double *bic,
			      double *hqc)
{
    double lnl, c[3];
    int err = 0;

    if (na(ess) || ess <= 0.0 || k < 1 || n <= k) {
	err = 1;
    } else {
	const double ln2pi1 = 2.837877066409345; /* log(2*pi) + 1 */

	errno = 0;

	lnl = -.5 * n * log(ess);

	if (errno == EDOM || errno == ERANGE) {
	    err = 1;
	} else {
	    lnl += -.5 * n * (ln2pi1 - log((double) n));
	    c[0] = -2.0 * lnl + 2 * k;
	    c[1] = -2.0 * lnl + k * log(n);
	    c[2] = -2.0 * lnl + 2 * k * log(log(n));
	}
    }

    if (err) {
	*ll = NADBL;
	*aic = NADBL;
	*bic = NADBL;
	*hqc = NADBL;
    } else {
	*ll = lnl;
	*aic = c[0];
	*bic = c[1];
	*hqc = c[2];
    }	

    return err;
}

/**
 * ls_criteria:
 * @pmod: pointer to gretl model structure.
 *
 * Fills out the model selection criteria members of @pmod, using
 * gretl_calculate_criteria().
 *
 * Returns: 0 on success, non-zero on error.
 */

int ls_criteria (MODEL *pmod)
{
    double ll, aic, bic, hqc;
    int err;

    err = gretl_calculate_criteria(pmod->ess, pmod->nobs, pmod->ncoeff,
				   &ll, &aic, &bic, &hqc);

    pmod->lnL = ll;
    pmod->criterion[C_AIC] = aic;
    pmod->criterion[C_BIC] = bic;
    pmod->criterion[C_HQC] = hqc;

    return err;
}

static char *
real_format_obs (char *obs, int maj, int min, int pd, char sep)
{
    if (pd >= 10) {
	int pdp = pd / 10, minlen = 2;
	char fmt[16];

	while ((pdp = pdp / 10)) minlen++;
	sprintf(fmt, "%%d%c%%0%dd", sep, minlen);
	sprintf(obs, fmt, maj, min);
    } else {
	sprintf(obs, "%d%c%d", maj, sep, min);
    }

    return obs;
}

/**
 * format_obs:
 * @obs: target string (should be of length #OBSLEN).
 * @maj: major period (e.g. year).
 * @min: minor period (e.g. quarter, month).
 * @pd: data frequency.
 *
 * Prints to @obs the gretl-type date string representing 
 * the observation given by @maj, @min and @pd.
 *
 * Returns: @obs.
 */

char *format_obs (char *obs, int maj, int min, int pd)
{
    return real_format_obs(obs, maj, min, pd, ':');
}

static int get_stobs_maj_min (char *stobs, int structure,
			      int *maj, int *min)
{
    int dotc = 0;
    char *p = stobs;
    int err = 0;

    while (*p) {
	if (*p == ':') {
	    *p = '.';
	    dotc++;
	} else if (*p == '.') {
	    dotc++;
	} else if (!isdigit((unsigned char) *p)) {
	    err = 1;
	    break;
	}
	p++;
    }

    if (!err) {
	if (dotc > 1 || *stobs == '.' || 
	    stobs[strlen(stobs) - 1] == '.') {
	    err = 1;
	}
    }

    if (!err) {
	if (dotc > 0) {
	    sscanf(stobs, "%d.%d", maj, min);
	    if (*maj <= 0 || *min <= 0) {
		err = 1;
	    }
	} else {
	    sscanf(stobs, "%d", maj);
	    if (*maj <= 0 && structure != SPECIAL_TIME_SERIES) {
		err = 1;
	    }
	}
    }

    return err;
}

static int 
catch_setobs_errors (const char *stobs, int pd, int n, int min, gretlopt opt)
{
    int err = 0;

    if (pd == 1) {
	if (min > 0) {
	    gretl_errmsg_set(_("no ':' allowed in starting obs with "
			       "frequency 1"));
	    err = 1;
	} else if (opt == OPT_S || opt == OPT_C) {
	    gretl_errmsg_set(_("panel data must have frequency > 1"));
	    err = 1;
	}
    } else {
	if (min == 0) {
	    gretl_errmsg_set(_("starting obs must contain a ':' with "
			       "frequency > 1"));
	    err = 1;
	} else if (min > pd) {
	    gretl_errmsg_sprintf(_("starting obs '%s' is incompatible with frequency"), 
				 stobs);
	    err = 1;
	} else if (opt == OPT_X) {
	    gretl_errmsg_set(_("cross-sectional data: frequency must be 1"));
	    err = 1;
	} else if (n % pd != 0) {
	    if (opt == OPT_S || opt == OPT_C) {
		gretl_errmsg_sprintf(_("Panel datasets must be balanced.\n"
				       "The number of observations (%d) is not a multiple\n"
				       "of the number of %s (%d)."), 
				     n, ((opt == OPT_S)? _("periods") : _("units")), pd);
		err = 1;
	    }
	}
    }

    return err;
}

#define recognized_ts_frequency(f) (f == 4 || f == 12 || f == 24)

/**
 * set_obs:
 * @line: command line.
 * @dset: dataset struct.
 * @opt: %OPT_S for stacked time-series, %OPT_C for stacked cross-section,
 * %OPT_T for time series, %OPT_X for cross section, %OPT_P to set
 * panel structure via two variables representing unit and period
 * respectively.
 * 
 * Set the frequency and initial observation for a dataset.
 *
 * Returns: 0 on successful completion, 1 on error.
 */

int set_obs (const char *line, DATASET *dset, gretlopt opt)
{
    char pdstr[VNAMELEN];
    char stobs[OBSLEN];
    int structure = STRUCTURE_UNKNOWN;
    double sd0 = dset->sd0;
    int pd, dated = 0;
    int err = 0;

    if (dset == NULL) {
	return E_NODATA;
    }

    if ((opt & (OPT_R | OPT_P)) && dset->Z == NULL) {
	return E_NODATA;
    }

    gretl_error_clear();

    if (opt & OPT_R) {
	/* restructure panel: "hidden" option */
	return switch_panel_orientation(dset);
    }    

    if (!strcmp(line, "setobs")) {
	/* we'll just print current obs info */
	return 0;
    }

    if (opt & OPT_P) {
	return set_panel_structure_from_line(line, dset);
    }

    /* now we get down to business */

    if (sscanf(line, "%*s %15s %10s", pdstr, stobs) != 2) {
	gretl_errmsg_set(_("Failed to parse line as frequency, startobs"));
	return 1;
    }

    pd = gretl_int_from_string(pdstr, &err);
    if (err) {
	return err;
    }

    /* truncate stobs if not a calendar date */
    if (strchr(stobs, '/') != NULL) {
	dated = 1;
    } else {
	stobs[8] = '\0';
    }

    /* does the supplied frequency make sense? */
    if (pd < 1 || (dset->n > 0 && pd > dset->n && opt != OPT_T)) {
	gretl_errmsg_sprintf(_("frequency (%d) does not make seem to make sense"), pd);
	return 1;
    }

    /* if an explicit structure option was passed in, respect it */
    if (opt == OPT_X) {
	structure = CROSS_SECTION;
    } else if (opt == OPT_T) {
	structure = TIME_SERIES;
    } else if (opt == OPT_S) {
	structure = STACKED_TIME_SERIES;
    } else if (opt == OPT_C) {
	structure = STACKED_CROSS_SECTION;
    } else if (opt == OPT_N) {
	structure = SPECIAL_TIME_SERIES;
    }

    if (dated) {
	if (opt == OPT_X || opt == OPT_S || opt == OPT_C) {
	    gretl_errmsg_sprintf(_("starting obs '%s' is invalid"), stobs);
	    return 1;
	}

	if (pd == 5 || pd == 6 || pd == 7 || pd == 52) {
	    /* calendar-dated data, daily or weekly */
	    double ed0 = get_epoch_day(stobs);

	    if (ed0 < 0) {
		gretl_errmsg_sprintf(_("starting obs '%s' is invalid"), stobs);
		return 1;
	    }

	    sd0 = ed0;
	    structure = TIME_SERIES;

	    /* replace any existing markers with date strings */
	    dataset_destroy_obs_markers(dset);
	} else {
	    gretl_errmsg_sprintf(_("starting obs '%s' is invalid"), stobs);
	    return 1;
	}
    } else if (structure == TIME_SERIES && pd == 10) {
	/* decennial data */
	sd0 = (double) atoi(stobs);
    } else {
	int maj = 0, min = 0;

	if (get_stobs_maj_min(stobs, structure, &maj, &min)) {
	    gretl_errmsg_sprintf(_("starting obs '%s' is invalid"), stobs);
	    return 1;
	}

	if ((pd == 5 || pd == 6 || pd == 7 || pd == 52)  
	    && min == 0 && opt != OPT_X && opt != OPT_S && opt != OPT_C) {
	    /* catch undated daily or weekly data */
	    structure = TIME_SERIES;
	} else {
	    if (catch_setobs_errors(stobs, pd, dset->n, min, opt)) {
		return 1;
	    }
	    if (pd == 1) {
		sprintf(stobs, "%d", maj);
		if (structure == STRUCTURE_UNKNOWN) {
		    if (maj > 1) {
			structure = TIME_SERIES; /* annual? */
		    } else {
			structure = CROSS_SECTION;
		    }
		}
	    } else {
		if (structure == TIME_SERIES && min > 0 &&
		    !recognized_ts_frequency(pd)) {
		    structure = SPECIAL_TIME_SERIES;
		}
		real_format_obs(stobs, maj, min, pd, '.');
		if (structure == STRUCTURE_UNKNOWN && 
		    recognized_ts_frequency(pd)) {
		    structure = TIME_SERIES;
		}
	    }
	}

	/* for non-calendar data */
	sd0 = dot_atof(stobs);
    }

    dset->pd = pd;
    dset->structure = structure;
    dset->sd0 = sd0;

    ntodate(dset->stobs, 0, dset); 
    ntodate(dset->endobs, dset->n - 1, dset);

    /* pre-process stacked cross-sectional panels: put into canonical
       stacked time series form
    */
    if (dset->structure == STACKED_CROSS_SECTION) {
	if (dset->Z == NULL) {
	    err = E_NODATA;
	} else {
	    err = switch_panel_orientation(dset);
	}
    }	

    return err;
}

/**
 * gretl_double_from_string:
 * @s: string to examine.
 * @err: location to receive error code.
 * 
 * If @s is a valid string representation of a double,
 * return that integer, otherwise if @s is the name of a
 * scalar variable, return the value of that variable,
 * otherwise set the content of @err to a non-zero value.
 *
 * Returns: double value.
 */

double gretl_double_from_string (const char *s, int *err)
{
    char *test;
    double x;

    if (s == NULL || *s == 0) {
	*err = E_DATA;
	return NADBL;
    }

    errno = 0;

    x = strtod(s, &test);

    if (errno == ERANGE) {
	*err = E_DATA;
	errno = 0;
	return NADBL;
    }

    if (*test == '\0') {
	return x;
    }

    if (gretl_is_scalar(s)) {
	x = gretl_scalar_get_value(s);
    } else {
	*err = E_DATA;
	x = NADBL;
    } 

    return x;    
}

/**
 * gretl_int_from_string:
 * @s: string to examine.
 * @err: location to receive error code.
 * 
 * If @s is a valid string representation of an integer,
 * return that integer, otherwise if @s is the name of a
 * scalar variable, return the value of that variable,
 * otherwise set the content of @err to a non-zero value.
 *
 * Returns: integer value.
 */

int gretl_int_from_string (const char *s, int *err)
{
    char *test;
    double x;
    int n = 0;

    if (s == NULL || *s == 0) {
	*err = E_DATA;
	return 0;
    }

    errno = 0;

    n = strtol(s, &test, 10);

    if (errno == ERANGE) {
	*err = E_DATA;
	errno = 0;
	return 0;
    }

    if (*test == '\0') {
	return n;
    } else if (gretl_is_scalar(s)) {
	x = gretl_scalar_get_value(s);
	if (na(x)) {
	    *err = E_MISSDATA;
	} else if (fabs(x) > INT_MAX) {
	    *err = E_DATA;
	} else {
	    n = (int) x;
	}	
    } else {
	*err = E_DATA;
    } 

    return n;    
}

/**
 * positive_int_from_string:
 * @s: string to examine.
 * 
 * If @s is a valid string representation of a positive integer,
 * return that integer, otherwise return -1.
 *
 * Returns: integer value.
 */

int positive_int_from_string (const char *s)
{
    int ret = -1;

    if (s != NULL && *s != '\0') {
	char *test;

	errno = 0;

	ret = strtol(s, &test, 10);
	if (*test != '\0' || !strcmp(s, test) || errno == ERANGE) {
	    ret = -1;
	} 
    }

    return ret;
}

/**
 * varnum_from_string:
 * @str: string representation of an integer ID number.
 * @dset: dataset information.
 * 
 * Returns: integer ID number, or -1 on failure.
 */

int varnum_from_string (const char *str, DATASET *dset)
{
    int v = positive_int_from_string(str);

    if (v <= 0 || v >= dset->v) {
	v = -1;
    } 
    
    return v;
}

/**
 * gretl_type_from_name:
 * @s: the name to check.
 * @dset: dataset information.
 * 
 * Returns: non-zero if @s is the name of an existing
 * series, matrix, scalar, list or string variable,
 * otherwise 0.
 */

GretlType gretl_type_from_name (const char *s, const DATASET *dset)
{
    if (gretl_is_series(s, dset)) {
	return GRETL_TYPE_SERIES;
    } else if (get_matrix_by_name(s) != NULL) {
	return GRETL_TYPE_MATRIX;
    } else if (gretl_is_scalar(s)) {
	return GRETL_TYPE_DOUBLE;
    } else if (get_list_by_name(s)) {
	return GRETL_TYPE_LIST;
    } else if (get_string_by_name(s)) {
	return GRETL_TYPE_STRING;
    } else if (gretl_is_bundle(s)) {
	return GRETL_TYPE_BUNDLE;
    } else {
	return GRETL_TYPE_NONE;
    }
}

/**
 * copyvec:
 * @src: array of doubles.
 * @n: number of elements to copy.
 * 
 * Returns: an allocated copy of the first @n elements of
 * array @src, or %NULL on failure.
 */

double *copyvec (const double *src, int n)
{
    double *targ = NULL;
    int i;

    if (n > 0 && src != NULL) {
	targ = malloc(n * sizeof *targ);
    }

    if (targ != NULL) {
	for (i=0; i<n; i++) {
	    targ[i] = src[i];
	}
    }

    return targ;
}

/**
 * doubles_array_free:
 * @X: 2-dimensional array of doubles.
 * @m: number of sub-arrays.
 *
 * Frees a 2-dimensional array of doubles, first freeing
 * each sub-array.
 */

void doubles_array_free (double **X, int m)
{
    if (X != NULL) {
	int i;

	for (i=0; i<m; i++) {
	    free(X[i]);
	}
	free(X);
    }
}

/**
 * doubles_array_new:
 * @m: number of sub-arrays.
 * @n: length of each sub-array.
 *
 * Allocates a 2-dimensional array of doubles, that is,
 * @m arrays each containing @n elements.  If @n is
 * zero the sub-arrays are just set to %NULL.
 * 
 * Returns: the allocated array, or %NULL on failure.
 */

double **doubles_array_new (int m, int n)
{
    double **X;
    int i;

    if (m == 0) {
	return NULL;
    }

    X = malloc(m * sizeof *X);
    if (X == NULL) {
	return X;
    }

    for (i=0; i<m; i++) {
	X[i] = NULL;
    }

    if (n > 0) {
	for (i=0; i<m; i++) {
	    X[i] = malloc(n * sizeof **X);
	    if (X[i] == NULL) {
		doubles_array_free(X, m);
		X = NULL;
		break;
	    }
	}
    }

    return X;
}

/**
 * doubles_array_new0:
 * @m: number of sub-arrays.
 * @n: length of each sub-array.
 *
 * Works just as doubles_array_new(), except that on
 * successful allocation all values in the arrays are
 * set to zero.
 * 
 * Returns: the allocated array, or %NULL on failure.
 */

double **doubles_array_new0 (int m, int n)
{
    double **X = doubles_array_new(m, n);

    if (X != NULL && n > 0) {
	int i, j;

	for (i=0; i<m; i++) {
	    for (j=0; j<n; j++) {
		X[i][j] = 0.0;
	    }
	}
    }

    return X;
}

/**
 * doubles_array_adjust_length:
 * @X: original two-dimensional array.
 * @m: number of sub-arrays in @X.
 * @new_n: new length of each sub-array.
 * 
 * For each of the @m sub-arrays in @X, reallocate to
 * a length of @new_n.
 * 
 * Returns: 0 on success, non-zero on error.
 */

int doubles_array_adjust_length (double **X, int m, int new_n)
{
    int err = 0;

    if (X == NULL || m == 0) {
	; /* no-op */
    } else {
	double *xi;
	int i;

	for (i=0; i<m && !err; i++) {
	    if (new_n == 0) {
		free(X[i]);
		X[i] = NULL;
	    } else {
		xi = realloc(X[i], new_n * sizeof *xi);
		if (xi == NULL) {
		    err = E_ALLOC;
		} else {
		    X[i] = xi;
		}
	    }
	}
    }

    return err;
}

/**
 * data_array_from_model:
 * @pmod: reference model.
 * @Z: main data array.
 * @missv: should equal 1 if there are missing values to be
 * skipped, else 0.
 *
 * Constructs a dataset containing all the variables referenced in
 * @pmod.  The arrays start at the correct sample offset for @pmod,
 * and are contiguous.  If @missvals equals 0, this is done by creating
 * a set of pointers into the main dataset, but if there are missing
 * values to be handled, the sub-arrays are newly allocated and purged
 * of NAs.
 *
 * Returns: two-dimensional array, or %NULL on failure.
 */

double **data_array_from_model (const MODEL *pmod, double **Z, int missv)
{
    double **X;
    int nv = pmod->list[0];
    int offset = pmod->t1;
    int v, i;

    if (missv) {
	X = doubles_array_new(nv, pmod->nobs);
    } else {
	X = malloc(nv * sizeof *X);
    }

    if (X == NULL) return NULL;

    if (missv) {
	int t, s;

	for (t=0; t<pmod->nobs; t++) {
	    X[0][t] = 1.0;
	}

	for (i=1; i<nv; i++) {
	    v = (i == 1)? pmod->list[1] : pmod->list[i + 1];
	    s = 0;
	    for (t=pmod->t1; t<=pmod->t2; t++) {
		if (!na(pmod->uhat[t])) {
		    X[i][s++] = Z[v][t];
		}
	    }
	}
    } else {
	/* constant in slot 0 */
	X[0] = Z[0] + offset;

	/* dependent var in slot 1 */
	X[1] = Z[pmod->list[1]] + offset;

	/* independent vars in slots 2, 3, ... */
	for (i=2; i<nv; i++) {
	    v = pmod->list[i + 1];
	    X[i] = Z[v] + offset;
	}
    }

    return X;
}

#ifndef WIN32

static int non_fatal (const char *s)
{
    /* 
       "Could not find/open font when opening font X, using default" 
       "gnuplot_x11: Some character sets not available" 
       "Warning: empty y2 range..."
       pango warning for, e.g., FreeSans font w/o GPOS table
       pango error on quartz
    */

    if (strstr(s, "using default") ||
	strstr(s, "trying default") ||
	strstr(s, "character sets not available") ||
	strstr(s, "Warning: empty ") ||
	strstr(s, "Pango-WARNING") ||
	strstr(s, "CGContextSetFont")) {
	return 1;
    } else {
	return 0;
    }
}

int gretl_spawn (char *cmdline)
{
    GError *error = NULL;
    gchar *errout = NULL;
    gchar *sout = NULL;
    int ok, status;
    int ret = 0;

    gretl_error_clear();

    signal(SIGCHLD, SIG_DFL);

    ok = g_spawn_command_line_sync (cmdline,
				    &sout,   /* standard output */
				    &errout, /* standard error */
				    &status, /* exit status */
				    &error);

    if (!ok) {
	gretl_errmsg_set(error->message);
	fprintf(stderr, "gretl_spawn: '%s'\n", error->message);
	g_error_free(error);
	ret = 1;
    } else if (errout && *errout) {
	fprintf(stderr, "stderr: '%s'\n", errout);
	if (!non_fatal(errout)) {
	    gretl_errmsg_set(errout);
	    fprintf(stderr, "gretl_errmsg: '%s'\n", gretl_errmsg_get());
	    ret = 1;
	}
    } else if (status != 0) {
	if (sout != NULL && *sout) {
	    gretl_errmsg_set(sout);
	    fprintf(stderr, "gretl_spawn: status = %d: '%s'\n", status, sout);
	} else {
	    gretl_errmsg_set(_("Command failed"));
	    fprintf(stderr, "gretl_spawn: status = %d\n", status);
	}
	ret = 1;
    }

    if (errout != NULL) g_free(errout);
    if (sout != NULL) g_free(sout);

    if (ret) {
	fprintf(stderr, "Failed command: '%s'\n", cmdline);
    } 

    return ret;
}

#endif /* !WIN32 */

/* file copying */

int gretl_copy_file (const char *src, const char *dest) 
{
    FILE *srcfd, *destfd;
    char buf[8192];
    size_t n;

    if (!strcmp(src, dest)) {
	return 1;
    }
   
    if ((srcfd = gretl_fopen(src, "rb")) == NULL) {
	gretl_errmsg_sprintf(_("Couldn't open %s"), src);
	return 1; 
    }

    if ((destfd = gretl_fopen(dest, "wb")) == NULL) {
	gretl_errmsg_sprintf(_("Couldn't write to %s"), dest);
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

/* handle the case of "delete bundle[key]" */

static int maybe_delete_bundle_value (const char *s, PRN *prn)
{
    char bname[VNAMELEN];
    char key[VNAMELEN];
    int err = 0;

    if (sscanf(s, "%15[^[][%15[^]]", bname, key) == 2) {
	gretl_bundle *bundle;
	const char *s;

	bundle = get_gretl_bundle_by_name(bname);
	if (bundle == NULL) {
	    err = E_UNKVAR;
	} else {
	    if (*key == '"') {
		s = gretl_unquote(key, &err);
	    } else if (gretl_is_string(key)) {
		s = get_string_by_name(key);
	    } else {
		err = E_UNKVAR;
	    }
	}

	if (!err) {
	    err = gretl_bundle_delete_data(bundle, s);
	}
    } else {
	err = E_UNKVAR;
    }

    return err;
}

/* note: this is not used for series */

int gretl_delete_var_by_name (const char *s, PRN *prn)
{
    int err = 0;

    if (s == NULL || *s == '\0') {
	return E_PARSE;
    }

    if (object_is_function_arg(s)) {
	gretl_errmsg_sprintf("The variable %s is read-only", s);
	return E_DATA;
    }

    if (!strcmp(s, "kalman")) {
	err = delete_kalman(prn);	
    } else if (gretl_is_scalar(s)) {
	err = gretl_scalar_delete(s, prn);
    } else if (get_matrix_by_name(s)) {
	err = user_matrix_destroy_by_name(s, prn);
    } else if (get_string_by_name(s)) {
	err = delete_saved_string(s, prn);
    } else if (gretl_is_bundle(s)) {
	err = gretl_bundle_delete_by_name(s, prn);
    } else {
	err = maybe_delete_bundle_value(s, prn);
    } 

    return err;
}

/* internal execution timer: on OpenMP use omp_get_wtime(); else
   on Windows use use GetTickCount; else use times() if available,
   otherwise fall back on clock()
*/ 

#if defined(_OPENMP) && defined(USE_OPENMP)

#include <omp.h>

static double tim0;

static void gretl_stopwatch_init (void)
{
    tim0 = omp_get_wtime();
}

double gretl_stopwatch (void)
{
    double tim1 = omp_get_wtime();
    double x = tim1 - tim0;

    tim0 = tim1;

    return x;
} 

#elif defined(WIN32)

#include <windows.h>

static DWORD tim0;

static void gretl_stopwatch_init (void)
{
    tim0 = GetTickCount();
}

double gretl_stopwatch (void)
{
    DWORD tim1 = GetTickCount();
    double x;

    x = (double) (tim1 - tim0) / 1000.0;
    tim0 = tim1;

    return x;
} 

#else /* !OPENMP, !WIN32 */

static clock_t tim0;

#ifdef HAVE_SYS_TIMES_H
# include <sys/times.h>
# include <unistd.h>
static unsigned ticks_per_sec;
#else
# include <time.h>
#endif

static void gretl_stopwatch_init (void)
{
#ifdef HAVE_SYS_TIMES_H
    struct tms timebuf;

    ticks_per_sec = sysconf(_SC_CLK_TCK);
    tim0 = times(&timebuf);
#else
    tim0 = clock();
#endif
}

double gretl_stopwatch (void)
{
    clock_t tim1;
    double x;

#ifdef HAVE_SYS_TIMES_H
    struct tms timebuf;

    tim1 = times(&timebuf);
    x = (double) (tim1 - tim0) / ticks_per_sec;
#else
    tim1 = clock();
    x = (double) (tim1 - tim0) / CLOCKS_PER_SEC;
#endif

    tim0 = tim1;

    return x;
} 

#endif /* stopwatch variants */

/* library init and cleanup functions */

void libgretl_init (void)
{
    libset_init();

#ifdef WIN32
    fprintf(stderr, "libgretl_init: initializing RNG\n");
#endif
    gretl_rand_init();
#ifdef WIN32
    fprintf(stderr, " RNG ready\n");
#endif

    gretl_xml_init();
    gretl_stopwatch_init();
}

void libgretl_session_cleanup (int mode)
{
    gretl_saved_objects_cleanup();

    if (mode != SESSION_CLEAR_OTHER) {
	/* trash dataset-related items */
	gretl_transforms_cleanup();
	gretl_lists_cleanup();
	gretl_tests_cleanup();
	gretl_plotx(NULL);
    }

    if (mode != SESSION_CLEAR_DATASET) {
	/* trash non-dataset items */
	destroy_user_scalars();
	destroy_user_matrices();
	destroy_user_bundles();
	destroy_user_strings();
    }
}

void libgretl_cleanup (void)
{
    libgretl_session_cleanup(SESSION_CLEAR_ALL);

    gretl_rand_free();
    gretl_functions_cleanup();
    libset_cleanup();
    gretl_command_hash_cleanup();
    gretl_function_hash_cleanup();
    saved_strings_cleanup();
    lapack_mem_free();
    forecast_matrix_cleanup();
    option_flags_cleanup();
    kalman_cleanup();
    gnuplot_cleanup();

#ifdef USE_RLIB
    gretl_R_cleanup();
#endif

    gretl_xml_cleanup();
}

/* record and retrieve hypothesis test results */

enum {
    SET_TEST_STAT,
    GET_TEST_STAT,
    GET_TEST_PVAL,
    GET_TEST_LNL,
    TESTS_CLEANUP
};

#define getcode(c) (c == GET_TEST_STAT || c == GET_TEST_PVAL || \
                    c == GET_TEST_LNL)

static GretlType last_test_type;

static double
record_or_get_test_result (double teststat, double pval, double lnl,
			   char *instr, int code)
{
    static char savestr[MAXLABEL] = {0};
    static double val = NADBL;
    static double pv = NADBL;
    static double ll = NADBL;
    double ret = NADBL;

    if (code == SET_TEST_STAT) {
	last_test_type = GRETL_TYPE_DOUBLE;
	val = teststat;
	pv = pval;
	ll = lnl;
	*savestr = '\0';
	if (instr != NULL) {
	    strncat(savestr, instr, MAXLABEL - 1);
	} 
    } else if (getcode(code) && last_test_type == GRETL_TYPE_DOUBLE) {
	if (instr != NULL) {
	    if (code == GET_TEST_STAT) {
		sprintf(instr, _("%s test"), savestr);
	    } else if (code == GET_TEST_PVAL) {
		sprintf(instr, _("p-value for %s test"), savestr);
	    } else if (code == GET_TEST_LNL) {
		sprintf(instr, _("log-likelihood for %s test"), savestr);
	    }
	}
	if (code == GET_TEST_STAT) {
	    ret = val;
	} else if (code == GET_TEST_PVAL) {
	    ret = pv;
	} else if (code == GET_TEST_LNL) {
	    ret = ll;
	}
    } 
	
    return ret;
}

static gretl_matrix *
record_or_get_test_matrix (gretl_matrix *tests, 
			   gretl_matrix *pvals, 
			   int code, int *err)
{
    static gretl_matrix *vals = NULL;
    static gretl_matrix *pvs = NULL;
    gretl_matrix *ret = NULL;

    if (code == TESTS_CLEANUP) {
	gretl_matrix_free(vals);
	gretl_matrix_free(pvs);
	vals = pvs = NULL;
	last_test_type = GRETL_TYPE_NONE;
	return NULL;
    }

    if (code == SET_TEST_STAT) {
	last_test_type = GRETL_TYPE_MATRIX;
	gretl_matrix_free(vals);
	vals = tests;
	gretl_matrix_free(pvs);
	pvs = pvals;
    } else if (getcode(code)) {
	gretl_matrix *src = (code == GET_TEST_STAT)? vals : pvs;

	if (src != NULL) {
	    ret = gretl_matrix_copy(src);
	    if (ret == NULL) {
		*err = E_ALLOC;
	    }
	} else {
	    *err = E_BADSTAT;
	}
    } 
	
    return ret;
}

static void gretl_tests_cleanup (void)
{
    record_or_get_test_matrix(NULL, NULL, TESTS_CLEANUP, NULL);
}

/* Returns the type (scalar, matrix or none) of the last recorded
   test statistic/p-value pair.
*/

int get_last_test_type (void)
{
    return last_test_type;
}

void record_test_result (double teststat, double pval, char *blurb)
{
    record_or_get_test_result(teststat, pval, NADBL, blurb, SET_TEST_STAT);
}

void record_LR_test_result (double teststat, double pval, double lnl,
			    char *blurb)
{
    record_or_get_test_result(teststat, pval, lnl, blurb, SET_TEST_STAT);
}

/* Note: the hypothesis-test recorder "takes ownership" of the
   two input matrices, @tests and @pvals, which should therefore 
   NOT be freed by the caller.
*/

void record_matrix_test_result (gretl_matrix *tests, 
				gretl_matrix *pvals)
{
    record_or_get_test_matrix(tests, pvals, SET_TEST_STAT, NULL);
}

double get_last_test_statistic (char *blurb)
{
    return record_or_get_test_result(0, 0, 0, blurb, GET_TEST_STAT);
}

double get_last_pvalue (char *blurb)
{
    return record_or_get_test_result(0, 0, 0, blurb, GET_TEST_PVAL);
}

double get_last_lnl (char *blurb)
{
    return record_or_get_test_result(0, 0, 0, blurb, GET_TEST_LNL);
}

gretl_matrix *get_last_test_matrix (int *err)
{
    return record_or_get_test_matrix(NULL, NULL, GET_TEST_STAT, err);
}

gretl_matrix *get_last_pvals_matrix (int *err)
{
    return record_or_get_test_matrix(NULL, NULL, GET_TEST_PVAL, err);
}





