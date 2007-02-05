/*
 *  Copyright (c) 2004 by Allin Cottrell
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
#include "libset.h"
#include "gretl_string_table.h"

typedef struct _col_table col_table;

struct _col_table {
    int idx;
    int n_strs;
    char **strs;
};

struct _gretl_string_table {
    int n_cols;
    col_table **cols;
};

static col_table *col_table_new (int colnum)
{
    col_table *ct = malloc(sizeof *ct);

    if (ct != NULL) {
	ct->strs = NULL;
	ct->n_strs = 0;
	ct->idx = colnum;
    }

    return ct;
}

gretl_string_table *gretl_string_table_new (void)
{
    gretl_string_table *st = malloc(sizeof *st);

    if (st != NULL) {
	st->cols = NULL;
	st->n_cols = 0;
    }

    return st;
}

gretl_string_table *string_table_new_from_cols_list (int *list)
{
    gretl_string_table *st;
    int ncols = list[0];
    int i, j;

    st = malloc(sizeof *st);
    if (st == NULL) return NULL;

    st->cols = malloc(ncols * sizeof *st->cols);
    if (st->cols == NULL) {
	free(st);
	st = NULL;
    } else {
	st->n_cols = ncols;
	for (i=0; i<ncols; i++) {
	    st->cols[i] = col_table_new(list[i+1]);
	    if (st->cols[i] == NULL) {
		for (j=0; j<i; j++) {
		    free(st->cols[j]);
		}
		free(st->cols);
		free(st);
		st = NULL;
	    } 
	}
    }

    return st;
}

static int col_table_get_index (const col_table *ct, const char *s)
{
    int ret = -1;
    int i;

    for (i=0; i<ct->n_strs; i++) {
	if (!strcmp(s, ct->strs[i])) {
	    ret = i + 1;
	    break;
	}
    }

    return ret;
}

static int 
col_table_add_string (col_table *ct, const char *s)
{
    char **strs;
    int n = ct->n_strs + 1;
    int ret = n;

    strs = realloc(ct->strs, n * sizeof *strs);
    if (strs == NULL) {
	ret = -1;
    } else {
	ct->strs = strs;
	strs[n-1] = gretl_strdup(s);

	if (strs[n-1] == NULL) {
	    ret = -1;
	} else {
	    ct->n_strs += 1;
	}
    }

    return ret;
}

static col_table *
gretl_string_table_add_column (gretl_string_table *st, int colnum)
{
    col_table **cols;
    int n = st->n_cols + 1;

    cols = realloc(st->cols, n * sizeof *cols);
    if (cols == NULL) return NULL;

    st->cols = cols;
    cols[n-1] = col_table_new(colnum);
    if (cols[n-1] == NULL) return NULL;

    st->n_cols += 1;

    return cols[n-1];
}

int 
gretl_string_table_index (gretl_string_table *st, const char *s, int col,
			  int addcol, PRN *prn)
{
    col_table *ct = NULL;
    int i, idx = -1;

    if (st == NULL) return idx;

    for (i=0; i<st->n_cols; i++) {
	if ((st->cols[i])->idx == col) {
	    ct = st->cols[i];
	    break;
	}
    }

    if (ct != NULL) {
	/* there's a table for this column already */
	idx = col_table_get_index(ct, s);
    } else if (addcol) {
	/* no table for this column yet: start one now */
	ct = gretl_string_table_add_column(st, col);
	if (ct != NULL) {
	    pprintf(prn, M_("variable %d: translating from strings to code numbers\n"), 
		    col);
	}
    }

    if (idx < 0 && ct != NULL) {
	idx = col_table_add_string(ct, s);
    }

    return idx;
}

static void col_table_destroy (col_table *ct)
{
    int i;

    if (ct == NULL) return;

    for (i=0; i<ct->n_strs; i++) {
	free(ct->strs[i]);
    }
    free(ct->strs);
    free(ct);
}

void gretl_string_table_destroy (gretl_string_table *st)
{
    int i;

    if (st == NULL) return;

    for (i=0; i<st->n_cols; i++) {
	col_table_destroy(st->cols[i]);
    }
    free(st->cols);
    free(st);
}

