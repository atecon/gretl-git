/* TRAMO/SEATS, X-12-ARIMA plugin for gretl */

#include "libgretl.h"

#ifdef OS_WIN32
# include <windows.h>
#endif

int write_tramo_data (char *fname, int varnum, 
		      double **Z, const DATAINFO *pdinfo, 
		      const char *tramodir)
{
    int i, t;
    int tsamp = pdinfo->t2 - pdinfo->t1 + 1;
    double x;
    char tmp[9], varname[9], cmd[MAXLEN];
    int startyr, startper;
    FILE *fp = NULL;

    *gretl_errmsg = 0;

    if (!pdinfo->vector[varnum]) {
	sprintf(gretl_errmsg, "%s %s", pdinfo->varname[varnum], 
		_("is a scalar"));
	return 1;
    }

    sprintf(varname, pdinfo->varname[varnum]);
    lower(varname);
    sprintf(fname, "%s%c%s", tramodir, SLASH, varname);

    fp = fopen(fname, "w");
    if (fp == NULL) return 1;

#ifdef ENABLE_NLS
    setlocale(LC_NUMERIC, "C");
#endif   

    x = date(pdinfo->t1, pdinfo->pd, pdinfo->sd0);
    startyr = (int) x;
    sprintf(tmp, "%g", x);
    startper = atoi(strchr(tmp, '.') + 1);

    fprintf(fp, "%s\n", pdinfo->varname[varnum]);
    fprintf(fp, "%d %d %d %d\n", tsamp, startyr, startper, pdinfo->pd);

    for (t=pdinfo->t1; t<=pdinfo->t2; ) {
	for (i=0; i<pdinfo->pd; i++) {
	    if (t == pdinfo->t1) {
		i += startper - 1;
	    }
	    if (na(Z[varnum][t])) {
		fprintf(fp, "-99999 ");
	    } else {
		fprintf(fp, "%g ", Z[varnum][t]);
	    }
	    t++;
	}
	fputs("\n", fp);
    }

    /* FIXME: make these values configurable */
    fprintf(fp, "$INPUT lam=-1,iatip=1,aio=2,va=3.3,noadmiss=1,seats=2,$\n");

#ifdef ENABLE_NLS
    setlocale(LC_NUMERIC, "");
#endif

    if (fp != NULL) {
	fclose(fp);
    }

    /* testing */
#ifdef notdef
    sprintf(cmd, "cd %s && ./tramo -i %s >/dev/null && mv seats.itr serie && ./seats -OF %s", 
	    tramodir, varname, varname);
#endif
#ifdef OS_WIN32 /* FIXME */
    sprintf(cmd, "tramo -i %s", 
	    tramodir, varname);
    WinExec(cmd, SW_SHOWMINIMIZED);
#else
    sprintf(cmd, "cd %s && ./tramo -i %s >/dev/null", 
	    tramodir, varname);
    system(cmd);
#endif

    strcpy(tmp, pdinfo->varname[varnum]);
    lower(tmp);
    sprintf(fname, "%s%coutput%c%s.out", tramodir, SLASH, SLASH, varname); /* .OUT for seats */

    return 0;
}

int write_x12a_data (char *fname, int varnum, 
		     double **Z, const DATAINFO *pdinfo, 
		     const char *x12adir)
{
    int i, t;
    char tmp[8], varname[9], cmd[MAXLEN];
    int startyr, startper;
    double x;
    FILE *fp = NULL;

    *gretl_errmsg = 0;

    if (!pdinfo->vector[varnum]) {
	sprintf(gretl_errmsg, "%s %s", pdinfo->varname[varnum], 
		_("is a scalar"));
	return 1;
    }

    /* make a default x12a.mdl file if it doesn't already exist */
    sprintf(fname, "%s%cx12a.mdl", x12adir, SLASH);
    fp = fopen(fname, "r");
    if (fp == NULL) {
	fp = fopen(fname, "w");
	if (fp == NULL) return 1;
	fprintf(fp, "(0 1 1)(0 1 1) X\n"
		"(0 1 2)(0 1 1) X\n"
		"(2 1 0)(0 1 1) X\n"
		"(0 2 2)(0 1 1) X\n"
		"(2 1 2)(0 1 1)\n");
	fclose(fp);
    } else {
	fclose(fp);
    }

    sprintf(varname, pdinfo->varname[varnum]);
    sprintf(fname, "%s%c%s.spc", x12adir, SLASH, varname);

    fp = fopen(fname, "w");
    if (fp == NULL) return 1;

#ifdef ENABLE_NLS
    setlocale(LC_NUMERIC, "C");
#endif 

    x = date(pdinfo->t1, pdinfo->pd, pdinfo->sd0);
    startyr = (int) x;
    sprintf(tmp, "%g", x);
    startper = atoi(strchr(tmp, '.') + 1);

    fprintf(fp, "series{\n period=%d\n title=\"%s\"\n", pdinfo->pd, varname);
    fprintf(fp, " start=%d.%d\n", startyr, startper);
    fprintf(fp, " data=(\n");

    i = 0;
    for (t=pdinfo->t1; t<=pdinfo->t2; t++) {
	if (na(Z[varnum][t])) {
	    fprintf(fp, "-99999 "); /* FIXME? */
	} else {
	    fprintf(fp, "%g ", Z[varnum][t]);
	}
	if ((i + 1) % 7 == 0) fputs("\n", fp);
	i++;
    }
    fputs(" )\n}\n", fp);


    /* FIXME: make these values configurable */
    fputs("automdl{}\nx11{}\n", fp);

#ifdef ENABLE_NLS
    setlocale(LC_NUMERIC, "");
#endif

    if (fp != NULL) {
	fclose(fp);
    }

    /* FIXME win32 */
    sprintf(cmd, "cd %s && x12a %s -r -p -q >/dev/null", x12adir, varname);
    system(cmd);

    sprintf(fname, "%s%c%s.out", x12adir, SLASH, varname); 

    return 0;
}
