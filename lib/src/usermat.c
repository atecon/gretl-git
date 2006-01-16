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

#include "libgretl.h"
#include "gretl_matrix.h"
#include "gretl_func.h"
#include "libset.h"
#include "usermat.h"

#include <errno.h>

#define MDEBUG 0

#define MNAMELEN 32
#define LEVEL_AUTO -1

enum {
    TRANSPOSE_NOT_OK,
    TRANSPOSE_OK
};

typedef struct user_matrix_ user_matrix;

struct user_matrix_ {
    gretl_matrix *M;
    int level;
    char name[MNAMELEN];
};

static user_matrix **matrices;
static int n_matrices;

static int 
make_slices (const char *s, int m, int n, int **rslice, int **cslice);
static int delete_matrix_by_name (const char *name);
static int name_is_series (const char *name, const DATAINFO *pdinfo);

static const double **gZ;
static const DATAINFO *gdinfo;

static void 
usermat_publish_dataset (const double **Z, const DATAINFO *pdinfo)
{
    gZ = Z;
    gdinfo = pdinfo;
}

static void usermat_unpublish_dataset (void)
{
    gZ = NULL;
    gdinfo = NULL;
}

static user_matrix *user_matrix_new (gretl_matrix *M, const char *name)
{
    user_matrix *u;

    u = malloc(sizeof *u);
    if (u == NULL) {
	return NULL;
    }

    u->M = M;

    u->level = gretl_function_stack_depth();

    *u->name = '\0';
    strncat(u->name, name, MNAMELEN - 1);

    return u;
}

static int add_user_matrix (gretl_matrix *M, const char *name)
{
    user_matrix **tmp;

    if (M == NULL) {
	return 0;
    }

    if (check_varname(name)) {
	return E_DATA;
    }

    tmp = realloc(matrices, (n_matrices + 1) * sizeof *tmp);
    if (tmp == NULL) {
	return E_ALLOC;
    }

    matrices = tmp;
    matrices[n_matrices] = user_matrix_new(M, name);

    if (matrices[n_matrices] == NULL) {
	return E_ALLOC;
    }

    n_matrices++;

#if MDEBUG
    fprintf(stderr, "add_user_matrix: allocated '%s' at %p (M at %p, %dx%d)\n",
	    name, (void *) matrices[n_matrices-1], (void *) M,
	    gretl_matrix_rows(M), gretl_matrix_cols(M));
#endif

    return 0;
}

static int matrix_insert_submatrix (gretl_matrix *M, gretl_matrix *S,
				    const char *mask)
{
    int mr = gretl_matrix_rows(M);
    int mc = gretl_matrix_cols(M);
    int sr = gretl_matrix_rows(S);
    int sc = gretl_matrix_cols(S);
    int *rslice = NULL;
    int *cslice = NULL;
    int err = 0;

    if (sr > mr || sc > mc) {
	err = E_NONCONF;
    }

    if (!err) {
	err = make_slices(mask, mr, mc, &rslice, &cslice);
#if MDEBUG
	printlist(rslice, "rslice");
	printlist(cslice, "cslice");
	fprintf(stderr, "M = %d x %d, S = %d x %d\n", mr, mc, sr, sc);
#endif
    }

    if (!err) {
	if (rslice != NULL && rslice[0] != sr) {
	    err = E_NONCONF;
	} else if (cslice != NULL && cslice[0] != sc) {
	    err = E_NONCONF;
	}
    }

    if (!err) {
	int i, j, k, l;
	int mi, mj;
	double x;

	k = 0;
	for (i=0; i<sr; i++) {
	    mi = (rslice == NULL)? k++ : rslice[i+1] - 1;
	    l = 0;
	    for (j=0; j<sc; j++) {
		mj = (cslice == NULL)? l++ : cslice[j+1] - 1;
		x = gretl_matrix_get(S, i, j);
		gretl_matrix_set(M, mi, mj, x);
	    }
	}
    }

    free(rslice);
    free(cslice);

    return err;
}

static int replace_user_matrix (user_matrix *u, gretl_matrix *M,
				gretl_matrix **R, const char *mask)
{
    int err = 0;

    if (R != NULL) {
#if MDEBUG
	fprintf(stderr, "replace_user_matrix: u->M = %p (matrix '%s')\n",
		u->M, u->name);
#endif
	*R = u->M;
    }  

    if (M == NULL) {
	return 0;
    }

    if (mask != NULL && *mask != '\0') {
	/* the new matrix M is actally a submatrix, to be written
	   into the original matrix, u->M */
	err = matrix_insert_submatrix(u->M, M, mask);
	gretl_matrix_free(M);
    } else {
	gretl_matrix_free(u->M);
	u->M = M;
    }

    return err;
}

/**
 * is_user_matrix:
 * @m: gretl_matrix to test.
 *
 * Returns: 1 if the matrix @m is saved on the stack of matrices,
 * else 0.
 */

int is_user_matrix (gretl_matrix *m)
{
    int i;

    for (i=0; i<n_matrices; i++) {
	if (m == matrices[i]->M) {
	    return 1;
	}
    }

    return 0;
}

/* If mod = TRANSPOSE_OK, it's OK to pick up the transpose
   of an existing matrix (e.g. A').  If slevel is LEVEL_AUTO
   search at the current function execution depth, otherwise
   search at the function execution depth given by slevel. 
*/

static gretl_matrix *
real_get_matrix_by_name (const char *name, int mod, int slevel,
			 const DATAINFO *pdinfo)
{
    char test[MNAMELEN];
    int level, transp = 0;
    int i;

    *test = '\0';
    strncat(test, name, MNAMELEN - 1);

    if (mod == TRANSPOSE_OK) {
	if (test[strlen(test) - 1] == '\'') {
	    test[strlen(test) - 1] = '\0';
	    transp = 1;
	}
    } 

    if (slevel == LEVEL_AUTO) {
	level = gretl_function_stack_depth();
    } else {
	level = slevel;
    }

    /* impose priority of data series over matrices */
    if (name_is_series(name, pdinfo)) {
	delete_matrix_by_name(name);
	return NULL;
    }

    for (i=0; i<n_matrices; i++) {
	if (!strcmp(test, matrices[i]->name) &&
	    matrices[i]->level == level) {
	    if (transp) {
		return gretl_matrix_copy_transpose(matrices[i]->M);
	    } else {
		return matrices[i]->M;
	    }
	}
    }

    return NULL;
}

