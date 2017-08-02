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

/* interface to libsvm for machine learning */

#include "libgretl.h"
#include "libset.h"
#include "version.h"

#include <svm.h>

typedef struct svm_problem sv_data;
typedef struct svm_node sv_cell;
typedef struct svm_model sv_model;
typedef struct svm_parameter sv_parm;
typedef struct svm_wrapper_ svm_wrapper;

static const char *svm_type_names[] = {
    "c_svc", "nu_svc", "one_class", "epsilon_svr", "nu_svr"
};

static const char *kernel_type_names[] = {
    "linear", "polynomial", "rbf", "sigmoid", "precomputed"
};

struct svm_wrapper_ {
    int auto_type;
    int scaling;
    int t2_train;
    int xvalid;
    int savemod;
    int loadmod;
    int predict;
    int quiet;
    int libsvm_ranges;
    gretl_matrix *ranges;
    char *ranges_outfile;
    char *ranges_infile;
    char *model_outfile;
    char *model_infile;
    char *data_outfile;
};

struct svm_parm_info {
    const char *key;
    GretlType type;
};

#define N_PARMS 15

static int gui_mode;

static void svm_flush (PRN *prn)
{
    if (gui_mode) {
	gretl_gui_flush();
    } else {
	gretl_print_flush_stream(prn);
    }
}

static void svm_wrapper_init (svm_wrapper *w)
{
    w->auto_type = EPSILON_SVR;
    w->scaling = 1;
    w->t2_train = 0;
    w->xvalid = 0;
    w->savemod = 0;
    w->loadmod = 0;
    w->predict = 2;
    w->quiet = 0;
    w->libsvm_ranges = 0;
    w->ranges = NULL;
    w->ranges_outfile = NULL;
    w->ranges_infile = NULL;
    w->model_outfile = NULL;
    w->model_infile = NULL;
    w->data_outfile = NULL;
}

static void svm_wrapper_free (svm_wrapper *w)
{
    gretl_matrix_free(w->ranges);
    free(w->ranges_outfile);
    free(w->ranges_infile);
    free(w->model_outfile);
    free(w->model_infile);
    free(w->data_outfile);
}

static int doing_file_io (svm_wrapper *w)
{
    return w->ranges_outfile != NULL ||
	w->ranges_infile != NULL ||
	w->model_outfile != NULL ||
	w->model_infile != NULL ||
	w->data_outfile != NULL;
}

static void set_svm_param_defaults (sv_parm *parm)
{
    parm->svm_type = -1; /* mark as unknown for now */
    parm->kernel_type = RBF;
    parm->degree = 3;   /* for polynomial */
    parm->gamma = 0;    /* poly/rbf/sigmoid: default 1.0 / num_features */
    parm->coef0 = 0;    /* for use in kernel function */

    /* training-only variables */
    parm->cache_size = 1024;   /* cache size in MB */
    parm->eps = 0.001;         /* stopping criterion */
    parm->C = 1;               /* cost: for C_SVC, EPSILON_SVR and NU_SVR */
    parm->nr_weight = 0;       /* for C_SVC */
    parm->weight_label = NULL; /* for C_SVC */
    parm->weight = NULL;       /* for C_SVC */
    parm->nu = 0.5;            /* for NU_SVC, ONE_CLASS, and NU_SVR */
    parm->p = 0.1;             /* for EPSILON_SVR */
    parm->shrinking = 1;       /* use the shrinking heuristics */
    parm->probability = 0;     /* do probability estimates */
}

static int set_or_print_svm_parm (sv_parm *parm, gretl_bundle *b,
				  PRN *prn)
{
    struct svm_parm_info pinfo[N_PARMS] = {
	{ "svm_type",     GRETL_TYPE_INT },
	{ "kernel_type",  GRETL_TYPE_INT },
	{ "degree",       GRETL_TYPE_INT },
	{ "gamma",        GRETL_TYPE_DOUBLE },
	{ "coef0",        GRETL_TYPE_DOUBLE },
	{ "cachesize",    GRETL_TYPE_DOUBLE },
	{ "toler",        GRETL_TYPE_DOUBLE },
	{ "cost",         GRETL_TYPE_DOUBLE },
	{ "nr_weight",    GRETL_TYPE_INT },
	{ "weight_label", GRETL_TYPE_SERIES },
	{ "weight",       GRETL_TYPE_SERIES },
	{ "nu",           GRETL_TYPE_DOUBLE },
	{ "epsilon",      GRETL_TYPE_DOUBLE },
	{ "shrinking",    GRETL_TYPE_BOOL },
	{ "probability",  GRETL_TYPE_BOOL }
    };
    void *elem[N_PARMS] = {
	&parm->svm_type,
	&parm->kernel_type,
	&parm->degree,
	&parm->gamma,
	&parm->coef0,
	&parm->cache_size,
	&parm->eps,
	&parm->C,
	&parm->nr_weight,
	&parm->weight_label,
	&parm->weight,
	&parm->nu,
	&parm->p,
	&parm->shrinking,
	&parm->probability
    };
    int i, ival;
    double xval;
    int err = 0;

    if (b == NULL) {
	/* we're just printing */
	pputs(prn, "libsvm parameter values:\n");
	for (i=0; i<N_PARMS && !err; i++) {
	    if (pinfo[i].type == GRETL_TYPE_DOUBLE) {
		xval = *(double *) elem[i];
		pprintf(prn, " %s = %g\n", pinfo[i].key, xval);
	    } else {
		ival = *(int *) elem[i];
		pprintf(prn, " %s = %d\n", pinfo[i].key, ival);
	    }
	}
	return 0;
    }

    set_svm_param_defaults(parm);

    for (i=0; i<N_PARMS && !err; i++) {
	if (gretl_bundle_has_key(b, pinfo[i].key)) {
	    if (i >= 8 && i <= 10) {
		pputs(prn, "Sorry, svm weighting not handled yet\n");
		err = E_INVARG;
	    } else if (pinfo[i].type == GRETL_TYPE_DOUBLE) {
		xval = gretl_bundle_get_scalar(b, pinfo[i].key, &err);
		if (!err) {
		    *(double *) elem[i] = xval;
		}
	    } else if (pinfo[i].type == GRETL_TYPE_INT ||
		       pinfo[i].type == GRETL_TYPE_BOOL) {
		ival = gretl_bundle_get_int(b, pinfo[i].key, &err);
		if (!err) {
		    if (pinfo[i].type == GRETL_TYPE_BOOL) {
			*(int *) elem[i] = (ival != 0);
		    } else {
			*(int *) elem[i] = ival;
		    }
		}
	    }
	}
    }

    return err;
}

