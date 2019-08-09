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
#include "clapack_complex.h"
#include "gretl_cmatrix.h"

/* Note: since we include gretl_cmatrix.h (which in turn includes
   C99's complex.h) before fftw3.h, FFTW's fftw_complex will be
   typedef'd to C99's "double complex" and can be manipulated as
   such.
*/

#include <fftw3.h>
#include <errno.h>

#define cscalar(m) (m->rows == 1 && m->cols == 1)

/* helper function for fftw-based real FFT functions */

static int fft_allocate (double **px, gretl_matrix **pm,
			 fftw_complex **pc, int r, int c)
{
    *pm = gretl_matrix_alloc(r, c);
    if (*pm == NULL) {
	return E_ALLOC;
    }

    *px = fftw_malloc(r * sizeof **px);
    if (*px == NULL) {
	gretl_matrix_free(*pm);
	return E_ALLOC;
    }

    *pc = fftw_malloc((r/2 + 1 + r % 2) * sizeof **pc);
    if (*pc == NULL) {
	gretl_matrix_free(*pm);
	free(*px);
	return E_ALLOC;
    }

    return 0;
}

/* start fftw-based real FFT functions */

static gretl_matrix *
real_matrix_fft (const gretl_matrix *y, int inverse, int *err)
{
    gretl_matrix *ft = NULL;
    fftw_plan p = NULL;
    double *ffx = NULL;
    double xr, xi;
    fftw_complex *ffz;
    int r, c, m, odd, cr, ci;
    int i, j, cdim;

    if (y->rows < 2) {
	*err = E_DATA;
	return NULL;
    }

    r = y->rows;
    m = r / 2;
    odd = r % 2;
    c = inverse ? y->cols / 2 : y->cols;

    if (c == 0) {
	*err = E_NONCONF;
	return NULL;
    }

    cdim = inverse ? c : 2 * c;
    *err = fft_allocate(&ffx, &ft, &ffz, r, cdim);
    if (*err) {
	return NULL;
    }

    cr = 0;
    ci = 1;
    for (j=0; j<c; j++) {
	/* load the data */
	if (inverse) {
	    for (i=0; i<=m+odd; i++) {
		xr = gretl_matrix_get(y, i, cr);
		xi = gretl_matrix_get(y, i, ci);
		ffz[i] = xr + xi * I;
	    }
	} else {
	    for (i=0; i<r; i++) {
		ffx[i] = gretl_matrix_get(y, i, j);
	    }
	}

	if (j == 0) {
	    /* make the plan just once */
	    if (inverse) {
		p = fftw_plan_dft_c2r_1d(r, ffz, ffx, FFTW_ESTIMATE);
	    } else {
		p = fftw_plan_dft_r2c_1d(r, ffx, ffz, FFTW_ESTIMATE);
	    }
	}

	/* run the transform */
	fftw_execute(p);

	/* transcribe the result */
	if (inverse) {
	    for (i=0; i<r; i++) {
		gretl_matrix_set(ft, i, j, ffx[i] / r);
	    }
	} else {
	    for (i=0; i<=m+odd; i++) {
		gretl_matrix_set(ft, i, cr, creal(ffz[i]));
		gretl_matrix_set(ft, i, ci, cimag(ffz[i]));
	    }
	    for (i=m; i>0; i--) {
		gretl_matrix_set(ft, r-i, cr,  creal(ffz[i]));
		gretl_matrix_set(ft, r-i, ci, -cimag(ffz[i]));
	    }
	}
	cr += 2;
	ci += 2;
    }

    fftw_destroy_plan(p);
    fftw_free(ffz);
    fftw_free(ffx);

    return ft;
}

gretl_matrix *gretl_matrix_fft (const gretl_matrix *y, int *err)
{
    return real_matrix_fft(y, 0, err);
}

gretl_matrix *gretl_matrix_ffti (const gretl_matrix *y, int *err)
{
    return real_matrix_fft(y, 1, err);
}

/* end fftw-based real FFT functions */

static int cmatrix_validate (const gretl_matrix *m, int square)
{
    int ret = 1;

    if (gretl_is_null_matrix(m)) {
	ret = 0;
    } else if (!m->is_complex || m->z == NULL) {
	ret = 0;
    } else if (square && m->rows != m->cols) {
	ret = 0;
    }

    if (!ret) {
	fprintf(stderr, "cmatrix_validate: matrix is not OK\n");
    }

    return ret;
}

/* Construct a complex matrix, with all-zero imaginary part,
   from real matrix @A.
*/

static gretl_matrix *complex_from_real (const gretl_matrix *A,
					int *err)
{
    gretl_matrix *C = NULL;

    if (gretl_is_null_matrix(A)) {
	*err = E_DATA;
	return NULL;
    }

    C = gretl_cmatrix_new0(A->rows, A->cols);

    if (C == NULL) {
	*err = E_ALLOC;
    } else {
	int i, n = A->rows * A->cols;

	for (i=0; i<n; i++) {
	    C->z[i] = A->val[i];
	}
    }

    return C;
}

/* Multiplication of complex matrices via BLAS zgemm(),
   allowing for conjugate transposition of @A or @B.
*/

static gretl_matrix *gretl_zgemm (const gretl_matrix *A,
				  GretlMatrixMod amod,
				  const gretl_matrix *B,
				  GretlMatrixMod bmod,
				  int *err)
{
    gretl_matrix *C;
    cmplx *a, *b, *c;
    cmplx alpha = {1, 0};
    cmplx beta = {0, 0};
    char transa = 'N';
    char transb = 'N';
    integer lda, ldb, ldc;
    integer m, n, k, kb;

    if (!cmatrix_validate(A, 0) || !cmatrix_validate(B, 0)) {
	*err = E_INVARG;
	return NULL;
    }

    if (amod == GRETL_MOD_CTRANSP) {
	transa = 'C';
	m = A->cols; /* rows of op(A) */
	k = A->rows; /* cols of op(A) */
    } else {
	m = A->rows; /* complex rows of A */
	k = A->cols; /* cols of A */
    }

    if (bmod == GRETL_MOD_CTRANSP) {
	transb = 'C';
	n = B->rows;  /* columns of op(B) */
	kb = B->cols; /* rows of op(B) */
    } else {
	n = B->cols;
	kb = B->rows;
    }

    if (k != kb) {
	*err = E_NONCONF;
	return NULL;
    }

    C = gretl_cmatrix_new(m, n);
    if (C == NULL) {
	*err = E_ALLOC;
	return NULL;
    }

    a = (cmplx *) A->val;
    b = (cmplx *) B->val;
    c = (cmplx *) C->val;

    lda = A->rows;
    ldb = B->rows;
    ldc = C->rows;

    zgemm_(&transa, &transb, &m, &n, &k, &alpha, a, &lda,
	   b, &ldb, &beta, c, &ldc);

    return C;
}