static user_matrix *get_user_matrix_by_name (const char *name)
{
    int level = gretl_function_stack_depth();
    int i;

    for (i=0; i<n_matrices; i++) {
	if (!strcmp(name, matrices[i]->name) &&
	    matrices[i]->level == level) {
	    return matrices[i];
	}
    }

    return NULL;
}

static user_matrix *get_user_matrix_by_data (gretl_matrix *M)
{
    int level = gretl_function_stack_depth();
    int i;

    for (i=0; i<n_matrices; i++) {
	if (matrices[i]->M == M && matrices[i]->level == level) {
	    return matrices[i];
	}
    }

    return NULL;
}

/**
 * get_matrix_by_name:
 * @name: name of the matrix.
 * @pdinfo: dataset information.
 *
 * Looks up a user-defined matrix by name.
 *
 * Returns: pointer to matrix, or %NULL if not found.
 */

gretl_matrix *get_matrix_by_name (const char *name, const DATAINFO *pdinfo)
{
    return real_get_matrix_by_name(name, TRANSPOSE_OK, LEVEL_AUTO,
				   pdinfo);
}

/**
 * get_matrix_by_name_at_level:
 * @name: name of the matrix.
 * @level: level of function execution at which to search.
 * @pdinfo: dataset information.
 *
 * Looks up a user-defined matrix by name, at the given
 * level fo function execution.
 *
 * Returns: pointer to matrix, or %NULL if not found.
 */

gretl_matrix *get_matrix_by_name_at_level (const char *name, int level,
					   const DATAINFO *pdinfo)
{
    return real_get_matrix_by_name(name, TRANSPOSE_NOT_OK, level,
				   pdinfo);
}

/**
 * copy_named_matrix_as:
 * @orig: the name of the original matrix.
 * @new: the name to be given to the copy.
 *
 * If a saved matrix is found by the name @orig, a copy of
 * this matrix is added to the stack of saved matrices under the
 * name @new.  This is intended for use when a matrix is given
 * as the argument to a user-defined function: it is copied
 * under the name assigned by the function's parameter list.
 *
 * Returns: 0 on success, non-zero on error.
 */

int copy_named_matrix_as (const char *orig, const char *new)
{
    user_matrix *u;
    int err = 0;

    u = get_user_matrix_by_name(orig);
    if (u == NULL) {
	err = 1;
    } else {
	gretl_matrix *M = gretl_matrix_copy(u->M);

	if (M == NULL) {
	    err = E_ALLOC;
	} else {
	    err = add_user_matrix(M, new);
	}
	if (!err) {
	    /* for use in functions: increment level of last-added matrix */
	    u = matrices[n_matrices - 1];
	    u->level += 1;
	}
    }

    return err;
}

int user_matrix_reconfigure (gretl_matrix *M, char *newname, int level)
{
    user_matrix *u = get_user_matrix_by_data(M);
    int err = 0;

    if (u == NULL) {
	err = 1;
    } else {
	*u->name = '\0';
	strncat(u->name, newname, MNAMELEN - 1);
	u->level = level;
    }

    return err;
}

/**
 * named_matrix_get_variable:
 * @mspec: name of matrix, possibly followed by selection string
 * in square backets (e.g. "m[,1]" to get column 1 of m).
 * @Z: data array.
 * @pdinfo: dataset information.
 * @px: location to receive allocated series (array).
 * @plen: location to receive length of array.
 *
 * If there exists a matrix of the given name at the current
 * level of function execution, try to assign to @x the
 * selection specified in @mspec.  This works only if (a)
 * the matrix is in fact a vector of length equal to either the 
 * full length of the dataset or the length of the current sample 
 * range, or (b) the selection string "extracts" such a vector,
 * or (c) the specified matrix is 1 x 1, in effect yielding a
 * scalar result (array of length 1).
 *
 * If the sample is currently restricted while the selection has 
 * a number of elements equal to the full length of the dataset, 
 * the "out of sample" observations are set to #NADBL.
 *
 * Returns: 0 on success or non-zero if no matrix is found, or
 * if the matrix selection does not have the required number of
 * elements.
 */

int named_matrix_get_variable (const char *mspec, 
			       const double **Z, const DATAINFO *pdinfo,
			       double **px, int *plen)
{
    double *x = NULL;
    gretl_matrix *M = NULL;
    gretl_matrix *S = NULL;
    int T = pdinfo->t2 - pdinfo->t1 + 1;
    int i, len = 0;
    int err = 0;

    *plen = 0;

    if (strchr(mspec, '[')) {
	S = user_matrix_get_slice(mspec, Z, pdinfo, &err);
	if (!err) {
	    len = gretl_vector_get_length(S);
	}
    } else {
	M = real_get_matrix_by_name(mspec, TRANSPOSE_NOT_OK, LEVEL_AUTO,
				    pdinfo);
	if (M == NULL) {
	    err = E_UNKVAR;
	} else {
	    len = gretl_vector_get_length(M);
	}
    }

    if (!err) {
	if (len != 1 && len != pdinfo->n && len != T) {
	    err = E_NONCONF;
	}
    }

    if (!err) {
	gretl_matrix *P = (S != NULL)? S : M;

	if (len == 1) {
	    x = malloc(sizeof *x);
	    if (x == NULL) {
		err = E_ALLOC;
	    } else {
		*x = gretl_vector_get(P, 0);
		*px = x;
	    }
	} else {
	    x = malloc(pdinfo->n * sizeof *x);
	    if (x == NULL) {
		err = E_ALLOC;
	    } else {
	    
		if (len < pdinfo->n) {
		    for (i=0; i<pdinfo->n; i++) {
			x[i] = NADBL;
		    }
		}
		for (i=0; i<len; i++) {
		    x[i + pdinfo->t1] = gretl_vector_get(P, i);
		}
		*px = x;
	    }
	}
    }

    *plen = len;

    if (S != NULL) {
	gretl_matrix_free(S);
    }

    return err;
}