static int set_svm_parm (sv_parm *parm, gretl_bundle *b,
			 PRN *prn)
{
    return set_or_print_svm_parm(parm, b, prn);
}

static void print_svm_parm (sv_parm *parm, PRN *prn)
{
    set_or_print_svm_parm(parm, NULL, prn);
}

static void gretl_destroy_svm_model (sv_model *model)
{
    if (model != NULL) {
	if (model->l > 0 && model->SV != NULL &&
	    model->SV[0] != NULL) {
	    free(model->SV[0]);
	}
	if (model->sv_coef != NULL) {
	    doubles_array_free(model->sv_coef, model->nr_class-1);
	}
	free(model->SV);
	free(model->rho);
	free(model->label);
	free(model->probA);
	free(model->probB);
	free(model->sv_indices);
	free(model->nSV);
	free(model);
    }
}

static int bundle_as_matrix (gretl_bundle *b, const char *key,
			     double *xvals, int n)
{
    gretl_matrix *m = gretl_matrix_alloc(n, 1);

    if (m == NULL) {
	return E_ALLOC;
    } else {
	memcpy(m->val, xvals, n * sizeof *xvals);
	gretl_bundle_donate_data(b, key, m, GRETL_TYPE_MATRIX, 0);
	return 0;
    }
}

static int bundle_as_list (gretl_bundle *b, const char *key,
			   int *ivals, int n)
{
    int *list = gretl_list_new(n);

    if (list == NULL) {
	return E_ALLOC;
    } else {
	memcpy(list + 1, ivals, n * sizeof *ivals);
	gretl_bundle_donate_data(b, key, list, GRETL_TYPE_LIST, 0);
	return 0;
    }
}

static int svm_model_save_to_bundle (const sv_model *model,
				     gretl_bundle *b)
{
    const struct svm_parameter *parm = &model->param;
    gretl_matrix *m = NULL;
    int i, j, nc, l, ntr;
    int err = 0;

    gretl_bundle_void_content(b);

    gretl_bundle_set_int(b, "svm_type", parm->svm_type);
    gretl_bundle_set_int(b, "kernel_type", parm->kernel_type);

    if (parm->kernel_type == POLY) {
	gretl_bundle_set_int(b, "degree", parm->degree);
    }

    if (parm->kernel_type == POLY ||
	parm->kernel_type == RBF ||
	parm->kernel_type == SIGMOID) {
	gretl_bundle_set_scalar(b, "gamma", parm->gamma);
    }

    if (parm->kernel_type == POLY || parm->kernel_type == SIGMOID) {
	gretl_bundle_set_scalar(b, "coef0", parm->coef0);
    }

    nc = model->nr_class;
    l = model->l;

    gretl_bundle_set_int(b, "nr_class", nc);
    gretl_bundle_set_int(b, "l", l);

    /* number of triangular elements */
    ntr = nc * (nc - 1) / 2;

    bundle_as_matrix(b, "rho", model->rho, ntr);

    if (model->label != NULL) {
	bundle_as_list(b, "label", model->label, nc);
    }
    if (model->probA != NULL) {
	bundle_as_matrix(b, "probA", model->probA, ntr);
    }
    if (model->probB != NULL) {
	bundle_as_matrix(b, "probB", model->probB, ntr);
    }
    if (model->nSV != NULL) {
	bundle_as_list(b, "nr_sv", model->nSV, nc);
    }

    /* store the SVs */

    m = gretl_matrix_alloc(l, nc - 1);
    if (m != NULL) {
	for (j=0; j<nc-1; j++) {
	    /* j = class index */
	    for (i=0; i<l; i++) {
		/* i = row index */
		gretl_matrix_set(m, i, j, model->sv_coef[j][i]);
	    }
	}
	gretl_bundle_donate_data(b, "sv_coef", m,
				 GRETL_TYPE_MATRIX, 0);
    }

    if (parm->kernel_type == PRECOMPUTED) {
	int *plist = gretl_list_new(l);

	if (plist != NULL) {
	    const sv_cell *p;

	    for (i=0; i<l; i++) {
		p = model->SV[i];
		plist[i+1] = (int) p->value;
	    }
	}
    } else {
	/* not a precomputed kernel: more complicated */
	gretl_array *aidx, *avec = NULL;
	gretl_matrix *vec;
	int *idx;
	int n_elements = 0;

	aidx = gretl_array_new(GRETL_TYPE_LISTS, l, &err);
	if (!err) {
	    avec = gretl_array_new(GRETL_TYPE_MATRICES, l, &err);
	}

	for (i=0; i<l && !err; i++) {
	    const sv_cell *p = model->SV[i];
	    int k, ni = 0;

	    /* count the nodes on this row */
	    while (p->index != -1) {
		ni++;
		p++;
	    }
	    idx = gretl_list_new(ni);
	    vec = gretl_matrix_alloc(1, ni);
	    if (idx == NULL || vec == NULL) {
		err = E_ALLOC;
		break;
	    }
	    p = model->SV[i]; /* reset */
	    for (k=0; k<ni; k++) {
		idx[k+1] = p[k].index;
		vec->val[k] = p[k].value;
	    }
	    gretl_array_set_list(aidx, i, idx, 0);
	    gretl_array_set_matrix(avec, i, vec, 0);
	    n_elements += ni + 1;
	}

	if (err) {
	    gretl_array_destroy(aidx);
	    gretl_array_destroy(avec);
	} else {
	    gretl_bundle_set_int(b, "n_elements", n_elements);
	    gretl_bundle_donate_data(b, "SV_indices", aidx,
				     GRETL_TYPE_ARRAY, 0);
	    gretl_bundle_donate_data(b, "SV_vecs", avec,
				     GRETL_TYPE_ARRAY, 0);
	}
    }

    if (err) {
	gretl_bundle_void_content(b);
    }

    return err;
}