static gretl_matrix *real_cmatrix_multiply (const gretl_matrix *A,
					    const gretl_matrix *B,
					    GretlMatrixMod amod,
					    int *err)
{
    gretl_matrix *C = NULL;
    gretl_matrix *T = NULL;

    if (A->is_complex && B->is_complex) {
	if (amod == 0 && (cscalar(A) || cscalar(B))) {
	    return gretl_cmatrix_dot_op(A, B, '*', err);
	} else {
	    C = gretl_zgemm(A, amod, B, 0, err);
	}
    } else if (A->is_complex) {
	/* case of real B */
	T = complex_from_real(B, err);
	if (T != NULL) {
	    C = gretl_zgemm(A, amod, T, 0, err);
	}
    } else if (B->is_complex) {
	/* case of real A */
	T = complex_from_real(A, err);
	if (T != NULL) {
	    C = gretl_zgemm(T, amod, B, 0, err);
	}
    } else {
	*err = E_TYPES;
    }

    gretl_matrix_free(T);

    return C;
}

/* Multiplication of @A times @B, where we know that at
   least one of them is complex; the other, if it's not
   complex, must be converted to a complex matrix with
   a zero imaginary part first.
*/

gretl_matrix *gretl_cmatrix_multiply (const gretl_matrix *A,
				      const gretl_matrix *B,
				      int *err)
{
    return real_cmatrix_multiply(A, B, 0, err);
}

/* Returns (conjugate transpose of A, or A^H) times B,
   allowing for the possibility that either A or B (but
   not both!) may be a real matrix on input.
*/

gretl_matrix *gretl_cmatrix_AHB (const gretl_matrix *A,
				 const gretl_matrix *B,
				 int *err)
{
    return real_cmatrix_multiply(A, B, GRETL_MOD_CTRANSP, err);
}

/* Eigen decomposition of complex (Hermitian) matrix using
   LAPACK's zheev() */

gretl_matrix *
gretl_zheev (const gretl_matrix *A, int eigenvecs, int *err)
{
    gretl_matrix *evals = NULL;
    integer n, info, lwork;
    double *w = NULL;
    double *rwork = NULL;
    cmplx *a = NULL;
    cmplx *work = NULL;
    cmplx wsz;
    char jobz = eigenvecs ? 'V' : 'N';
    char uplo = 'U';

    if (!cmatrix_validate(A, 1)) {
	*err = E_INVARG;
	return NULL;
    }

    n = A->rows;
    evals = gretl_matrix_alloc(n, 1);
    if (evals == NULL) {
	*err = E_ALLOC;
	goto bailout;
    }

    w = evals->val;
    a = (cmplx *) A->val;

    /* get optimal workspace size */
    lwork = -1;
    zheev_(&jobz, &uplo, &n, a, &n, w, &wsz, &lwork, rwork, &info);

    lwork = (integer) wsz.r;
    work = malloc(lwork * sizeof *work);
    rwork = malloc((3 * n - 2) * sizeof *rwork);
    if (work == NULL || rwork == NULL) {
	*err = E_ALLOC;
	goto bailout;
    }

    /* do the actual decomposition */
    zheev_(&jobz, &uplo, &n, a, &n, w, work, &lwork, rwork, &info);
    if (info != 0) {
	fprintf(stderr, "zheev: info = %d\n", info);
	*err = E_DATA;
    }

 bailout:

    free(rwork);
    free(work);

    if (*err) {
	gretl_matrix_free(evals);
	evals = NULL;
    }

    return evals;
}

/* Eigen decomposition of complex (non-Hermitian) matrix using
   LAPACK's zgeev() */

gretl_matrix *gretl_zgeev (const gretl_matrix *A,
			   gretl_matrix *VL,
			   gretl_matrix *VR,
			   int *err)
{
    gretl_matrix *ret = NULL;
    gretl_matrix *Acpy = NULL;
    gretl_matrix *Ltmp = NULL;
    gretl_matrix *Rtmp = NULL;
    integer n, info, lwork;
    integer ldvl, ldvr;
    double *w = NULL;
    double *rwork = NULL;
    cmplx *a = NULL;
    cmplx *work = NULL;
    cmplx *vl = NULL, *vr = NULL;
    cmplx wsz;
    char jobvl = VL != NULL ? 'V' : 'N';
    char jobvr = VR != NULL ? 'V' : 'N';

    if (!cmatrix_validate(A, 1)) {
	*err = E_INVARG;
	return NULL;
    }

    n = A->rows;
    ldvl = VL != NULL ? n : 1;
    ldvr = VR != NULL ? n : 1;

    /* we need a copy of @A, which gets overwritten */
    Acpy = gretl_matrix_copy(A);
    if (Acpy == NULL) {
	*err = E_ALLOC;
	goto bailout;
    }

    a = (cmplx *) Acpy->val;

    if (VL != NULL) {
	/* left eigenvectors wanted */
	if (VL->is_complex && VL->rows * VL->cols == A->rows * A->cols) {
	    /* VL is useable as is */
	    VL->rows = A->rows;
	    VL->cols = A->cols;
	    vl = (cmplx *) VL->val;
	} else {
	    /* we need to allocate storage */
	    Ltmp = gretl_cmatrix_new0(A->rows, A->cols);
	    if (Ltmp == NULL) {
		*err = E_ALLOC;
		goto bailout;
	    }
	    vl = (cmplx *) Ltmp->val;
	}
    }

    if (VR != NULL) {
	/* right eigenvectors wanted */
	if (VR->is_complex && VR->rows * VR->cols == A->rows * A->cols) {
	    /* VR is useable as is */
	    VR->rows = A->rows;
	    VR->cols = A->cols;
	    vr = (cmplx *) VR->val;
	} else {
	    /* we need to allocate storage */
	    Rtmp = gretl_cmatrix_new0(A->rows, A->cols);
	    if (Rtmp == NULL) {
		*err = E_ALLOC;
		goto bailout;
	    }
	    vr = (cmplx *) Rtmp->val;
	}
    }

    rwork = malloc(2 * n * sizeof *rwork);
    ret = gretl_cmatrix_new(n, 1);
    if (rwork == NULL || ret == NULL) {
	*err = E_ALLOC;
	goto bailout;
    }

    w = ret->val;

    /* get optimal workspace size */
    lwork = -1;
    zgeev_(&jobvl, &jobvr, &n, a, &n, w, vl, &ldvl, vr, &ldvr,
	   &wsz, &lwork, rwork, &info);
    lwork = (integer) wsz.r;
    work = malloc(lwork * sizeof *work);
    if (work == NULL || rwork == NULL) {
	*err = E_ALLOC;
	goto bailout;
    }

    /* do the actual decomposition */
    zgeev_(&jobvl, &jobvr, &n, a, &n, w, vl, &ldvl, vr, &ldvr,
	   work, &lwork, rwork, &info);
    if (info != 0) {
	fprintf(stderr, "zgeev: info = %d\n", info);
	*err = E_DATA;
    } else {
	if (Ltmp != NULL) {
	    gretl_matrix_replace_content(VL, Ltmp);
	    VL->is_complex = 1;
	}
	if (Rtmp != NULL) {
	    gretl_matrix_replace_content(VR, Rtmp);
	    VR->is_complex = 1;
	}
    }

 bailout:

    free(rwork);
    free(work);
    gretl_matrix_free(Acpy);
    gretl_matrix_free(Ltmp);
    gretl_matrix_free(Rtmp);

    if (*err) {
	gretl_matrix_free(ret);
	ret = NULL;
    }

    return ret;
}

/* Inverse of a complex matrix via LU decomposition using the
   LAPACK functions zgetrf() and zgetri()
*/