/**
 * get_matrix_from_variable:
 * @Z: data array.
 * @pdinfo: dataset information.
 * @v: ID number of variable.
 *
 * Converts the specified variable into a gretl matrix.  If
 * the variable is a scalar, the returned matrix is 1 x 1;
 * if the variable is a data series, the returned matrix is
 * a column vector of length equal to the current sample
 * range.
 *
 * Returns: allocated matrix, or %NULL on failure.
 */

gretl_matrix *
get_matrix_from_variable (const double **Z, const DATAINFO *pdinfo, int v)
{
    gretl_matrix *m = NULL;

    if (v < 0 || v >= pdinfo->v) {
	return NULL;
    }

    if (pdinfo->vector[v]) {
	int i, n = pdinfo->t2 - pdinfo->t1 + 1;
	double x;

	m = gretl_column_vector_alloc(n);
	if (m != NULL) {
	    for (i=0; i<n; i++) {
		x = Z[v][i + pdinfo->t1];
		if (na(x)) {
		    gretl_matrix_free(m);
		    m = NULL;
		    break;
		} else {
		    gretl_vector_set(m, i, x);
		}
	    }
	}
    } else {
	m = gretl_matrix_from_scalar(Z[v][0]);
    }

    return m;
}

static gretl_matrix *
original_matrix_by_name (const char *name, const DATAINFO *pdinfo)
{
    return real_get_matrix_by_name(name, TRANSPOSE_NOT_OK, LEVEL_AUTO,
				   pdinfo);
}

static int *slice_from_index_vector (gretl_matrix *v, int *err)
{
    int sn = gretl_vector_get_length(v);
    int *slice = NULL;
    int i;

    if (sn > 0) {
	slice = gretl_list_new(sn);
	if (slice == NULL) {
	    *err = E_ALLOC;
	} else {
	    for (i=0; i<sn; i++) {
		slice[i+1] = gretl_vector_get(v, i);
	    }
	}
    } else {
	*err = E_DATA;
    }

    return slice;
}

static int *slice_from_scalar (const char *s, int *err)
{
    int *slice = NULL;
    int v;

    if (gZ == NULL || gdinfo == NULL) {
	*err = E_DATA;
	return NULL;
    }

    v = varindex(gdinfo, s);

    if (v < gdinfo->v && !gdinfo->vector[v] && !na(gZ[v][0])) {
	slice = gretl_list_new(1);
	slice[1] = gZ[v][0];
    } else {
	*err = E_DATA;
    }

    return slice;
}

/* A "slice" is a gretl list: the first element holds the count of the
   following elements, and the following elements indicate from which
   row (or column) of the source matrix to draw the successive rows
   (columns) of the submatrix.  A NULL value is also OK, indicating
   that all rows (columns) should be used.
*/

static int *parse_slice_spec (const char *s, int n, int *err)
{
    int *slice = NULL;
    int i, sn = 0;

    if (*s == '\0') {
	/* null spec: use all rows or columns */
	return NULL;
    }

    if (isalpha(*s)) {
	/* is it an index matrix? */
	gretl_matrix *v = get_matrix_by_name(s, gdinfo);

#if MDEBUG
	fprintf(stderr, "index matrix? s='%s', v=%p\n", s, (void *) v);
#endif
	if (v != NULL) {
	    slice = slice_from_index_vector(v, err);
	} else {
	    slice = slice_from_scalar(s, err);
	}
	if (*err) {
	    sprintf(gretl_errmsg, "'%s' is not an index matrix or scalar", s);
	    *err = E_DATA;
	}
    } else {
	/* numerical specification, either p:q or plain p */
	int idx[2];

	if (strchr(s, ':')) {
	    if (sscanf(s, "%d:%d", &idx[0], &idx[1]) != 2) {
		*err = 1;
	    } else if (idx[0] < 1 || idx[1] < 1 || idx[1] < idx[0]) {
		*err = 1;
	    } else {
		sn = idx[1] - idx[0] + 1;
	    }
	} else if (sscanf(s, "%d", &idx[0]) != 1) {
	    *err = 1;
	} else if (idx[0] < 1) {
	    *err = 1;
	} else {
	    idx[1] = idx[0];
	    sn = 1;
	}

	if (!*err) {
	    slice = gretl_list_new(sn);
	    if (slice == NULL) {
		*err = E_ALLOC;
	    } else {
		for (i=0; i<sn; i++) {
		    slice[i+1] = idx[0] + i;
		}
	    }
	}
    }

    if (slice != NULL) {
#if MDEBUG
	printlist(slice, "slice");
#endif
	for (i=1; i<=slice[0] && !*err; i++) {
	    if (slice[i] < 0 || slice[i] > n) {
		fprintf(stderr, "index value %d is out of bounds\n", 
			slice[i]);
		*err = 1;
	    }
	}
    }

    if (*err && slice != NULL) {
	free(slice);
	slice = NULL;
    }

    return slice;    
}

enum {
    RSLICE,
    CSLICE,
    VSLICE
};

static int get_slice_string (const char *s, char *spec, int i)
{
    int err = 0;

    *spec = '\0';

    if (i == RSLICE) {
	s = strchr(s, '[');
	if (s == NULL || strchr(s, ',') == NULL) {
	    err = 1;
	} else {
	    sscanf(s + 1, "%31[^,]]", spec);
	}
    } else if (i == CSLICE) {
	s = strchr(s, ',');
	if (s == NULL || strchr(s, ']') == NULL) {
	    err = 1;
	} else {	
	    sscanf(s + 1, "%31[^]]", spec);
	}	    
    } else if (i == VSLICE) {
	s = strchr(s, '[');
	if (s == NULL || strchr(s, ']') == NULL) {
	    err = 1;
	} else {
	    sscanf(s + 1, "%31[^]]", spec);
	}
    }

#if MDEBUG
    fprintf(stderr, "get_get_slice_string: spec = '%s', err = %d\n",
	    spec, err);
#endif

    return err;
}