int gretl_string_table_print (gretl_string_table *st, DATAINFO *pdinfo,
			      const char *fname, PRN *prn)
{
    int i, j;
    const col_table *ct;
    const char *fshort;
    char stname[MAXLEN];
    FILE *fp;
    int err = 0;

    if (st == NULL) return 1;

    strcpy(stname, "string_table.txt");
    gretl_path_prepend(stname, gretl_user_dir());

    fp = gretl_fopen(stname, "w");
    if (fp == NULL) {
	err = E_FOPEN;
	goto bailout;
    }

    fshort = strrchr(fname, SLASH);
    if (fshort != NULL) {
	fprintf(fp, "%s\n\n", fshort + 1);
    } else {
	fprintf(fp, "%s\n\n", fname);
    }

    fputs(M_("One or more non-numeric variables were found.\n"
	     "Gretl cannot handle such variables directly, so they\n"
	     "have been given numeric codes as follows.\n\n"), fp);

    for (i=0; i<st->n_cols; i++) {
	ct = st->cols[i];
	if (!err) {
	    fprintf(fp, M_("String code table for variable %d (%s):\n"), 
		    ct->idx, pdinfo->varname[ct->idx]);
	} else {
	    pprintf(prn, M_("String code table for variable %d (%s):\n"), 
		    ct->idx, pdinfo->varname[ct->idx]);
	}
	for (j=0; j<ct->n_strs; j++) {
	    if (!err) {
		fprintf(fp, "%3d = '%s'\n", j+1, ct->strs[j]);
	    } else {
		pprintf(prn, "%3d = '%s'\n", j+1, ct->strs[j]);
	    }
	}
    }

    if (fp != NULL) {
	pprintf(prn, M_("String code table written to\n %s\n"), stname);
	fclose(fp);
	set_string_table_written();
    }

 bailout:

    gretl_string_table_destroy(st);

    return err;
}

/* below: saving of user-defined strings */

typedef struct saved_string_ saved_string;

struct saved_string_ {
    char name[VNAMELEN];
    char *s;
};

static int n_saved_strings;
static saved_string *saved_strings;

static saved_string built_ins[] = {
    { "gretldir", NULL },
    { "userdir",  NULL },
    { "gnuplot",  NULL },
    { "x12a",     NULL },
    { "x12adir",  NULL },
    { "tramo",    NULL },
    { "tramodir", NULL }
};

void gretl_insert_builtin_string (const char *name, const char *s)
{
    int i, n = sizeof built_ins / sizeof built_ins[0];

    for (i=0; i<n; i++) {
	if (!strcmp(name, built_ins[i].name)) {
	    free(built_ins[i].s);
	    built_ins[i].s = gretl_strdup(s);
	    return;
	}
    }
}

static void gretl_free_builtin_strings (void)
{
    int i, n = sizeof built_ins / sizeof built_ins[0];

    for (i=0; i<n; i++) {
	free(built_ins[i].s);
    }    
}

static saved_string *get_saved_string_by_name (const char *name,
					       int *builtin)
{
    int i;

    if (builtin != NULL) {
	int n = sizeof built_ins / sizeof built_ins[0];

	for (i=0; i<n; i++) {
	    if (!strcmp(name, built_ins[i].name)) {
		*builtin = 1;
		return &built_ins[i];
	    }
	}
    }	

    for (i=0; i<n_saved_strings; i++) {
	if (!strcmp(name, saved_strings[i].name)) {
	    return &saved_strings[i];
	}
    }

    return NULL;
}

static int append_to_saved_string (const char *name, char **s)
{
    saved_string *str;
    char *tmp;
    int n;

    str = get_saved_string_by_name(name, NULL);
    if (str == NULL) {
	return E_UNKVAR;
    }

    if (str->s != NULL) {
	n = strlen(str->s) + strlen(*s) + 1;
    } else {
	n = strlen(*s) + 1;
    }

    tmp = malloc(n);

    if (tmp == NULL) {
	return E_ALLOC;
    }

    if (str->s != NULL) {
	strcpy(tmp, str->s);
	free(str->s);
    } else {
	*tmp = '\0';
    }

    strcat(tmp, *s);
    free(*s);
    *s = NULL;
    str->s = tmp;
    
    return 0;
}

static saved_string *add_named_string (const char *name)
{
    int n = n_saved_strings;
    saved_string *S;

    S = realloc(saved_strings, (n + 1) * sizeof *S);
    if (S == NULL) {
	return NULL;
    }

    strcpy(S[n].name, name);
    S[n].s = NULL;
    saved_strings = S;
    n_saved_strings += 1;

    return &S[n];
}

void saved_strings_cleanup (void)
{
    int i;

    for (i=0; i<n_saved_strings; i++) {
	free(saved_strings[i].s);
    }

    free(saved_strings);
    saved_strings = NULL;
    n_saved_strings = 0;

    gretl_free_builtin_strings();
}

static char *get_string_element (const char **pline, int *err)
{
    const char *line = *pline;
    const char *s;
    char *cpy;
    int closed = 0;
    int n = 0;

    line += strspn(line, " \t");
    if (*line != '"') {
	*err = E_PARSE;
	return NULL;
    }

    line++;
    s = line;
    while (*s) {
	/* allow for escaped quotes */
	if (*s == '"' && *(s-1) != '\\') {
	    closed = 1;
	    break;
	}
	s++;
	n++;
    }

    if (!closed) {
	*err = E_PARSE;
	return NULL;
    }
	
    cpy = gretl_strndup(line, n);
    if (cpy == NULL) {
	*err = E_ALLOC;
    }

    s++; /* eat closing quote */
    *pline = s;

    return cpy;
}

