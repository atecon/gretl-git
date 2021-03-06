/*
 *  gretl -- Gnu Regression, Econometrics and Time-series Library
 *  Copyright (C) 2017 Allin Cottrell and Riccardo "Jack" Lucchetti
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

/* Code for regularized least squares (LASSO and Ridge). Includes
   these methods:

   ADMM: Based on Boyd et al, "Distributed Optimization and
   Statistical Learning via the Alternating Direction Method of
   Multipliers", Foundations and Trends in Machine Learning, Vol. 3,
   No. 1 (2010) 1-122.

   CCD (Cyclical Coordinate Descent): Based on the Fortran code
   employed by R's glmnet for the Gaussian case and the "covariance"
   algorithm.

   SVD for Ridge.
*/

#include "libgretl.h"
#include "matrix_extra.h"
#include "version.h"

#ifdef HAVE_MPI
# include "gretl_mpi.h"
# include "gretl_foreign.h"
#endif

#if defined(USE_AVX)
# define USE_SIMD
# if defined(HAVE_IMMINTRIN_H)
#  include <immintrin.h>
# else
#  include <mmintrin.h>
#  include <xmmintrin.h>
#  include <emmintrin.h>
# endif
#endif

#define ADMM_MAX_ITER 20000
#define ADMM_RELTOL_DEFAULT 1.0e-4
#define ADMM_ABSTOL_DEFAULT 1.0e-6

double admm_reltol;
double admm_abstol;

#define CCD_MAX_ITER 100000
#define CCD_TOLER_DEFAULT 1.0e-7
#define BIG_LAMBDA 9.9e35

double ccd_toler;

enum {
    CRIT_MSE,
    CRIT_MAE
};

enum {
    LAMSCALE_NONE,
    LAMSCALE_GLMNET,
    LAMSCALE_FROB
};

typedef struct regls_info_ {
    gretl_bundle *b;
    gretl_matrix *X;
    gretl_matrix *y;
    gretl_matrix *lfrac;
    gretl_matrix *Xty;
    double rho;
    double infnorm;
    int nlam;
    int n;
    int k;
    gint8 ccd;
    gint8 ridge;
    gint8 stdize;
    gint8 xvalid;
    gint8 verbose;
    gint8 lamscale;
} regls_info;

#ifdef HAVE_MPI
static int mpi_parent_action (regls_info *ri, PRN *prn);
#endif

static void prepare_ccd_param (regls_info *ri)
{
    double tol;

    tol = gretl_bundle_get_scalar(ri->b, "ccd_toler", NULL);

    if (!na(tol) && tol > 0.0 && tol < 1.0) {
	ccd_toler = tol;
    } else {
	ccd_toler = CCD_TOLER_DEFAULT;
    }
}

static void maybe_set_lambda_scale (regls_info *ri)
{
    if (gretl_bundle_has_key(ri->b, "lambda_scale")) {
	ri->lamscale = gretl_bundle_get_int(ri->b, "lambda_scale", NULL);
    }
}

static void prepare_admm_params (regls_info *ri)
{
    gretl_matrix *ctrl;
    int len;

    /* set defaults */
    admm_reltol = ADMM_RELTOL_DEFAULT;
    admm_abstol = ADMM_ABSTOL_DEFAULT;

    ctrl = gretl_bundle_get_matrix(ri->b, "admmctrl", NULL);
    len = gretl_vector_get_length(ctrl);

    if (len > 0 && ctrl->val[0] > 0) {
	ri->rho = ctrl->val[0];
    }
    if (len > 1 && ctrl->val[1] > 0) {
	admm_reltol = ctrl->val[1];
    }
    if (len > 2 && ctrl->val[2] > 0) {
	admm_abstol = ctrl->val[2];
    }

    /* scale the absolute tolerance */
    admm_abstol *= sqrt(ri->X->cols);
}

regls_info *regls_info_new (gretl_matrix *X,
			    gretl_matrix *y,
			    gretl_bundle *b,
			    int *err)
{
    regls_info *ri = malloc(sizeof *ri);

    if (ri == NULL) {
	*err = E_ALLOC;
    } else {
	ri->b = b;
	ri->X = X;
	ri->y = y;
	ri->stdize =  gretl_bundle_get_int(b, "stdize", err);
	ri->xvalid =  gretl_bundle_get_int(b, "xvalidate", err);
	ri->verbose = gretl_bundle_get_bool(b, "verbosity", 1);
	ri->ridge =   gretl_bundle_get_bool(b, "ridge", 0);
	ri->ccd =     gretl_bundle_get_bool(b, "ccd", 0);
	ri->lfrac =   gretl_bundle_get_matrix(b, "lfrac", err);
    }

    if (*err) {
	free(ri);
	ri = NULL;
    } else {
	ri->n = ri->X->rows;
	ri->k = ri->X->cols;
	ri->nlam = gretl_vector_get_length(ri->lfrac);
	ri->rho = 8.0;
	ri->infnorm = 0.0;
	ri->lamscale = LAMSCALE_GLMNET;
	ri->Xty = NULL;
	if (ri->ccd) {
	    prepare_ccd_param(ri);
	} else if (!ri->ridge) {
	    prepare_admm_params(ri);
	}
	if (ri->ridge) {
	    maybe_set_lambda_scale(ri);
	}
    }

    return ri;
}

static double vector_infnorm (const gretl_vector *z)
{
    const int n = gretl_vector_get_length(z);
    double azi, ret = 0;
    int i;

    for (i=0; i<n; i++) {
	azi = fabs(z->val[i]);
	if (azi > ret) ret = azi;
    }

    return ret;
}

/* compute X'y and its infinity-norm for all training data */

static int regls_set_Xty (regls_info *ri)
{
    int err = 0;

    ri->Xty = gretl_matrix_alloc(ri->X->cols, 1);
    if (ri->Xty == NULL) {
	err = E_ALLOC;
    } else {
	gretl_matrix_multiply_mod(ri->X, GRETL_MOD_TRANSPOSE,
				  ri->y, GRETL_MOD_NONE,
				  ri->Xty, GRETL_MOD_NONE);
	ri->infnorm = vector_infnorm(ri->Xty);
    }

    return err;
}

static const char *crit_string (int crit)
{
    if (crit == CRIT_MSE) {
	return "MSE";
    } else if (crit == CRIT_MAE) {
	return "MAE";
    } else {
	return "pc correct";
    }
}

static int randomize_rows (gretl_matrix *X, gretl_matrix *y)
{
    gretl_vector *vp;
    double x, tmp;
    int i, j, src;

    vp = gretl_matrix_alloc(X->rows, 1);
    if (vp == NULL) {
	return E_ALLOC;
    }

    fill_permutation_vector(vp, X->rows);

    for (i=0; i<X->rows; i++) {
	src = vp->val[i] - 1;
	if (src == i) {
	    continue;
	}
	for (j=0; j<X->cols; j++) {
	    tmp = gretl_matrix_get(X, i, j);
	    x = gretl_matrix_get(X, src, j);
	    gretl_matrix_set(X, i, j, x);
	    gretl_matrix_set(X, src, j, tmp);
	}
	tmp = y->val[i];
	y->val[i] = y->val[src];
	y->val[src] = tmp;
    }

    gretl_matrix_free(vp);

    return 0;
}

static void vector_copy_values (gretl_vector *targ,
				const gretl_vector *src,
				int n)
{
    memcpy(targ->val, src->val, n * sizeof *targ->val);
}

static inline double max (double x, double y)
{
    return x >= y ? x : y;
}

#if defined(USE_SIMD)

static inline double hsum_double_avx (__m256d v)
{
    __m128d vlow  = _mm256_castpd256_pd128(v);
    __m128d vhigh = _mm256_extractf128_pd(v, 1);
    __m128d high64;

    vlow   = _mm_add_pd(vlow, vhigh);
    high64 = _mm_unpackhi_pd(vlow, vlow);
    return  _mm_cvtsd_f64(_mm_add_sd(vlow, high64));
}

static void vector_add_into (const gretl_vector *a,
			     const gretl_vector *b,
			     gretl_vector *c, int n)
{
    const double *ax = a->val;
    const double *bx = b->val;
    double *cx = c->val;
    int imax = n / 4;
    int rem = n % 4;
    int i;

    __m256d a256, b256, c256;

    for (i=0; i<imax; i++) {
	a256 = _mm256_loadu_pd(ax);
	b256 = _mm256_loadu_pd(bx);
	c256 = _mm256_add_pd(a256, b256);
	_mm256_storeu_pd(cx, c256);
	ax += 4;
	bx += 4;
	cx += 4;
    }
    for (i=0; i<rem; i++) {
	cx[i] = ax[i] + bx[i];
    }
}

static void vector_add_to (gretl_vector *a,
			   const gretl_vector *b,
			   int n)
{
    double *ax = a->val;
    const double *bx = b->val;
    int imax = n / 4;
    int rem = n % 4;
    int i;

    __m256d a256, b256, sum;

    for (i=0; i<imax; i++) {
	a256 = _mm256_loadu_pd(ax);
	b256 = _mm256_loadu_pd(bx);
	sum = _mm256_add_pd(a256, b256);
	_mm256_storeu_pd(ax, sum);
	ax += 4;
	bx += 4;
    }
    for (i=0; i<rem; i++) {
	ax[i] += bx[i];
    }
}

/* a = a - b */

static void vector_subtract_from (gretl_vector *a,
				  const gretl_vector *b,
				  int n)
{
    double *ax = a->val;
    const double *bx = b->val;
    int imax = n / 4;
    int rem = n % 4;
    int i;

    __m256d a256, b256, dif;

    for (i=0; i<imax; i++) {
	a256 = _mm256_loadu_pd(ax);
	b256 = _mm256_loadu_pd(bx);
	dif = _mm256_sub_pd(a256, b256);
	_mm256_storeu_pd(ax, dif);
	ax += 4;
	bx += 4;
    }
    for (i=0; i<rem; i++) {
	ax[i] -= bx[i];
    }
}

/* c = a - b */