static int 
make_slices (const char *s, int m, int n, int **rslice, int **cslice)
{
    char spec[32];
    int err = 0;

    *rslice = *cslice = NULL;

    if ((m == 1 || n == 1) && strchr(s, ',') == NULL) {
	/* vector: a single slice is acceptable */
	err = get_slice_string(s, spec, VSLICE);
	if (!err) {
	    if (m == 1) {
		*cslice = parse_slice_spec(spec, n, &err);
	    } else {
		*rslice = parse_slice_spec(spec, m, &err);
	    }
	}
    } else {
	/* not a vector: must have both slice strings "in principle",
	   even if one or the other is empty */
	err = get_slice_string(s, spec, RSLICE);
	if (!err) {
	    *rslice = parse_slice_spec(spec, m, &err);
	}

	if (!err) {
	    err = get_slice_string(s, spec, CSLICE);
	    if (!err) {
		*cslice = parse_slice_spec(spec, n, &err);
	    } 
	}
    }   

    if (err) {
	free(*rslice);
	*rslice = NULL;
	free(*cslice);
	*cslice = NULL;
    }

    return err;
}

/* This supports the extraction of scalars, vectors or sub-matrices.
   E.g. B[1,2] extracts a scalar, B[1,] extracts a row vector, and
   B[,2] extracts a column vector.  B[1:2,2:3] extracts a sub-matrix
   composed of the intersection of rows 1 and 2 with columns 2 and 3.
*/

gretl_matrix *
matrix_get_submatrix (gretl_matrix *M, const char *s, 
		      const double **Z, const DATAINFO *pdinfo,
		      int *err)
{
    gretl_matrix *S;
    int *rslice = NULL;
    int *cslice = NULL;
    int m = gretl_matrix_rows(M);
    int n = gretl_matrix_cols(M);
    int nr, nc;

    usermat_publish_dataset(Z, pdinfo);

    *err = make_slices(s, m, n, &rslice, &cslice);
    if (*err) {
	return NULL;
    }

    nr = (rslice == NULL)? m : rslice[0];
    nc = (cslice == NULL)? n : cslice[0];

    S = gretl_matrix_alloc(nr, nc);
    if (S == NULL) {
	*err = E_ALLOC;	
    }

    if (S != NULL) {
	int i, j, k, l;
	int mi, mj;
	double x;

	k = 0;
	for (i=0; i<nr; i++) {
	    mi = (rslice == NULL)? k++ : rslice[i+1] - 1;
	    l = 0;
	    for (j=0; j<nc; j++) {
		mj = (cslice == NULL)? l++ : cslice[j+1] - 1;
		x = gretl_matrix_get(M, mi, mj);
		gretl_matrix_set(S, i, j, x);
	    }
	}
    }

    usermat_unpublish_dataset();

    free(rslice);
    free(cslice);
	
    return S;
}

/**
 * user_matrix_get_slice:
 * @s: string specifying a sub-matrix.
 * @Z: data array.
 * @pdinfo: dataset information.
 * @err: location to receive error code.
 *
 * If @s specifies a valid "slice" of an existing named
 * matrix at the current level of function execution, 
 * constructs a newly allocated sub-matrix.
 *
 * Returns: allocated sub-matrix on success, %NULL on failure.
 */

gretl_matrix *user_matrix_get_slice (const char *s, 
				     const double **Z, 
				     const DATAINFO *pdinfo,
				     int *err)
{
    gretl_matrix *M = NULL;
    gretl_matrix *S = NULL;
    char test[MNAMELEN];
    int len = strcspn(s, "[");

    if (len < MNAMELEN) {
	*test = '\0';
	strncat(test, s, len);
	M = real_get_matrix_by_name(test, TRANSPOSE_NOT_OK, LEVEL_AUTO,
				    pdinfo);
	if (M != NULL) {
	    S = matrix_get_submatrix(M, s, Z, pdinfo, err);
	}
    }

    return S;
}

/**
 * add_or_replace_user_matrix:
 * @M: gretl matrix.
 * @name: name for the matrix.
 * @mask: submatrix specification (or empty string).
 * @R: location to receive address of matrix that was
 * replaced, if any (or %NULL).
 * @pZ: pointer to data array.
 * @pdinfo: dataset information.
 *
 * Checks whether a matrix of the given @name already exists.
 * If so, the original matrix is replaced by @M; if not, the
 * the matrix @M is added to the stack of user-defined
 * matrices.
 *
 * Returns: 0 on success, %E_ALLOC on failure.
 */

int add_or_replace_user_matrix (gretl_matrix *M, const char *name,
				const char *mask, gretl_matrix **R,
				const double **Z, const DATAINFO *pdinfo)
{
    user_matrix *u;
    int err = 0;

    u = get_user_matrix_by_name(name);
    if (u != NULL) {
	usermat_publish_dataset(Z, pdinfo);
	err = replace_user_matrix(u, M, R, mask);
	usermat_unpublish_dataset();
    } else {
	err = add_user_matrix(M, name);
    }

    return err;
}

static void destroy_user_matrix (user_matrix *u)
{
    if (u == NULL) {
	return;
    }

#if MDEBUG
    fprintf(stderr, "destroy_user_matrix: freeing matrix at %p...", 
	    (void *) u->M);
#endif
    gretl_matrix_free(u->M);
    free(u);
#if MDEBUG
    fprintf(stderr, " done\n");
#endif
}

/**
 * destroy_user_matrices_at_level:
 * @level: stack level of function execution.
 *
 * Destroys and removes from the stack of user matrices all
 * matrices that were created at the given @level.  This is 
 * part of the cleanup that is performed when a user-defined
 * function terminates.
 *
 * Returns: 0 on success, non-zero on error.
 */