gretl_matrix *gretl_cmatrix_inverse (const gretl_matrix *A, int *err)
{
    gretl_matrix *Ainv = NULL;
    integer lwork = -1;
    integer *ipiv;
    cmplx *a, *work = NULL;
    integer n, info;

    if (!cmatrix_validate(A, 1)) {
	*err = E_INVARG;
	return NULL;
    }

    Ainv = gretl_matrix_copy(A);
    if (Ainv == NULL) {
	*err = E_ALLOC;
	return NULL;
    }

    n = A->cols;

    ipiv = malloc(2 * n * sizeof *ipiv);
    if (ipiv == NULL) {
	*err = E_ALLOC;
	goto bailout;
    }

    a = (cmplx *) Ainv->val;

    zgetrf_(&n, &n, a, &n, ipiv, &info);
    if (info != 0) {
	printf("zgetrf: info = %d\n", info);
	*err = E_DATA;
    }

    if (!*err) {
	/* workspace size query */
	cmplx wsz;

	zgetri_(&n, a, &n, ipiv, &wsz, &lwork, &info);
	lwork = (integer) wsz.r;
	work = malloc(lwork * sizeof *work);
	if (work == NULL) {
	    *err = E_ALLOC;
	}
    }

    if (!*err) {
	/* actual inversion */
	zgetri_(&n, a, &n, ipiv, work, &lwork, &info);
	if (info != 0) {
	    printf("zgetri: info = %d\n", info);
	    *err = E_DATA;
	}
    }

 bailout:

    free(work);
    free(ipiv);

    if (*err && Ainv != NULL) {
	gretl_matrix_free(Ainv);
	Ainv = NULL;
    }

    return Ainv;
}

enum {
    SVD_THIN,
    SVD_FULL
};

/* SVD of a complex matrix via the LAPACK function zgesvd() */

int gretl_cmatrix_SVD (const gretl_matrix *x, gretl_matrix **pu,
		       gretl_vector **ps, gretl_matrix **pvt,
		       int smod)
{
    integer m, n, lda;
    integer ldu = 1, ldvt = 1;
    integer lwork = -1L;
    double *rwork = NULL;
    integer info;
    gretl_matrix *a = NULL;
    gretl_matrix *s = NULL;
    gretl_matrix *u = NULL;
    gretl_matrix *vt = NULL;
    cmplx *az;
    char jobu = 'N', jobvt = 'N';
    cmplx zu, zvt;
    cmplx *uval = &zu;
    cmplx *vtval = &zvt;
    cmplx *work = NULL;
    int xsize, k, err = 0;

    if (pu == NULL && ps == NULL && pvt == NULL) {
	/* no-op */
	return 0;
    }

    if (!cmatrix_validate(x, 0)) {
	return E_INVARG;
    }

    lda = m = x->rows;
    n = x->cols;
    xsize = lda * n;

    if (smod == SVD_THIN && m < n) {
	fprintf(stderr, "gretl_cmatrix_SVD: a is %d x %d, should be 'thin'\n",
		a->rows, a->cols);
	return E_NONCONF;
    }

    az = malloc(xsize * sizeof *az);
    if (az == NULL) {
	return E_ALLOC;
    }
    memcpy(az, x->val, xsize * sizeof *az);

    k = MIN(m, n);

    s = gretl_vector_alloc(k);
    if (s == NULL) {
	err = E_ALLOC;
	goto bailout;
    }

    if (pu != NULL) {
	ldu = m;
	if (smod == SVD_FULL) {
	    u = gretl_cmatrix_new(ldu, m);
	} else {
	    u = gretl_cmatrix_new(ldu, n);
	}
	if (u == NULL) {
	    err = E_ALLOC;
	    goto bailout;
	} else {
	    uval = (cmplx *) u->val;
	    jobu = (smod == SVD_FULL)? 'A' : 'S';
	}
    }

    if (pvt != NULL) {
	ldvt = n;
	vt = gretl_cmatrix_new(ldvt, n);
	if (vt == NULL) {
	    err = E_ALLOC;
	    goto bailout;
	} else {
	    vtval = (cmplx *) vt->val;
	    jobvt = 'A';
	}
    }

    work = malloc(sizeof *work);
    rwork = malloc((5 * MIN(m,n)) * sizeof *rwork);
    if (work == NULL || rwork == NULL) {
	err = E_ALLOC;
	goto bailout;
    }

    /* workspace query */
    lwork = -1;
    zgesvd_(&jobu, &jobvt, &m, &n, az, &lda, s->val, uval, &ldu,
	    vtval, &ldvt, work, &lwork, rwork, &info);

    if (info != 0 || work[0].r <= 0.0) {
	fprintf(stderr, "zgesvd: workspace query failed\n");
	err = E_DATA;
	goto bailout;
    }

    lwork = (integer) work[0].r;
    work = realloc(work, lwork * sizeof *work);
    if (work == NULL) {
	err = E_ALLOC;
	goto bailout;
    }

    /* actual computation */
    zgesvd_(&jobu, &jobvt, &m, &n, az, &lda, s->val, uval, &ldu,
	    vtval, &ldvt, work, &lwork, rwork, &info);

    if (info != 0) {
	fprintf(stderr, "gretl_cmatrix_SVD: info = %d\n", (int) info);
	err = E_DATA;
	goto bailout;
    }

    if (ps != NULL) {
	*ps = s;
	s = NULL;
    }
    if (pu != NULL) {
	*pu = u;
	u = NULL;
    }
    if (pvt != NULL) {
	*pvt = vt;
	vt = NULL;
    }

 bailout:

    free(az);
    free(work);
    free(rwork);
    gretl_matrix_free(s);
    gretl_matrix_free(u);
    gretl_matrix_free(vt);

    return err;
}

/* generalized inverse of a complex matrix via its SVD */

gretl_matrix *gretl_cmatrix_ginv (const gretl_matrix *A, int *err)
{
    gretl_matrix *U = NULL;
    gretl_matrix *V = NULL;
    gretl_matrix *s = NULL;
    gretl_matrix *Vt = NULL;
    gretl_matrix *ret = NULL;

    *err = gretl_cmatrix_SVD(A, &U, &s, &V, 1);

    if (!*err) {
	double complex vij;
	int i, j, h = 0;

	for (i=0; i<s->cols; i++) {
	    h += s->val[i] > 1.0e-13;
	}
	Vt = gretl_ctrans(V, 1, err);
	if (!*err) {
	    for (j=0; j<h; j++) {
		for (i=0; i<Vt->rows; i++) {
		    vij = gretl_cmatrix_get(Vt, i, j);
		    gretl_cmatrix_set(Vt, i, j, vij / s->val[j]);
		}
	    }
	}
	if (!*err) {
	    Vt->cols = U->cols = h;
	    ret = gretl_zgemm(Vt, 0, U, GRETL_MOD_CTRANSP, err);
	}
    }

    gretl_matrix_free(U);
    gretl_matrix_free(V);
    gretl_matrix_free(s);
    gretl_matrix_free(Vt);

    return ret;
}