static double *array_from_bundled_matrix (gretl_bundle *b,
					  const char *key,
					  int required,
					  int *err)
{
    double *ret = NULL;

    if (*err) return NULL;

    if (gretl_bundle_has_key(b, key)) {
	gretl_matrix *m = gretl_bundle_get_matrix(b, key, err);

	if (m != NULL) {
	    int n = m->rows * m->cols;

	    ret = malloc(n * sizeof *ret);
	    if (ret == NULL) {
		*err = E_ALLOC;
	    } else {
		memcpy(ret, m->val, n * sizeof *ret);
	    }
	}
    } else if (required) {
	gretl_errmsg_sprintf("svm model: required matrix %s was not found", key);
	*err = E_DATA;
    }

    return ret;
}

static int *array_from_bundled_list (gretl_bundle *b,
				     const char *key,
				     int required,
				     int *err)
{
    int *ret = NULL;

    if (*err) return NULL;

    if (gretl_bundle_has_key(b, key)) {
	int *list = gretl_bundle_get_list(b, key, err);

	if (list != NULL) {
	    int n = list[0];

	    ret = malloc(n * sizeof *ret);
	    if (ret == NULL) {
		*err = E_ALLOC;
	    } else {
		memcpy(ret, list + 1, n * sizeof *ret);
	    }
	}
    } else if (required) {
	gretl_errmsg_sprintf("svm model: required list %s was not found", key);
	*err = E_DATA;
    }

    return ret;
}

static sv_model *svm_model_from_bundle (gretl_bundle *b,
					int *err)
{
    struct svm_parameter *parm;
    sv_model *model;
    gretl_matrix *m;
    int n_elements = 0;
    int nc = 0, l = 0;
    int i, j;

    model = malloc(sizeof *model);
    if (model == NULL) {
	*err = E_ALLOC;
	return NULL;
    }

    memset(model, 0, sizeof *model);
    parm = &model->param;
    *err = set_svm_parm(parm, b, NULL);

    if (!*err) {
	nc = model->nr_class = gretl_bundle_get_int(b, "nr_class", err);
	l = model->l = gretl_bundle_get_int(b, "l", err);
	n_elements = gretl_bundle_get_int(b, "n_elements", err);
	if (nc <= 0 || l <= 0 || n_elements <= 0) {
	    *err = E_DATA;
	}
    }

    model->rho = array_from_bundled_matrix(b, "rho", 1, err);

    /* not always present */
    model->label = array_from_bundled_list(b, "label", 0, err);
    model->probA = array_from_bundled_matrix(b, "probA", 0, err);
    model->probB = array_from_bundled_matrix(b, "probB", 0, err);
    model->nSV = array_from_bundled_list(b, "nr_sv", 0, err);

    /* load the SVs */

    if (!*err) {
	m = gretl_bundle_get_matrix(b, "sv_coef", err);
	if (m == NULL) {
	    *err = E_DATA;
	} else {
	    model->sv_coef = doubles_array_new(nc-1, l);
	    if (model->sv_coef == NULL) {
		*err = E_ALLOC;
	    } else {
		double *val = m->val;

		for (j=0; j<nc-1; j++) {
		    memcpy(model->sv_coef[j], val, l * sizeof *val);
		    val += l;
		}
	    }
	}
    }

    if (!*err && parm->kernel_type == PRECOMPUTED) {
	gretl_errmsg_set("svm precomputed kernel: not handled yet");
	*err = E_DATA;
    } else if (!*err) {
	sv_cell *p, *x_space = NULL;
	gretl_array *aidx = NULL;
	gretl_array *avec = NULL;
	gretl_matrix *vec;
	int *idx;

	model->SV = malloc(l * sizeof *model->SV);
	if (model->SV == NULL) {
	    *err = E_ALLOC;
	} else {
	    x_space = malloc(n_elements * sizeof *x_space);
	    if (x_space == NULL) {
		*err = E_ALLOC;
	    } else {
		model->SV[0] = p = x_space;
	    }
	}

	if (!*err) {
	    aidx = gretl_bundle_get_array(b, "SV_indices", NULL);
	    avec = gretl_bundle_get_array(b, "SV_vecs", NULL);
	    if (gretl_array_get_type(aidx) != GRETL_TYPE_LISTS ||
		gretl_array_get_type(avec) != GRETL_TYPE_MATRICES) {
		*err = E_DATA;
	    }
	}

	for (i=0; i<l && !*err; i++) {
	    int ni;

	    model->SV[i] = p;
	    idx = gretl_array_get_element(aidx, i, NULL, err);
	    vec = gretl_array_get_element(avec, i, NULL, err);
	    ni = idx[0];
	    for (j=0; j<ni; j++) {
		p[j].index = idx[j+1];
		p[j].value = vec->val[j];
	    }
	    /* add -1 sentinel */
	    p[j].index = -1;
	    p += ni + 1;
	}
    }

    if (*err) {
	gretl_destroy_svm_model(model);
	model = NULL;
    }

    return model;
}

