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
  */

#define FULL_XML_HEADERS

#include "libgretl.h"
#include "gretl_xml.h"
#include "gretl_func.h"
#include "gretl_bundle.h"
#include "usermat.h"
#include "gretl_string_table.h"
#include "libset.h"
#include "uservar.h"

#define UVDEBUG 0

#define LEVEL_AUTO -1
#define LEV_PRIVATE -1

typedef enum {
    UV_PRIVATE = 1 << 0,
    UV_SHELL   = 1 << 1
} UVFlags;

struct user_var_ {
    GretlType type;
    int level;
    UVFlags flags;
    char name[VNAMELEN];
    void *ptr;
};

static user_var **uvars;
static int n_vars;
static int n_alloc;
static int scalar_imin;

static int data_is_bundled (void *ptr, const char *msg);

/* callback for the benefit of the edit scalars window
   in the gretl GUI */

static void (*scalar_edit_callback)(void);

/* callback for adding or deleting icons representing 
   things in the GUI session window */

static USER_VAR_FUNC user_var_callback;

#define UV_CHUNK 32

#define var_is_private(u) ((u->flags & UV_PRIVATE) || *u->name == '$')
#define var_is_shell(u)   (u->flags & UV_SHELL)

static double *na_ptr (void)
{
    double *px = malloc(sizeof *px);

    if (px != NULL) {
	*px = NADBL;
    }

    return px;
}

static user_var *user_var_new (const char *name, int type, void *value)
{
    user_var *u = malloc(sizeof *u);

    if (u != NULL) {
	u->type = type;
	u->level = gretl_function_depth();
	u->flags = 0;
	*u->name = '\0';
	strncat(u->name, name, VNAMELEN - 1);
	u->ptr = NULL;

	if (type == GRETL_TYPE_MATRIX) {
	    gretl_matrix *m = value;

	    if (m == NULL) {
		u->ptr = gretl_null_matrix_new();
	    } else if (matrix_is_saved(m)) {
		fprintf(stderr, "*** user_var_new: got matrix_is_saved\n");
		u->ptr = gretl_matrix_copy(m);
	    } else {
		u->ptr = value;
	    }
	} else if (type == GRETL_TYPE_BUNDLE) {
	    if (value == NULL) {
		u->ptr = gretl_bundle_new();
	    } else {
		u->ptr = value;
	    }
	} else if (type == GRETL_TYPE_STRING) {
	    if (value == NULL) {
		u->ptr = gretl_strdup("");
	    } else {
		u->ptr = value;
	    }
	} else if (type == GRETL_TYPE_LIST) {
	    if (value == NULL) {
		u->ptr = gretl_null_list();
	    } else {
		u->ptr = value;
	    }
	} else if (type == GRETL_TYPE_DOUBLE) {
	    if (value == NULL) {
		u->ptr = na_ptr();
	    } else {
		u->ptr = value;
	    }
	}
    }

    if (u->ptr == NULL) {
	free(u);
	u = NULL;
    }
    
    return u;
}

static void uvar_free_value (user_var *u)
{
    if (u->type == GRETL_TYPE_MATRIX) {
	gretl_matrix_free(u->ptr);
    } else if (u->type == GRETL_TYPE_BUNDLE) {
	gretl_bundle_destroy(u->ptr);
    } else {
	/* scalar, string, list */
	free(u->ptr);
    }
}

static void user_var_destroy (user_var *u)
{
    int free_val = 1;

    if (u->type == GRETL_TYPE_MATRIX) {
	/* At this point only matrices have to be checked in this
	   manner: note that in geneval.c, all other types are
	   unconditionally copied out of bundles, while matrices
	   may be subject to pointer-sharing.
	*/
	if (var_is_shell(u)) {
	    free_val = 0;
	} else if (data_is_bundled(u->ptr, "user_var_destroy")) {
	    free_val = 0;
	}
    }

    if (free_val) {
	 uvar_free_value(u);
    }

    free(u);
}

static int resize_uvar_stack (int n)
{
    int err = 0;

    if (n > n_alloc) {
	int n_new = n_alloc + UV_CHUNK;
	user_var **tmp;

	tmp = realloc(uvars, n_new * sizeof *tmp);
	if (tmp == NULL) {
	    err = E_ALLOC;
	} else {
	    uvars = tmp;
	    n_alloc = n_new;
	}
    }

    return err;
}

static void set_nvars (int n, const char *caller)
{
#if UVDEBUG
    fprintf(stderr, "%s: setting n_vars = %d (was %d)\n", 
	    caller, n, n_vars);
#endif
    n_vars = n;
}

static int bname_is_temp (const char *name)
{
    return !strncmp(name, "btmp___", 7) && isdigit(name[7]);
}