static gretl_matrix *real_cmatrix_hdp (const gretl_matrix *A,
				       const gretl_matrix *B,
				       int *err)
{
    gretl_matrix *C = NULL;
    int r, p, q;

    if (!cmatrix_validate(A,0) || !cmatrix_validate(B,0)) {
	*err = E_INVARG;
	return NULL;
    }

    if (B->rows != A->rows) {
	*err = E_NONCONF;
	return NULL;
    }

    r = A->rows;
    p = A->cols;
    q = B->cols;

    C = gretl_cmatrix_new0(r, p*q);

    if (C == NULL) {
	*err = E_ALLOC;
    } else {
	double complex aij, bik;
	int i, j, k, joff;

	for (i=0; i<r; i++) {
	    for (j=0; j<p; j++) {
		aij = gretl_cmatrix_get(A, i, j);
		if (aij != 0.0) {
		    joff = j * q;
		    for (k=0; k<q; k++) {
			bik = gretl_cmatrix_get(B, i, k);
			gretl_cmatrix_set(C, i, joff + k, aij*bik);
		    }
		}
	    }
	}
    }

    return C;
}

static gretl_matrix *real_cmatrix_kron (const gretl_matrix *A,
					const gretl_matrix *B,
					int *err)
{
    gretl_matrix *K = NULL;
    int p, q, r, s;

    if (!cmatrix_validate(A,0) || !cmatrix_validate(B,0)) {
	*err = E_INVARG;
	return NULL;
    }

    p = A->rows;
    q = A->cols;
    r = B->rows;
    s = B->cols;

    K = gretl_cmatrix_new0(p*r, q*s);

    if (K == NULL) {
	*err = E_ALLOC;
    } else {
	double complex x, aij, bkl;
	int i, j, k, l;
	int ioff, joff;
	int Ki, Kj;

	for (i=0; i<p; i++) {
	    ioff = i * r;
	    for (j=0; j<q; j++) {
		/* block ij is an r * s matrix, a_{ij} * B */
		aij = gretl_cmatrix_get(A, i, j);
		joff = j * s;
		for (k=0; k<r; k++) {
		    Ki = ioff + k;
		    for (l=0; l<s; l++) {
			bkl = gretl_cmatrix_get(B, k, l);
			Kj = joff + l;
			x = aij * bkl;
			gretl_cmatrix_set(K, Ki, Kj, x);
		    }
		}
	    }
	}
    }

    return K;
}

gretl_matrix *gretl_cmatrix_kronlike (const gretl_matrix *A,
				      const gretl_matrix *B,
				      int hdp, int *err)
{
    gretl_matrix *L = (gretl_matrix *) A;
    gretl_matrix *R = (gretl_matrix *) B;
    gretl_matrix *C = NULL;

    if (A->is_complex && B->is_complex) {
	; /* OK */
    } else if (A->is_complex) {
	R = complex_from_real(B, err);
    } else if (B->is_complex) {
	L = complex_from_real(A, err);
    } else {
	*err = E_TYPES;
    }

    if (!*err) {
	if (hdp) {
	    C = real_cmatrix_hdp(L, R, err);
	} else {
	    C = real_cmatrix_kron(L, R, err);
	}
    }

    if (L != A) gretl_matrix_free(L);
    if (R != B) gretl_matrix_free(R);

    return C;
}

gretl_matrix *gretl_cmatrix_hdprod (const gretl_matrix *A,
				    const gretl_matrix *B,
				    int *err)
{
    return gretl_cmatrix_kronlike(A, B, 1, err);
}

gretl_matrix *gretl_cmatrix_kronecker (const gretl_matrix *A,
				       const gretl_matrix *B,
				       int *err)
{
    return gretl_cmatrix_kronlike(A, B, 0, err);
}

/* Complex FFT (or inverse) via fftw */

gretl_matrix *gretl_complex_fft (const gretl_matrix *A,
				 int inverse, int *err)
{
    gretl_matrix *B = NULL;
    fftw_complex *tmp, *ptr;
    fftw_plan p;
    int sign;
    int r, c, j;

    if (!cmatrix_validate(A, 0)) {
	*err = E_INVARG;
	return NULL;
    }

    B = gretl_matrix_copy(A);
    if (B == NULL) {
	*err = E_ALLOC;
	return NULL;
    }

    r = A->rows;
    c = A->cols;

    tmp = (fftw_complex *) B->val;
    sign = inverse ? FFTW_BACKWARD : FFTW_FORWARD;

    ptr = tmp;
    for (j=0; j<c; j++) {
	p = fftw_plan_dft_1d(r, ptr, ptr, sign, FFTW_ESTIMATE);
	fftw_execute(p);
	fftw_destroy_plan(p);
	/* advance pointer to next column */
	ptr += r;
    }

    if (inverse) {
	/* "FFTW computes an unnormalized transform: computing a
	    forward followed by a backward transform (or vice versa)
	    will result in the original data multiplied by the size of
	    the transform (the product of the dimensions)."
	    So should we do the following?
	*/
	for (j=0; j<r*c; j++) {
	    tmp[j] /= r;
	}
    }

    return B;
}

int complex_matrix_print_range (const gretl_matrix *A,
				const char *name,
				int rmin, int rmax,
				PRN *prn)
{
    double complex aij;
    double re, im, xmax;
    char s[4] = "   ";
    int all_ints = 1;
    int zwidth = 0;
    int r, c, i, j;

    if (!cmatrix_validate(A, 0)) {
	return E_INVARG;
    }

    r = A->rows;
    c = A->cols;

    if (rmin < 0) rmin = 0;
    if (rmax < 0) rmax = r;

    xmax = 0;

    for (j=0; j<c && all_ints; j++) {
	for (i=rmin; i<rmax && all_ints; i++) {
	    aij = gretl_cmatrix_get(A, i, j);
	    re = creal(aij);
	    im = cimag(aij);
	    if (floor(re) != re || floor(im) != im) {
		all_ints = 0;
	    } else {
		re = MAX(fabs(re), fabs(im));
		if (re > xmax) {
		    xmax = re;
		}
	    }
	}
    }

    if (all_ints && xmax > 0) {
	/* try for a more compact format */
	xmax = log10(xmax);
	if (xmax > 0 && xmax < 3) {
	    zwidth = floor(xmax) + 2;
	}
    }

    if (name != NULL && *name != '\0') {
	pprintf(prn, "%s (%d x %d)", name, r, c);
	pputs(prn, "\n\n");
    }

    for (i=rmin; i<rmax; i++) {
	for (j=0; j<c; j++) {
	    aij = gretl_cmatrix_get(A, i, j);
	    re = creal(aij);
	    im = cimag(aij);
	    s[1] = (im >= 0) ? '+' : '-';
	    if (zwidth > 0) {
		pprintf(prn, "%*g%s%*gi", zwidth, re, s, zwidth-1, fabs(im));
	    } else {
		pprintf(prn, "%7.4f%s%6.4fi", re, s, fabs(im));
	    }
	    if (j < c - 1) {
		pputs(prn, "  ");
	    }
        }
        pputc(prn, '\n');
    }
    pputc(prn, '\n');

    return 0;
}

int complex_matrix_print (const gretl_matrix *A,
			  const char *name,
			  PRN *prn)
{
    return complex_matrix_print_range(A, name, -1, -1, prn);
}