int destroy_user_matrices_at_level (int level)
{
    user_matrix **tmp;
    int i, j, nm = 0;
    int err = 0;

#if MDEBUG
    fprintf(stderr, "destroy_user_matrices_at_level: level = %d, "
	    "n_matrices = %d\n", level, n_matrices);
#endif

    for (i=0; i<n_matrices; i++) {
	if (matrices[i] == NULL) {
	    break;
	}
	if (matrices[i]->level == level) {
#if MDEBUG
	    fprintf(stderr, "destroying matrix[%d] (M at %p)\n",
		    i, (void *) matrices[i]->M);
#endif
	    destroy_user_matrix(matrices[i]);
	    for (j=i; j<n_matrices - 1; j++) {
		matrices[j] = matrices[j+1];
	    }
	    matrices[n_matrices - 1] = NULL;
	} else {
	    nm++;
	}
    }

    if (nm < n_matrices) {
	n_matrices = nm;
	if (nm == 0) {
	    free(matrices);
	    matrices = NULL;
	} else {
	    tmp = realloc(matrices, nm * sizeof *tmp);
	    if (tmp == NULL) {
		err = E_ALLOC;
	    } else {
		matrices = tmp;
	    }
	}
    }

    return err;
}

/**
 * destroy_user_matrices:
 *
 * Frees all resources associated with the stack of user-
 * defined matrices.
 */

void destroy_user_matrices (void)
{
    int i;

#if MDEBUG
    fprintf(stderr, "destroy_user_matrices called, n_matrices = %d\n",
	    n_matrices);
#endif

    if (matrices == NULL) {
	return;
    }

    for (i=0; i<n_matrices; i++) {
#if MDEBUG
	fprintf(stderr, "destroying user_matrix %d (%s) at %p\n", i,
		matrices[i]->name, (void *) matrices[i]);
#endif
	destroy_user_matrix(matrices[i]);
    }

    free(matrices);
    matrices = NULL;
    n_matrices = 0;
}

static int delete_matrix_by_name (const char *name)
{
    user_matrix *u = get_user_matrix_by_name(name);
    int err = 0;

    if (u == NULL) {
	err = E_DATA;
    } else {
	int i, j, nm = n_matrices - 1;

	for (i=0; i<n_matrices; i++) {
	    if (matrices[i] == u) {
		destroy_user_matrix(matrices[i]);
		for (j=i; j<n_matrices - 1; j++) {
		    matrices[j] = matrices[j+1];
		}
		matrices[nm] = NULL;
		break;
	    }
	} 

	if (nm == 0) {
	    free(matrices);
	    matrices = NULL;
	} else {
	    user_matrix **tmp = realloc(matrices, nm * sizeof *tmp);

	    n_matrices = nm;
	    if (tmp == NULL) {
		err = E_ALLOC;
	    } else {
		matrices = tmp;
	    }
	}
    }

    return err;
}

static int first_field_is_series (const char *s, const DATAINFO *pdinfo)
{
    char word[16];
    int v, ret = 0;

    s += strspn(s, " {");

    *word = '\0';
    sscanf(s, "%15[^,; ]", word);
    v = varindex(pdinfo, word);
    if (v < pdinfo->v && pdinfo->vector[v]) {
	ret = 1;
    }

    return ret;
}

static int 
get_rows_cols (const char *str, const DATAINFO *pdinfo,
	       int *r, int *c, int *series)
{
    const char *p = str;
    char sepstr[2] = ";";
    char sep = ';';
    char *s;
    int nf0 = 0, nf1;
    int i, len;
    int err = 0;

    if (first_field_is_series(p, pdinfo)) {
	*series = 1;
	*sepstr = ',';
	sep = ',';
    }

    *r = 1;
    while (*p) {
	if (*p == sep) {
	    *r += 1;
	}
	p++;
    }

    for (i=0; i<*r && !err; i++) {
	len = strcspn(str, sepstr);

	s = gretl_strndup(str, len);
	if (s == NULL) {
	    err = E_ALLOC;
	    break;
	}

	/* clean up */
	charsub(s, ',', ' ');
	charsub(s, '}', ' ');
	charsub(s, '\'', ' ');

	nf1 = count_fields(s);
	if (i > 0 && nf1 != nf0) {
	    strcpy(gretl_errmsg, "Inconsistent matrix specification");
	    err = 1;
	    break;
	}

	nf0 = nf1;
	str += len + 1;
	free(s);
    }

    if (!err) {
	if (*series) {
	    *c = *r;
	    *r = nf0;
	} else {
	    *c = nf0;
	}
    }

    /* FIXME series and matrix orientation */

    return err;
}

static int 
get_varnum (const char **s, const DATAINFO *pdinfo, int *err)
{
    char vname[12];
    int v = 0;

    if (sscanf(*s, "%11s", vname)) {
	int len = strlen(vname);
	int vlen = gretl_varchar_spn(vname);

	if (vlen < len) {
	    vname[vlen] = '\0';
	}

	v = varindex(pdinfo, vname);
	if (v < pdinfo->v) {
	    *s += len;
	} else {
	    sprintf(gretl_errmsg, _("Unknown variable '%s'"), vname);
	    *err = 1;
	} 
    } else {
	*err = 1;
    }

    return v;
}

static double get_var_double (const char **s, const double **Z,
			      const DATAINFO *pdinfo, int *err)
{
    char vname[VNAMELEN];
    double x = NADBL;
    int v, len = gretl_varchar_spn(*s);

    if (len > VNAMELEN - 1) {
	*err = 1;
    } else {
	*vname = '\0';
	strncat(vname, *s, len);
	v = varindex(pdinfo, vname);
	if (v == pdinfo->v) {
	    *err = E_UNKVAR;
	} else if (pdinfo->vector[v]) {
	    *err = E_DATA;
	} else {
	    x = Z[v][0];
	    if (na(x)) {
		*err = E_MISSDATA;
	    } else {
		*s += len;
	    }
	}
    }

    return x;
}

static double get_numeric_double (const char **s, int *err)
{
    double x = NADBL;
    char *p;

    x = strtod(*s, &p);

    if (!strcmp(*s, p)) {
	sprintf(gretl_errmsg, _("'%s' -- no numeric conversion performed!"), *s);
	*err = 1;
    } else if (*p != '\0' && *p != ',' && *p != ';' && *p != ' ' && *p != '}') {
	if (isprint(*p)) {
	    sprintf(gretl_errmsg, _("Extraneous character '%c' in data"), *p);
	} else {
	    sprintf(gretl_errmsg, _("Extraneous character (0x%x) in data"), *p);
	}
	*err = 1;
    } else if (errno == ERANGE) {
	sprintf(gretl_errmsg, _("'%s' -- number out of range!"), *s);
	*err = 1;
    }

    *s = p;

    return x;
}