static int real_user_var_add (const char *name, 
			      GretlType type, 
			      void *value, 
			      gretlopt opt)
{
    user_var *u = user_var_new(name, type, value);
    int err = 0;

    /* use OPT_P for a private variable */

#if UVDEBUG
    fprintf(stderr, "user_var_add: '%s'\n", name);
#endif

    if (u == NULL) {
	err = E_ALLOC;
    } else {
	err = resize_uvar_stack(n_vars + 1);
	if (!err) {
	    if (opt & OPT_P) {
		u->flags = UV_PRIVATE;
	    }
	    uvars[n_vars] = u;
	    set_nvars(n_vars + 1, "user_var_add");
	}
    }

    if (user_var_callback != NULL && u->level == 0 &&
	!(opt & OPT_P) && 
	(type == GRETL_TYPE_MATRIX ||
	 type == GRETL_TYPE_BUNDLE) &&
	!(type == GRETL_TYPE_BUNDLE && bname_is_temp(name))) {
	return (*user_var_callback)(name, type, UVAR_ADD);
    }

    /* FIXME GUI scalars callback? */

    return err;
}

int user_var_add (const char *name, GretlType type, void *value)
{
    return real_user_var_add(name, type, value, OPT_NONE);
}

int private_matrix_add (gretl_matrix *M, const char *name)
{
    return real_user_var_add(name, GRETL_TYPE_MATRIX, M, OPT_P);
}

int user_var_delete_by_name (const char *name, PRN *prn)
{
    GretlType type = 0;
    int level = gretl_function_depth();
    user_var *targ = NULL;
    int i, j, k = 0;
    int err = 0;

    for (i=0; i<n_vars; i++) {
	if (uvars[i]->level == level && !strcmp(uvars[i]->name, name)) {
	    targ = uvars[i];
	    k = i;
	    break;
	}
    }

    if (targ == NULL) {
	return E_UNKVAR;
    }

    if (user_var_callback != NULL && level == 0 &&
	!var_is_private(targ) && 
	(targ->type == GRETL_TYPE_MATRIX ||
	 targ->type == GRETL_TYPE_BUNDLE)) {
	/* run this deletion through the GUI program to ensure
	   that things stay in sync 
	*/
	return (*user_var_callback)(name, targ->type,
				    UVAR_DELETE);
    }

    type = targ->type;
    user_var_destroy(targ);
    for (j=k; j<n_vars-1; j++) {
	uvars[j] = uvars[j+1];
    }
    resize_uvar_stack(n_vars - 1);
    set_nvars(n_vars - 1, "user_var_delete_by_name");

    if (prn != NULL && gretl_messages_on()) {
	pprintf(prn, _("Deleted %s"), name);
	pputc(prn, '\n');
    } 
    if (level == 0 && type == GRETL_TYPE_DOUBLE &&
	scalar_edit_callback != NULL) {
	scalar_edit_callback();
    }

    return err;
}

int user_var_delete (user_var *uvar)
{
    int i, j, err = E_UNKVAR;

    for (i=0; i<n_vars; i++) {
	if (uvar == uvars[i]) {
	    user_var_destroy(uvars[i]);
	    for (j=i; j<n_vars-1; j++) {
		uvars[j] = uvars[j+1];
	    }
	    set_nvars(n_vars - 1, "user_var_delete");
	    err = 0;
	    break;
	}
    }

    return err;
}

user_var *get_user_var_by_name (const char *name)
{
    int i, d = gretl_function_depth();

    if (name == NULL || *name == '\0') {
	return NULL;
    }

    for (i=0; i<n_vars; i++) {
	if (uvars[i]->level == d && !strcmp(uvars[i]->name, name)) {
	    return uvars[i];
	}
    }

    return NULL;
}

user_var *get_user_var_of_type_by_name (const char *name,
					GretlType type)
{
    int i, imin = 0, d = gretl_function_depth();

    if (name == NULL || *name == '\0') {
	return NULL;
    }

#if UVDEBUG
    fprintf(stderr, "get_user_var_of_type_by_name: n_vars = %d, level = %d\n",
	    n_vars, d);
    for (i=0; i<n_vars; i++) {
	fprintf(stderr, " %d: '%s' type %d, level %d, ptr %p\n", i, 
		uvars[i]->name, uvars[i]->type, uvars[i]->level,
		uvars[i]->ptr);
    }
#endif

    if (type == GRETL_TYPE_DOUBLE) {
	/* support "auxiliary scalars" mechanism */
	imin = scalar_imin;
    }

    for (i=imin; i<n_vars; i++) {
	if (uvars[i]->level == d && 
	    uvars[i]->type == type &&
	    !strcmp(uvars[i]->name, name)) {
	    return uvars[i];
	}
    }

    return NULL;
}

/* used in kalman.c */

gretl_matrix *get_matrix_by_name_at_level (const char *name, int level)
{
    int i;

    for (i=0; i<n_vars; i++) {
	if (uvars[i]->type == GRETL_TYPE_MATRIX &&
	    uvars[i]->level == level && 
	    strcmp(uvars[i]->name, name) == 0) {
	    return uvars[i]->ptr;
	}
    }

    return NULL;
}

int gretl_is_user_var (const char *name)
{
    return get_user_var_by_name(name) != NULL;
}

int user_var_get_type_by_name (const char *name)
{
    int i, d = gretl_function_depth();

    for (i=0; i<n_vars; i++) {
	if (uvars[i]->level == d && !strcmp(uvars[i]->name, name)) {
	    return uvars[i]->type;
	}
    }

    return GRETL_TYPE_NONE;
}