int complex_matrix_printf (const gretl_matrix *A,
			   const char *fmt,
			   PRN *prn)
{
    double complex aij;
    double re, im;
    gchar *fmtstr = NULL;
    char s[3] = "  ";
    int r, c, i, j;

    if (!cmatrix_validate(A, 0)) {
	return E_INVARG;
    }

    if (fmt == NULL) {
	fmt = "%7.4f";
    } else {
	/* we only accept floating-point formats */
	char c = fmt[strlen(fmt) - 1];

	if (c != 'f' && c != 'g') {
	    return E_INVARG;
	}
    }

    fmtstr = g_strdup_printf("%s%%s%si", fmt, fmt);

    r = A->rows;
    c = A->cols;

    for (i=0; i<r; i++) {
	for (j=0; j<c; j++) {
	    aij = gretl_cmatrix_get(A, i, j);
	    re = creal(aij);
	    im = cimag(aij);
	    s[1] = (im >= 0) ? '+' : '-';
	    pprintf(prn, fmtstr, re, s, fabs(im));
	    if (j < c - 1) {
		pputs(prn, "  ");
	    }
        }
        pputc(prn, '\n');
    }
    pputc(prn, '\n');

    g_free(fmtstr);

    return 0;
}

/* Compose a complex matrix from its real and imaginary
   components. If @Im is NULL the matrix will have a
   constant imaginary part given by @ival; otherwise
   the matrices @Re and @Im must be of the same
   dimensions.
*/

gretl_matrix *gretl_cmatrix (const gretl_matrix *Re,
			     const gretl_matrix *Im,
			     double ival, int *err)
{
    gretl_matrix *C = NULL;
    int i, n;

    if (gretl_is_null_matrix(Re)) {
	*err = E_INVARG;
    } else if (Im != NULL) {
	if (Im->rows != Re->rows || Im->cols != Re->cols) {
	    *err = E_NONCONF;
	}
    }

    if (!*err) {
	n = Re->rows * Re->cols;
	C = gretl_cmatrix_new(Re->rows, Re->cols);
	if (C == NULL) {
	    *err = E_ALLOC;
	}
    }

    if (!*err) {
	for (i=0; i<n; i++) {
	    if (Im != NULL) {
		ival = Im->val[i];
	    }
	    C->z[i] = Re->val[i] + ival * I;
	}
    }

    return C;
}

/* Extract the real part of @A if @im = 0 or the imaginary part
   if @im is non-zero.
*/

gretl_matrix *gretl_cxtract (const gretl_matrix *A, int im,
			     int *err)
{
    gretl_matrix *B = NULL;
    int i, n;

    if (!cmatrix_validate(A, 0)) {
	*err = E_INVARG;
	return NULL;
    }

    B = gretl_matrix_alloc(A->rows, A->cols);

    if (B == NULL) {
	*err = E_ALLOC;
    } else {
	n = A->rows * A->cols;
	for (i=0; i<n; i++) {
	    B->val[i] = im ? cimag(A->z[i]) : creal(A->z[i]);
	}
    }

    return B;
}

/* Return [conjugate] transpose of complex matrix @A */

gretl_matrix *gretl_ctrans (const gretl_matrix *A,
			    int conjugate, int *err)
{
    gretl_matrix *C = NULL;
    double complex aij;
    int i, j;

    if (!cmatrix_validate(A, 0)) {
	*err = E_INVARG;
	return NULL;
    }

    C = gretl_cmatrix_new(A->cols, A->rows);
    if (C == NULL) {
	*err = E_ALLOC;
	return NULL;
    }

    for (j=0; j<A->cols; j++) {
	for (i=0; i<A->rows; i++) {
	    aij = gretl_cmatrix_get(A, i, j);
	    if (conjugate) {
		aij = conj(aij);
	    }
	    gretl_cmatrix_set(C, j, i, aij);
	}
    }

    return C;
}

/* Convert complex matrix @A to its conjugate transpose */

int gretl_ctrans_in_place (gretl_matrix *A)
{
    gretl_matrix *C = NULL;
    double complex aij;
    int i, j, n;
    int err = 0;

    if (!cmatrix_validate(A, 0)) {
	return E_INVARG;
    }

    /* temporary matrix */
    C = gretl_cmatrix_new(A->cols, A->rows);
    if (C == NULL) {
	return E_ALLOC;
    }

    for (j=0; j<A->cols; j++) {
	for (i=0; i<A->rows; i++) {
	    aij = gretl_cmatrix_get(A, i, j);
	    gretl_cmatrix_set(C, j, i, conj(aij));
	}
    }

    /* now rejig @A */
    A->rows = C->rows;
    A->cols = C->cols;
    n = C->rows * C->cols;
    memcpy(A->z, C->z, n * sizeof *A->z);
    gretl_matrix_destroy_info(A);

    gretl_matrix_free(C);

    return err;
}

/* Addition or subtraction of matrices: handle the case
   where one operand is complex and the other is real.
   In addition, if both matrices are complex, handle the
   case where one of them is a complex scalar.
*/

gretl_matrix *cmatrix_add_sub (const gretl_matrix *A,
			       const gretl_matrix *B,
			       int sgn, int *err)
{
    gretl_matrix *C = NULL;
    int cr = A->rows;
    int cc = A->cols;
    int a_scalar = 0;
    int b_scalar = 0;

    if (A->is_complex && cscalar(B)) {
	b_scalar = 1;
	goto allocate;
    } else if (cscalar(A) && B->is_complex) {
	cr = B->rows;
	cc = B->cols;
	a_scalar = 1;
	goto allocate;
    }

    if (B->cols != A->cols) {
	*err = E_NONCONF;
	return NULL;
    }

    if (A->is_complex && B->is_complex) {
	/* both complex */
	if (B->rows != cr) {
	    *err = E_NONCONF;
	}
    } else if (A->is_complex) {
	/* A complex, B real */
	if (B->rows != cr) {
	    *err = E_NONCONF;
	}
    } else {
	/* A real, B complex */
	cr = B->rows;
	if (A->rows != cr) {
	    *err = E_NONCONF;
	}
    }

 allocate:

    if (!*err) {
	C = gretl_cmatrix_new(cr, cc);
	if (C == NULL) {
	    *err = E_ALLOC;
	}
    }

    if (!*err) {
	int i, n = cc * cr;

	if (b_scalar) {
	    for (i=0; i<n; i++) {
		C->z[i] = sgn < 0 ? A->z[i] - B->z[0] : A->z[i] + B->z[0];
	    }
	} else if (a_scalar) {
	    for (i=0; i<n; i++) {
		C->z[i] = sgn < 0 ? A->z[0] - B->z[i] : A->z[0] + B->z[i];
	    }
	} else if (A->is_complex && B->is_complex) {
	    for (i=0; i<n; i++) {
		C->z[i] = sgn < 0 ? A->z[i] - B->z[i] : A->z[i] + B->z[i];
	    }
	} else if (A->is_complex) {
	    for (i=0; i<n; i++) {
		C->z[i] = sgn < 0 ? A->z[i] - B->val[i] : A->z[i] + B->val[i];
	    }
	} else {
	    for (i=0; i<n; i++) {
		C->z[i] = sgn < 0 ? A->val[i] - B->z[i] : A->val[i] + B->z[i];
	    }
	}
    }

    return C;
}

/* When adding a real scalar to a complex matrix, only
   the real elements of the matrix should be changed.
*/

int cmatrix_add_scalar (gretl_matrix *targ,
			const gretl_matrix *A,
			double x, int Asign)
{
    int i, n = A->rows * A->cols;

    for (i=0; i<n; i++) {
	targ->z[i] = Asign < 0 ? x - A->z[i] : x + A->z[i];
    }

    return 0;
}