int string_is_defined (const char *sname)
{
    saved_string *str;
    int builtin = 0;
    
    str = get_saved_string_by_name(sname, &builtin);

    return (str != NULL && str->s != NULL);
}

/* for use in "sprintf" command */

int save_named_string (const char *name, const char *s, PRN *prn)
{
    saved_string *str;
    int builtin = 0;

    if (s == NULL) {
	return E_DATA;
    }

    str = get_saved_string_by_name(name, &builtin);
    
    if (str != NULL && builtin) {
	pprintf(prn, "You cannot overwrite '%s'\n", name);
	return E_DATA;
    }

    if (str == NULL) {
	str = add_named_string(name);
	if (str == NULL) {
	    return E_ALLOC;
	}
    }

    if (str->s != NULL) {
	free(str->s);
    }

    str->s = gretl_strdup(s);
    if (str->s == NULL) {
	return E_ALLOC;
    }

    if (gretl_messages_on()) {
	pprintf(prn, "Saved string as '%s'\n", name);
    }

    return 0;
}

/* respond to commands of the forms:

     string <name> = "<s1>" "<s2>" ... "<sn>"
     string <name> += "<s2>" "<s2>"  ... "<sn>"
*/

int process_string_command (const char *line, PRN *prn)
{
    saved_string *str;
    char *s1 = NULL;
    char targ[VNAMELEN];
    int builtin = 0;
    int add = 0;
    int err = 0;

    /* skip "string" plus any following space */
    line += 6;
    line += strspn(line, " \t");

    if (sscanf(line, "%15s", targ) != 1) {
	return E_PARSE;
    }

    /* eat space before operator */
    line += strlen(targ);
    line += strcspn(line, " \t");
    line += strspn(line, " \t");

    if (*line == '\0') {
	/* just a call to echo an existing string? */
	str = get_saved_string_by_name(targ, &builtin);
	if (str == NULL) {
	    return E_UNKVAR;
	} else {
	    pprintf(prn, " %s\n", str->s);
	    return 0;
	}
    }

    /* operator must be '=' or '+=' */
    if (!strncmp(line, "+=", 2)) {
	add = 1;
    } else if (*line != '=') {
	return E_PARSE;
    }

    line += (add)? 2 : 1;

    /* set up the target */
    str = get_saved_string_by_name(targ, &builtin);

    if (str != NULL && builtin) {
	pprintf(prn, "You cannot overwrite '%s'\n", targ);
	return E_DATA;
    }	

    if (str == NULL) {
	if (add) {
	    return E_UNKVAR;
	} else {
	    str = add_named_string(targ);
	    if (str == NULL) {
		return E_ALLOC;
	    }
	}
    } else if (!add) {
	free(str->s);
	str->s = NULL;
    }

    /* add strings(s) to target */
    while (!err && *line != '\0') {
	s1 = get_string_element(&line, &err);
	if (!err) {
	    err = append_to_saved_string(targ, &s1);
	}
    }

    if (err) {
	free(s1);
    } else if (gretl_messages_on()) {
	pprintf(prn, "Saved string as '%s'\n", targ);
    }

    return err;
}

static char *maybe_get_subst (char *name, int *n)
{
    saved_string *str;
    int builtin = 0;
    int k = *n - 1;

    while (k >= 0) {
	str = get_saved_string_by_name(name, &builtin);
	if (str != NULL && str->s != NULL) {
	    *n = k + 1;
	    return str->s;
	}
	name[k--] = '\0';
    }

    return NULL;
}

static void too_long (void)
{
    sprintf(gretl_errmsg, _("Maximum length of command line "
			    "(%d bytes) exceeded\n"), MAXLINE);
}

int substitute_named_strings (char *line)
{
    char sname[VNAMELEN];
    int len = strlen(line);
    char *sub, *tmp, *s = line;
    int n, m, err = 0;

    if (strchr(s, '@') == NULL) {
	return 0;
    }

    while (*s && !err) {
	if (*s == '@') {
	    n = gretl_varchar_spn(s + 1);
	    if (n > 0) {
		if (n >= VNAMELEN) {
		    n = VNAMELEN - 1;
		}
		*sname = '\0';
		strncat(sname, s + 1, n);
		sub = maybe_get_subst(sname, &n);
		if (sub != NULL) {
		    m = strlen(sub);
		    if (len + m >= MAXLINE) {
			too_long();
			err = 1;
			break;
		    } 
		    tmp = gretl_strdup(s + n + 1);
		    if (tmp == NULL) {
			err = E_ALLOC;
		    } else {
			strcpy(s, sub);
			strcpy(s + m, tmp);
			free(tmp);
			len += m - (n + 1);
			s += m - 1;
		    }
		}
	    }
	}
	s++;
    }

    return err;
}