user_var *get_user_var_by_data (const void *data)
{
    int i, d = gretl_function_depth();

    for (i=0; i<n_vars; i++) {
	if (uvars[i]->level == d && uvars[i]->ptr == data) {
	    return uvars[i];
	}
    }

    return NULL;
}

const char *user_var_get_name (user_var *uvar)
{
    return uvar->name;
}

const char *user_var_get_name_by_data (const void *data)
{
    user_var *u = get_user_var_by_data(data);

    return u == NULL ? NULL : u->name;
}

int user_var_get_level (user_var *uvar)
{
    return (uvar == NULL)? -1 : uvar->level;
}

void *user_var_get_value (user_var *uvar)
{
    return (uvar == NULL)? NULL : uvar->ptr;
}

void *user_var_get_value_by_name (const char *name)
{
    user_var *u = get_user_var_by_name(name);

    return (u == NULL)? NULL : u->ptr;
}

void *user_var_get_value_and_type (const char *name,
				   GretlType *type)
{
    user_var *u = get_user_var_by_name(name);
    void *ret = NULL;

    if (u != NULL) {
	ret = u->ptr;
	*type = u->type;
    } else {
	*type = GRETL_TYPE_NONE;
    }

    return ret;
}

/* special for scalars since user_var_get_value returns
   a pointer */

double user_var_get_scalar_value (user_var *uvar)
{
    if (uvar != NULL && uvar->type == GRETL_TYPE_DOUBLE) {
	return *(double *) uvar->ptr;
    } else {
	return NADBL;
    }
}

int user_var_set_scalar_value (user_var *uvar, double x)
{
    if (uvar != NULL && uvar->type == GRETL_TYPE_DOUBLE) {
	*(double *) uvar->ptr = x;
	return 0;
    } else {
	return E_DATA;
    }
}

int user_var_adjust_level (user_var *uvar, int adj)
{
    if (uvar == NULL) {
	return E_UNKVAR;
    } else {
	uvar->level += adj;
	return 0;
    }
}

int user_var_set_name (user_var *uvar, const char *name)
{
    if (uvar == NULL) {
	return E_UNKVAR;
    } else {
	*uvar->name = '\0';
	strncat(uvar->name, name, VNAMELEN - 1);
	return 0;
    }
}

/**
 * user_var_localize:
 * @origname: name of variable at caller level.
 * @localname: name to be used within function.
 *
 * On entry to a function, renames the named variable (provided 
 * as an argument) and sets its level so that is is accessible
 * within the function.
 * 
 * Returns: 0 on success, non-zero on error.
 */

int user_var_localize (const char *origname,
		       const char *localname,
		       GretlType type)
{
    user_var *u;
    int err = 0;

    if (type == GRETL_TYPE_SCALAR_REF) {
	type = GRETL_TYPE_DOUBLE;
    } else if (type == GRETL_TYPE_MATRIX_REF) {
	type = GRETL_TYPE_MATRIX;
    } else if (type == GRETL_TYPE_BUNDLE_REF) {
	type = GRETL_TYPE_BUNDLE;
    }

    u = get_user_var_of_type_by_name(origname, type);

    if (u == NULL) {
	err = E_DATA;
    } else {
	user_var_set_name(u, localname);
	u->level += 1;
    }

    return err;
}

/**
 * user_var_unlocalize:
 * @localname: name of variable within function.
 * @origname: name used at caller level.
 *
 * On exit from a function, restores the original name and
 * level of a variable which has been made available as an argument.
 * 
 * Returns: 0 on success, non-zero on error.
 */

int user_var_unlocalize (const char *localname,
			 const char *origname,
			 GretlType type)
{
    user_var *u;
    int err = 0;

    if (type == GRETL_TYPE_SCALAR_REF) {
	type = GRETL_TYPE_DOUBLE;
    } else if (type == GRETL_TYPE_MATRIX_REF) {
	type = GRETL_TYPE_MATRIX;
    } else if (type == GRETL_TYPE_BUNDLE_REF) {
	type = GRETL_TYPE_BUNDLE;
    }

    u = get_user_var_of_type_by_name(localname, type);

    if (u == NULL) {
	err = E_DATA;
    } else {
	user_var_set_name(u, origname);
	u->level -= 1;
    }

    return err;
}

static int user_var_count_for_type (GretlType type)
{
    int i, n = 0;

    for (i=0; i<n_vars; i++) {
	if (uvars[i]->type == type) {
	    n++;
	}
    }

    return n;
}

int n_user_matrices (void)
{
    return user_var_count_for_type(GRETL_TYPE_MATRIX);
}

int n_user_scalars (void)
{
    return user_var_count_for_type(GRETL_TYPE_DOUBLE);
}

int n_user_lists (void)
{
    return user_var_count_for_type(GRETL_TYPE_LIST);
}

int n_user_bundles (void)
{
    return user_var_count_for_type(GRETL_TYPE_BUNDLE);
}

/**
 * user_var_replace_value:
 * @uvar: user variable.
 * @value: the new value to place as the value or @uvar.
 *
 * Replaces the value of @uvar; the existing value is
 * freed first.
 *
 * Returns: 0 on success, non-zero on error.
 */