static int 
fill_matrix_from_scalars (gretl_matrix *M, const char *s, 
			  int r, int c, int transp,
			  const double **Z, const DATAINFO *pdinfo)
{
    double x;
    int i, j;
    int err = 0;

    if (transp) {
	for (j=0; j<c && !err; j++) {
	    for (i=0; i<r && !err; i++) {
		if (isalpha(*s)) {
		    x = get_var_double(&s, Z, pdinfo, &err);
		} else {
		    x = get_numeric_double(&s, &err);
		}
		if (!err) {
		    gretl_matrix_set(M, i, j, x);
		    s += strspn(s, " ,;");
		}
	    }
	}
    } else {
	for (i=0; i<r && !err; i++) {
	    for (j=0; j<c && !err; j++) {
		if (isalpha(*s)) {
		    x = get_var_double(&s, Z, pdinfo, &err);
		} else {
		    x = get_numeric_double(&s, &err);
		}
		if (!err) {
		    gretl_matrix_set(M, i, j, x);	
		    s += strspn(s, " ,;");
		}
	    }
	}
    }

    return err;
}

/* Fill a matrix with values from one or more series: since
   gretl's matrix methods cannot handle missing values, it is
   an error if any missing values are encountered in the
   given series.  Each series occupies a row by default
   (unless transp is non-zero).
*/

static int fill_matrix_from_series (gretl_matrix *M, const char *s,
				    int r, int c, int transp,
				    const double **Z, const DATAINFO *pdinfo)
{
    int T = pdinfo->t2 - pdinfo->t1 + 1;
    int nvr = r, nvc = c;
    int i, j, k, t, v;
    double x;
    int err = 0;

    s += strspn(s, " ");

    if (!transp) {
	nvr /= T;
	for (j=0; j<c && !err; j++) {
	    for (i=0; i<nvr && !err; i++) {
		v = get_varnum(&s, pdinfo, &err);
		if (!err) {
		    for (t=0; t<T; t++) {
			if (pdinfo->vector[v]) {
			    x = Z[v][t + pdinfo->t1];
			} else {
			    x = Z[v][0];
			}
			if (na(x)) {
			    err = E_MISSDATA;
			} else {
			    k = i * T + t;
			    gretl_matrix_set(M, k, j, x);
			}
		    }
		    s += strspn(s, " ,;");
		}
	    }
	}
    } else {
	nvc /= T;
	for (i=0; i<r && !err; i++) {
	    for (j=0; j<nvc && !err; j++) {
		v = get_varnum(&s, pdinfo, &err);
		if (!err) {
		    for (t=0; t<T; t++) {
			if (pdinfo->vector[v]) {
			    x = Z[v][t + pdinfo->t1]; 
			} else {
			    x = Z[v][0];
			}
			if (na(x)) {
			    err = E_MISSDATA;
			} else {			
			    k = j * T + t;
			    gretl_matrix_set(M, i, k, x);
			}
		    }
		    s += strspn(s, " ,;");
		}
	    }
	}
    } 

    return err;
}

static int matrix_genr (const char *name, const char *mask, 
			const char *s, double ***pZ, 
			DATAINFO *pdinfo)
{
    char genline[MAXLINE];
    int err;

    if (strlen(name) + strlen(mask) + strlen(s) + 7 > MAXLINE - 1) {
	err = 1;
    } else {
	sprintf(genline, "genr %s%s %s", name, mask, s);
	err = generate(genline, pZ, pdinfo, OPT_M);
    }

    return err;
}

gretl_matrix *fill_matrix_from_list (const char *s, const double **Z,
				     const DATAINFO *pdinfo, int transp,
				     int *err)
{
    gretl_matrix *M = NULL;
    char word[32];
    char *mask = NULL;
    const int *list;
    int len;

    while (isspace(*s)) s++;

    len = gretl_varchar_spn(s);
    if (len == 0 || len > 31) {
	return NULL;
    }

    *word = '\0';
    strncat(word, s, len);
    list = get_list_by_name(word);
    if (list == NULL) {
	return NULL;
    }

    M = gretl_matrix_data_subset(list, Z, pdinfo->t1, pdinfo->t2, &mask);

    if (M == NULL) {
	*err = 1;
    }

    if (mask != NULL) {
	*err = E_MISSDATA;
	free(mask);
	gretl_matrix_free(M);
	M = NULL;
    }

    if (M != NULL && transp) {
	gretl_matrix *R = gretl_matrix_copy_transpose(M);

	if (R == NULL) {
	    *err = E_ALLOC;
	    gretl_matrix_free(M);
	    M = NULL;
	} else {
	    gretl_matrix_free(M);
	    M = R;
	}
    }

    return M;
}

static int name_is_series (const char *name, const DATAINFO *pdinfo)
{
    int v = varindex(pdinfo, name);
    int ret = 0;

    if (v < pdinfo->v && pdinfo->vector[v]) {
	ret = 1;
    }

    return ret;
}

/* Currently we can create a user matrix in any one of four ways (but
   we can't mix these in a single matrix specification).

   1. Full specification of scalar elements, either numerical values or
      by reference to scalar variables.

   2. Specification of individual data series to place in the matrix.

   3. Specification of one named list of variables to place in matrix.

   4. Use of a "genr"-type expression referring to existing matrices
      and/or the matrix-from-scratch functions such as I(n), which 
      generates an n x n identity matrix.

   If the name supplied for a matrix is already taken by an "ordinary"
   series variable, the attempt to create a matrix of the same name
   fails with an error message.
*/