/* can use for testing against svm-scale */

static int write_ranges (svm_wrapper *w)
{
    FILE *fp;
    double lo, hi;
    int i, idx, vi;

    fp = gretl_fopen(w->ranges_outfile, "wb");
    if (fp == NULL) {
	return E_FOPEN;
    }

    gretl_push_c_numeric_locale();

    if (w->libsvm_ranges) {
	fprintf(fp, "x\n%d %d\n",
		(int) gretl_matrix_get(w->ranges, 0, 0),
		(int) gretl_matrix_get(w->ranges, 0, 1));
    } else {
	fprintf(fp, "x\n%d %d %d\n",
		(int) gretl_matrix_get(w->ranges, 0, 0),
		(int) gretl_matrix_get(w->ranges, 0, 1),
		(int) gretl_matrix_get(w->ranges, 0, 2));
    }

    for (i=1; i<w->ranges->rows; i++) {
	idx = gretl_matrix_get(w->ranges, i, 0);
	lo  = gretl_matrix_get(w->ranges, i, 1);
	hi  = gretl_matrix_get(w->ranges, i, 2);
	if (w->libsvm_ranges) {
	    fprintf(fp, "%d %.16g %.16g\n", idx, lo, hi);
	} else {
	    vi = gretl_matrix_get(w->ranges, i, 3);
	    fprintf(fp, "%d %.16g %.16g %d\n", idx, lo, hi, vi);
	}
    }

    gretl_pop_c_numeric_locale();

    fclose(fp);

    return 0;
}

static int read_ranges (svm_wrapper *w)
{
    FILE *fp;
    char line[512];
    double lo, hi, j;
    int read_lims = 0;
    int ncols = 4;
    int i, vi, idx, n = 0;
    int err = 0;

    fp = gretl_fopen(w->ranges_infile, "rb");
    if (fp == NULL) {
	return E_FOPEN;
    }

    gretl_push_c_numeric_locale();

    while (fgets(line, sizeof line, fp) && !err) {
	if (*line == 'x') {
	    read_lims = 1;
	    continue;
	}
	if (read_lims) {
	    if (ncols == 3) {
		n = sscanf(line, "%lf %lf\n", &lo, &hi);
	    } else {
		n = sscanf(line, "%lf %lf %lf\n", &lo, &hi, &j);
	    }
	    if (n != ncols - 1) {
		err = E_DATA;
	    }
	    read_lims = 0;
	} else if (!string_is_blank(line)) {
	    n++;
	}
    }

    w->ranges = gretl_matrix_alloc(n+1, ncols);
    if (w->ranges == NULL) {
	err = E_ALLOC;
    } else {
	gretl_matrix_set(w->ranges, 0, 0, lo);
	gretl_matrix_set(w->ranges, 0, 1, hi);
	gretl_matrix_set(w->ranges, 0, 2, j);
	if (ncols == 4) {
	    gretl_matrix_set(w->ranges, 0, 3, 0);
	}
	rewind(fp);
	i = 1;
    }

    while (fgets(line, sizeof line, fp) && !err) {
	if (*line == 'x') {
	    fgets(line, sizeof line, fp);
	    continue;
	}
	if (ncols == 3) {
	    n = sscanf(line, "%d %lf %lf\n", &idx, &lo, &hi);
	} else {
	    n = sscanf(line, "%d %lf %lf %d\n", &idx, &lo, &hi, &vi);
	}
	if (n != ncols) {
	    err = E_DATA;
	} else {
	    gretl_matrix_set(w->ranges, i, 0, idx);
	    gretl_matrix_set(w->ranges, i, 1, lo);
	    gretl_matrix_set(w->ranges, i, 2, hi);
	    if (ncols == 4) {
		gretl_matrix_set(w->ranges, i, 3, vi);
	    }
	    i++;
	}
    }

    gretl_pop_c_numeric_locale();

    fclose(fp);

    return err;
}

/* can use for testing against svm-scale */

static int write_problem (sv_data *p, int k, const char *fname)
{
    FILE *fp;
    int i, t, idx;
    double val;

    fp = gretl_fopen(fname, "wb");
    if (fp == NULL) {
	return E_FOPEN;
    }

    gretl_push_c_numeric_locale();

    for (t=0; t<p->l; t++) {
	fprintf(fp, "%g ", p->y[t]);
	for (i=0; i<k; i++) {
	    idx = p->x[t][i].index;
	    val = p->x[t][i].value;
	    if (val != 0) {
		fprintf(fp, "%d:%g ", idx, val);
	    }
	}
	fputc('\n', fp);
    }

    gretl_pop_c_numeric_locale();

    fclose(fp);

    return 0;
}

static void gretl_sv_data_destroy (sv_data *p, sv_cell *x_space)
{
    if (p != NULL) {
	free(p->y);
	free(p->x);
	free(p);
    }
    if (x_space != NULL) {
	free(x_space);
    }
}