int user_var_replace_value (user_var *uvar, void *value)
{
    if (uvar == NULL) {
	return E_UNKVAR;
    }

    if (value != uvar->ptr) {
	if (uvar->ptr != NULL) {
	    int free_val = 1;

	    if (uvar->type == GRETL_TYPE_MATRIX &&
		data_is_bundled(uvar->ptr, "user_var_replace_value")) {
		free_val = 0;
	    }
	    if (free_val) {
		uvar_free_value(uvar);
	    }
	}	
	uvar->ptr = value;
    }

    return 0;
}

int user_var_add_or_replace (const char *name,
			     GretlType type,
			     void *value)
{
    user_var *u = get_user_var_by_name(name);
    int err = 0;

    if (u != NULL && u->type != type) {
	err = E_TYPES;
    } else if (u != NULL) {
	err = user_var_replace_value(u, value);
    } else {
	err = user_var_add(name, type, value);
    }

    return err;
}

void *user_var_steal_value (user_var *uvar)
{
    void *ret = NULL;

    if (uvar != NULL) {
	ret = uvar->ptr;
	uvar->ptr = NULL;
    }

    return ret;
}

/* FIXME: are both the above and the below necessary? */

void *user_var_unstack_value (user_var *uvar)
{
    void *ret = NULL;
    int i, j;

    for (i=0; i<n_vars; i++) {
	if (uvar == uvars[i]) {
	    ret = uvar->ptr;
	    uvars[i]->ptr = NULL;
	    user_var_destroy(uvars[i]);
	    for (j=i; j<n_vars-1; j++) {
		uvars[j] = uvars[j+1];
	    }
	    set_nvars(n_vars - 1, "user_var_unstack_value");
	    break;
	}
    }

    return ret;
}

int user_matrix_replace_matrix_by_name (const char *name, 
					gretl_matrix *m)
{
    user_var *u = get_user_var_by_name(name);

    if (u != NULL && u->type == GRETL_TYPE_MATRIX) {
	return user_var_replace_value(u, m);
    } else {
	return E_DATA;
    }
}

GList *user_var_names_for_type (GretlType type)
{
    GList *list = NULL;
    int i;

    for (i=0; i<n_vars; i++) {
	if (uvars[i]->type == type) {
	    list = g_list_append(list, (gpointer) uvars[i]->name);
	}
    }

    return list;
}

GList *user_var_list_for_type (GretlType type)
{
    GList *list = NULL;
    int i;

    for (i=0; i<n_vars; i++) {
	if (uvars[i]->type == type) {
	    list = g_list_append(list, (gpointer) uvars[i]);
	}
    }

    return list;
}

/**
 * set_user_var_callback:
 * @callback: function function to put in place.
 *
 * Sets the callback function to be invoked when a user-defined
 * matrix is added to or removed from the stack of saved objects.  
 * Intended for synchronizing the GUI program with the saved object
 * state.
 */

void set_user_var_callback (USER_VAR_FUNC callback)
{
    user_var_callback = callback; 
}

void set_scalar_edit_callback (void (*callback))
{
    scalar_edit_callback = callback; 
}

/* used in response to bare declaration of a user variable */

int create_user_var (const char *name, GretlType type)
{
    return user_var_add(name, type, NULL);
}

/**
 * matrix_add_as_shell:
 * @M: the matrix to add.
 * @name: the name to be given to the "shell".
 *
 * Matrix @M is added to the stack of saved matrices under
 * the name @name with the shell flag set.  This is used
 * when an anonymous matrix is given as a %const argument to a 
 * user-defined function: it is temporarily given user_matrix, 
 * status, so that it is accessible by name within the function,
 * but the content @M is protected from destruction on exit 
 * from the function.
 *
 * Returns: 0 on success, non-zero on error.
 */

int matrix_add_as_shell (gretl_matrix *M, const char *name)
{
    int err = user_var_add(name, GRETL_TYPE_MATRIX, M);

    if (!err) {
	user_var *u = uvars[n_vars-1];

	u->flags |= UV_SHELL;
	u->level += 1;
    }

    return err;
}

/**
 * copy_matrix_as:
 * @m: the original matrix.
 * @newname: the name to be given to the copy.
 * @fnarg: 0 for regular use.
 *
 * A copy of matrix @m is added to the stack of saved matrices
 * under the name @newname.  
 *
 * The @fnarg argument should be non-zero only if this function
 * is used to handle the case where a matrix is given as the argument 
 * to a user-defined function.
 *
 * Returns: 0 on success, non-zero on error.
 */

int copy_matrix_as (const gretl_matrix *m, const char *newname,
		    int fnarg)
{
    gretl_matrix *m2 = gretl_matrix_copy(m);
    int err = 0;

    if (m2 == NULL) {
	err = E_ALLOC;
    } else {
	err = user_var_add(newname, GRETL_TYPE_MATRIX, m2);
	if (!err && fnarg) {
	    uvars[n_vars-1]->level += 1;
	}
    }

    return err;
}