static int create_matrix (const char *name, const char *mask,
			  const char *s, double ***pZ, DATAINFO *pdinfo,
			  PRN *prn)
{
    gretl_matrix *M = NULL;
    char *p;
    int nm = n_matrices;
    int series = 0;
    int transp = 0;
    int r = 0, c = 0;
    int err = 0;

    if (name_is_series(name, pdinfo)) {
	/* can't overwrite data series with matrix */
	sprintf(gretl_errmsg, _("'%s' is the name of a data series"), name);
	return E_DATA;
    }

    p = strchr(s, '{');
    if (p == NULL) {
	err = matrix_genr(name, mask, s, pZ, pdinfo);
	goto finalize;
    }
    s = p + 1;

    if (!err) {
	p = strchr(s, '}');
	if (p == NULL) {
	    err = 1;
	}
	if (*(p+1) == '\'') {
	    transp = 1;
	}
    }

    if (!err) {
	M = fill_matrix_from_list(s, (const double **) *pZ, pdinfo, 
				  transp, &err);
	if (err) {
	    goto finalize;
	} else if (M != NULL) {
	    err = add_or_replace_user_matrix(M, name, mask, NULL,
					     (const double **) *pZ, 
					     pdinfo);
	    goto finalize;
	}
    }

    if (!err) {
	err = get_rows_cols(s, pdinfo, &r, &c, &series);
	if (!err && c == 0) {
	    err = 1;
	}
    }

    if (!err && series) {
	r *= pdinfo->t2 - pdinfo->t1 + 1;
    }

    if (!err && transp) {
	int tmp = r;

	r = c;
	c = tmp;
    }

#if MDEBUG
    fprintf(stderr, "r=%d, c=%d, transp=%d, series=%d\n",
	    r, c, transp, series);
#endif

    if (!err) {
	M = gretl_matrix_alloc(r, c);
	if (M == NULL) {
	   err = E_ALLOC;
	} 
    }

    if (!err) {
	if (series) {
	    err = fill_matrix_from_series(M, s, r, c, transp, 
					  (const double **) *pZ, 
					  pdinfo);
	} else {
	    err = fill_matrix_from_scalars(M, s, r, c, transp,
					   (const double **) *pZ,
					   pdinfo);
	}
    }
    
    if (!err) {
	err = add_or_replace_user_matrix(M, name, mask, NULL,
					 (const double **) *pZ, 
					 pdinfo);
    }

 finalize:

    if (!err) {
	if (gretl_messages_on()) {
	    if (n_matrices > nm) {
		pprintf(prn, "Added matrix '%s'\n", name);
	    } else {
		pprintf(prn, "Replaced matrix '%s'\n", name);
	    }
	}
    } else {
	pprintf(prn, "Error adding matrix '%s'\n", name);
    }

    return err;
}

static int 
print_matrix_by_name (const char *name, const char *mask, 
		      const double **Z, const DATAINFO *pdinfo,
		      PRN *prn)
{
    gretl_matrix *M;
    gretl_matrix *S;
    int err = 0;

    M = get_matrix_by_name(name, pdinfo);
    if (M == NULL) {
	pprintf(prn, _("'%s': no such matrix\n"), name);
	err = 1;
    } else {
	if (*mask != '\0') {
	    S = matrix_get_submatrix(M, mask, Z, pdinfo, &err);
	    if (!err) {
		char mspec[96];

		sprintf(mspec, "%s%s", name, mask);
		gretl_matrix_print_to_prn(S, mspec, prn);
	    }
	    gretl_matrix_free(S);
	} else {
	    gretl_matrix_print_to_prn(M, name, prn);
	}
	if (!original_matrix_by_name(name, pdinfo)) {
	    /* we got a transpose, created on the fly */
	    gretl_matrix_free(M);
	}
    }

    return err;
}

/**
 * matrix_command:
 * @s: string that specifies matrix command.
 * @prn: pointer to printing struct.
 *
 * To be written.
 * 
 * Returns: 0 on success, non-zero code on failure.
 */

int matrix_command (const char *line, double ***pZ, DATAINFO *pdinfo, PRN *prn)
{
    char mask[48] = {0};
    char name[48];
    char word[9];
    char *p;
    int err = 0;

    if (!strncmp(line, "matrix ", 7)) line += 7;
    while (isspace(*line)) line++;

    if (!sscanf(line, "%47s", name)) {
	return 1;
    } 

    line += strlen(name);
    while (isspace(*line)) line++;

    /* extract submatrix "mask", if any */
    p = strchr(name, '[');
    if (p != NULL) {
	strncat(mask, p, 47);
	*p = '\0';
	if (original_matrix_by_name(name, pdinfo) == NULL) {
	    return E_UNKVAR;
	}
    } 

    if (*line == '=') {
	/* defining a matrix */
	err = create_matrix(name, mask, line, pZ, pdinfo, prn);
    } else {
	*word = '\0';
	sscanf(line, "%8s", word);
	if (*word == '\0' || !strcmp(word, "print")) {
	    err = print_matrix_by_name(name, mask, 
				       (const double **) *pZ, 
				       pdinfo, 
				       prn);
	} else if (!strcmp(word, "delete")) {
	    err = delete_matrix_by_name(name);
	} else {
	    /* no other commands available yet */
	    err = 1;
	}
    } 

    return err;
}

/* for use in genr, for matrices */