static sv_data *gretl_sv_data_alloc (int T, int k,
				     sv_cell **px_space,
				     int *err)
{
    sv_data *p = malloc(sizeof *p);

    if (p != NULL) {
	p->l = T;
	p->y = malloc(T * sizeof *p->y);
	p->x = malloc(T * sizeof *p->x);
	if (p->y == NULL || p->x == NULL) {
	    *err = E_ALLOC;
	} else {
	    /* we need an extra cell on each row to hold a
	       sentinel index value of -1
	    */
	    *px_space = malloc(T * (k+1) * sizeof(sv_cell));
	    if (*px_space == NULL) {
		*err = E_ALLOC;
	    }
	}
	if (*err) {
	    gretl_sv_data_destroy(p, NULL);
	    p = NULL;
	}
    } else {
	*err = E_ALLOC;
    }

    return p;
}

/* initial discovery of ranges of the RHS data using the
   training data */

static int get_data_ranges (const int *list,
			    const DATASET *dset,
			    svm_wrapper *w)
{
    const double *x;
    double xmin, xmax;
    int k = list[0] - 1;
    int i, j, vi;
    int err = 0;

    w->ranges = gretl_matrix_alloc(k+1, 4);
    if (w->ranges == NULL) {
	return E_ALLOC;
    }

    /* scaling limits */
    xmin = w->scaling == 2 ? 0 : -1;
    gretl_matrix_set(w->ranges, 0, 0, xmin); /* lower */
    gretl_matrix_set(w->ranges, 0, 1, 1);    /* upper */

    /* padding */
    gretl_matrix_set(w->ranges, 0, 2, 0);
    gretl_matrix_set(w->ranges, 0, 3, 0);

    /* note: we make no provision for scaling y at present */

    j = 0;
    for (i=2; i<=list[0]; i++) {
	vi = list[i];
	x = dset->Z[vi];
	gretl_minmax(dset->t1, dset->t2, x, &xmin, &xmax);
	if (xmin != xmax) {
	    j++;
	    gretl_matrix_set(w->ranges, j, 0, j); /* or... ? */
	    gretl_matrix_set(w->ranges, j, 1, xmin);
	    gretl_matrix_set(w->ranges, j, 2, xmax);
	    gretl_matrix_set(w->ranges, j, 3, vi);
	} else {
	    fprintf(stderr, "training data: dropping var %d (%s)\n",
		    vi, dset->varname[vi]);
	}
    }

    /* FIXME check for pathologies! (NAs, no non-constant
       series or whatever) */

    /* record number of rows actually occupied, which
       could be less than the number allocated
    */
    gretl_matrix_set(w->ranges, 0, 2, j + 1);

    return err;
}

/* The following requires a gretl-special "ranges" matrix
   with 4 columns, including the dataset IDs of the series.
*/

static int check_test_data (const DATASET *dset,
			    gretl_matrix *ranges,
			    int k)
{
    double xmin, xmax;
    int i, n, vi;
    int err = 0;

    n = 0;
    for (i=1; i<=k; i++) {
	vi = gretl_matrix_get(ranges, i, 3);
	gretl_minmax(dset->t1, dset->t2, dset->Z[vi], &xmin, &xmax);
	if (xmin != xmax) {
	    n++;
	} else {
	    fprintf(stderr, "test data: dropping var %d (%s)\n",
		    vi, dset->varname[vi]);
	    /* arrange to exclude this variable by setting the
	       record of its series ID to zero
	    */
	    gretl_matrix_set(ranges, i, 3, 0);
	}
    }

    if (n != k) {
	fprintf(stderr, "test data: number of usable variables (%d) "
		"differs from training data (%d)\n", n, k);
    } else {
	fprintf(stderr, "test data: number of usable variables "
		"agrees with training data\n");
    }

    return err;
}

/* apply scaling as per the svm-scale binary */

static double scale_x (double val, double lo, double hi,
		       double scalemin, double scalemax)
{
    if (val == lo) {
	val = scalemin;
    } else if (val == hi) {
	val = scalemax;
    } else {
	val = scalemin + (scalemax - scalemin) *
	    (val - lo) / (hi - lo);
    }

    return val;
}

static int sv_data_fill (sv_data *prob,
			 sv_cell *x_space, int k,
			 svm_wrapper *w,
			 const int *list,
			 const DATASET *dset,
			 int pass)
{
    double scalemin, scalemax;
    double xit, xmin, xmax;
    int i, j, s, t, vi, idx;
    int pos = 0;

    /* deal with the LHS variable */
    vi = list[1];
    if (pass == 1 &&
	(gretl_isdummy(dset->t1, dset->t2, dset->Z[vi]) ||
	 series_is_coded(dset, vi))) {
	/* classification, not regression */
	w->auto_type = C_SVC;
    }
    for (i=0, t=dset->t1; t<=dset->t2; t++, i++) {
	prob->y[i] = dset->Z[vi][t];
    }

    /* retrieve the global x-scaling limits */
    scalemin = gretl_matrix_get(w->ranges, 0, 0);
    scalemax = gretl_matrix_get(w->ranges, 0, 1);

    /* write the scaled x-data into the problem struct */
    for (s=0, t=dset->t1; t<=dset->t2; t++, s++) {
	prob->x[s] = &x_space[pos];
	j = 0;
	for (i=1; i<=k; i++) {
	    if (w->ranges->cols == 4) {
		vi = (int) gretl_matrix_get(w->ranges, i, 3);
		if (vi <= 0) {
		    /* may happen when we get to the test data */
		    continue;
		}
	    }
	    idx = (int) gretl_matrix_get(w->ranges, i, 0);
	    xmin = gretl_matrix_get(w->ranges, i, 1);
	    xmax = gretl_matrix_get(w->ranges, i, 2);
	    xit = dset->Z[vi][t];
	    if (w->scaling != 0) {
		xit = scale_x(xit, xmin, xmax, scalemin, scalemax);
	    }
	    if (xit == 0) {
		/* fprintf(stderr, "skipping a 0 data value (var %d)\n", vi); */
		continue;
	    }
	    prob->x[s][j].index = idx;
	    prob->x[s][j].value = xit;
	    pos++;
	    j++;
	}
	/* end-of-row sentinel */
	prob->x[s][j].index = -1;
	prob->x[s][j].value = 0;
	pos++;
    }

    return 0;
}