int copy_as_arg (const char *param_name, GretlType type, void *value)
{
    void *copyval = NULL;
    int err = 0;

    if (type == GRETL_TYPE_MATRIX) {
	gretl_matrix *mcpy = gretl_matrix_copy((gretl_matrix *) value);

	if (mcpy == NULL) {
	    err = E_ALLOC;
	} else {
	    copyval = mcpy;
	}
    } else if (type == GRETL_TYPE_LIST) {
	int *lcpy = gretl_list_copy((int *) value);

	if (lcpy == NULL) {
	    err = E_ALLOC;
	} else {
	    copyval = lcpy;
	}	
    } else if (type == GRETL_TYPE_STRING) {
	char *scpy = gretl_strdup((char *) value);

	if (scpy == NULL) {
	    err = E_ALLOC;
	} else {
	    copyval = scpy;
	}
    } else if (type == GRETL_TYPE_DOUBLE) {
	double *px = malloc(sizeof *px);

	if (px == NULL) {
	    err = E_ALLOC;
	} else {
	    *px = *(double *) value;
	    copyval = px;
	}
    } else if (type == GRETL_TYPE_BUNDLE) {
	gretl_bundle *bcpy = gretl_bundle_copy((gretl_bundle *) value,
					       &err);

	if (!err) {
	    copyval = bcpy;
	}
    }	

    if (!err) {
 	err = user_var_add(param_name, type, copyval);
	if (!err) {
	    uvars[n_vars-1]->level += 1;
	}
    }

    return err;
}

int *copy_list_as_arg (const char *param_name, int *list,
		       int *err)
{
    int *ret = NULL;

    *err = copy_as_arg(param_name, GRETL_TYPE_LIST, list);
    if (!*err) {
	ret = uvars[n_vars-1]->ptr;
    }

    return ret;
}

void destroy_user_vars (void)
{
    int i, j;

    for (i=0; i<n_vars; i++) {
	if (uvars[i] == NULL) {
	    break;
	}
	user_var_destroy(uvars[i]);
	for (j=i; j<n_vars-1; j++) {
	    uvars[j] = uvars[j+1];
	}
	uvars[n_vars-1] = NULL;
	i--;
    }

    set_nvars(0, "destroy_user_vars");

    free(uvars);
    uvars = NULL;
    n_alloc = 0;
}

static int uvar_levels_match (user_var *u, int level)
{
    int ret = 0;

    if (u->level == level) {
	ret = 1;
    } else if (level == LEV_PRIVATE && var_is_private(u)) {
	ret = 1;
    }

    return ret;
}

static int real_destroy_user_vars_at_level (int level, int type,
					    int imin)
{
    int i, j, nv = 0;
    int err = 0;

    for (i=imin; i<n_vars; i++) {
	if (uvars[i] == NULL) {
	    break;
	}
	if (type > 0 && uvars[i]->type != type) {
	    nv++;
	    continue;
	}
	if (uvar_levels_match(uvars[i], level)) {
	    user_var_destroy(uvars[i]);
	    for (j=i; j<n_vars-1; j++) {
		uvars[j] = uvars[j+1];
	    }
	    uvars[n_vars-1] = NULL;
	    i--;
	} else {
	    nv++;
	}
    }

    set_nvars(nv, "real_destroy_user_vars_at_level");

    return err;
}

/**
 * destroy_user_vars_at_level:
 * @level: stack level of function execution.
 *
 * Destroys and removes from the stack of user matrices all
 * matrices that were created at the given @level.  This is 
 * part of the cleanup that is performed when a user-defined
 * function terminates.
 *
 * Returns: 0 on success, non-zero on error.
 */

int destroy_user_vars_at_level (int level)
{
    return real_destroy_user_vars_at_level(level, 0, 0);
}

int destroy_private_uvars (void)
{
    return real_destroy_user_vars_at_level(LEV_PRIVATE, 0, 0);    
}

int destroy_private_matrices (void)
{
    return real_destroy_user_vars_at_level(LEV_PRIVATE, 
					   GRETL_TYPE_MATRIX,
					   0);    
}

/**
 * destroy_private_scalars:
 *
 * Gets rid of private or "internal" scalars whose
 * names begin with '$'.
 */

void destroy_private_scalars (void)
{
    real_destroy_user_vars_at_level(LEV_PRIVATE, 
				    GRETL_TYPE_DOUBLE,
				    0);
}

char *temp_name_for_bundle (void)
{
    char tmpname[VNAMELEN];
    int i, nb = 0;

    for (i=0; i<n_vars; i++) {
	if (uvars[i]->type == GRETL_TYPE_BUNDLE) {
	    nb++;
	}
    }

    sprintf(tmpname, "btmp___%d", nb);
    return gretl_strdup(tmpname);
}

static void xml_put_user_matrix (user_var *u, FILE *fp)
{
    gretl_matrix *M;
    const char **S;
    int i, j;

    if (u == NULL || u->ptr == NULL) {
	return;
    }

    M = u->ptr;

    fprintf(fp, "<gretl-matrix name=\"%s\" rows=\"%d\" cols=\"%d\"", 
	    u->name, M->rows, M->cols);

    S = gretl_matrix_get_colnames(M);

    if (S != NULL) {
	fputs(" colnames=\"", fp);
	for (j=0; j<M->cols; j++) {
	    fputs(S[j], fp);
	    fputc((j < M->cols - 1)? ' ' : '"', fp);
	}
    } 

    S = gretl_matrix_get_rownames(M);

    if (S != NULL) {
	fputs(" rownames=\"", fp);
	for (j=0; j<M->rows; j++) {
	    fputs(S[j], fp);
	    fputc((j < M->rows - 1)? ' ' : '"', fp);
	}
    }     

    fputs(">\n", fp);

    for (i=0; i<M->rows; i++) {
	for (j=0; j<M->cols; j++) {
	    fprintf(fp, "%.16g ", gretl_matrix_get(M, i, j));
	}
	fputc('\n', fp);
    }

    fputs("</gretl-matrix>\n", fp); 
}

