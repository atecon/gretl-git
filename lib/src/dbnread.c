#include "gretl_func.h"
#include "gretl_string_table.h" /* for csvdata */
#include "csvdata.h"

static gretl_bundle *get_dbn_series_bundle (const char *datacode,
					    int *err)
{
    gretl_bundle *b = NULL;
    fncall *fc;

    fc = get_pkg_function_call("dbnomics_get_series", "dbnomics");
    if (fc == NULL) {
	*err = E_DATA;
    } else {
	*err = push_function_arg(fc, NULL, GRETL_TYPE_STRING,
				 (void *) datacode);
	if (!*err) {
	    *err = gretl_function_exec(fc, GRETL_TYPE_BUNDLE, NULL,
				       &b, NULL, NULL);
	}
	if (b != NULL) {
	    int dberr = gretl_bundle_get_int(b, "error", NULL);

	    if (dberr) {
		const char *msg =
		    gretl_bundle_get_string(b, "errmsg", NULL);

		*err = E_DATA;
		gretl_errmsg_set(msg);
		gretl_bundle_destroy(b);
		b = NULL;
	    }
	}
    }

    return b;
}

static int dbn_dset_from_csv (DATASET *dbset,
			      gretl_array *A,
			      gretl_matrix *v)
{
    gchar *fname;
    FILE *fp;
    int T, err = 0;

    fname = g_strdup_printf("%sdnomics_tmp.txt", gretl_dotdir());
    fp = gretl_fopen(fname, "w");

    if (fp == NULL) {
	err = E_FOPEN;
    } else {
	char **S = gretl_array_get_strings(A, &T);
	int t;

	gretl_push_c_numeric_locale();
	fputs("obs dbnomics_data\n", fp);
	for (t=0; t<T; t++) {
	    fprintf(fp, "%s %.12g\n", S[t], v->val[t]);
	}
	gretl_pop_c_numeric_locale();

	fclose(fp);
	err = import_csv(fname, dbset, OPT_NONE, NULL);
	gretl_remove(fname);
    }
    
    g_free(fname);

    return err;
}

static int
get_dbnomics_series_info (const char *id, SERIESINFO *sinfo)
{
    gretl_bundle *b;
    DATASET dbset = {0};
    gretl_array *A;
    gretl_matrix *v;
    int T, err = 0;

    /* FIXME check for required form PROV/DSET/SERIES */

    b = get_dbn_series_bundle(id, &err);
    if (err) {
	fprintf(stderr, "get_dbn_series_bundle: err=%d\n", err);
	goto bailout;
    }

    T = gretl_bundle_get_int(b, "actobs", &err);
    A = gretl_bundle_get_array(b, "periods", &err);
    v = gretl_bundle_get_matrix(b, "vals", &err);
    if (!err && (T <= 0 || A == NULL || v == NULL)) {
	fprintf(stderr, "get_dbnomics_series_info: invalid bundle content\n");
	err = E_DATA;
	goto bailout;
    }

    /* write bundle content as CSV and use CSV reader to
       construct a one-series dataset */
    err = dbn_dset_from_csv(&dbset, A, v);

    if (!err) {
	/* transcribe info to SERIESINFO format */
	const char *s2 = gretl_bundle_get_string(b, "series_name", NULL);
	char *rawname = strrchr(id, '/') + 1;
	gchar *descrip;

	sinfo->t1 = dbset.t1;
	sinfo->t2 = dbset.t2;
	sinfo->nobs = dbset.n;
	sinfo->pd = dbset.pd;
	strcpy(sinfo->stobs, dbset.stobs);
	strcpy(sinfo->endobs, dbset.endobs);
	/* set up name and description */
	normalize_join_colname(sinfo->varname, rawname, 0);
	descrip = g_strdup_printf("%s: %s", id, s2);
	strncat(sinfo->descrip, descrip, MAXLABEL-1);
	g_free(descrip);
	/* steal the data array */
	sinfo->data = dbset.Z[1];
	dbset.Z[1] = NULL;
    }

    clear_datainfo(&dbset, CLEAR_FULL);

 bailout:

    gretl_bundle_destroy(b);

    return err;
}

static int get_dbnomics_data (const char *fname,
			      SERIESINFO *sinfo,
			      double **Z)
{
    memcpy(Z[1], sinfo->data, sinfo->nobs * sizeof(double));
    sinfo->data = NULL;

    return 0;
}