static void vector_subtract_into (const gretl_vector *a,
				  const gretl_vector *b,
				  gretl_vector *c, int n,
				  int cumulate)
{
    const double *ax = a->val;
    const double *bx = b->val;
    double *cx = c->val;
    int imax = n / 4;
    int rem = n % 4;
    int i;

    __m256d a256, b256, c256;

    for (i=0; i<imax; i++) {
	a256 = _mm256_loadu_pd(ax);
	b256 = _mm256_loadu_pd(bx);
	if (cumulate) {
	    __m256d d256 = _mm256_sub_pd(a256, b256);

	    c256 = _mm256_loadu_pd(cx);
	    d256 = _mm256_add_pd(c256, d256);
	    _mm256_storeu_pd(cx, d256);
	} else {
	    c256 = _mm256_sub_pd(a256, b256);
	    _mm256_storeu_pd(cx, c256);
	}
	ax += 4;
	bx += 4;
	cx += 4;
    }
    for (i=0; i<rem; i++) {
	if (cumulate) {
	    cx[i] += ax[i] - bx[i];
	} else {
	    cx[i] = ax[i] - bx[i];
	}
    }
}

/* compute q = rho * (b - u) + X'y */

static inline void compute_q (gretl_vector *q,
			      const gretl_vector *b,
			      const gretl_vector *u,
			      const gretl_vector *a, /* X'y */
			      double rho, int n)
{
    __m256d b256, u256, a256;
    __m256d r256, tmp;
    const double *bx = b->val;
    const double *ux = u->val;
    const double *ax = a->val;
    double *qx = q->val;
    const int mul = rho != 1.0;
    int imax = n / 4;
    int rem = n % 4;
    int i;

    if (mul) {
	/* broadcast rho */
	r256 = _mm256_broadcast_sd(&rho);
    }

    for (i=0; i<imax; i++) {
	b256 = _mm256_loadu_pd(bx);
	u256 = _mm256_loadu_pd(ux);
	a256 = _mm256_loadu_pd(ax);
	/* subtract u from b */
	tmp = _mm256_sub_pd(b256, u256);
	if (mul) {
	    /* multiply by rho */
	    tmp = _mm256_mul_pd(tmp, r256);
	}
	/* add a */
	tmp = _mm256_add_pd(tmp, a256);
	/* write result into q */
	_mm256_storeu_pd(qx, tmp);
	bx += 4;
	ux += 4;
	ax += 4;
	qx += 4;
    }

    for (i=0; i<rem; i++) {
	if (mul) {
	    qx[i] = rho * (bx[i] - ux[i]) + ax[i];
	} else {
	    qx[i] = bx[i] - ux[i] + ax[i];
	}
    }
}

static double dot_product (const double *x, const double *y, int n)
{
    double ret = 0.0;
    int i, imax = n / 4;
    int rem = n % 4;

    __m256d x256, y256, tmp;

    for (i=0; i<imax; i++) {
	x256 = _mm256_loadu_pd(x);
	y256 = _mm256_loadu_pd(y);
	tmp = _mm256_mul_pd(x256, y256);
	ret += hsum_double_avx(tmp);
	x += 4;
	y += 4;
    }

    for (i=0; i<rem; i++) {
	ret += x[i] * y[i];
    }

    return ret;
}

#else

static void vector_add_into (const gretl_vector *a,
			     const gretl_vector *b,
			     gretl_vector *c, int n)
{
    int i;

    for (i=0; i<n; i++) {
	c->val[i] = a->val[i] + b->val[i];
    }
}

static void vector_add_to (gretl_vector *a,
			   const gretl_vector *b,
			   int n)
{
    int i;

    for (i=0; i<n; i++) {
	a->val[i] += b->val[i];
    }
}

static void vector_subtract_from (gretl_vector *a,
				  const gretl_vector *b,
				  int n)
{
    int i;

    for (i=0; i<n; i++) {
	a->val[i] -= b->val[i];
    }
}

static void vector_subtract_into (const gretl_vector *a,
				  const gretl_vector *b,
				  gretl_vector *c, int n,
				  int cumulate)
{
    int i;

    for (i=0; i<n; i++) {
	if (cumulate) {
	    c->val[i] += a->val[i] - b->val[i];
	} else {
	    c->val[i] = a->val[i] - b->val[i];
	}
    }
}

static double dot_product (const double *x, const double *y, int n)
{
    double ret = 0.0;
    int i;

    for (i=0; i<n; i++) {
	ret += x[i] * y[i];
    }
    return ret;
}

static inline void compute_q (gretl_vector *q,
			      const gretl_vector *b,
			      const gretl_vector *u,
			      const gretl_vector *Xty,
			      double rho, int n)
{
    const int mul = rho != 1.0;
    int i;

    for (i=0; i<n; i++) {
	if (mul) {
	    q->val[i] = rho * (b->val[i] - u->val[i]) + Xty->val[i];
	} else {
	    q->val[i] = b->val[i] - u->val[i] + Xty->val[i];
	}
    }
}

#endif /* AVX or not */

/* fortran: dot_product(X(:,j), X(:,k)) for @X with @n rows */

static double dot_prod_jk (const gretl_matrix *X, int j, int k, int n)
{
    const double *xj = X->val + n * j;
    const double *xk = X->val + n * k;

    return dot_product(xj, xk, n);
}

/* fortran: dot_product(v(1:n), m(j,1:n)) */

static double dot_prod_vm (const double *v,
			   const gretl_matrix *m,
			   int j, int n)
{
    double ret = 0;
    int i;

    for (i=0; i<n; i++) {
	ret += v[i] * gretl_matrix_get(m, j, i);
    }

    return ret;
}

/* implement these fortran lines:

   x(1:n) = y(idx(1:n)) - x(1:n) !! sub = 1
   x(1:n) = y(idx(1:n))          !! sub = 0
*/

static void range_set_sub (double *x, const double *y,
			   const int *idx, int n, int sub)
{
    int i;

    if (sub) {
	for (i=0; i<n; i++) {
	    x[i] = y[idx[i]] - x[i];
	}
    } else {
	for (i=0; i<n; i++) {
	    x[i] = y[idx[i]];
	}
    }
}

/* fortran: B(1:n,j) = a(idx(1:n)) */

static void fill_coeff_column (gretl_matrix *B, int nx, int j,
			       const double *a, const int *idx,
			       int n)
{
    int i, offset = B->rows > nx;
    double *b = B->val + j * B->rows;

    for (i=0; i<n; i++) {
	b[i+offset] = a[idx[i]];
    }
}

/* sign(x,y): gives "the value of x with the sign of y",
   but in context @x will always be positive.
*/

static inline double sign (double x, double y)
{
    return y >= 0 ? x : -x;
}

static double abs_sum (const gretl_vector *z)
{
    const int n = gretl_vector_get_length(z);
    double ret = 0;
    int i;

    for (i=0; i<n; i++) {
	ret += fabs(z->val[i]);
    }

    return ret;
}

/* Cyclical Coordinate Descent (CCD) auxiliary functions */

static int ccd_scale (gretl_matrix *x, double *y,
		      double *xty, double *xv)
{
    int i, j, n = x->rows;
    double *xj, v = sqrt(1.0/n);

    for (i=0; i<n; i++) {
	y[i] *= v;
    }
    for (j=0; j<x->cols; j++) {
	xj = x->val + j * n;
	for (i=0; i<n; i++) {
	    xj[i] *= v;
	}
	if (xv != NULL) {
	    xv[j] = dot_product(xj, xj, n);
	}
	if (xty != NULL) {
	    xty[j] = dot_product(y, xj, n);
	}
    }

    return 0;
}

static void finalize_ccd_coeffs (gretl_matrix *B,
				 double *a, int nx,
				 int *ia)
{
    int offset = B->rows > nx;
    size_t asize = nx * sizeof *a;
    double *bj;
    int i, j;

    for (j=0; j<B->cols; j++) {
	bj = B->val + j*B->rows + offset;
	memcpy(a, bj, asize);
	for (i=0; i<nx; i++) {
	    bj[i] = 0.0;
	}
	for (i=0; i<nx; i++) {
	    if (a[i] != 0) {
		bj[ia[i]] = a[i];
	    }
	}
    }
}

static int ccd_iteration (double alpha, const gretl_matrix *X, double *g,
			  int nlam, const double *ulam, double thr,
			  int maxit, const double *xv, int *lmu,
			  gretl_matrix *B, int *ia, int *kin,
			  double *Rsq, int *pnlp)
{
    gretl_matrix *C;
    double alm, u, v, rsq = 0;
    double ak, del, dlx, cij;
    double omb, dem, ab;
    double *a, *da;
    int *mm, nin, jz, iz = 0;
    int j, k, l, m, nlp = 0;
    int nx = X->cols;
    int err = 0;

    C = gretl_matrix_alloc(nx, nx);
    a = malloc(nx * sizeof *a);
    da = malloc(nx * sizeof *da);
    mm = malloc(nx * sizeof *mm);
    if (C == NULL || a == NULL || da == NULL || mm == NULL) {
	return E_ALLOC;
    }
    /* "zero" @a and @mm */
    for (j=0; j<nx; j++) {
	a[j] = 0.0;
	mm[j] = -1;
    }
    nin = nlp = *pnlp = 0;
    omb = 1.0 - alpha; /* = 0 for lasso */

    for (m=0; m<nlam; m++) {
	alm = ulam[m];
	dem = alm*omb;
	ab = alm*alpha;
	jz = 1;
    maybe_restart:
	if (iz*jz == 0) {
            nlp++;
            dlx = 0.0;
	    for (k=0; k<nx; k++) {
		ak = a[k];
		u = g[k] + ak*xv[k];
		v = fabs(u) - ab;
		a[k] = v > 0.0 ? sign(v,u) / (xv[k]+dem) : 0.0;
		if (a[k] != ak) {
		    if (mm[k] < 0) {
			if (nin >= nx) goto check_conv;
			for (j=0; j<nx; j++) {
			    if (mm[j] >= 0) {
				cij = gretl_matrix_get(C, k, mm[j]);
				gretl_matrix_set(C, j, nin, cij);
			    } else if (j != k) {
				cij = dot_prod_jk(X, j, k, X->rows);
				gretl_matrix_set(C, j, nin, cij);
			    } else {
				gretl_matrix_set(C, j, nin, xv[j]);
			    }
			}
			mm[k] = nin;
			ia[nin] = k;
			nin++;
		    }
		    del = a[k] - ak;
		    rsq += del * (2*g[k] - del*xv[k]);
		    dlx = max(xv[k]*del*del, dlx);
		    for (j=0; j<nx; j++) {
			cij = gretl_matrix_get(C, j, mm[k]);
			g[j] -= cij*del;
		    }
		}
            }
	check_conv:
	    if (dlx < thr || nin > nx) {
		goto m_finish;
	    } else if (nlp > maxit) {
		fprintf(stderr, "ccd: max iters reached\n");
		err = E_NOCONV;
		goto getout;
            }
	}
	iz = 1;
	range_set_sub(da, a, ia, nin, 0);
    nlp_plus:
	nlp++;
	dlx = 0.0;
	for (l=0; l<nin; l++) {
	    k = ia[l];
	    ak = a[k];
	    u = g[k] + ak*xv[k];
	    v = fabs(u) - ab;
	    a[k] = v > 0.0 ? sign(v,u) / (xv[k]+dem) : 0.0;
	    if (a[k] != ak) {
		del = a[k] - ak;
		rsq += del * (2*g[k] - del*xv[k]);
		dlx = max(xv[k]*del*del, dlx);
		for (j=0; j<nin; j++) {
		    cij = gretl_matrix_get(C, ia[j], mm[k]);
		    g[ia[j]] -= cij*del;
		}
	    }
	}
	if (dlx < thr) {
	    range_set_sub(da, a, ia, nin, 1);
	    for (j=0; j<nx; j++) {
		if (mm[j] < 0) {
		    g[j] -= dot_prod_vm(da, C, j, nin);
		}
	    }
	    jz = 0;
	    goto maybe_restart;
	} else if (nlp <= maxit) {
	    /* try another iteration */
	    goto nlp_plus;
	} else {
	    /* reached max iterations */
	    err = E_NOCONV;
	    goto getout;
	}
    m_finish:
	if (nin <= nx) {
	    if (nin > 0) {
		fill_coeff_column(B, nx, m, a, ia, nin);
	    }
	    kin[m] = nin;
	    if (Rsq != NULL) {
		Rsq[m] = rsq;
	    }
	    *lmu = m + 1;
	} else {
            err = E_NOCONV;
	    fprintf(stderr, "ccd: error at foot of loop\n");
            goto getout;
	}
    } /* end loop over lambda values */

 getout:

    if (!err) {
	finalize_ccd_coeffs(B, a, nx, ia);
    }

    *pnlp = nlp;
    free(a);
    free(mm);
    free(da);
    gretl_matrix_free(C);

    return err;
}