void write_matrices_to_file (FILE *fp)
{
    int i, nm = 0;

    for (i=0; i<n_vars; i++) {
	if (uvars[i]->type == GRETL_TYPE_MATRIX) {
	    nm++;
	}
    }

    if (nm == 0) {
	return;
    }

    gretl_xml_header(fp);
    fprintf(fp, "<gretl-matrices count=\"%d\">\n", nm);

    gretl_push_c_numeric_locale();

    for (i=0; i<n_vars; i++) {
	if (uvars[i]->type == GRETL_TYPE_MATRIX) {
	    xml_put_user_matrix(uvars[i], fp);
	}
    }

    gretl_pop_c_numeric_locale();

    fputs("</gretl-matrices>\n", fp);
}

/**
 * write_scalars_to_file:
 * @fp: stream to which to write.
 *
 * Prints information on any saved scalars as XML, for use
 * when saving a gretl session.
 */

void write_scalars_to_file (FILE *fp)
{
    double x;
    int i, ns = 0;

    for (i=0; i<n_vars; i++) {
	if (uvars[i]->type == GRETL_TYPE_DOUBLE) {
	    ns++;
	}
    }

    if (ns == 0) {
	return;
    }

    gretl_xml_header(fp);
    fputs("<gretl-scalars>\n", fp);

    gretl_push_c_numeric_locale();

    for (i=0; i<n_vars; i++) {
	if (uvars[i]->type == GRETL_TYPE_DOUBLE) {
	    x = *(double *) uvars[i]->ptr;
	    fprintf(fp, " <gretl-scalar name=\"%s\" value=\"%.15g\"/>\n", 
		    uvars[i]->name, x);
	}
    }

    gretl_pop_c_numeric_locale();

    fputs("</gretl-scalars>\n", fp);
}

/**
 * print_scalars:
 * @prn: pointer to gretl printing struct.
 *
 * Prints names and values of any saved scalars.
 */

void print_scalars (PRN *prn)
{
    double x;
    int level = gretl_function_depth();
    int len, ns = 0, maxlen = 0;
    int i;

    for (i=0; i<n_vars; i++) {
	if (uvars[i]->type == GRETL_TYPE_DOUBLE &&
	    uvars[i]->level == level) {
	    len = strlen(uvars[i]->name);
	    if (len > maxlen) {
		maxlen = len;
	    }
	    ns++;
	}
    }

    if (ns == 0) {
	pprintf(prn, "%s\n", _("none"));
	return;
    }

    pputc(prn, '\n');

    for (i=0; i<n_vars; i++) {
	if (uvars[i]->type == GRETL_TYPE_DOUBLE &&
	    uvars[i]->level == level) {
	    x = *(double *) uvars[i]->ptr;
	    pprintf(prn, " %*s = %.15g\n", maxlen, uvars[i]->name, x);
	}
    }

    pputc(prn, '\n');
}

void print_scalar_by_name (const char *name, PRN *prn)
{
    user_var *u;

    u = get_user_var_of_type_by_name(name, GRETL_TYPE_DOUBLE);

    if (u != NULL) {
	double x = *(double *) u->ptr;

	pprintf(prn, "\n%15s = ", u->name);
	if (na(x)) {
	    pputs(prn, " NA\n");
	} else {
	    pprintf(prn, "% #.8g\n", x);
	}
    }
}

/* "auxiliary scalars": this apparatus is used when we want to do
   "private" NLS estimation (e.g. in ARMA initialization).  It ensures
   that the scalar NLS parameters don't collide with the public scalar
   namespace. FIXME.
*/

void set_auxiliary_scalars (void)
{
    scalar_imin = n_vars;
}

void unset_auxiliary_scalars (void)
{
    real_destroy_user_vars_at_level(0, GRETL_TYPE_DOUBLE, scalar_imin);
    scalar_imin = 0;
}

int gretl_scalar_add (const char *name, double val)
{
    user_var *u;
    int level = gretl_function_depth();
    int err = 0;

    u = get_user_var_by_name(name);

    if (u != NULL) {
	if (u->type == GRETL_TYPE_DOUBLE) {
	    *(double *) u->ptr = val;
	} else {
	    err = E_TYPES;
	}
	return err;
    } else {
	double *px = malloc(sizeof *px);

	if (px == NULL) {
	    err = E_ALLOC;
	} else {
	    *px = val;
	    err = user_var_add(name, GRETL_TYPE_DOUBLE, px);
	}

	if (!err && level == 0 && scalar_edit_callback != NULL) {
	    scalar_edit_callback();
	}	
    }

    return err;
}