gretl_matrix *matrix_calc_AB (gretl_matrix *A, gretl_matrix *B, 
			      char op, int *err) 
{
    gretl_matrix *C = NULL;
    gretl_matrix *D = NULL;
    double x;
    int ra, ca;
    int rb, cb;

    *err = 0;

#if MDEBUG
    fprintf(stderr, "\n*** matrix_calc_AB: A = %p, B = %p, ", 
	    (void *) A, (void *) B);
    if (isprint(op)) fprintf(stderr, "op='%c'\n", op);
    else fprintf(stderr, "op=%d\n", op);
    debug_print_matrix(A, "input A");
    debug_print_matrix(B, "input B");
#endif

    switch (op) {
    case '\0':
	C = B;
	break;
    case '+':
    case '-':
	C = gretl_matrix_copy(A);
	if (C == NULL) {
	    *err = E_ALLOC;
	} else if (op == '+') {
	    *err = gretl_matrix_add_to(C, B);
	} else {
	    *err = gretl_matrix_subtract_from(C, B);
	}
	break;
    case '~':
	/* column-wise concatenation */
	C = gretl_matrix_col_concat(A, B, err);
	break;
    case '*':
	ra = gretl_matrix_rows(A);
	ca = gretl_matrix_cols(A);
	rb = gretl_matrix_rows(B);
	cb = gretl_matrix_cols(B);

	if (ra == 1 && ca == 1) {
	    C = gretl_matrix_copy(B);
	    if (C == NULL) {
		*err = E_ALLOC;	
	    } else {
		x = gretl_matrix_get(A, 0, 0);
		gretl_matrix_multiply_by_scalar(C, x);
	    }
	} else if (rb == 1 && cb == 1) {
	    C = gretl_matrix_copy(A);
	    if (C == NULL) {
		*err = E_ALLOC;	
	    } else {
		x = gretl_matrix_get(B, 0, 0);
		gretl_matrix_multiply_by_scalar(C, x);
	    }
	} else {
	    C = gretl_matrix_alloc(ra, cb);
#if MDEBUG
	    fprintf(stderr, "matrix_calc_AB: allocated 'C' (%dx%d) at %p\n", 
		    ra, cb, (void *) C);
#endif
	    if (C == NULL) {
		*err = E_ALLOC;
	    } else {
		*err = gretl_matrix_multiply(A, B, C);
	    }
	}
	break;
    case '/':
	/* matrix "division" */
	C = gretl_matrix_copy(A);
	if (C == NULL) {
	    *err = E_ALLOC;
	} else {
	    D = gretl_matrix_copy(B);
	    if (D == NULL) {
		gretl_matrix_free(C);
		C = NULL;
		*err = E_ALLOC;
	    } else {	
		*err = gretl_LU_solve(D, C);
		gretl_matrix_free(D);
	    }
	}
	break;
    case OP_DOTMULT:
	/* element-wise multiplication */
	C = gretl_matrix_dot_multiply(A, B, err);
	break;
    case OP_DOTDIV:
	/* element-wise division */
	C = gretl_matrix_dot_divide(A, B, err);
	break;
    case OP_DOTPOW:
	/* element-wise exponentiation */
	if (gretl_matrix_rows(B) != 1 ||
	    gretl_matrix_cols(B) != 1) {
	    *err = 1;
	} else {
	    C = gretl_matrix_copy(A);
	    if (C == NULL) {
		*err = E_ALLOC;
	    } else {
		x = gretl_matrix_get(B, 0, 0);
		gretl_matrix_dot_pow(C, x);
	    }
	}
	break;
    case OP_KRON:
	/* Kronecker product */
	C = gretl_matrix_kronecker_product(A, B);
	if (C == NULL) {
	    *err = E_ALLOC;
	}
	break;
    default:
	*err = 1;
	break;
    } 

    if (*err && C != NULL) {
	gretl_matrix_free(C);
	C = NULL;
    }

    return C;
}

static double 
real_user_matrix_get_determinant (gretl_matrix *m, int log)
{
    double d = NADBL;

    if (m != NULL) {
	gretl_matrix *tmp = gretl_matrix_copy(m);

	if (tmp != NULL) {
	    if (log) {
		d = gretl_matrix_log_determinant(tmp);
	    } else {
		d = gretl_matrix_determinant(tmp);
	    }
	    gretl_matrix_free(tmp);
	}
    }

    return d;
}

double user_matrix_get_determinant (gretl_matrix *m)
{
    return real_user_matrix_get_determinant(m, 0);
}

double user_matrix_get_log_determinant (gretl_matrix *m)
{
    return real_user_matrix_get_determinant(m, 1);
}

static gretl_matrix *
real_user_matrix_get_determinant_as_matrix (gretl_matrix *m, int log)
{
    gretl_matrix *dm = NULL;
    double d;

    if (log) {
	d = user_matrix_get_log_determinant(m);
    } else {
	d = user_matrix_get_determinant(m);
    }

    dm = gretl_matrix_from_scalar(d);

    return dm;
}

gretl_matrix *user_matrix_get_determinant_as_matrix (gretl_matrix *m)
{
    return real_user_matrix_get_determinant_as_matrix(m, 0);
}

gretl_matrix *user_matrix_get_log_determinant_as_matrix (gretl_matrix *m)
{
    return real_user_matrix_get_determinant_as_matrix(m, 1);
}

gretl_matrix *user_matrix_get_inverse (gretl_matrix *m)
{
    gretl_matrix *R = NULL;

    if (m != NULL) {
	R = gretl_matrix_copy(m);
	if (R != NULL) {
	    if (gretl_invert_matrix(R)) {
		gretl_matrix_free(R);
		R = NULL;
	    }
	} 
    }

    if (R == NULL) {
	strcpy(gretl_errmsg, _("Matrix inversion failed"));
    }

    return R;
}

gretl_matrix *
user_matrix_get_transformation (gretl_matrix *m, GretlMathFunc fn)
{
    gretl_matrix *R = NULL;

    if (m != NULL) {
	R = gretl_matrix_copy(m);
	if (R != NULL) {
	    if (gretl_matrix_transform_elements(R, fn)) {
		gretl_matrix_free(R);
		R = NULL;
	    }
	}
    }

    return R;
}  

/* move tranpose symbol ' in front of parenthesized
   matrix expression so genr can handle it as a function
*/

int reposition_transpose_symbol (char *s)
{
    int pc, len = strlen(s);
    int offset;
    int i, j, sz;
    int err = 0;

    for (i=3; i<len; i++) {
	if (s[i] == '\'' && s[i-1] == ')') {
	    pc = sz = 1;
	    /* back up to matching left paren */
	    for (j=i-2; j>=0; j--) {
		if (s[j] == ')') {
		    pc++;
		} else if (s[j] == '(') {
		    pc--;
		}
		sz++;
		if (pc == 0) {
		    offset = i - sz;
		    memmove(s + offset + 1, s + offset, sz);
		    s[offset] = '\'';
		    i++;
		    break;
		}
	    }
	    if (j <= 0 && pc != 0) {
		err = E_UNBAL;
		break;
	    }
	}
    }

    return err;
}