static int real_svm_predict (double *yhat,
			     sv_data *prob,
			     sv_model *model,
			     int training,
			     const DATASET *dset,
			     PRN *prn)
{
    const char *label;
    int n_correct = 0;
    int regression = 0;
    double ymean = 0;
    double TSS = 0.0;
    double SSR = 0.0;
    double dev, yhi;
    sv_cell *x;
    int i;

    if (model->param.svm_type == EPSILON_SVR ||
	model->param.svm_type == NU_SVR) {
	regression = 1;
	ymean = gretl_mean(0, prob->l - 1, prob->y);
    }

    pprintf(prn, "Calling prediction function (this may take a while)\n");
    svm_flush(prn);
    for (i=0; i<prob->l; i++) {
	x = prob->x[i];
	yhi = svm_predict(model, x);
	yhat[dset->t1 + i] = yhi;
	if (regression) {
	    dev = prob->y[i] - ymean;
	    TSS += dev * dev;
	    dev = prob->y[i] - yhi;
	    SSR += dev * dev;
	} else {
	    n_correct += (yhi == prob->y[i]);
	}
    }

    label = training ? "Training data" : "Test data";

    if (regression) {
	double r;

	r = gretl_corr(0, prob->l - 1, prob->y, yhat + dset->t1, NULL);
	pprintf(prn, "%s: MSE = %g, R^2 = %g, squared corr = %g\n", label,
		SSR / prob->l, 1.0 - SSR / TSS, r * r);
    } else {
	pprintf(prn, "%s: correct predictions = %d (%.1f percent)\n", label,
		n_correct, 100 * n_correct / (double) prob->l);
    }

    return 0;
}

static int do_cross_validation (sv_data *prob,
				sv_parm *parm,
				int xvalid,
				PRN *prn)
{
    double *targ;
    int i, n = prob->l;

    targ = malloc(n * sizeof *targ);
    if (targ == NULL) {
	return E_ALLOC;
    }

    svm_cross_validation(prob, parm, xvalid, targ);

    if (parm->svm_type == EPSILON_SVR || parm->svm_type == NU_SVR) {
	double total_error = 0;
	double sumv = 0, sumy = 0, sumvv = 0, sumyy = 0, sumvy = 0;
	double yi, vi;

	for (i=0; i<prob->l; i++) {
	    yi = prob->y[i];
	    vi = targ[i];
	    total_error += (vi-yi)*(vi-yi);
	    sumv += vi;
	    sumy += yi;
	    sumvv += vi*vi;
	    sumyy += yi*yi;
	    sumvy += vi*yi;
	}
	pprintf(prn, "Cross Validation Mean squared error = %g\n",
		total_error / n);
	pprintf(prn, "Cross Validation Squared correlation coefficient = %g\n",
	       ((n*sumvy-sumv*sumy)*(n*sumvy-sumv*sumy)) /
	       ((n*sumvv-sumv*sumv)*(n*sumyy-sumy*sumy)));
    } else {
	int n_correct = 0;

	for (i=0; i<n; i++) {
	    if (targ[i] == prob->y[i]) {
		n_correct++;
	    }
	}
	pprintf(prn, "Cross Validation Accuracy = %g%%\n",
		100.0 * n_correct / (double) n);
    }

    free(targ);

    return 0;
}

/* It's OK if there's no @key value in bundle @b, but if it
   is present it should hold an integer value. Return 1
   if key found and integer retrieved OK, else 0.
*/

static int get_optional_int (gretl_bundle *b, const char *key,
			     int *ival, int *err)
{
    if (!*err && gretl_bundle_has_key(b, key)) {
	*ival = gretl_bundle_get_int(b, key, err);
	return (*err == 0);
    } else {
	return 0;
    }
}