static int ccd_get_crit (const gretl_matrix *B,
		         const gretl_matrix *lam,
		         const gretl_matrix *R2,
			 const gretl_matrix *y,
		         gretl_matrix *crit,
			 double alpha, int nx)
{
    double *bj, bsum, SSR;
    double nulldev = 1.0;
    int imin = B->rows > nx;
    int i, j;

    // fprintf(stderr, "HERE B->rows %d, nx %d\n", B->rows, nx);

    if (B->rows == nx) {
	/* no intercept */
	nulldev = 0.0;
	for (i=0; i<y->rows; i++) {
	    nulldev += y->val[i] * y->val[i];
	}
    } else if (alpha < 1.0) {
	/* for comparability with SVD */
	nulldev = y->rows;
    }

    for (j=0; j<B->cols; j++) {
	bsum = 0;
	bj = B->val + j*B->rows;
	for (i=imin; i<B->rows; i++) {
	    if (alpha == 1.0) {
		/* lasso */
		bsum += fabs(bj[i]);
	    } else {
		/* ridge */
		bsum += bj[i] * bj[i];
	    }
	}
	SSR = nulldev * (1.0 - R2->val[j]);
	if (alpha == 1.0) {
	    /* lasso */
	    gretl_vector_set(crit, j, 0.5 * SSR + lam->val[j] * bsum);
	} else {
	    /* ridge */
	    gretl_vector_set(crit, j, SSR + lam->val[j] * bsum);
	}
    }

    return 0;
}

static gretl_matrix *sv_squared (const gretl_matrix *X)
{
    gretl_matrix *sv2 = NULL;
    int err;

    err = gretl_matrix_SVD(X, NULL, &sv2, NULL, 0);

    if (!err) {
	int i, k = gretl_vector_get_length(sv2);
	double sq;

	for (i=0; i<k; i++) {
	    sq = sv2->val[i] * sv2->val[i];
	    sv2->val[i] = sq;
	}
    }

    return sv2;
}

static gchar *crit_print_format (const gretl_matrix *crit,
				 int ridge)
{
    gchar *fmt = NULL;
    double cmax = 0;
    int j;

    for (j=0; j<crit->rows; j++) {
	if (crit->val[j] > cmax) {
	    cmax = crit->val[j];
	}
    }

    /* FIXME better handling for very large criterion values */

    if (cmax < 1000) {
	if (ridge) {
	    fmt = g_strdup_printf("%%12f  %%6.2f    %%f   %%.4f\n");
	} else {
	    fmt = g_strdup_printf("%%12f  %%5d    %%f   %%.4f\n");
	}
    } else {
	int fdig = 6 - floor(log10(cmax));

	if (ridge) {
	    fmt = g_strdup_printf("%%12f  %%6.2f    %%8.%df   %%.4f\n", fdig);
	} else {
	    fmt = g_strdup_printf("%%12f  %%5d    %%8.%df   %%.4f\n", fdig);
	}
    }

    return fmt;
}

static void lambda_sequence_header (PRN *prn)
{
    pputc(prn, '\n');
    pputs(prn, "      lambda     df   criterion      R^2\n");
}

static void ccd_print (const gretl_matrix *B,
		       const gretl_matrix *R2,
		       const gretl_matrix *lam,
		       const gretl_matrix *crit,
		       int nx, PRN *prn)
{
    gchar *cfmt = NULL;
    double *bj;
    int k = B->rows;
    int nlam = B->cols;
    int i, j, dfj;

    if (crit != NULL) {
	/* header for output showing penalized criterion */
	lambda_sequence_header(prn);
    } else {
	/* as per R, more or less */
	pputc(prn, '\n');
	pputs(prn, "    df     R^2  lambda\n");
    }

    cfmt = crit_print_format(crit, 0);

    for (j=0; j<nlam; j++) {
	bj = B->val + j*k;
	dfj = 0;
	for (i=0; i<k; i++) {
	    dfj += fabs(bj[i]) > 0;
	}
	if (crit != NULL) {
	    pprintf(prn, cfmt, lam->val[j], dfj, crit->val[j], R2->val[j]);
	} else {
	    pprintf(prn, "%-2d  %2d  %.4f  %.4f\n", j+1, dfj, R2->val[j],
		    lam->val[j]);
	}
    }

    g_free(cfmt);
}

static double effective_df (const gretl_matrix *sv2, double lam)
{
    int i, k = gretl_vector_get_length(sv2);
    double ret = 0.0;

    for (i=0; i<k; i++) {
	ret += sv2->val[i] / (sv2->val[i] + lam);
    }

    return ret;
}

static void ridge_print (const gretl_matrix *lam,
			 const gretl_matrix *sv2,
			 const gretl_matrix *crit,
			 const gretl_matrix *R2,
			 PRN *prn)
{
    gchar *cfmt = NULL;
    double edf;
    int j;

    pprintf(prn, "\n  %s\n", _("df = effective number of free parameters"));
    pprintf(prn, "  %s\n\n", _("criterion = ridge minimand"));
    pputs(prn, "      lambda      df   criterion      R^2\n");

    cfmt = crit_print_format(crit, 1);

    for (j=0; j<lam->rows; j++) {
	edf = effective_df(sv2, lam->val[j]);
	pprintf(prn, cfmt, lam->val[j], edf, crit->val[j], R2->val[j]);
    }
    g_free(cfmt);
}

static void xv_ridge_print (const gretl_matrix *lam,
			    const gretl_matrix *sv2,
			    PRN *prn)
{
    int j, nlam = lam->rows;
    double edf;

    pputc(prn, '\n');
    pputs(prn, "      lambda     df\n");
    for (j=0; j<nlam; j++) {
	edf = effective_df(sv2, lam->val[j]);
	pprintf(prn, "%12f  %.3f\n", lam->val[j], edf);
    }
}

/* end functions specific to CCD */

/* calculate the lasso objective function */

static double lasso_objective (const gretl_matrix *X,
			       const gretl_vector *y,
			       const gretl_vector *b,
			       double lambda,
			       gretl_vector *u,
			       double TSS,
			       double *R2)
{
    double SSR;
    double obj = 0;

    gretl_matrix_multiply(X, b, u);
    vector_subtract_from(u, y, y->rows);
    SSR = gretl_vector_dot_product(u, u, NULL);
    obj = 0.5 * SSR + lambda * abs_sum(b);
    *R2 = 1.0 - SSR/TSS;

    return obj / y->rows;
}

/* calculate the cross validation criterion */

static double xv_score (const gretl_matrix *X,
			const gretl_vector *y,
			const gretl_vector *b,
			gretl_vector *Xb,
			int crit_type)
{
    double sum = 0;

    /* get fitted values */
    gretl_matrix_multiply(X, b, Xb);
    /* compute and process residuals */
    vector_subtract_from(Xb, y, X->rows);
    if (crit_type == CRIT_MSE) {
	sum = gretl_vector_dot_product(Xb, Xb, NULL);
    } else {
	sum = abs_sum(Xb);
    }

    return sum / X->rows;
}

static void soft_threshold (gretl_vector *v, double lambda,
			    double rho)
{
    double vi, k;
    int i;

    k = rho == 1.0 ? lambda : lambda / rho;

    for (i=0; i<v->rows; i++) {
	vi = v->val[i];
	if (vi > k)       { v->val[i] = vi - k; }
	else if (vi < -k) { v->val[i] = vi + k; }
	else              { v->val[i] = 0; }
    }
}

static int get_cholesky_factor (const gretl_matrix *X,
				gretl_matrix *L,
				double rho)
{
    double d;
    int i;

    if (X->rows >= X->cols) {
	/* "skinny": L = chol(X'X + rho*I) */
	gretl_matrix_multiply_mod(X, GRETL_MOD_TRANSPOSE,
				  X, GRETL_MOD_NONE,
				  L, GRETL_MOD_NONE);
	for (i=0; i<X->cols; i++) {
	    d = gretl_matrix_get(L, i, i);
	    gretl_matrix_set(L, i, i, d + rho);
	}
    } else {
	/* "fat": L = chol(I + 1/rho*XX') */
	gretl_matrix_multiply_mod(X, GRETL_MOD_NONE,
				  X, GRETL_MOD_TRANSPOSE,
				  L, GRETL_MOD_NONE);
	if (rho != 1.0) {
	    gretl_matrix_multiply_by_scalar(L, 1/rho);
	}
	for (i=0; i<X->rows; i++) {
	    d = gretl_matrix_get(L, i, i);
	    gretl_matrix_set(L, i, i, d + 1.0);
	}
    }

    return gretl_matrix_cholesky_decomp(L);
}