void gretl_scalar_set_value (const char *name, double val)
{
    user_var *u;

    u = get_user_var_of_type_by_name(name, GRETL_TYPE_DOUBLE);

    if (u != NULL) {
	*(double *) u->ptr = val;

	if (scalar_edit_callback != NULL) {
	    scalar_edit_callback();
	}
    }
}

double gretl_scalar_get_value (const char *name)
{
    user_var *u;
    double ret = NADBL;

    u = get_user_var_of_type_by_name(name, GRETL_TYPE_DOUBLE);
    
    if (u != NULL) {
	ret = *(double *) u->ptr;
    } else {
	ret = get_const_by_name(name);
    }

    return ret;
}

static int is_scalar_index (int i)
{
    return i >= 0 && i < n_vars && uvars[i]->type == GRETL_TYPE_DOUBLE;
}

double gretl_scalar_get_value_by_index (int i)
{
    if (is_scalar_index(i)) {
	return *(double *) uvars[i]->ptr;
    } else {
	return NADBL;
    }
}

const char *gretl_scalar_get_name (int i)
{
    if (is_scalar_index(i)) {
	return uvars[i]->name;
    } else {
	return NULL;
    }
}

int gretl_scalar_get_level (int i)
{
    if (is_scalar_index(i)) {
	return uvars[i]->level;
    } else {
	return -1;
    }
}

int gretl_scalar_get_index (const char *name, int *err)
{
    int i, level = gretl_function_depth();

    for (i=0; i<n_vars; i++) {
	if (uvars[i]->type == GRETL_TYPE_DOUBLE &&
	    level == uvars[i]->level &&
	    !strcmp(name, uvars[i]->name)) {
	    return i;
	}
    }

    *err = E_UNKVAR;

    return -1;
}

int gretl_is_scalar (const char *name)
{
    int ret = 0;

    if (get_user_var_of_type_by_name(name, GRETL_TYPE_DOUBLE) != NULL) {
	ret = 1;
    }

    if (!ret) {
	ret = const_lookup(name);
    }

    return ret;
}

/**
 * get_string_by_name:
 * @name: the name of the string variable to access.
 *
 * Returns: the value of string variable @name, or %NULL
 * if there is no such variable. Note that this is the
 * actual string value, not a copy thereof.
 */

char *get_string_by_name (const char *name)
{
    user_var *u;

    u = get_user_var_of_type_by_name(name, GRETL_TYPE_STRING);

    if (u != NULL) {
	return (char *) u->ptr;
    } else {
	return get_built_in_string_by_name(name);
    }
}

/**
 * add_string_as:
 * @s: string value to be added.
 * @name: the name of the string variable to add.
 *
 * Adds @s to the saved array of string variables 
 * under the name @name.
 *
 * Returns: 0 on success, non-zero on failure.
 */

int add_string_as (const char *s, const char *name)
{
    char *scpy = gretl_strdup(s);
    int err;

    if (scpy == NULL) {
	err = E_ALLOC;
    } else {
	err = user_var_add(name, GRETL_TYPE_STRING, scpy);
	if (!err) {
	    uvars[n_vars-1]->level += 1;
	}
    }

    return err;
}

/**
 * gretl_is_string:
 * @name: name to test.
 *
 * Returns: 1 if @name is the name of a currently defined
 * string variable, otherwise 0.
 */

int gretl_is_string (const char *name)
{
    if (*name == '@' && *(name + 1) != '@') {
	name++;
    }

    if (get_user_var_of_type_by_name(name, GRETL_TYPE_STRING) != NULL) {
	return 1;
    } else if (get_built_in_string_by_name(name) != NULL) {
	return 1;
    } else {
	return 0;
    }
}

int is_user_string (const char *name)
{
    if (*name == '@' && *(name + 1) != '@') {
	name++;
    }

    if (get_user_var_of_type_by_name(name, GRETL_TYPE_STRING) != NULL) {
	return 1;
    } else {
	return 0;
    }
}

/**
 * data_is_bundled:
 * @ptr: pointer to check.
 *
 * Returns: 1 if @ptr corresponds to an object that is
 * contained within a currently-defined gretl bundle,
 * otherwise 0.
 */

static int data_is_bundled (void *ptr, const char *msg)
{
    int i, ret = 0;

    if (ptr == NULL) {
	return 0;
    }
    
    for (i=0; i<n_vars && !ret; i++) {
	if (uvars[i] != NULL && 
	    uvars[i]->type == GRETL_TYPE_BUNDLE &&
	    uvars[i]->ptr != NULL) {
	    ret = bundle_contains_data(uvars[i]->ptr, ptr);
	}
    }

#if 1
    if (ret) {
	fprintf(stderr, "*** data_is_bundled! (%s) ***\n", msg);
    }
#endif

    return ret;
}

int matrix_is_saved (const gretl_matrix *m)
{
    if (get_user_var_by_data(m) != NULL) {
	return 1;
    } else if (data_is_bundled((void *) m, "matrix_is_saved")) {
	return 1;
    } else {
	return 0;
    }
}

/**
 * write_bundles_to_file:
 *
 * Serializes all saved bundles as XML, writing to @fp.
 */