static int read_params_bundle (gretl_bundle *bparm,
			       gretl_bundle *bmod,
			       svm_wrapper *wrap,
			       sv_parm *parm,
			       const int *list,
			       const DATASET *dset,
			       PRN *prn)
{
    const char *strval;
    int ival, err = 0;

    /* start by reading some info that's not included in
       the libsvm @parm struct
    */

    if (get_optional_int(bparm, "loadmod", &ival, &err)) {
	if (ival != 0 && bmod == NULL) {
	    fprintf(stderr, "invalid 'loadmod' arg %d\n", ival);
	    err = E_INVARG;
	} else {
	    wrap->loadmod = ival;
	}
    }

    if (get_optional_int(bparm, "scaling", &ival, &err)) {
	if (ival < 0 || ival > 2) {
	    fprintf(stderr, "invalid 'scaling' arg %d\n", ival);
	    err = E_INVARG;
	} else {
	    wrap->scaling = ival;
	}
    }

    if (get_optional_int(bparm, "predict", &ival, &err)) {
	if (ival < 0 || ival > 2) {
	    fprintf(stderr, "invalid 'predict' arg %d\n", ival);
	    err = E_INVARG;
	} else {
	    wrap->predict = ival;
	}
    }

    if (get_optional_int(bparm, "n_train", &ival, &err)) {
	if (ival != 0 && (ival < list[0] || ival > dset->n)) {
	    fprintf(stderr, "invalid 'n_train' arg %d\n", ival);
	    err = E_INVARG;
	} else if (ival > 0) {
	    wrap->t2_train = ival - 1; /* zero-based */
	}
    }

    if (get_optional_int(bparm, "xvalidate", &ival, &err)) {
	if (ival < 2) {
	    fprintf(stderr, "invalid 'xvalidate' arg %d\n", ival);
	    err = E_INVARG;
	} else {
	    wrap->xvalid = ival;
	}
    }

    if (get_optional_int(bparm, "quiet", &ival, &err)) {
	wrap->quiet = (ival != 0);
    }

    if (!err) {
	strval = gretl_bundle_get_string(bparm, "ranges_outfile", NULL);
	if (strval != NULL && *strval != '\0') {
	    wrap->ranges_outfile = gretl_strdup(strval);
	}
	strval = gretl_bundle_get_string(bparm, "data_outfile", NULL);
	if (strval != NULL && *strval != '\0') {
	    wrap->data_outfile = gretl_strdup(strval);
	}
	strval = gretl_bundle_get_string(bparm, "ranges_infile", NULL);
	if (strval != NULL && *strval != '\0') {
	    wrap->ranges_infile = gretl_strdup(strval);
	}
	strval = gretl_bundle_get_string(bparm, "model_outfile", NULL);
	if (strval != NULL && *strval != '\0') {
	    wrap->model_outfile = gretl_strdup(strval);
	}
	strval = gretl_bundle_get_string(bparm, "model_infile", NULL);
	if (strval != NULL && *strval != '\0') {
	    wrap->model_infile = gretl_strdup(strval);
	}
	strval = gretl_bundle_get_string(bparm, "range_format", NULL);
	if (strval != NULL && !strcmp(strval, "libsvm")) {
	    wrap->libsvm_ranges = 1;
	}
    }

    if (!err && !wrap->loadmod && bmod != NULL) {
	/* implicitly, the model should be saved to @bmod */
	wrap->savemod = 1;
    }

    /* if we're still OK, fill out the libsvm @parm struct */

    if (!err) {
	err = set_svm_parm(parm, bparm, prn);
    }

    return err;
}

/* wrap a couple of libsvm I/0 functions so they don't
   fall foul of filename encoding issues on Windows
*/

static sv_model *svm_load_model_wrapper (const char *fname,
					 int *err)
{
    sv_model *model = NULL;
    char *conv = NULL;

    *err = maybe_recode_path(fname, &conv, -1);

    if (!*err) {
	model = svm_load_model(conv != NULL ? conv : fname);
	if (model == NULL) {
	    *err = E_EXTERNAL;
	}
	free(conv);
    }

    return model;
}

static int svm_save_model_wrapper (const char *fname,
				   const sv_model *model)
{
    char *conv = NULL;
    int err;

    err = maybe_recode_path(fname, &conv, -1);

    if (!err) {
	err = svm_save_model(conv != NULL ? conv : fname, model);
	if (err) {
	    err = E_EXTERNAL;
	}
	free(conv);
    }

    return err;
}

static PRN *svm_prn;

/* callback function for setting on libsvm printing */

static void gretl_libsvm_print (const char *s)
{
    if (svm_prn != NULL) {
	pputs(svm_prn, s);
	gretl_gui_flush();
    } else {
	fputs(s, stdout);
    }
}

static void gretl_libsvm_noprint (const char *s)
{
    return;
}

static void report_result (int err, PRN *prn)
{
    if (prn != NULL) {
	if (err) {
	    pprintf(prn, "err = %d\n", err);
	} else {
	    pputs(prn, "OK\n");
	}
    }
}

static int get_svm_ranges (const int *list,
			   DATASET *dset,
			   svm_wrapper *wrap,
			   PRN *prn)
{
    int err = 0;

    if (wrap->ranges_infile != NULL) {
	pprintf(prn, "Getting data ranges from %s... ",
		wrap->ranges_infile);
	err = read_ranges(wrap);
	report_result(err, prn);
	svm_flush(prn);
    } else {
	pprintf(prn, "Getting data ranges (sample = %d to %d)... ",
		dset->t1 + 1, dset->t2 + 1);
	err = get_data_ranges(list, dset, wrap);
	report_result(err, prn);
	if (!err && wrap->ranges_outfile != NULL) {
	    err = write_ranges(wrap);
	    report_result(err, prn);
	}
	svm_flush(prn);
    }

    return err;
}

static int check_svm_params (sv_data *data,
			     sv_parm *parm,
			     PRN *prn)
{
    const char *msg = svm_check_parameter(data, parm);
    int err = 0;

    pputs(prn, "Checking parameter values... ");
    if (msg != NULL) {
	pputs(prn, "problem\n");
	gretl_errmsg_sprintf("svm: %s", msg);
	err = E_INVARG;
    } else if (prn != NULL) {
	pputs(prn, "OK\n");
	pprintf(prn, "svm_type %s, kernel_type %s\n",
		svm_type_names[parm->svm_type],
		kernel_type_names[parm->kernel_type]);
    }

    if (0 && prn != NULL) {
	/* do this only if we got a verbose flag? */
	print_svm_parm(parm, prn);
    }

    return err;
}