#define RHO_DEBUG 0

static int admm_iteration (const gretl_matrix *X,
			   const gretl_vector *Xty,
			   gretl_matrix *L,
			   gretl_vector *v, gretl_vector *b,
			   gretl_vector *u, gretl_vector *q,
			   gretl_vector *p, gretl_vector *r,
			   gretl_vector *bprev, gretl_vector *bdiff,
			   double lambda, double *prho,
			   int tune_rho, int *iters)
{
    double nxstack, nystack;
    double prires, dualres;
    double eps_pri, eps_dual;
    double rho = *prho;
    double nrm2, rho2 = rho*rho;
    int itermin = 1;
    int n = X->cols;
    int iter = 0;
    int err = 0;

#if RHO_DEBUG
    fprintf(stderr, "*** admm: lambda %g, rho %g ***\n", lambda, rho);
#endif

    while (iter < ADMM_MAX_ITER && !err) {
	/* u-update: u = u + r */
	vector_add_to(u, r, n);

	/* v-update: v = (X^T X + rho I) \ (X^T y + rho b - u) */

	compute_q(q, b, u, Xty, rho, n);
	if (X->rows >= X->cols) {
	    /* v = U \ (L \ q) */
	    gretl_cholesky_solve(L, q);
	    vector_copy_values(v, q, n);
	} else {
	    /* v = q/rho - 1/rho^2 * X^T * (U \ (L \ (X*q))) */
	    gretl_matrix_multiply(X, q, p);
	    err = gretl_cholesky_solve(L, p);
	    gretl_matrix_multiply_mod(X, GRETL_MOD_TRANSPOSE,
				      p, GRETL_MOD_NONE,
				      v, GRETL_MOD_NONE);
	    gretl_matrix_multiply_by_scalar(v, -1/rho2);
	    gretl_matrix_multiply_by_scalar(q, 1/rho);
	    vector_add_to(v, q, n);
	}

	/* sqrt(sum ||r_i||_2^2) */
	prires  = sqrt(gretl_vector_dot_product(r, r, NULL));
	/* sqrt(sum ||v_i||_2^2) */
	nxstack = sqrt(gretl_vector_dot_product(v, v, NULL));
	/* sqrt(sum ||u_i||_2^2) */
	nystack = gretl_vector_dot_product(u, u, NULL) / rho2;
	nystack = sqrt(nystack);

	vector_copy_values(bprev, b, n);
	vector_add_into(v, u, b, n);
	soft_threshold(b, lambda, rho);

	/* Termination checks */

	/* dual residual */
	vector_subtract_into(b, bprev, bdiff, n, 0); /* bdiff = b - bprev */
	/* ||s^k||_2^2 = N rho^2 ||b - bprev||_2^2 */
	nrm2 = sqrt(gretl_vector_dot_product(bdiff, bdiff, NULL));
	dualres = rho * nrm2;

	/* compute primal and dual feasibility tolerances */
	nrm2 = sqrt(gretl_vector_dot_product(b, b, NULL));
	eps_pri  = admm_abstol + admm_reltol * fmax(nxstack, nrm2);
	eps_dual = admm_abstol + admm_reltol * nystack;

	if (iter >= itermin && prires <= eps_pri && dualres <= eps_dual) {
	    break;
	}

	/* Compute residual: r = v - b */
	vector_subtract_into(v, b, r, n, 0);

	if (tune_rho && iter > 0 && (iter == 32 || iter % 200 == 0)) {
	    double mult = 10;
	    double adj = 0.0;

	    if (prires > mult * dualres) {
		adj = 2.0;
	    } else if (dualres > mult * prires) {
		adj = 0.5;
	    }
	    if (adj > 0) {
		rho *= adj;
# if RHO_DEBUG
		fprintf(stderr, "  iter %d: rho *= %g (now %g)\n",
			iter, adj, rho);
# endif
		rho2 = rho * rho;
		gretl_matrix_multiply_by_scalar(u, 1.0/adj);
		gretl_matrix_multiply_by_scalar(r, 1.0/adj);
		get_cholesky_factor(X, L, rho);
		/* ensure a fair number of subsequent iterations */
		itermin = iter + 100;
	    }
	}

	iter++;
    }

    *iters = iter;
    *prho = rho;

    return err;
}

static gretl_matrix *make_coeff_matrix (regls_info *ri,
					int *jmin,
					int *jmax)
{
    gretl_matrix *B = NULL;
    int xv_single_b = 0;
    int rows;

    if (ri->xvalid) {
	/* do we want just the "best" coeff vector? */
	xv_single_b = gretl_bundle_get_bool(ri->b, "single_b", 0);
    }

    rows = ri->k + ri->stdize;

    if (xv_single_b) {
	int use_1se = gretl_bundle_get_bool(ri->b, "use_1se", 0);
	const char *ikey = use_1se ? "idx1se" : "idxmin";
	int idx;

	idx = gretl_bundle_get_int(ri->b, ikey, NULL);
	B = gretl_zero_matrix_new(rows, 1);
	*jmin = idx - 1; /* zero-based */
	*jmax = *jmin + 1;
    } else {
	B = gretl_zero_matrix_new(rows, ri->nlam);
	*jmin = 0;
	*jmax = ri->nlam;
    }

    if (B != NULL) {
	gretl_bundle_donate_data(ri->b, "B", B, GRETL_TYPE_MATRIX, 0);
    }

    return B;
}

static void ccd_make_lambda (regls_info *ri,
			     gretl_matrix *lam,
			     double *lmax,
			     double alpha)
{
    int i;

    gretl_matrix_copy_values(lam, ri->lfrac);
    if (ri->lamscale == 0) {
	for (i=0; i<ri->nlam; i++) {
	    lam->val[i] /= ri->n;
	}
	return;
    }
    if (alpha < 1.0) {
	*lmax /= max(alpha, 1.0e-3);
    }
    for (i=0; i<ri->nlam; i++) {
	lam->val[i] *= *lmax;
    }
    if (alpha < 1.0 && ri->nlam > 1) {
	lam->val[0] = BIG_LAMBDA;
    }
}

static void lasso_lambda_report (regls_info *ri, PRN *prn)
{
    pprintf(prn, "lambda-max = %g\n", ri->infnorm);
    if (ri->nlam > 1) {
	pprintf(prn, "using lambda-fraction sequence of length %d, starting at %g\n",
		ri->nlam, ri->lfrac->val[0]);
    } else {
	pprintf(prn, "using lambda-fraction %g\n", ri->lfrac->val[0]);
    }
}

/* Cyclical Coordinate Descent driver: we come here either
   to get coefficient estimates right away, or after
   cross validation. Handles both LASSO and Ridge.
*/

static int ccd_regls (regls_info *ri, PRN *prn)
{
    gretl_matrix_block *MB;
    gretl_matrix *Xty, *xv;
    gretl_matrix *B = NULL;
    gretl_matrix *lam = NULL;
    gretl_matrix *R2 = NULL;
    gretl_matrix *crit = NULL;
    gretl_matrix *sv2 = NULL;
    double *Rsq = NULL;
    double lmax, alpha;
    int maxit = CCD_MAX_ITER;
    int *ia, *nnz;
    int nlp = 0, lmu = 0;
    int nlam = ri->nlam;
    int k = ri->k;
    int err = 0;

    alpha = ri->ridge ? 0.0 : 1.0;

    MB = gretl_matrix_block_new(&xv, k, 1, &Xty, k, 1,
				&lam, nlam, 1, NULL);
    B = gretl_zero_matrix_new(k + ri->stdize, nlam);
    if (MB == NULL || B == NULL) {
	return E_ALLOC;
    }

    if (ri->ridge && ri->verbose) {
	sv2 = sv_squared(ri->X);
    }

    /* scale data by sqrt(1/n) */
    ccd_scale(ri->X, ri->y->val, Xty->val, xv->val);

    /* and compute lambda sequence */
    lmax = vector_infnorm(Xty);
    ccd_make_lambda(ri, lam, &lmax, alpha);

    /* integer workspace */
    ia = malloc((k + nlam) * sizeof *ia);
    if (ia == NULL) {
	err = E_ALLOC;
	goto bailout;
    }
    nnz = ia + k;

    if (!ri->xvalid) {
	R2 = gretl_matrix_alloc(nlam, 1);
	crit = gretl_matrix_alloc(nlam, 1);
	if (R2 == NULL || crit == NULL) {
	    err = E_ALLOC;
	    goto bailout;
	} else {
	    Rsq = R2->val;
	}
    }

    if (!ri->ridge && !ri->xvalid && ri->verbose) {
	lasso_lambda_report(ri, prn);
    }

    err = ccd_iteration(alpha, ri->X, Xty->val, nlam, lam->val,
			ccd_toler, maxit, xv->val, &lmu, B,
			ia, nnz, Rsq, &nlp);
#if 0
    fprintf(stderr, "ccd: err=%d, nlp=%d, lmu=%d\n", err, nlp, lmu);
#endif
    if (err) {
	goto bailout;
    }

    if (ri->lamscale == 0) {
	gretl_matrix_multiply_by_scalar(ri->y, sqrt(ri->n));
	gretl_matrix_copy_values(lam, ri->lfrac);
    } else if (alpha < 1.0) {
	/* not entirely truthful! */
	lam->val[0] = ri->lfrac->val[0] * lmax;
    }

    if (ri->xvalid && ri->verbose && ri->ridge && nlam > 1) {
	xv_ridge_print(lam, sv2, prn);
    }

    if (!ri->xvalid) {
	ccd_get_crit(B, lam, R2, ri->y, crit, alpha, k);
	if (ri->verbose) {
	    if (alpha < 1.0) {
		ridge_print(lam, sv2, crit, R2, prn);
	    } else {
		ccd_print(B, R2, lam, crit, k, prn);
	    }
	}
	if (nlam > 1) {
	    double critmin = 1e200;
	    int j, idxmin = 0;

	    for (j=0; j<nlam; j++) {
		if (crit->val[j] < critmin) {
		    critmin = crit->val[j];
		    idxmin = j;
		}
	    }
	    gretl_bundle_set_scalar(ri->b, "idxmin", idxmin + 1);
	    gretl_bundle_set_scalar(ri->b, "lfmin", ri->lfrac->val[idxmin]);
	}
	if (crit->rows > 1) {
	    gretl_bundle_donate_data(ri->b, "crit", crit, GRETL_TYPE_MATRIX, 0);
	    crit = NULL;
	} else {
	    gretl_bundle_set_scalar(ri->b, "crit", crit->val[0]);
	}
    }

    if (!err) {
	gretl_bundle_donate_data(ri->b, "B", B, GRETL_TYPE_MATRIX, 0);
	B = NULL;
	if (ri->lamscale > 0) {
	    gretl_bundle_set_scalar(ri->b, "lmax", lmax * ri->n);
	}
	if (nlam == 1) {
	    double lambda = ri->lfrac->val[0];

	    if (ri->lamscale > 0) {
		/* show a value comparable with ADMM (??) */
		lambda *= lmax * ri->n;
	    }
	    gretl_bundle_set_scalar(ri->b, "lambda", lambda);
	}
    }

 bailout:

    gretl_matrix_free(crit);
    gretl_matrix_free(R2);
    gretl_matrix_free(B);
    gretl_matrix_free(sv2);
    gretl_matrix_block_destroy(MB);
    free(ia);

    return err;
}