int apply_cmatrix_dfunc (gretl_matrix *targ,
			const gretl_matrix *src,
			double (*dfunc) (double complex))
{
    int n = src->cols * src->rows;
    int i, err = 0;

    if (!cmatrix_validate(src, 0)) {
	return E_INVARG;
    }

    errno = 0;

    for (i=0; i<n; i++) {
	targ->val[i] = dfunc(src->z[i]);
    }

    if (errno) {
	gretl_errmsg_set_from_errno(NULL, errno);
	err = E_DATA;
    }

    return err;
}

int apply_cmatrix_cfunc (gretl_matrix *targ,
			 const gretl_matrix *src,
			 double complex (*cfunc) (double complex))
{
    int n = src->cols * src->rows;
    int i, err;

    if (!cmatrix_validate(src, 0) || !cmatrix_validate(targ, 0)) {
	return E_INVARG;
    }

    errno = 0;

    for (i=0; i<n; i++) {
	targ->z[i] = cfunc(src->z[i]);
    }

    if (errno) {
	gretl_errmsg_set_from_errno(NULL, errno);
	err = E_DATA;
    }

    return err;
}

int apply_cmatrix_unary_op (gretl_matrix *targ,
			    const gretl_matrix *src,
			    int op)
{
    int i, n = src->cols * src->rows;
    int err;

    if (!cmatrix_validate(src, 0) || !cmatrix_validate(targ, 0)) {
	return E_INVARG;
    }

    for (i=0; i<n && !err; i++) {
	if (op == 1) {
	    /* U_NEG */
	    targ->z[i] = -src->z[i];
	} else if (op == 2) {
	    /* U_POS */
	    targ->z[i] = src->z[i];
	} else if (op == 3) {
	    /* U_NOT */
	    targ->z[i] = (src->z[i] == 0);
	} else {
	    err = E_INVARG;
	}
    }

    return err;
}

static gretl_matrix *complex_scalar_to_mat (double complex z,
					    int *err)
{
    gretl_matrix *ret = gretl_cmatrix_new(1, 1);

    if (ret == NULL) {
	*err = E_ALLOC;
    } else {
	ret->z[0] = z;
    }

    return ret;
}

gretl_matrix *gretl_cmatrix_determinant (const gretl_matrix *X,
					 int log, int *err)
{
    gretl_matrix *E = NULL;
    gretl_matrix *ret = NULL;

    if (!cmatrix_validate(X, 1)) {
	*err = E_INVARG;
	return ret;
    }

    E = gretl_zgeev(X, NULL, NULL, err);

    if (E != NULL) {
	double complex cret = 1;
	int i;

	for (i=0; i<E->rows; i++) {
	    cret *= E->z[i];
	}
	gretl_matrix_free(E);
	if (log) {
	    cret = clog(cret);
	}
	ret = complex_scalar_to_mat(cret, err);
    }

    return ret;
}

gretl_matrix *gretl_cmatrix_trace (const gretl_matrix *X,
				   int *err)
{
    gretl_matrix *ret = NULL;

    if (!cmatrix_validate(X, 1)) {
	*err = E_INVARG;
    } else {
	double complex tr = 0;
	int i;

	for (i=0; i<X->rows; i++) {
	    tr += gretl_cmatrix_get(X, i, i);
	}
	ret = complex_scalar_to_mat(tr, err);
    }

    return ret;
}

/* Retrieve the diagonal of complex matrix @X in the form
   of a complex column vector.
*/

gretl_matrix *gretl_cmatrix_get_diagonal (const gretl_matrix *X,
					  int *err)
{
    gretl_matrix *ret = NULL;
    int d, i;

    if (!cmatrix_validate(X, 0)) {
	*err = E_INVARG;
    } else {
	d = MIN(X->rows, X->cols);
	ret = gretl_cmatrix_new(d, 1);
	if (ret == NULL) {
	    *err = E_ALLOC;
	} else {
	    for (i=0; i<d; i++) {
		ret->z[i] = gretl_cmatrix_get(X, i, i);
	    }
	}
    }

    return ret;
}

/* Set the diagonal of complex matrix @targ using either
   @src (if not NULL) or @x. In the first case @src can
   be either a complex vector of the right length, or a
   real vector, or a complex scalar.
*/

int gretl_cmatrix_set_diagonal (gretl_matrix *targ,
				const gretl_matrix *src,
				double x)
{
    double complex zi = 0;
    int d, i;
    int match = 0;
    int err = 0;

    if (!cmatrix_validate(targ, 0)) {
	return E_INVARG;
    }

    d = MIN(targ->rows, targ->cols);

    if (src != NULL) {
	if (src->is_complex) {
	    if (gretl_vector_get_length(src) == d) {
		/* conformable complex vector */
		match = 1;
	    } else if (cscalar(src)) {
		/* complex scalar */
		zi = *(double complex *) src->val;
		match = 2;
	    }
	} else if (gretl_vector_get_length(src) == d) {
	    /* conformable real vector */
	    match = 3;
	}
    } else {
	/* use real scalar, @x */
	zi = x;
	match = 4;
    }

    if (match == 0) {
	return E_NONCONF;
    }

    for (i=0; i<d; i++) {
	if (match == 1) {
	    gretl_cmatrix_set(targ, i, i, src->z[i]);
	} else if (match == 3) {
	    gretl_cmatrix_set(targ, i, i, src->val[i]);
	} else {
	    gretl_cmatrix_set(targ, i, i, zi);
	}
    }

    return err;
}

gretl_matrix *gretl_cmatrix_vec (const gretl_matrix *X,
				 int *err)
{
    gretl_matrix *ret = NULL;

    if (!cmatrix_validate(X, 0)) {
	*err = E_INVARG;
    } else {
	int n = X->cols * X->rows;

	ret = gretl_cmatrix_new(n, 1);
	if (ret == NULL) {
	    *err = E_ALLOC;
	} else {
	    memcpy(ret->z, X->z, n * sizeof *ret->z);
	}
    }

    return ret;
}

gretl_matrix *gretl_cmatrix_vech (const gretl_matrix *X,
				  int *err)
{
    gretl_matrix *ret = NULL;

    if (!cmatrix_validate(X, 1)) {
	*err = E_INVARG;
    } else {
	int r = X->rows;
	int m = r * (r+1) / 2;

	ret = gretl_cmatrix_new(m, 1);
	if (ret == NULL) {
	    *err = E_ALLOC;
	} else {
	    int i, j, k = 0;

	    for (i=0; i<r; i++) {
		for (j=i; j<r; j++) {
		    ret->z[k++] = gretl_cmatrix_get(X, i, j);
		}
	    }
	}
    }

    return ret;
}

gretl_matrix *gretl_cmatrix_unvech (const gretl_matrix *X,
				    int *err)
{
    gretl_matrix *ret = NULL;

    if (!cmatrix_validate(X, 0) || X->cols != 1) {
	*err = E_INVARG;
    } else {
	int r = X->rows;
	int n = (int) ((sqrt(1.0 + 8.0 * r) - 1.0) / 2.0);

	ret = gretl_cmatrix_new(n, n);
	if (ret == NULL) {
	    *err = E_ALLOC;
	} else {
	    double complex zk;
	    int i, j, k = 0;

	    for (j=0; j<n; j++) {
		for (i=j; i<n; i++) {
		    zk = X->z[k++];
		    gretl_cmatrix_set(ret, i, j, zk);
		    gretl_cmatrix_set(ret, j, i, zk);
		}
	    }
	}
    }

    return ret;
}