int gretl_svm_predict (const int *list,
		       gretl_bundle *bparams,
		       gretl_bundle *bmodel,
		       double *yhat,
		       int *yhat_written,
		       DATASET *dset,
		       PRN *inprn)
{
    sv_parm parm;
    svm_wrapper wrap;
    sv_data *prob1 = NULL;
    sv_data *prob2 = NULL;
    sv_cell *x_space1 = NULL;
    sv_cell *x_space2 = NULL;
    sv_model *model = NULL;
    int save_t1 = dset->t1;
    int save_t2 = dset->t2;
    PRN *prn = inprn;
    int T, k = 0;
    int err = 0;

    gui_mode = gretl_in_gui_mode();

    if (list == NULL || list[0] < 2) {
	fprintf(stderr, "svm: invalid list argument\n");
	err = E_INVARG;
    } else {
	svm_wrapper_init(&wrap);
	err = read_params_bundle(bparams, bmodel, &wrap, &parm,
				 list, dset, prn);
    }

    if (err) {
	return err;
    }

    if (wrap.quiet) {
	svm_set_print_string_function(gretl_libsvm_noprint);
	prn = NULL;
    } else if (gui_mode) {
	svm_prn = prn;
	svm_set_print_string_function(gretl_libsvm_print);
    }

    if (wrap.t2_train > 0) {
	dset->t2 = wrap.t2_train;
    }
    T = sample_size(dset);

    if (doing_file_io(&wrap)) {
	/* try to ensure we're in workdir */
	gretl_chdir(gretl_workdir());
    }

    err = get_svm_ranges(list, dset, &wrap, prn);

    if (!err) {
	k = (int) gretl_matrix_get(wrap.ranges, 0, 2) - 1;
	pputs(prn, "Allocating problem space... ");
	prob1 = gretl_sv_data_alloc(T, k, &x_space1, &err);
	report_result(err, prn);
    }
    svm_flush(prn);

    if (!err) {
	/* fill out the "problem" data */
	pputs(prn, "Scaling and transcribing data... ");
	sv_data_fill(prob1, x_space1, k, &wrap, list, dset, 1);
	if (parm.svm_type < 0) {
	    parm.svm_type = wrap.auto_type;
	}
	if (parm.gamma == 0) {
	    parm.gamma = 1.0 / k;
	}
	pputs(prn, "OK\n");
    }

    if (!err && wrap.data_outfile != NULL) {
	err = write_problem(prob1, k, wrap.data_outfile);
	report_result(err, prn);
    }

    if (!err) {
	/* we're now in a position to run a check on @parm */
	err = check_svm_params(prob1, &parm, prn);
    }

    if (!err && wrap.loadmod) {
	/* restore previously saved model via bundle */
	pputs(prn, "Loading svm model from bundle... ");
	model = svm_model_from_bundle(bmodel, &err);
	report_result(err, prn);
    } else if (!err && wrap.model_infile != NULL) {
	/* restore previously saved model from file */
	pprintf(prn, "Loading svm model from %s... ", wrap.model_infile);
	model = svm_load_model_wrapper(wrap.model_infile, &err);
	report_result(err, prn);
    } else if (!err) {
	/* cross-validate (?) or build a model by training */
	if (wrap.xvalid > 0) {
	    pprintf(prn, "Calling cross-validation function (this may take a while)\n");
	    svm_flush(prn);
	    err = do_cross_validation(prob1, &parm, wrap.xvalid, prn);
	    /* And then what? Apparently we don't get a model out of this. */
	    goto getout;
	} else {
	    pprintf(prn, "Calling training function (this may take a while)\n");
	    svm_flush(prn);
	    model = svm_train(prob1, &parm);
	}
	if (model == NULL) {
	    err = E_DATA;
	}
	pprintf(prn, "Training done, err = %d\n", err);
	svm_flush(prn);
    }

    if (model != NULL) {
	/* We have a model: should it be saved (to bundle or file)? */
	if (wrap.savemod) {
	    pputs(prn, "Saving svm model to bundle... ");
	    err = svm_model_save_to_bundle(model, bmodel);
	    report_result(err, prn);
	}
	if (wrap.model_outfile != NULL) {
	    pprintf(prn, "Saving svm model as %s... ", wrap.model_outfile);
	    err = svm_save_model_wrapper(wrap.model_outfile, model);
	    report_result(err, prn);
	}
    }

    if (!err && wrap.predict) {
	/* Now do some prediction */
	int training = (wrap.t2_train > 0);
	int T_os = -1;

	real_svm_predict(yhat, prob1, model, training, dset, prn);
	*yhat_written = 1;
	dset->t2 = save_t2;
	if (training && wrap.predict == 2) {
	    T_os = dset->t2 - wrap.t2_train;
	}
	if (T_os >= wrap.t2_train) {
	    /* If we have enough out-of-sample data, go
	       ahead and predict out of sample.
	    */
	    dset->t1 = wrap.t2_train + 1;
	    T = sample_size(dset);
	    pprintf(prn, "Found %d testing observations\n", T);
	    err = check_test_data(dset, wrap.ranges, k);
	    if (!err) {
		prob2 = gretl_sv_data_alloc(T, k, &x_space2, &err);
	    }
	    if (!err) {
		sv_data_fill(prob2, x_space2, k, &wrap, list, dset, 2);
		real_svm_predict(yhat, prob2, model, 0, dset, prn);
	    }
	}
    }

 getout:

    dset->t1 = save_t1;
    dset->t2 = save_t2;

    gretl_sv_data_destroy(prob1, x_space1);
    gretl_sv_data_destroy(prob2, x_space2);
    if (wrap.loadmod) {
	gretl_destroy_svm_model(model);
    } else {
	svm_free_and_destroy_model(&model);
    }
    svm_destroy_param(&parm);
    svm_wrapper_free(&wrap);
    svm_prn = NULL;

    return err;
}