static gretl_matrix *svd_ridge_vcv (regls_info *ri,
				    double lam,
				    int *err)
{
    gretl_matrix_block *MB = NULL;
    gretl_matrix *V = NULL;
    gretl_matrix *Vt = NULL;
    gretl_matrix *sv = NULL;
    gretl_matrix *sve = NULL;
    gretl_matrix *RI = NULL;
    gretl_matrix *Tmp = NULL;
    gretl_matrix *Ve = NULL;
    gretl_matrix *b = NULL;
    gretl_matrix *u = NULL;
    double vij, s2;
    int n = ri->X->rows;
    int k = ri->X->cols;
    int i, j;

    *err = gretl_matrix_SVD(ri->X, NULL, &sv, &Vt, 0);

    if (!*err) {
	MB = gretl_matrix_block_new(&sve, 1, k, &b, k, 1,
				    &u, n, 1, &RI, k, k,
				    &Ve, k, k, &Tmp, k, k,
				    NULL);
	if (MB == NULL) {
	    *err = E_ALLOC;
	    goto bailout;
	}
    }

    if (!*err) {
	V = gretl_matrix_alloc(k, k);
	if (V == NULL) {
	    *err = E_ALLOC;
	    goto bailout;
	}
    }

    /* sve = 1 / (sv.^2 + lambda) */
    for (i=0; i<k; i++) {
	sve->val[i] = 1.0 / (sv->val[i] * sv->val[i] + lam);
    }

    /* Ve = Vt' .* sve */
    for (j=0; j<k; j++) {
	for (i=0; i<k; i++) {
	    vij = gretl_matrix_get(Vt, j, i);
	    gretl_matrix_set(Ve, i, j, vij * sve->val[j]);
	}
    }

    /* RI = Ve * Vt */
    gretl_matrix_multiply(Ve, Vt, RI);

    /* b = RI * Xty */
    gretl_matrix_multiply(RI, ri->Xty, b);

    /* u = X*b - y */
    gretl_matrix_multiply(ri->X, b, u);
    gretl_matrix_subtract_from(u, ri->y);

    /* residual variance */
    s2 = gretl_vector_dot_product(u, u, NULL) / n;

    /* V = s2 * ridgeI * X'X * ridgeI */
    gretl_matrix_multiply_mod(ri->X, GRETL_MOD_TRANSPOSE,
			      ri->X, GRETL_MOD_NONE,
			      Ve, GRETL_MOD_NONE);
    gretl_matrix_multiply(RI, Ve, Tmp);
    gretl_matrix_multiply(Tmp, RI, V);
    gretl_matrix_multiply_by_scalar(V, s2);

 bailout:

    gretl_matrix_block_destroy(MB);

    return V;
}

static int ridge_bhat (double *lam, int nlam, gretl_matrix *X,
		       gretl_matrix *y, gretl_matrix *B,
		       gretl_matrix *R2, int covmat)
{
    gretl_matrix_block *MB = NULL;
    gretl_matrix *U = NULL;
    gretl_matrix *Vt = NULL;
    gretl_matrix *sv = NULL;
    gretl_matrix *sve = NULL;
    gretl_matrix *Uty = NULL;
    gretl_matrix *L = NULL;
    gretl_matrix *yh = NULL;
    gretl_matrix *b = NULL;
    double vij, ui, SSR, TSS = 0;
    int offset = 0;
    int n = X->rows;
    int k = X->cols;
    int i, j, l, err;

    err = gretl_matrix_SVD(X, &U, &sv, &Vt, 0);

    if (!err) {
	MB = gretl_matrix_block_new(&sve, 1, k, &Uty, k, 1,
				    &L, k, k, &b, k, 1,
				    &yh, n, 1, NULL);
	if (MB == NULL) {
	    err = E_ALLOC;
	}
    }
    if (err) {
	goto bailout;
    }

    if (R2 != NULL) {
	for (i=0; i<n; i++) {
	    TSS += y->val[i] * y->val[i];
	}
    }

    offset = B->rows > k;

    gretl_matrix_multiply_mod(U, GRETL_MOD_TRANSPOSE,
			      y, GRETL_MOD_NONE,
			      Uty, GRETL_MOD_NONE);

    for (l=0; l<nlam; l++) {
	for (i=0; i<k; i++) {
	    sve->val[i] = sv->val[i] / (sv->val[i] * sv->val[i] + lam[l]);
	}
	for (j=0; j<k; j++) {
	    for (i=0; i<k; i++) {
		vij = gretl_matrix_get(Vt, j, i);
		gretl_matrix_set(L, i, j, vij * sve->val[j]);
	    }
	}
	gretl_matrix_multiply(L, Uty, b);
	gretl_matrix_multiply(X, b, yh);
	if (R2 != NULL) {
	    SSR = 0.0;
	    for (i=0; i<n; i++) {
		ui = y->val[i] - yh->val[i];
		SSR += ui * ui;
	    }
	    R2->val[l] = 1.0 - SSR/TSS;
	}
	memcpy(B->val + l * B->rows + offset, b->val, k * sizeof(double));
    }

 bailout:

    gretl_matrix_block_destroy(MB);
    gretl_matrix_free(U);
    gretl_matrix_free(sv);
    gretl_matrix_free(Vt);

    return err;
}

/* called only from svd_ridge() */

static double ridge_scale (regls_info *ri,
			   gretl_matrix *lam)
{
    double lmax = NADBL;
    int i;

    if (ri->lamscale == LAMSCALE_GLMNET) {
	gretl_matrix *Xty = gretl_matrix_alloc(ri->X->cols, 1);

	if (Xty == NULL) {
	    return lmax;
	} else {
	    /* as per glmnet, scale data by sqrt(1/n) */
	    ccd_scale(ri->X, ri->y->val, Xty->val, NULL);
	    lmax = vector_infnorm(Xty);
	    lmax *= 1000;
	    for (i=0; i<ri->nlam; i++) {
		lam->val[i] *= lmax;
	    }
	    if (ri->nlam > 1) {
		lam->val[0] = BIG_LAMBDA;
	    }
	    gretl_matrix_free(Xty);
	}
    } else {
	/* max = squared Frobenius norm = X->cols */
	lmax = ri->X->cols;
	for (i=0; i<ri->nlam; i++) {
	    lam->val[i] *= lmax;
	}
    }

    return lmax;
}

static int svd_ridge (regls_info *ri, PRN *prn)
{
    gretl_matrix *B = NULL;
    gretl_matrix *lam = NULL;
    gretl_matrix *R2 = NULL;
    gretl_matrix *crit = NULL;
    gretl_matrix *sv2 = NULL;
    double lmax = 1.0;
    int err = 0;

    lam = gretl_matrix_copy(ri->lfrac);
    B = gretl_zero_matrix_new(ri->k + ri->stdize, ri->nlam);
    if (lam == NULL || B == NULL) {
	return E_ALLOC;
    }

    if (ri->verbose) {
	sv2 = sv_squared(ri->X);
    }

    if (ri->lamscale != LAMSCALE_NONE) {
	lmax = ridge_scale(ri, lam);
    }

    if (!ri->xvalid) {
	R2 = gretl_matrix_alloc(ri->nlam, 1);
	crit = gretl_matrix_alloc(ri->nlam, 1);
	if (R2 == NULL || crit == NULL) {
	    err = E_ALLOC;
	    goto bailout;
	}
    }

    err = ridge_bhat(lam->val, ri->nlam, ri->X, ri->y, B, R2, 0);
#if 0
    fprintf(stderr, "svd ridge: err=%d, nlam=%d\n", err, ri->nlam);
#endif
    if (err) {
	goto bailout;
    }

    if (ri->lamscale == LAMSCALE_GLMNET) {
	/* not entirely truthful! */
	lam->val[0] = ri->lfrac->val[0] * lmax;
    }

    if (!ri->xvalid) {
	ccd_get_crit(B, lam, R2, ri->y, crit, 0.0, ri->k);
	if (ri->verbose) {
	    ridge_print(lam, sv2, crit, R2, prn);
	}
	if (ri->nlam > 1) {
	    double critmin = 1e200;
	    int j, idxmin = 0;

	    for (j=0; j<ri->nlam; j++) {
		if (crit->val[j] < critmin) {
		    critmin = crit->val[j];
		    idxmin = j;
		}
	    }
	    gretl_bundle_set_scalar(ri->b, "idxmin", idxmin + 1);
	    gretl_bundle_set_scalar(ri->b, "lfmin", ri->lfrac->val[idxmin]);
	}
	if (crit->rows > 1) {
	    gretl_bundle_donate_data(ri->b, "crit", crit, GRETL_TYPE_MATRIX, 0);
	    crit = NULL;
	} else {
	    gretl_bundle_set_scalar(ri->b, "crit", crit->val[0]);
	}
    }

    if (!err) {
	gretl_bundle_donate_data(ri->b, "B", B, GRETL_TYPE_MATRIX, 0);
	B = NULL;
	if (ri->lamscale != LAMSCALE_NONE) {
	    gretl_bundle_set_scalar(ri->b, "lmax", lmax * ri->n);
	}
	if (ri->nlam == 1) {
	    double lamval = ri->lfrac->val[0] * lmax;
	    gretl_matrix *vcv;

	    gretl_bundle_set_scalar(ri->b, "lambda", ri->lfrac->val[0] * lmax);
	    vcv = svd_ridge_vcv(ri, lamval, &err);
	    if (vcv != NULL) {
		gretl_bundle_donate_data(ri->b, "vcv", vcv, GRETL_TYPE_MATRIX, 0);
	    }
	}
    }

 bailout:

    gretl_matrix_free(crit);
    gretl_matrix_free(R2);
    gretl_matrix_free(B);
    gretl_matrix_free(sv2);
    gretl_matrix_free(lam);

    return err;
}