/*
  Copies the values from row @is of @src into row @id
  of @dest, provided @src and @dest have the same number
  of columns.
*/

static void cmatrix_copy_row (gretl_matrix *dest, int id,
			      const gretl_matrix *src, int is)
{
    double complex zj;
    int j;

    for (j=0; j<src->cols; j++) {
	zj = gretl_cmatrix_get(src, is, j);
	gretl_cmatrix_set(dest, id, j, zj);
    }
}

gretl_matrix *gretl_cmatrix_reverse_rows (const gretl_matrix *X,
					  int *err)
{
    gretl_matrix *ret;
    int i, r, c;

    if (!cmatrix_validate(X, 0)) {
	*err = E_INVARG;
	return NULL;
    } else if (gretl_is_null_matrix(X)) {
	return gretl_null_matrix_new();
    }

    r = X->rows;
    c = X->cols;
    ret = gretl_cmatrix_new(r, c);

    if (ret == NULL) {
	*err = E_ALLOC;
    } else {
	for (i=0; i<r; i++) {
	    cmatrix_copy_row(ret, i, X, r-i-1);
	}
    }

    return ret;
}

gretl_matrix *gretl_cmatrix_shape (const gretl_matrix *A,
				   int r, int c, int *err)
{
    gretl_matrix *B;
    int i, k, nA, nB;

    if (!cmatrix_validate(A, 0) || r < 0 || c < 0) {
	*err = E_INVARG;
	return NULL;
    }

    if (r == 0 && c == 0) {
	return gretl_null_matrix_new();
    }

    B = gretl_cmatrix_new(r, c);
    if (B == NULL) {
	*err = E_ALLOC;
	return NULL;
    }

    nA = A->cols * A->rows;
    nB = r * c;

    k = 0;
    for (i=0; i<nB; i++) {
	B->z[i] = A->z[k++];
	if (k == nA) {
	    k = 0;
	}
    }

    return B;
}

int gretl_cmatrix_zero_triangle (gretl_matrix *m, char t)
{
    double complex z0;
    int i, j, r;

    if (!cmatrix_validate(m, 1)) {
	return E_INVARG;
    }

    z0 = 0;
    r = m->rows;

    if (t == 'U') {
	for (i=0; i<r-1; i++) {
	    for (j=i+1; j<m->cols; j++) {
		gretl_cmatrix_set(m, i, j, z0);
	    }
	}
    } else {
	for (i=1; i<r; i++) {
	    for (j=0; j<i; j++) {
		gretl_cmatrix_set(m, i, j, z0);
	    }
	}
    }

    return 0;
}

/* switch between "legacy" and new representations of a
   complex matrix */

gretl_matrix *gretl_cmatrix_switch (const gretl_matrix *m,
				    int to_new, int *err)
{
    gretl_matrix *ret = NULL;
    int r, c, i, j, k;

    if (gretl_is_null_matrix(m)) {
	return gretl_null_matrix_new();
    }

    if ((to_new && m->is_complex) ||
	(!to_new && !cmatrix_validate(m, 0))) {
	*err = E_INVARG;
	return NULL;
    }

    r = m->rows;
    c = to_new ? m->cols / 2 : m->cols * 2;

    if (to_new) {
	ret = gretl_cmatrix_new(r, c);
    } else {
	ret = gretl_matrix_alloc(r, c);
    }
    if (ret == NULL) {
	*err = E_ALLOC;
	return NULL;
    }

    k = 0;

    if (to_new) {
	/* make re and im components contiguous */
	double xr, xi;

	for (j=0; j<ret->cols; j++) {
	    for (i=0; i<m->rows; i++) {
		xr = gretl_matrix_get(m, i, k);
		xi = gretl_matrix_get(m, i, k+1);
		gretl_cmatrix_set(ret, i, j, xr + xi * I);
	    }
	    k += 2;
	}
    } else {
	/* put re and im components in adjacent columns */
	double complex mij;

	for (j=0; j<m->cols; j++) {
	    for (i=0; i<ret->rows; i++) {
		mij = gretl_cmatrix_get(m, i, j);
		gretl_matrix_set(ret, i, k, creal(mij));
		gretl_matrix_set(ret, i, k+1, cimag(mij));
	    }
	    k += 2;
	}
    }

    return ret;
}

gretl_matrix *gretl_cmatrix_vector_stat (const gretl_matrix *m,
					 GretlVecStat vs, int rowwise,
					 int *err)
{
    double complex z;
    gretl_matrix *ret;
    int r, c, i, j;

    if (!cmatrix_validate(m, 0)) {
	*err = E_INVARG;
	return NULL;
    }

    r = rowwise ? m->rows : 1;
    c = rowwise ? 1 : m->cols;

    ret = gretl_cmatrix_new(r, c);
    if (ret == NULL) {
	*err = E_ALLOC;
	return NULL;
    }

    if (rowwise) {
	/* by rows */
	int jmin = vs == V_PROD ? 1 : 0;

	for (i=0; i<m->rows; i++) {
	    z = vs == V_PROD ? m->z[0] : 0;
	    for (j=jmin; j<m->cols; j++) {
		if (vs == V_PROD) {
		    z *= gretl_cmatrix_get(m, i, j);
		} else {
		    z += gretl_cmatrix_get(m, i, j);
		}
	    }
	    if (vs == V_MEAN) {
		z /= m->cols;
	    }
	    gretl_cmatrix_set(ret, i, 0, z);
	}
    } else {
	/* by columns */
	int imin = vs == V_PROD ? 1 : 0;

	for (j=0; j<m->cols; j++) {
	    z = vs == V_PROD ? m->z[0] : 0;
	    for (i=imin; i<m->rows; i++) {
		if (vs == V_PROD) {
		    z *= gretl_cmatrix_get(m, i, j);
		} else {
		    z += gretl_cmatrix_get(m, i, j);
		}
	    }
	    if (vs == V_MEAN) {
		z /= m->rows;
	    }
	    gretl_cmatrix_set(ret, 0, j, z);
	}
    }

    return ret;
}

int gretl_cmatrix_fill (gretl_matrix *m, double complex z)
{
    double complex *mz = (double complex *) m->val;
    int i, n = m->cols * m->rows;

    for (i=0; i<n; i++) {
	mz[i] = z;
    }

    return 0;
}

gretl_matrix *scalar_to_complex (double x, int *err)
{
    gretl_matrix *m = gretl_cmatrix_new(1, 1);

    if (m != NULL) {
	m->z[0] = x;
    } else {
	*err = E_ALLOC;
    }

    return m;
}

gretl_matrix *two_scalars_to_complex (double xr, double xi, int *err)
{
    gretl_matrix *m = gretl_cmatrix_new(1, 1);

    if (m != NULL) {
	m->z[0] = xr + xi * I;
    } else {
	*err = E_ALLOC;
    }

    return m;
}