void write_bundles_to_file (FILE *fp)
{
    int i, nb = 0;

    for (i=0; i<n_vars; i++) {
	if (uvars[i]->type == GRETL_TYPE_BUNDLE) {
	    nb++;
	}
    }

    if (nb == 0) {
	return;
    }

    gretl_xml_header(fp);
    fprintf(fp, "<gretl-bundles count=\"%d\">\n", nb);

    gretl_push_c_numeric_locale();

    for (i=0; i<n_vars; i++) {
	if (uvars[i]->type == GRETL_TYPE_BUNDLE) {
	    xml_put_bundle(uvars[i]->ptr, uvars[i]->name, fp);
	}
    }

    gretl_pop_c_numeric_locale();

    fputs("</gretl-bundles>\n", fp);
}

int max_varno_in_saved_lists (void)
{
    int *list;
    int i, j, vmax = 0;

    for (i=0; i<n_vars; i++) {
	if (uvars[i]->type == GRETL_TYPE_LIST) {
	    list = uvars[i]->ptr;
	    if (list != NULL) {
		for (j=1; j<=list[0]; j++) {
		    if (list[j] > vmax) {
			vmax = list[j];
		    }
		}
	    }
	}
    }    

    return vmax;
}

static int var_is_deleted (const int *dlist, int dmin, int i)
{
    int v = dmin + i - 1;

    if (dlist != NULL) {
	return in_gretl_list(dlist, v);
    } else {
	return (v >= dmin);
    }
}

/**
 * gretl_lists_revise:
 * @dlist: list of variables to be deleted (or NULL).
 * @dmin: lowest ID number of deleted var (referenced only
 * if @dlist is NULL).
 *
 * Goes through any saved lists, adjusting the ID numbers
 * they contain to reflect the deletion from the dataset of
 * certain variables: those referenced in @dlist, if given, 
 * or if @dlist is NULL, those variables with IDs greater 
 * than or equal to @dmin.
 *
 * Returns: 0 on success, non-zero code on failure.
 */

int gretl_lists_revise (const int *dlist, int dmin)
{
    int *list, *maplist;
    int lmax = 0;
    int i, j, k;

    if (dlist != NULL) {
	/* determine lowest deleted ID */
	dmin = dlist[1];
	for (i=2; i<=dlist[0]; i++) {
	    if (dlist[i] > 0 && dlist[i] < dmin) {
		dmin = dlist[i];
	    }
	}
    }

    /* find highest ID ref'd in any saved list */
    for (j=0; j<n_vars; j++) {
	if (uvars[j]->type == GRETL_TYPE_LIST) {
	    list = uvars[j]->ptr;
	    if (list != NULL) {
		for (i=1; i<=list[0]; i++) {
		    if (list[i] > lmax) {
			lmax = list[i];
		    }
		}
	    }
	}
    }

    if (lmax < dmin) {
	/* nothing to be done */
	return 0;
    }

    /* make mapping from old to new IDs */

    maplist = gretl_list_new(lmax - dmin + 1);
    if (maplist == NULL) {
	return E_ALLOC;
    }

    j = dmin;

    for (i=1; i<=maplist[0]; i++) {
	if (var_is_deleted(dlist, dmin, i)) {
	    maplist[i] = -1;
	} else {
	    maplist[i] = j++;
	}
    }

    /* use mapping to revise saved lists */
    for (j=0; j<n_vars; j++) {
	if (uvars[j]->type == GRETL_TYPE_LIST) {
	    list = uvars[j]->ptr;
	    if (list != NULL) {
		for (i=list[0]; i>0; i--) {
		    k = list[i] - dmin + 1;
		    if (k >= 1) {
			if (maplist[k] == -1) {
			    gretl_list_delete_at_pos(list, i);
			} else {
			    list[i] = maplist[k];
			}
		    }
		}
	    }
	}
    }

    free(maplist);

    return 0;
}

/**
 * gretl_lists_cleanup:
 *
 * Frees all resources associated with the internal
 * apparatus for saving and retrieving named lists.
 */

void gretl_lists_cleanup (void)
{
    real_destroy_user_vars_at_level(0,
				    GRETL_TYPE_LIST,
				    0);
}

/**
 * gretl_serialize_lists:
 * @fname: name of file to which output should be written.
 *
 * Prints an XML representation of the current saved lists,
 * if any.
 *
 * Returns: 0 on success, or if there are no saved lists, 
 * non-zero code on error.
 */

int gretl_serialize_lists (const char *fname)
{
    FILE *fp;
    int i, nl = 0;

    for (i=0; i<n_vars; i++) {
	if (uvars[i]->type == GRETL_TYPE_LIST) {
	    nl++;
	}
    }

    if (nl == 0) {
	return 0;
    }

    fp = gretl_fopen(fname, "w");
    if (fp == NULL) {
	return E_FOPEN;
    }

    gretl_xml_header(fp); 

    fprintf(fp, "<gretl-lists count=\"%d\">\n", nl);

    for (i=0; i<n_vars; i++) {
	if (uvars[i]->type == GRETL_TYPE_LIST) {
	    gretl_xml_put_named_list(uvars[i]->name, 
				     uvars[i]->ptr, 
				     fp);
	}
    }

    fputs("</gretl-lists>\n", fp);

    fclose(fp);

    return 0;
}