/* This function is executed when we want to obtain a set
   of coefficients using the full training data, with either
   a single value of lambda or a vector of lambdas. We come
   here straight away if the user has not requested cross
   validation; we also come here after cross validation.
*/

static int admm_lasso (regls_info *ri, PRN *prn)
{
    gretl_matrix_block *MB;
    double critmin = 1e200;
    gretl_matrix *B = NULL;
    gretl_matrix *crit = NULL;
    double lmax, rho = ri->rho;
    int k = ri->k;
    int n = ri->n;
    int i, j, ldim;
    int jmin, jmax;
    int idxmin = 0;
    int err = 0;

    gretl_vector *v, *u, *b, *r, *bprev, *bdiff;
    gretl_vector *q, *n1;
    gretl_matrix *L;

    ldim = n >= k ? k : n;
    MB = gretl_matrix_block_new(&v, k, 1, &u, k, 1,
				&b, k, 1, &r, k, 1,
				&bprev, k, 1, &bdiff, k, 1,
				&q, k, 1, &n1, n, 1,
				&L, ldim, ldim, NULL);
    if (MB == NULL) {
	return E_ALLOC;
    }
    gretl_matrix_block_zero(MB);

    lmax = ri->infnorm;

    if (!ri->xvalid && ri->nlam > 1) {
	crit = gretl_matrix_alloc(ri->nlam, 1);
    }

    if (!ri->xvalid && ri->verbose > 0) {
	lasso_lambda_report(ri, prn);
    }

    if (!err) {
	get_cholesky_factor(ri->X, L, rho);
	B = make_coeff_matrix(ri, &jmin, &jmax);
	if (B == NULL) {
	    err = E_ALLOC;
	}
    }

    if (err) {
	gretl_matrix_block_destroy(MB);
	return err;
    }

    if (!ri->xvalid && ri->verbose > 0 && ri->nlam > 1) {
	lambda_sequence_header(prn);
    }

    for (j=jmin; j<jmax && !err; j++) {
	/* loop across lambda values */
	double critj, lambda = ri->lfrac->val[j] * lmax;
	int tune_rho = 1;
	int iters = 0;
	int nnz = 0;

	err = admm_iteration(ri->X, ri->Xty, L, v, b, u, q, n1, r,
			     bprev, bdiff, lambda, &rho, tune_rho,
			     &iters);

	if (!err) {
	    for (i=0; i<k; i++) {
		if (b->val[i] != 0.0) {
		    nnz++;
		}
		if (B->cols == 1) {
		    gretl_matrix_set(B, i + ri->stdize, 0, b->val[i]);
		} else {
		    gretl_matrix_set(B, i + ri->stdize, j, b->val[i]);
		}
	    }
	    if (!ri->xvalid) {
		double R2, TSS = gretl_vector_dot_product(ri->y, ri->y, NULL);

		critj = lasso_objective(ri->X, ri->y, b, lambda, n1, TSS, &R2);
		if (ri->verbose > 0 && ri->nlam > 1) {
		    pprintf(prn, "%12f  %5d    %f   %.4f\n",
			    lambda/n, nnz, critj, R2);
		}
		if (critj < critmin) {
		    critmin = critj;
		    idxmin = j;
		}
		if (crit != NULL) {
		    crit->val[j] = critj;
		}
	    }
	}
    }

    gretl_bundle_set_scalar(ri->b, "lmax", lmax);
    if (!ri->xvalid) {
	if (ri->nlam > 1) {
	    gretl_bundle_set_scalar(ri->b, "idxmin", idxmin + 1);
	    gretl_bundle_set_scalar(ri->b, "lfmin", ri->lfrac->val[idxmin]);
	}
	if (crit != NULL) {
	    gretl_bundle_donate_data(ri->b, "crit", crit, GRETL_TYPE_MATRIX, 0);
	} else {
	    gretl_bundle_set_scalar(ri->b, "crit", critmin);
	}
    }
    if (ri->nlam == 1) {
	gretl_bundle_set_scalar(ri->b, "lambda", ri->lfrac->val[0] * lmax);
    }

    gretl_matrix_block_destroy(MB);

    return err;
}

static int admm_do_fold (const gretl_matrix *X,
			 const gretl_matrix *y,
			 const gretl_matrix *X_out,
			 const gretl_matrix *y_out,
			 const gretl_matrix *lfrac,
			 gretl_matrix *XVC,
			 double lmax, double rho0,
			 int fold, int crit_type)
{
    static gretl_vector *v, *u, *b;
    static gretl_vector *r, *bprev, *bdiff;
    static gretl_vector *q, *Xty, *n1, *L;
    static gretl_matrix_block *MB;
    double rho = rho0;
    int ldim, nlam;
    int n, k, j;
    int err = 0;

    if (X == NULL) {
	/* cleanup signal */
	gretl_matrix_block_destroy(MB);
	MB = NULL;
	return 0;
    }

    nlam = gretl_vector_get_length(lfrac);
    n = X->rows;
    k = X->cols;
    ldim = n >= k ? k : n;

    if (MB == NULL) {
	MB = gretl_matrix_block_new(&v, k, 1, &u, k, 1,
				    &b, k, 1, &r, k, 1,
				    &bprev, k, 1, &bdiff, k, 1,
				    &q, k, 1, &n1, n, 1,
				    &Xty, k, 1, &L, ldim, ldim,
				    NULL);
	if (MB == NULL) {
	    return E_ALLOC;
	}
	gretl_matrix_block_zero(MB);
    }

    /* compute X'y for the estimation sample */
    gretl_matrix_multiply_mod(X, GRETL_MOD_TRANSPOSE,
			      y, GRETL_MOD_NONE,
			      Xty, GRETL_MOD_NONE);

    get_cholesky_factor(X, L, rho);

    for (j=0; j<nlam && !err; j++) {
	/* loop across lambda values */
	double score, lambda = lfrac->val[j] * lmax;
	int tune_rho = 1;
	int iters = 0;

	err = admm_iteration(X, Xty, L, v, b, u, q, n1, r, bprev, bdiff,
			     lambda, &rho, tune_rho, &iters);

	if (!err) {
	    /* record out-of-sample criterion */
	    gretl_matrix_reuse(n1, X_out->rows, 1);
	    score = xv_score(X_out, y_out, b, n1, crit_type);
	    gretl_matrix_reuse(n1, n, 1);
	    gretl_matrix_set(XVC, j, fold, score);
	}
    }

    return err;
}

static int ccd_do_fold (gretl_matrix *X,
			gretl_matrix *y,
			gretl_matrix *X_out,
			gretl_matrix *y_out,
			const gretl_matrix *lam,
			gretl_matrix *XVC,
			int fold, int crit_type,
			double alpha)
{
    static gretl_matrix_block *MB;
    static gretl_matrix *Xty, *xv;
    static gretl_matrix *B;
    static gretl_matrix *u;
    static gretl_matrix *b;
    static int *ia, *nnz;
    int maxit = CCD_MAX_ITER;
    int nlp = 0, lmu = 0;
    int nlam, nout;
    int k, j;
    int err = 0;

    if (X == NULL) {
	/* cleanup signal */
	gretl_matrix_block_destroy(MB);
	MB = NULL;
	free(ia);
	ia = NULL;
	return 0;
    }

    /* dimensions */
    nlam = gretl_vector_get_length(lam);
    nout = X_out->rows;
    k = X->cols;

    if (MB == NULL) {
	MB = gretl_matrix_block_new(&xv, k, 1, &Xty, k, 1,
				    &B, k, nlam, &u, nout, 1,
				    &b, k, 1, NULL);
	ia = malloc((k + nlam) * sizeof *ia);
	if (MB == NULL || ia == NULL) {
	    return E_ALLOC;
	}
	nnz = ia + k;
    }
    gretl_matrix_zero(B);

    /* scale the estimation subset by sqrt(1/n) */
    ccd_scale(X, y->val, Xty->val, xv->val);

    err = ccd_iteration(alpha, X, Xty->val, nlam, lam->val,
			ccd_toler, maxit, xv->val, &lmu, B,
			ia, nnz, NULL, &nlp);
#if 0
    fprintf(stderr, "ccd: err=%d, nlp=%d, lmu=%d\n", err, nlp, lmu);
#endif

    if (!err) {
	/* record out-of-sample criteria */
	size_t bsize = k * sizeof(double);
	double score;

	for (j=0; j<nlam; j++) {
	    memcpy(b->val, B->val + j*k, bsize);
	    score = xv_score(X_out, y_out, b, u, crit_type);
	    gretl_matrix_set(XVC, j, fold, score);
	}
    }

    return err;
}

static int svd_do_fold (gretl_matrix *X,
			gretl_matrix *y,
			gretl_matrix *X_out,
			gretl_matrix *y_out,
			const gretl_matrix *lam,
			gretl_matrix *XVC,
			int fold, int crit_type,
			gint8 lamscale)
{
    static gretl_matrix_block *MB;
    static gretl_matrix *B;
    static gretl_matrix *u;
    static gretl_matrix *b;
    int nlam, nout;
    int k, j;
    int err = 0;

    if (X == NULL) {
	/* cleanup signal */
	gretl_matrix_block_destroy(MB);
	MB = NULL;
	return 0;
    }

    nlam = gretl_vector_get_length(lam);
    nout = X_out->rows;
    k = X->cols;

    if (MB == NULL) {
	MB = gretl_matrix_block_new(&B, k, nlam, &u, nout, 1,
				    &b, k, 1, NULL);
	if (MB == NULL) {
	    return E_ALLOC;
	}
    }
    gretl_matrix_zero(B);

    if (lamscale == LAMSCALE_GLMNET) {
	/* scale the estimation sample by sqrt(1/n) */
	ccd_scale(X, y->val, NULL, NULL);
    }

    err = ridge_bhat(lam->val, nlam, X, y, B, NULL, 0);

#if 0
    fprintf(stderr, "svd: err=%d, nlp=%d, lmu=%d\n", err, nlp, lmu);
#endif

    if (!err) {
	/* record out-of-sample criteria */
	size_t bsize = k * sizeof(double);
	double score;

	for (j=0; j<nlam; j++) {
	    memcpy(b->val, B->val + j*k, bsize);
	    score = xv_score(X_out, y_out, b, u, crit_type);
	    gretl_matrix_set(XVC, j, fold, score);
	}
    }

    return err;
}