static void vec_x_op_vec_y (double complex *z,
			    const double complex *x,
			    const double complex *y,
			    int n, int op)
{
    int i;

    switch (op) {
    case '*':
	for (i=0; i<n; i++) {
	    z[i] = x[i] * y[i];
	}
	break;
    case '/':
	for (i=0; i<n; i++) {
	    z[i] = x[i] / y[i];
	}
	break;
    case '+':
	for (i=0; i<n; i++) {
	    z[i] = x[i] + y[i];
	}
	break;
    case '-':
	for (i=0; i<n; i++) {
	    z[i] = x[i] - y[i];
	}
	break;
    case '^':
	for (i=0; i<n; i++) {
	    z[i] = cpow(x[i], y[i]);
	}
	break;
    case '=':
	for (i=0; i<n; i++) {
	    z[i] = x[i] == y[i];
	}
	break;
    case '!':
	for (i=0; i<n; i++) {
	    z[i] = x[i] != y[i];
	}
	break;
    default:
	break;
    }
}

static void vec_x_op_y (double complex *z,
			const double complex *x,
			double complex y,
			int n, int op)
{
    int i;

    switch (op) {
    case '*':
	for (i=0; i<n; i++) {
	    z[i] = x[i] * y;
	}
	break;
    case '/':
	for (i=0; i<n; i++) {
	    z[i] = x[i] / y;
	}
	break;
    case '+':
	for (i=0; i<n; i++) {
	    z[i] = x[i] + y;
	}
	break;
    case '-':
	for (i=0; i<n; i++) {
	    z[i] = x[i] - y;
	}
	break;
    case '^':
	for (i=0; i<n; i++) {
	    z[i] = cpow(x[i], y);
	}
	break;
    case '=':
	for (i=0; i<n; i++) {
	    z[i] = x[i] == y;
	}
	break;
    case '!':
	for (i=0; i<n; i++) {
	    z[i] = x[i] != y;
	}
	break;
    default:
	break;
    }
}

static void x_op_vec_y (double complex *z,
			double complex x,
			const double complex *y,
			int n, int op)
{
    int i;

    switch (op) {
    case '*':
	for (i=0; i<n; i++) {
	    z[i] = x * y[i];
	}
	break;
    case '/':
	for (i=0; i<n; i++) {
	    z[i] = x / y[i];
	}
	break;
    case '+':
	for (i=0; i<n; i++) {
	    z[i] = x + y[i];
	}
	break;
    case '-':
	for (i=0; i<n; i++) {
	    z[i] = x - y[i];
	}
	break;
    case '^':
	for (i=0; i<n; i++) {
	    z[i] = cpow(x, y[i]);
	}
	break;
    case '=':
	for (i=0; i<n; i++) {
	    z[i] = x == y[i];
	}
	break;
    case '!':
	for (i=0; i<n; i++) {
	    z[i] = x != y[i];
	}
	break;
    default:
	break;
    }
}

static double complex x_op_y (double complex x,
			      double complex y,
			      int op)
{
    switch (op) {
    case '*':
	return x * y;
    case '/':
	return x / y;
    case '+':
	return x + y;
    case '-':
	return x - y;
    case '^':
	return cpow(x, y);
    case '=':
	return x == y;
    case '!':
	return x != y;
    default:
	return 0;
    }
}

static gretl_matrix *cmatrix_dot_op (const gretl_matrix *A,
				     const gretl_matrix *B,
				     int op, int *err)
{
    gretl_matrix *C = NULL;
    double complex x, y;
    int nr, nc;
    int conftype;
    int i, j, off;

    if (gretl_is_null_matrix(A) || gretl_is_null_matrix(B)) {
	*err = E_DATA;
	return NULL;
    }

    conftype = dot_operator_conf(A, B, &nr, &nc);

    if (conftype == CONF_NONE) {
	fputs("gretl_cmatrix_dot_op: matrices not conformable\n", stderr);
	fprintf(stderr, " op = '%c', A is %d x %d, B is %d x %d\n",
		(char) op, A->rows/2, A->cols, B->rows/2, B->cols);
	*err = E_NONCONF;
	return NULL;
    }

    C = gretl_cmatrix_new(nr, nc);
    if (C == NULL) {
	*err = E_ALLOC;
	return NULL;
    }

#if 0 /* maybe expose this in gretl_matrix.h? */
    math_err_init();
#endif

    switch (conftype) {
    case CONF_ELEMENTS:
	vec_x_op_vec_y(C->z, A->z, B->z, nr*nc, op);
	break;
    case CONF_A_COLVEC:
	for (i=0; i<nr; i++) {
	    x = A->z[i];
	    for (j=0; j<nc; j++) {
		y = gretl_cmatrix_get(B, i, j);
		y = x_op_y(x, y, op);
		gretl_cmatrix_set(C, i, j, y);
	    }
	}
	break;
    case CONF_B_COLVEC:
	for (i=0; i<nr; i++) {
	    y = B->z[i];
	    for (j=0; j<nc; j++) {
		x = gretl_cmatrix_get(A, i, j);
		x = x_op_y(x, y, op);
		gretl_cmatrix_set(C, i, j, x);
	    }
	}
	break;
    case CONF_A_ROWVEC:
	off = 0;
	for (j=0; j<nc; j++) {
	    x = A->z[j];
	    x_op_vec_y(C->z + off, x, B->z + off, nr, op);
	    off += nr;
	}
	break;
    case CONF_B_ROWVEC:
	off = 0;
	for (j=0; j<nc; j++) {
	    y = B->z[j];
	    vec_x_op_y(C->z + off, A->z + off, y, nr, op);
	    off += nr;
	}
	break;
    case CONF_A_SCALAR:
	x_op_vec_y(C->z, A->z[0], B->z, nr*nc, op);
	break;
    case CONF_B_SCALAR:
	vec_x_op_y(C->z, A->z, B->z[0], nr*nc, op);
	break;
    case CONF_AC_BR:
	for (i=0; i<nr; i++) {
	    x = A->z[i];
	    for (j=0; j<nc; j++) {
		y = B->z[j];
		y = x_op_y(x, y, op);
		gretl_cmatrix_set(C, i, j, y);
	    }
	}
	break;
    case CONF_AR_BC:
	for (j=0; j<nc; j++) {
	    x = A->z[j];
	    for (i=0; i<nr; i++) {
		y = B->z[i];
		y = x_op_y(x, y, op);
		gretl_cmatrix_set(C, i, j, y);
	    }
	}
	break;
    default: /* hush a warning */
	break;
    }

    if (errno) {
#if 0 /* not yet */
	*err = math_err_check("gretl_matrix_dot_op", errno);
#endif
	if (*err) {
	    gretl_matrix_free(C);
	    C = NULL;
	}
    }

    return C;
}

gretl_matrix *gretl_cmatrix_dot_op (const gretl_matrix *A,
				    const gretl_matrix *B,
				    int op, int *err)
{
    gretl_matrix *T = NULL;
    gretl_matrix *C = NULL;

    if (A->is_complex && B->is_complex) {
	C = cmatrix_dot_op(A, B, op, err);
    } else if (A->is_complex) {
	/* case of real B */
	T = complex_from_real(B, err);
	if (T != NULL) {
	    C = cmatrix_dot_op(A, T, op, err);
	}
    } else if (B->is_complex) {
	/* case of real A */
	T = complex_from_real(A, err);
	if (T != NULL) {
	    C = cmatrix_dot_op(T, B, op, err);
	}
    } else {
	*err = E_TYPES;
    }

    gretl_matrix_free(T);

    return C;
}