/* Note: @X and @y are the full data matrices; @Xe and @ye will hold
   the estimation sample; and @Xf and @yf will hold the "fold" for
   which prediction is to be performed. We need to be careful not to
   write values out of bounds in case the two disjoint sub-samples do
   not exhaust the full data (i.e. the full number of observations is
   not divisible by the fold size without remainder).
*/

static void prepare_xv_data (const gretl_matrix *X,
			     const gretl_matrix *y,
			     gretl_matrix *Xe,
			     gretl_matrix *ye,
			     gretl_matrix *Xf,
			     gretl_matrix *yf,
			     int f)
{
    int i, j, re, rf;
    double xij;

    for (j=0; j<X->cols; j++) {
	re = rf = 0;
	for (i=0; i<X->rows; i++) {
	    xij = gretl_matrix_get(X, i, j);
	    if (i/Xf->rows == f) {
		/* "out of sample" range */
		if (rf < Xf->rows) {
		    gretl_matrix_set(Xf, rf, j, xij);
		    if (j == 0) {
			yf->val[rf] = y->val[i];
		    }
		}
		rf++;
	    } else {
		/* estimation sample */
		if (re < Xe->rows) {
		    gretl_matrix_set(Xe, re, j, xij);
		    if (j == 0) {
			ye->val[re] = y->val[i];
		    }
		}
		re++;
	    }
	}
    }
}

/* Given @XVC holding criterion values per lambda (rows) and
   per fold (columns), compose a matrix holding the means,
   plus standard errors if wanted.
*/

static gretl_matrix *process_xv_criterion (gretl_matrix *XVC,
					   gretl_matrix *lfrac,
					   int *imin, int *i1se,
					   int crit_type,
					   PRN *prn)
{
    gretl_matrix *metrics;
    double avg, d, v, se, se1, avgmin = 1e200;
    int mcols = 2;
    int nf = XVC->cols;
    int i, j;

    metrics = gretl_zero_matrix_new(XVC->rows, mcols);
    if (metrics == NULL) {
	return NULL;
    }

    *imin = 0;

    if (prn != NULL) {
	pprintf(prn, "          s        %s         se\n",
		crit_string(crit_type));
    }

    for (i=0; i<XVC->rows; i++) {
	v = avg = 0;
	for (j=0; j<nf; j++) {
	    avg += gretl_matrix_get(XVC, i, j);
	}
	avg /= nf;
	if (i == 0) {
	    avgmin = avg;
	} else if (avg < avgmin) {
	    avgmin = avg;
	    *imin = i;
	}
	gretl_matrix_set(metrics, i, 0, avg);
	for (j=0; j<nf; j++) {
	    d = gretl_matrix_get(XVC, i, j) - avg;
	    v += d * d;
	}
	v /= (nf - 1);
	se = sqrt(v/nf);
	gretl_matrix_set(metrics, i, 1, se);
	if (prn != NULL) {
	    pprintf(prn, "%11f %10f %10f\n", lfrac->val[i], avg, se);
	}
    }

    *i1se = *imin;

    /* estd. standard error of minimum average XVC */
    se1 = gretl_matrix_get(metrics, *imin, 1);

    /* Find the index of the largest lamba that gives
       an average XVC within one standard error of the
       minimum (glmnet's "$lambda.1se").
    */
    for (i=*imin-1; i>=0; i--) {
	avg = gretl_matrix_get(metrics, i, 0);
	if (avg - avgmin < se1) {
	    *i1se = i;
	} else {
	    break;
	}
    }

    return metrics;
}

/* Analyse and record results after cross-validation */

static int post_xvalidation_task (regls_info *ri,
				  gretl_matrix *XVC,
				  int crit_type,
				  PRN *prn)
{
    gretl_matrix *metrics;
    int imin = 0, i1se = 0;

    metrics = process_xv_criterion(XVC, ri->lfrac, &imin, &i1se,
				   crit_type, prn);
    if (metrics == NULL) {
	return E_ALLOC;
    }

    if (prn != NULL) {
	pprintf(prn, "\nAverage out-of-sample %s minimized at %#g for s=%#g\n",
		crit_string(crit_type), gretl_matrix_get(metrics, imin, 0),
		ri->lfrac->val[imin]);
	pprintf(prn, "Largest s within one s.e. of best criterion: %#g\n",
		ri->lfrac->val[i1se]);
    }

    gretl_bundle_donate_data(ri->b, "XVC", metrics, GRETL_TYPE_MATRIX, 0);
    gretl_bundle_set_int(ri->b, "idxmin", imin + 1);
    gretl_bundle_set_int(ri->b, "idx1se", i1se + 1);
    gretl_bundle_set_scalar(ri->b, "lfmin", ri->lfrac->val[imin]);
    gretl_bundle_set_scalar(ri->b, "lf1se", ri->lfrac->val[i1se]);

    return 0;
}

static int get_crit_type (gretl_bundle *bun)
{
    const char *s = gretl_bundle_get_string(bun, "xvcrit", NULL);
    int ret = 0;

    if (s != NULL) {
	if (g_ascii_strcasecmp(s, "mse") == 0) {
	    ret = CRIT_MSE;
	} else if (g_ascii_strcasecmp(s, "mae") == 0) {
	    ret = CRIT_MAE;
	} else {
	    gretl_errmsg_sprintf("'%s' invalid criterion", s);
	    ret = -1;
	}
    }

    return ret;
}

/* called by the cross-validation driver functions, regls_xv()
   and real_regls_xv_mpi(), for all algorithms: ADMM, CCD,
   SVD.
*/

static double get_xvalidation_lmax (regls_info *ri,
				    int esize,
				    double alpha)
{
    double lmax = ri->infnorm;

#if 0 /* lmax shouldn't really be sample-size dependent? */
    lmax *= esize / (double) X->rows;
#endif

    if (ri->ccd) {
	if (alpha < 1.0) {
	    lmax /= max(alpha, 1.0e-3);
	} else {
	    /* per observation ? */
	    lmax /= esize;
	}
    } else if (ri->ridge && ri->lamscale == LAMSCALE_GLMNET) {
	if (alpha < 1.0) {
	    lmax /= max(alpha, 1.0e-3);
	}
    } else if (ri->ridge && ri->lamscale == LAMSCALE_FROB) {
	lmax = ri->X->cols; /* ?? */
    }

    return lmax;
}

static int get_xvalidation_details (regls_info *ri,
				    int *nf, int *randfolds,
				    int *crit_type)
{
    int err = 0;

    *nf = gretl_bundle_get_int(ri->b, "nfolds", &err);
    *randfolds = gretl_bundle_get_bool(ri->b, "randfolds", 0);

    if (!err && *nf < 2) {
	err = E_INVARG;
    }
    if (!err) {
	*crit_type = get_crit_type(ri->b);
	if (*crit_type < 0) {
	    err = E_INVARG;
	}
    }

    return err;
}

static void xv_cleanup (regls_info *ri)
{
    if (ri->ccd) {
	ccd_do_fold(NULL, NULL, NULL, NULL, NULL, NULL, 0, 0, 0);
    } else if (ri->ridge) {
	svd_do_fold(NULL, NULL, NULL, NULL, NULL, NULL, 0, 0, 0);
    } else {
	admm_do_fold(NULL, NULL, NULL, NULL, NULL, NULL, 0, 0, 0, 0);
    }
}

static gretl_matrix *make_xv_lambda (regls_info *ri,
				     double lmax,
				     int *err)
{
    gretl_matrix *lam;
    int i;

    lam = gretl_matrix_copy(ri->lfrac);
    if (lam == NULL) {
	*err = E_ALLOC;
    } else if (ri->lamscale != LAMSCALE_NONE) {
	for (i=0; i<ri->nlam; i++) {
	    lam->val[i] *= lmax;
	}
	if (ri->ridge && ri->lamscale == LAMSCALE_GLMNET) {
	    lam->val[0] = BIG_LAMBDA;
	}
    }

    return lam;
}

/* unified cross validation function, employed when we're
   not doing MPI
*/

static int regls_xv (regls_info *ri, PRN *prn)
{
    gretl_matrix_block *XY;
    gretl_matrix *Xe, *Xf;
    gretl_matrix *ye, *yf;
    gretl_matrix *lam = NULL;
    gretl_matrix *XVC = NULL;
    double lmax, alpha;
    int fsize, esize;
    int randfolds = 0;
    int crit_type = 0;
    int f, nf;
    int err;

    err = get_xvalidation_details(ri, &nf, &randfolds, &crit_type);
    if (err) {
	return err;
    }

    fsize = ri->n / nf;
    esize = (nf - 1) * fsize;
    alpha = ri->ridge ? 0.0 : 1.0;

    if (ri->verbose) {
	pprintf(prn, "regls_xv: nf=%d, fsize=%d, randfolds=%d, crit=%s, "
		"ridge=%d, ccd=%d\n", nf, fsize, randfolds,
		crit_string(crit_type), ri->ridge, ri->ccd);
	gretl_flush(prn);
    }

    XY = gretl_matrix_block_new(&Xe, esize, ri->k,
				&Xf, fsize, ri->k,
				&ye, esize, 1,
				&yf, fsize, 1, NULL);
    if (XY == NULL) {
	return E_ALLOC;
    }

    lmax = get_xvalidation_lmax(ri, esize, alpha);
    if (ri->verbose) {
	pprintf(prn, "cross-validation lmax = %g\n\n", lmax);
	gretl_flush(prn);
    }

    if (ri->ccd || ri->ridge) {
	lam = make_xv_lambda(ri, lmax, &err);
    }

    if (!err && randfolds) {
	/* scramble the row order of X and y */
	randomize_rows(ri->X, ri->y);
    }

    if (!err) {
	XVC = gretl_zero_matrix_new(ri->nlam, nf);
	if (XVC == NULL) {
	    err = E_ALLOC;
	}
    }

    for (f=0; f<nf && !err; f++) {
	prepare_xv_data(ri->X, ri->y, Xe, ye, Xf, yf, f);
	if (ri->ccd) {
	    err = ccd_do_fold(Xe, ye, Xf, yf, lam, XVC, f,
			      crit_type, alpha);
	} else if (ri->ridge) {
	    err = svd_do_fold(Xe, ye, Xf, yf, lam, XVC, f,
			      crit_type, ri->lamscale);
	} else {
	    err = admm_do_fold(Xe, ye, Xf, yf, ri->lfrac, XVC,
			       lmax, ri->rho, f, crit_type);
	}
    }

    /* send deallocation signal */
    xv_cleanup(ri);

    if (!err) {
	PRN *myprn = ri->verbose ? prn : NULL;

	err = post_xvalidation_task(ri, XVC, crit_type, myprn);
	if (!err) {
	    /* determine coefficient vector(s) on full training set */
	    if (ri->ccd) {
		err = ccd_regls(ri, myprn);
	    } else if (ri->ridge) {
		err = svd_ridge(ri, myprn);
	    } else {
		err = admm_lasso(ri, myprn);
	    }
	}
    }

    gretl_matrix_free(lam);
    gretl_matrix_free(XVC);
    gretl_matrix_block_destroy(XY);

    return err;
}

#ifdef HAVE_MPI

static int real_regls_xv_mpi (regls_info *ri, PRN *prn)
{
    gretl_matrix_block *XY = NULL;
    gretl_matrix *XVC = NULL;
    gretl_matrix *Xe = NULL;
    gretl_matrix *Xf = NULL;
    gretl_matrix *ye = NULL;
    gretl_matrix *yf = NULL;
    gretl_matrix *lam = NULL;
    double lmax, alpha;
    int fsize, esize;
    int folds_per;
    int folds_rem;
    int randfolds;
    int rank;
    int crit_type = 0;
    int np, rankmax = 0;
    int f, nf, r;
    int my_f = 0;
    int err = 0;

    rank = gretl_mpi_rank();
    np = gretl_mpi_n_processes();
    rankmax = np - 1;

    err = get_xvalidation_details(ri, &nf, &randfolds, &crit_type);
    if (err) {
	return err;
    }

    fsize = ri->X->rows / nf;
    esize = (nf - 1) * fsize;
    folds_per = nf / np;
    folds_rem = nf % np;
    alpha = ri->ridge ? 0.0 : 1.0;

    /* matrix-space for per-fold data */
    XY = gretl_matrix_block_new(&Xe, esize, ri->k,
				&Xf, fsize, ri->k,
				&ye, esize, 1,
				&yf, fsize, 1, NULL);
    if (XY == NULL) {
	return E_ALLOC;
    }

    if (rank == 0) {
	lmax = get_xvalidation_lmax(ri, esize, alpha);
    }
    gretl_mpi_bcast(&lmax, GRETL_TYPE_DOUBLE, 0);

    if (randfolds) {
	/* generate the same random folds in all processes */
	unsigned seed;

	if (rank == 0) {
	    if (gretl_bundle_has_key(ri->b, "seed")) {
		seed = gretl_bundle_get_unsigned(ri->b, "seed", NULL);
	    } else {
		seed = gretl_rand_get_seed();
	    }
	}
	gretl_mpi_bcast(&seed, GRETL_TYPE_UNSIGNED, 0);
	gretl_rand_set_seed(seed);
	randomize_rows(ri->X, ri->y);
    }

    if (rank < folds_rem) {
	XVC = gretl_zero_matrix_new(ri->nlam, folds_per + 1);
    } else {
	XVC = gretl_zero_matrix_new(ri->nlam, folds_per);
    }
    if (XVC == NULL) {
	err = E_ALLOC;
    }

    if (ri->ccd || ri->ridge) {
	lam = make_xv_lambda(ri, lmax, &err);
    }

    if (rank == 0) {
	if (ri->verbose) {
	    pprintf(prn, "regls_xv_mpi: nf=%d, fsize=%d, randfolds=%d, crit=%s\n\n",
		    nf, fsize, randfolds, crit_string(crit_type));
	    gretl_flush(prn);
	}
    }

    /* process all folds */
    r = 0;
    for (f=0; f<nf && !err; f++) {
	if (rank == r) {
	    prepare_xv_data(ri->X, ri->y, Xe, ye, Xf, yf, f);
	    if (ri->verbose > 1) {
		pprintf(prn, "rank %d: taking fold %d\n", rank, f+1);
	    }
	    if (ri->ccd) {
		err = ccd_do_fold(Xe, ye, Xf, yf, lam, XVC, my_f++,
				  crit_type, alpha);
	    } else if (ri->ridge) {
		err = svd_do_fold(Xe, ye, Xf, yf, lam, XVC, my_f++,
				  crit_type, ri->lamscale);
	    } else {
		err = admm_do_fold(Xe, ye, Xf, yf, ri->lfrac, XVC, lmax,
				   ri->rho, my_f++, crit_type);
	    }
	}
	if (r == rankmax) {
	    r = 0;
	} else {
	    r++;
	}
    }

    /* reduce @XVC to root by column concatenation */
    gretl_matrix_mpi_reduce(XVC, &XVC, GRETL_MPI_HCAT, 0, OPT_NONE);

    /* send deallocation signal, all processes */
    xv_cleanup(ri);

    if (rank == 0 && !err) {
	PRN *myprn = ri->verbose ? prn : NULL;

	err = post_xvalidation_task(ri, XVC, crit_type, myprn);
	if (!err) {
	    /* determine coefficient vector on full training set */
	    if (ri->ccd) {
		err = ccd_regls(ri, myprn);
	    } else if (ri->ridge) {
		err = svd_ridge(ri, myprn);
	    } else {
		err = admm_lasso(ri, myprn);
	    }
	}
    }

    gretl_matrix_free(lam);
    gretl_matrix_free(XVC);
    gretl_matrix_block_destroy(XY);

    return err;
}

static int xv_use_mpi (regls_info *ri)
{
    int no_mpi = gretl_bundle_get_bool(ri->b, "no_mpi", 0);
    int ret = (no_mpi == 0);

    /* It's not yet clear whether MPI is useful for the ridge case, or
       for ccd in general. This may depend on the size of the data;
       experimentation is needed!
    */
    if (ret && (ri->ccd || ri->ridge)) {
	ret = 0;
    }

    return ret;
}

#endif /* HAVE_MPI or not */

int gretl_regls (gretl_matrix *X,
		 gretl_matrix *y,
		 gretl_bundle *bun,
		 PRN *prn)
{
    int (*regfunc) (regls_info *, PRN *) = NULL;
    regls_info *ri;
    int err = 0;

    ri = regls_info_new(X, y, bun, &err);
    if (err) {
	return err;
    }

    if (ri->xvalid) {
#ifdef HAVE_MPI
	if (xv_use_mpi(ri)) {
	    if (gretl_mpi_n_processes() > 1) {
		regfunc = real_regls_xv_mpi;
	    } else if (auto_mpi_ok()) {
		regfunc = mpi_parent_action;
	    }
	}
#endif
	if (regfunc == NULL) {
	    regfunc = regls_xv;
	}
    } else if (ri->ccd) {
	regfunc = ccd_regls;
    } else if (ri->ridge) {
	regfunc = svd_ridge;
    } else {
	regfunc = admm_lasso;
    }

#ifdef HAVE_MPI
    if (regfunc != mpi_parent_action) {
	err = regls_set_Xty(ri);
    }
#else
    err = regls_set_Xty(ri);
#endif

    if (!err) {
	err = regfunc(ri, prn);
    }

    free(ri);

    return err;
}

#ifdef HAVE_MPI

/* We come here if a parent process has called our
   automatic local MPI routine for cross validation:
   this function will be executed by all gretlmpi
   instances.
*/

int regls_xv_mpi (PRN *prn)
{
    regls_info *ri = NULL;
    gretl_bundle *bun = NULL;
    gretl_matrix *X;
    gretl_matrix *y;
    int err = 0;

    /* read matrices deposited by parent process */
    X = gretl_matrix_read_from_file("regls_X.bin", 1, &err);
    y = gretl_matrix_read_from_file("regls_y.bin", 1, &err);

    if (!err) {
	bun = gretl_bundle_read_from_file("regls_bun.xml", 1, &err);
    }

    if (!err) {
	ri = regls_info_new(X, y, bun, &err);
    }
    if (!err) {
	err = regls_set_Xty(ri);
    }

    if (!err) {
	err = real_regls_xv_mpi(ri, prn);
	if (!err && gretl_mpi_rank() == 0) {
	    /* write results, to be picked up by parent */
	    gretl_bundle_write_to_file(bun, "regls_XV_result.xml", 1);
	}
    }

    gretl_matrix_free(X);
    gretl_matrix_free(y);
    gretl_bundle_destroy(bun);
    free(ri);

    return err;
}

static int mpi_parent_action (regls_info *ri, PRN *prn)
{
    int err;

    err = gretl_matrix_write_to_file(ri->X, "regls_X.bin", 1);
    if (!err) {
	err = gretl_matrix_write_to_file(ri->y, "regls_y.bin", 1);
    }

    if (!err) {
	err = gretl_bundle_write_to_file(ri->b, "regls_bun.xml", 1);
    }

    if (!err) {
	/* compose and execute MPI script */
	err = foreign_start(MPI, NULL, OPT_NONE, prn);
	if (!err) {
	    int np = gretl_bundle_get_int(ri->b, "np", NULL);
	    int hpc = gretl_bundle_get_int(ri->b, "hpc", NULL);
	    gretlopt mpi_opt = OPT_S | OPT_Q;

	    if (np > 0) {
		/* user-specified number of processes */
		mpi_opt |= OPT_N;
		set_optval_int(MPI, OPT_N, np);
	    }
	    if (hpc == 0) {
		/* local machine only */
		mpi_opt |= OPT_L;
	    }
	    if (ri->verbose) {
		pputs(prn, "Invoking MPI...\n\n");
		gretl_flush(prn);
	    } else {
		fprintf(stderr, "doing MPI\n");
	    }
	    foreign_append("_regls()", MPI);
	    err = foreign_execute(NULL, mpi_opt, prn);
	    if (err) {
		fprintf(stderr, "mpi_parent: foreign exec error %d\n", err);
	    }
	}
    }

    if (!err) {
	/* retrieve results bundle written by gretlmpi */
	gretl_bundle *res;

	res = gretl_bundle_read_from_file("regls_XV_result.xml", 1, &err);
	if (!err) {
	    gretl_bundles_swap_content(ri->b, res);
	    gretl_bundle_destroy(res);
	}
    }

    return err;
}

#endif /* HAVE_MPI */
