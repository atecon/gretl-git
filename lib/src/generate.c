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

/* generate.c for gretl */

#include "libgretl.h"
#include "internal.h"

static int _cstack (double *xstack, const double *xxvec, const char op, 
		    const DATAINFO *pdinfo, GENERATE *genr);
static int _domath (double *xxvec, const double *xmvec, const int nt, 
		    const DATAINFO *pdinfo);
static int _evalexp (char *ss, double *xmvec, double *xxvec, 
		     const double *Z, const DATAINFO *pdinfo, 
		     const MODEL *pmod, GENERATE *genr);
static void _getvar (char *str, char *word, char *c);
static int _getxvec (char *ss, double *xxvec, 
		     const double *Z, const DATAINFO *pdinfo, 
		     const MODEL *pmod, char *msg);
static int _scanb (const char *ss, char *word);
static char _strtype (char *ss, const DATAINFO *pdinfo);
static int _whichtrans (const char *ss);
static int _normal_dist (double *a, const int t1, const int t2); 
static void _uniform (double *a, const int t1, const int t2);
static int _createvar (double *xxvec, char *snew, char *sleft, 
		       char *sright, int nv, int nvtmp, double **pZ, 
		       DATAINFO *pdinfo);
static void _genrfree (double **pZ, DATAINFO *pdinfo, GENERATE *pgenr,
		       double *mystack, double *mvec, const int nv);
static void _lag (const char *ss, const int vi, double *xmvec, double *Z, 
		  const DATAINFO *pdinfo);
static double _genr_cov (const char *str, double **pZ,
			 const DATAINFO *pdinfo);
static double _genr_corr (const char *str, double **pZ,
			  const DATAINFO *pdinfo);
static double _genr_vcv (const char *str, double **pZ, 
			 const DATAINFO *pdinfo, MODEL *pmod);
static void _genr_msg (GENERATE *pgenr, const int nv);
static int _ismatch (const int lv, const int *list);
static void _varerror (const char *ss);
static void _genrtime (DATAINFO *pdinfo, GENERATE *genr, int time);

extern double esl_median (const double *zx, const int n);

enum transformations {
	T_LOG = 1, 
	T_EXP, 
	T_SIN, 
	T_COS, 
	T_DIFF,
	T_LDIFF, 
	T_MEAN, 
	T_SD, 
	T_SORT, 
	T_INT, 
	T_LN, 
	T_COEFF,
	T_ABS, 
	T_RHO, 
	T_SQRT, 
	T_SUM, 
	T_NORMAL, 
	T_UNIFORM, 
	T_STDERR,
	T_CUM, 
	T_MISSING,
	T_MISSZERO,
	T_CORR,
	T_VCV,
	T_VAR,
	T_COV,
	T_MEDIAN,
	T_ZEROMISS
};	

static char *math[] = {
    "log", 
    "exp", 
    "sin", 
    "cos", 
    "diff",
    "ldiff", 
    "mean", 
    "sd", 
    "sort", 
    "int", 
    "ln", 
    "coeff",
    "abs", 
    "rho", 
    "sqrt", 
    "sum", 
    "normal", 
    "uniform", 
    "stderr",
    "cum", 
    "missing",
    "misszero",
    "corr",
    "vcv",
    "var",
    "cov",
    "median",
    "zeromiss",
    NULL
};

static char operators[] = {
    '+', '-', '|',
    '*', '/', '%', '&',
    '^', '<', '>', '=', '!', '@', 0
};

#define LEVELS 7

/* ...................................................... */

static int is_operator (char c)
{
    int i;

    for (i=0; operators[i] != 0; i++) 
	if (c == operators[i]) return 1;
    return 0;
}

/* ...................................................... */

static void catch_not_equals (char *str)
{
    int i, n = strlen(str);

    for (i=1; i<n; i++) {
	if (str[i] == '=' && str[i-1] == '!') {
	    str[i-1] = '@';
	    str[i] = ' ';
	}
    }
}

/* ...................................................... */

static int op_level (int c)
{
    if (c == '^' || c == '!') 
	return 1;
    if (c == '*' || c == '/' || c == '%') 
	return 2;
    if (c == '+' || c == '-') 
	return 3;
    if (c == '>' || c == '<') 
	return 4;
    if (c == '=' || c == '@') /* '@' is internal version of != */
	return 5;
    if (c == '&') 
	return 6;
    if (c == '|') 
	return 7;
    return 0;
}

/* ...................................................... */

static void count_ops (char *s, int opcount[])
{
    while (*s++) 
	opcount[op_level(*s)] += 1;
}

/* ...................................................... */

static int insert_paren (char *s, int pos, char lr)
{
    int i, n = strlen(s);

    if (n + 1 >= MAXLEN) return 1;
    for (i=n+1; i>=pos+1; i--) s[i] = s[i - 1];
    if (lr == 'L') s[pos + 1] = '(';
    else s[pos + 1] = ')';
    return 0;
}

/* ...................................................... */

static int paren_state (char c, int *state, char lr)
{
    if (c == '(') {
	if (lr == 'L') {
	    if (*state > 0) *state -= 1;
	} else *state += 1;
    }
    else if (c == ')') {
	if (lr == 'R') {
	    if (*state > 0) *state -= 1;
	} else *state += 1;
    }
    return *state;
}

/* ...................................................... */

static int parenthesize (char *str)
{
    int i, k, oppos, n = strlen(str);
    int level1 = 0, level2;  
    int priority, start, lpins, inparens;
    int rpar, pbak;
    int opcount[LEVELS + 1];

    for (i=0; i<=LEVELS; i++) opcount[i] = 0;
    count_ops(str, opcount);

    priority = 1;
    k = 0;
    oppos = 0;
    while (priority < LEVELS) {
	if (opcount[priority] == 0) {
	    priority++;
	    continue;
	}
	start = oppos + 1;
	oppos = 0;
	lpins = 0;
	for (i=start; i<n; i++) {
	    if ((level1 = op_level(str[i])) == priority) {
		oppos = i;
		break;
	    }
	}
	if (oppos == 0) break;
	/* work to left of operator... */
	inparens = 0;
	pbak = 0; 
	for (i=oppos; i>=0; i--) {
	    if (str[i] == '(') pbak++; 
	    else if (str[i] == ')') pbak--;
	    paren_state(str[i], &inparens, 'L');
	    if (inparens) continue;
	    level2 = op_level(str[i]);
	    if (level2 > level1) {
		if (!pbak) {
		    if (insert_paren(str, i, 'L')) return 1;
		    n++;
		    lpins = 1;
		    oppos++;
		}
		break;
	    }
	}
	if (lpins == 0) {
	    continue;
	}
	/* ...and to right of operator */
	inparens = 0;
	rpar = 0;
	for (i=oppos; i<n; i++) {
	    paren_state(str[i], &inparens, 'R');
	    if (inparens) continue;
	    level2 = op_level(str[i]);
	    if (str[i] == '(') rpar--;
	    if (str[i] == ')') rpar++;
	    if (level2 > level1 || i == n - 1 || 
		(str[i] == ')' && rpar == 1)) {
		if (insert_paren(str, (i == n - 1)? i: i - 1, 'R'))
		    return 1;
		n++;
		break;
	    }
	}
	k++;
	if (k == opcount[priority]) {
	    k = 0;
	    oppos = 0;
	    priority++;
	}
    }
    return 0;
}

/* ...................................................... */

int _identical (const double *x, const double *y, const int n)
/* check whether two vars are identical or not */
{
    int t;

    for (t=0; t<n; t++) 
	if (floatneq(x[t], y[t])) 
	    return 0;
    return 1;
}

/* ........................................................  */

static void otheruse (const char *str1, const char *str2, char *targ)
{
    sprintf(targ, "'%s' refers to a %s and may not be used as a "
	    "variable name\n", str1, str2); 
}

/* .......................................................... */

static int reserved (const char *str, GENERATE *genr)
{
    static char *resword[] = {"uhat", 
			      "c", "const", "C", "CONST", 
			      "coeff", "stderr", "rho",
			      "mean", "median", "var", "cov", "vcv", "sd",
			      "full", "subdum", 
			      "t", "annual", "qtrs", "months", "hours", "i",
			      "log", "exp", "sin", "cos", "diff", "ldiff", 
			      "sort", "int", "ln", "abs", "sqrt", "cum",
			      ""};
    register int i = 0;

    while (strlen(resword[i])) {
        if (strcmp(str, resword[i]) == 0) {
            switch(i) {
	    case 0: 
		otheruse(str, "residual vector", genr->errmsg);
		break;
	    case 1: case 2: case 3: case 4:
		otheruse(str, "constant", genr->errmsg);
		break;
	    case 5:
		otheruse(str, "regr. coeff.", genr->errmsg);
		break;
	    case 6:
		otheruse(str, "standard error", genr->errmsg);
		break;
	    case 7:
		otheruse(str, "autocorr. coeff.", genr->errmsg);
		break;
	    case 8: case 9: case 10: case 11: case 12: case 13:
		otheruse(str, "stats function", genr->errmsg);
		break;
	    case 14: case 15:
		otheruse(str, "sampling concept", genr->errmsg);
		break;
	    case 16: case 17: case 18: case 19: case 20:
		otheruse(str, "plotting variable", genr->errmsg);
		break;
	    case 21:
		otheruse(str, "internal variable", genr->errmsg);
		break;
	    default:
		otheruse(str, "math function", genr->errmsg);
		break;
            }
            return i+1;
        }
	i++; 
    }  
    return 0;
}

/* .......................................................... */

static void copy (const char *str, const int indx, 
		  const int count, char *dest)
/* copies count chars from indx in str to dest */
{
    int i;

    dest[0] = '\0';
    for (i=0; i<count; ++i) dest[i] = str[indx+i];
    dest[count] = '\0';
}

/* .........................................................    */

static int getword (const char c, char *str, char *word, GENERATE *genr,
		    const int oflag)

/* scans string str for char c, gets word to the left of it as word
   and deletes word from str.
   Returns no of chars deleted, -1 if no
   occurrence, or 0 if reserved word is used */
{
    register int i;

    i = haschar(c, str);
    word[0] = '\0';
    if (i == -1) return -1;
    copy(str, 0, i, word);
    delete(str, 0, i+1);
    /* special case for auto sub-sampling dummy */
    if (oflag && strcmp(word, "subdum") == 0)
	return i+1;
    if (reserved(word, genr)) 
	return 0;
    return i+1;
}

/* ........................................................... */

static void get_genr_formula (char *formula, const char *line)
{
    int i, k = 0, n = strlen(line);

    if (n > MAXLEN - 10) n = MAXLEN - 10;
    /* skip over " genr " */
    while (isspace((unsigned char) line[k])) k++;
    while (isalpha((unsigned char) line[k])) k++;
    while (isspace((unsigned char) line[k])) k++;
    for (i=k; i<n; i++) formula[i-k] = line[i];
    formula[n-k] = '\0';
}

/* ........................................................... */

int genr_scalar_index (int opt, int put)
{
    /* opt = 1, set index (using "put")
       opt = 2, increment index value
       Refers to an "internal" variable named "i",
       available in genr commands, and with ID number 1001
    */
    static int i;

    if (opt == 1) i = put;
    else if (opt == 2) i += put;
    return i;
}

/* ............................................................ */

GENERATE genr_func (double **pZ, DATAINFO *pdinfo, 
		    const char *line, const int model_count, 
		    MODEL *pmod, const int oflag)
{
    int nleft1, nleft2, nright1, nright2, vi, lv, ig, iw, nt; 
    int v, ls, nv = pdinfo->v, er, lword, nv1, nvtmp = 0;
    int t1 = pdinfo->t1, t2 = pdinfo->t2, n = pdinfo->n;
    char *indx1, *indx2, s[MAXLEN], sright[MAXLEN], sleft[MAXLEN];
    char sexpr[MAXLEN], snew[MAXLEN], word[16], s1[MAXLEN];
    char newvar[16], genrs[160];
    char op0, op1, type2;
    register int i;
    double xx, *mystack = NULL, *mvec = NULL;
    GENERATE genr;

    /* mystack cumulates value of expression
       genr.xvec cumulates value of expression inside ()
       mvec gets values for each variable or function
    */

    genr.errcode = 0;
    genr.errmsg[0] = genr.msg[0] = genr.label[0] = '\0';
    if ((genr.xvec = malloc(n * sizeof(double))) == NULL) {
	genr.errcode = E_ALLOC;
	return genr;
    }

    *s = *genrs = *snew = '\0';
    get_genr_formula(s, line);
    delchar('\n', s);
    strcpy(genrs, s); 
    catch_not_equals(s);
    delchar(' ', s);
    genr.special = 0;
 
    if (strcmp(s, "dummy") == 0) {
	if ((genr.errcode = dummy(pZ, pdinfo)) == 0) 
	    strcpy(genr.msg, "Periodic dummy variables generated.\n");
	genr.special = 1;
	_genrfree(pZ, pdinfo, &genr, mystack, mvec, pdinfo->v);
	return genr;
    }
    if (strcmp(s, "paneldum") == 0) {
	if ((genr.errcode = paneldum(pZ, pdinfo, oflag)) == 0)
	    strcpy(genr.msg, "Panel dummy variables generated.\n");
	genr.special = 1;
	_genrfree(pZ, pdinfo, &genr, mystack, mvec, pdinfo->v);
	return genr;
    }
    if (strcmp(s, "index") == 0) {
	_genrtime(pdinfo, &genr, 0);
	_genr_msg(&genr, nv);
	_genrfree(pZ, pdinfo, NULL, mystack, mvec, nv);
	return genr;
    }
    if (strcmp(s, "time") == 0) {
	_genrtime(pdinfo, &genr, 1);
	_genr_msg(&genr, nv);
	_genrfree(pZ, pdinfo, NULL, mystack, mvec, nv);
	return genr;
    }

    *newvar = '\0';
    op0 = '\0';

    if ((mystack = malloc(n * sizeof(double))) == NULL) {
	genr.errcode = E_ALLOC; 
	_genrfree(pZ, pdinfo, &genr, mystack, mvec, nv);
	return genr; 
    } else for (i=0; i<n; i++) mystack[i] = 0;
    if ((mvec = malloc(n * sizeof(double))) == NULL) {
	genr.errcode = E_ALLOC;
	_genrfree(pZ, pdinfo, &genr, mystack, mvec, nv);
	return genr; 
    } else for (i=0; i<n; i++) mvec[i] = 0;
     
    /* get equation newvar = s, where s is expression */
    i = getword('=', s, newvar, &genr, oflag);
    if (i > 0) {
	if (!strlen(newvar)) {
	    genr.errcode = E_NOVAR;
	    _genrfree(pZ, pdinfo, &genr, mystack, mvec, nv);
	    return genr;
	}
	esl_trunc(newvar, 8);
	if (!isalpha((unsigned char) newvar[0])) {
	    genr.errcode = E_NOTALPH;
	    _genrfree(pZ, pdinfo, &genr, mystack, mvec, nv);
	    return genr;
	}
	v = varindex(pdinfo, newvar);
	if (v == 0) { 
	    genr.errcode = E_CONST;
	    _genrfree(pZ, pdinfo, &genr, mystack, mvec, nv);
	    return genr;
	}
	if (haschar('=', s) == strlen(s) - 1) {
	    genr.errcode = E_EQN;
	    _genrfree(pZ, pdinfo, &genr, mystack, mvec, nv);
	    return(genr);
	}
    } else {
	genr.errcode = E_NOEQ;
	_genrfree(pZ, pdinfo, &genr, mystack, mvec, nv);
	return genr;
    }

    /* deal with leading (unary) minus */
    if (s[0] == '-') {
	strcpy(s1, "0");
	strcat(s1, s);
	strcpy(s, s1);
    }

    /* impose operator hierarchy. Needs more testing */
    if (parenthesize(s)) { 
	genr.errcode = E_ALLOC;
	_genrfree(pZ, pdinfo, &genr, mystack, mvec, nv);
	return genr;
    }

    while ((ls = strlen(s)) > 0) {
	for (i=t1; i<=t2; i++) mvec[i] = genr.xvec[i] = 0.;
	indx1 = strrchr(s, '('); /* point to last '('  */
	if (indx1 == NULL) { /* no parenthesis  */
	    indx2 = strchr(s, ')');
	    if (indx2 != NULL) {
		genr.errcode = E_UNBAL;
		_genrfree(pZ, pdinfo, &genr, mystack, mvec, nv);
		return genr;
	    }
            _getvar(s, s1, &op1);

	    if (is_operator(op1) && strlen(s) == 0) {
		    genr.errcode = E_SYNTAX;
		    _genrfree(pZ, pdinfo, &genr, mystack, mvec, nv);
		    return genr;
	    } 
	    else if (op1 == '\0' || is_operator(op1)) {
		er = _getxvec(s1, genr.xvec, *pZ, pdinfo, pmod, genr.errmsg);
		if (er == E_BADSTAT) {
		    genr.errcode = E_BADSTAT;
		    _genrfree(pZ, pdinfo, &genr, mystack, mvec, nv);
		    return genr;
		}
		if (er != 0) {
		    genr.errcode = E_UNKVAR;
		    _genrfree(pZ, pdinfo, &genr, mystack, mvec, nv);
		    return genr;
		}
	    } else {
		genr.errcode = E_BADOP;
		_genrfree(pZ, pdinfo, &genr, mystack, mvec, nv);
		return genr;
            }
#ifdef GENR_DEBUG
	    printf("_getvar: op1 = %d, s = \"%s\"\n", op1, s);
	    printf("genr.xvec[1] = %f\n", genr.xvec[1]);
#endif
            if (_cstack(mystack, genr.xvec, op0, pdinfo, &genr)) {
		genr.errcode = E_UNSPEC; 
		_genrfree(pZ, pdinfo, &genr, mystack, mvec, nv);
		return genr;
	    }
            op0 = op1;
            if (strlen(s) == 0) {
		/* add or replace transformed variable */
                if (v < nv && !oflag && model_count > 0) 
		    sprintf(genr.label, "Replaced after model %d: ", 
			    model_count);
		strcat(genr.label, genrs);
                for (i=t1; i<=t2; i++) genr.xvec[i] = mystack[i];
                strcpy(genr.varname, newvar);
		genr.varnum = v;
		_genr_msg(&genr, nv);
		_genrfree(pZ, pdinfo, NULL, mystack, mvec, nv);
                return genr;
            }
        } else { /* indx1 != NULL */
            nright1 = strlen(indx1);    /* no. of characters to right of ( */
            nleft1 = ls - nright1;      /* no. of characters before ( */
            strncpy (sleft, s, nleft1); /*string to left of (  */
            strcpy(sleft + nleft1, "\0");
            /* calculate equation inside parenthesis */
            strcpy(sright, indx1);         /*string to right of ( */
            indx2 = strchr(sright, ')');  /* point to first ) */
            if (indx2 == NULL) {
                genr.errcode = E_UNBAL;
		_genrfree(pZ, pdinfo, &genr, mystack, mvec, nv);
                return genr;
            }
            nright2 = strlen(indx2);        /* no chars at end of string */
            nleft2 = nright1 - nright2 -1 ; /* no of character inside the (),
					       including */
            indx1++;
            strcpy(sright, indx1);
            strncpy(sexpr, sright, nleft2);   /* sexpr is expr inside ()  */
            strcpy(sexpr + nleft2, "\0");
            iw = _scanb(sleft, word);  /* scan backwards for word in
						front of ( */
            if (iw == 0) {
		/* there is an operator in front of (  */
                nvtmp++;
                if (nvtmp > 20) {
		    genr.errcode = E_NEST;
		    _genrfree(pZ, pdinfo, &genr, mystack, mvec, nv);
                    return genr;
                }
                nv1 = nv + nvtmp;
                ig = _evalexp(sexpr, mvec, genr.xvec, 
			      *pZ, pdinfo, pmod, &genr);
                if (ig != 0) {
		    genr.errcode = E_IGNONZERO;
		    _genrfree(pZ, pdinfo, &genr, mystack, mvec, nv);
		    return genr;
                }
		/* create new temporary variable and var string here */
                strcpy(sright, indx2);
		ig = _createvar(genr.xvec, snew, sleft, sright, 
				nv, nvtmp, pZ, pdinfo);
		if (ig != 0) {
		    genr.errcode = E_UNSPEC;
		    _genrfree(pZ, pdinfo, &genr, mystack, mvec, nv);
		    return genr; 
		}
                strcpy(s, snew);
            } else  {
		/* there is a math fn or lag/lead in form of (  */
                nvtmp++;
                if (nvtmp > 20) {
		    genr.errcode = E_NEST;
		    _genrfree(pZ, pdinfo, &genr, mystack, mvec, nv);
                    return genr;
                }
                nv1 = nv + nvtmp;

                type2 = _strtype(word, pdinfo);

		switch (type2) {

		case 'v':    /* name of variable */
		    if ( !isnumber(sexpr))  {
			genr.errcode = E_NOTINTG;
			_genrfree(pZ, pdinfo, &genr, mystack, mvec, nv);
			return genr;
		    }
		    vi = varindex(pdinfo, word);
		    _lag(sexpr, vi, mvec, *pZ, pdinfo);
		    for (i=t1; i<=t2; i++) genr.xvec[i] = mvec[i];
		    break;

		case 't':    /* "math" label */
		    nt = _whichtrans(word);
		    if (nt == T_RHO) {
			if (!(isnumber(sexpr))) {
			    genr.errcode = E_INVARG;
			    _genrfree(pZ, pdinfo, &genr, mystack, mvec, nv);
			    return genr;
			}
			if (atof(sexpr) == 1 && (pmod->ci == CORC ||
						 pmod->ci == HILU)) {
			    for (i=t1; i<=t2; i++)
				genr.xvec[i] = pmod->rho_in;
			    break;
			}
			if (pmod->ci != AR && atof(sexpr) == 1) {
			    for (i=t1; i<=t2; i++)
				genr.xvec[i] = pmod->rho;
			    break;
			}
			if (pmod->arlist == NULL) {
			    genr.errcode = E_INVARG;
			    _genrfree(pZ, pdinfo, &genr, mystack, mvec, nv);
			    return genr;
			}
			if (!(vi = _ismatch(atoi(sexpr), pmod->arlist))) {
			    genr.errcode = E_INVARG;
			    _genrfree(pZ, pdinfo, &genr, mystack, mvec, nv);
			    return genr;
			}
			for (i=t1; i<=t2; i++) 
			    genr.xvec[i] = pmod->rhot[vi]; 
			break;
		    }
		    if (nt == T_NORMAL) {
			er = _normal_dist(genr.xvec, t1, t2);
			if (er) {
			    genr.errcode = er;
			    _genrfree(pZ, pdinfo, &genr, mystack, mvec, nv);
			    return genr;
			}
			break;
		    }   
		    if (nt == T_UNIFORM) {
			_uniform(genr.xvec, t1, t2);
			break;
		    }
		    if (nt == T_COV) {
			xx = _genr_cov(sexpr, pZ, pdinfo);
			if (na(xx)) {
			    genr.errcode = E_INVARG;
			    _genrfree(pZ, pdinfo, &genr, mystack, mvec, nv);
			    return genr;
			} else 
			    for (i=t1; i<=t2; i++)
				genr.xvec[i] = xx;
			break;
		    }
		    if (nt == T_CORR) {
			xx = _genr_corr(sexpr, pZ, pdinfo);
			if (na(xx)) {
			    genr.errcode = E_INVARG;
			    _genrfree(pZ, pdinfo, &genr, mystack, mvec, nv);
			    return genr;
			} else 
			    for (i=t1; i<=t2; i++)
				genr.xvec[i] = xx;
			break;
		    }
		    if (nt == T_VCV) {
			xx = _genr_vcv(sexpr, pZ, pdinfo, pmod);
			if (na(xx)) {
			    genr.errcode = E_INVARG;
			    _genrfree(pZ, pdinfo, &genr, mystack, mvec, nv);
			    return genr;
			} else 
			    for (i=t1; i<=t2; i++)
				genr.xvec[i] = xx;
			break;
		    }
		    if (nt == T_COEFF || nt == T_STDERR) {
			if (pmod == NULL || pmod->list == NULL) {
			    genr.errcode = E_INVARG;
			    _genrfree(pZ, pdinfo, &genr, mystack, mvec, nv);
			    return genr;
			}
			lv = isnumber(sexpr)? atoi(sexpr) : 
			    varindex(pdinfo, sexpr);
			vi = _ismatch(lv, pmod->list);
			if (vi == 1) vi = 0;
			if (!vi) {
			    genr.errcode = E_INVARG;
			    _genrfree(pZ, pdinfo, &genr, mystack, mvec, nv);
			    return genr;
			}
			if (nt == T_COEFF && pmod->coeff != NULL) 
			    for (i=t1; i<=t2; i++) 
				genr.xvec[i] = pmod->coeff[vi-1];
			else if (pmod->sderr != NULL)
			    for (i=t1; i<=t2; i++) 
				genr.xvec[i] = pmod->sderr[vi-1];
			else {
			    genr.errcode = E_INVARG;
			    _genrfree(pZ, pdinfo, &genr, mystack, mvec, nv);
			    return genr;
			}
			break;
		    } else {
			ig = _evalexp(sexpr, mvec, genr.xvec, 
				      *pZ, pdinfo, pmod, &genr);
			if (ig != 0) {  
			    genr.errcode = E_IGNONZERO;
			    _genrfree(pZ, pdinfo, &genr, mystack, mvec, nv);
			    return genr;
			}
			for (i=t1; i<=t2; i++) mvec[i] = genr.xvec[i];
			er = _domath(genr.xvec, mvec, nt, pdinfo);
			if (er != 0) {
			    genr.errcode = er;
			    _genrfree(pZ, pdinfo, &genr, mystack, mvec, nv);
			    return genr;
			} 
			break;
		    }

		case 'u': genr.errcode = E_CASEU;
		    _genrfree(pZ, pdinfo, &genr, mystack, mvec, nv);
		    return genr;

		default:
		    if (strlen(word) != 0) 
			sprintf(genr.errmsg, 
				"%s is not a var. or function\n", word);
		    genr.errcode = E_UNSPEC;
		    _genrfree(pZ, pdinfo, &genr, mystack, mvec, nv);
		    return genr;

                }  /* end of switch on type2 */

                lword = strlen(word);
                strcpy(sleft+nleft1-lword, "\0");
                strcpy(sright, indx2);
		/* create temp var */
		ig = _createvar(genr.xvec, snew, sleft, sright, 
				nv, nvtmp, pZ, pdinfo);
                strcpy(s, snew);
            } /* end of if (iw == 0) */
        }  /* end of if (indx1=='\0') loop */
    }  /* end of while loop */
    _genrfree(pZ, pdinfo, NULL, mystack, mvec, nv);
    return genr;
}

/* ............................................................ */

static int _cstack (double *xstack, const double *xxvec, const char op, 
		    const DATAINFO *pdinfo, GENERATE *genr)
     /*  calculate stack vector  */
{
    register int i;
    long int ny;
    double xx, yy, *st2;
    int t1 = pdinfo->t1, t2 = pdinfo->t2;

    st2 = malloc(pdinfo->n * sizeof(double));
    if (st2 == NULL) {
	sprintf(genr->errmsg, "Out of memory in genr");
	return E_ALLOC;
    }    
    for (i=t1; i<=t2; i++) st2[i] = xstack[i];

    switch (op) {
    case '\0':
	for (i=t1; i<=t2; i++) xstack[i] = xxvec[i];
	break;
    case '+':
	for (i=t1; i<=t2; i++) xstack[i] = xstack[i] + xxvec[i];
	break;
    case '|':
	for (i=t1; i<=t2; i++) {
	    xstack[i] = xstack[i] + xxvec[i];
	    if (floatneq(xstack[i], 0.)) xstack[i] = 1.0;
	}
	break;
    case '-':
	for (i=t1; i<=t2; i++) xstack[i] = xstack[i] - xxvec[i];
	break;
    case '*':
	for (i=t1; i<=t2; i++) xstack[i] = xstack[i] * xxvec[i];
	break;
    case '&':
	for (i=t1; i<=t2; i++) {
	    xstack[i] = xstack[i] * xxvec[i];
	    if (xstack[i] != 0.) xstack[i] = 1.0;
	}
	break;
    case '%':
	for (i=t1; i<=t2; i++) 
	    xstack[i] = (double) ((int) xstack[i] % (int) xxvec[i]);
	break;
    case '/':
	for (i=t1; i<=t2; i++)  {
	    xx = xxvec[i];
	    if (floateq(xx, 0.0)) {  
		sprintf(genr->errmsg, "Zero denominator for obs %d", i+1);
		free(st2);
		return 1;
	    }
	    xstack[i] = xstack[i]/xx;
	}
	break;
    case '^':
	for (i=t1; i<=t2; i++) {
	    xx = xstack[i];
	    yy = xxvec[i];
	    ny = (long) yy;
	    if ((floateq(xx, 0.0) && yy <= 0.0) || 
		(xx < 0.0 && (double) ny != yy)) {
		sprintf(genr->errmsg, 
			"Invalid power function args for obs. %d"
			"\nbase value = %f, exponent = %f", i, xx, yy);
		free(st2);
		return 1;
	    }
	    if (floateq(xx, 0.0)) xstack[i] = 0.0;
	    else xstack[i] = pow(xx, yy);
	}
	break;
    case '<':
	for (i=t1; i<=t2; i++) 
            if (xstack[i] < xxvec[i]) xstack[i] = 1.0;
            else xstack[i] = 0.0;
	break;
    case '>':
	for (i=t1; i<=t2; i++) 
            if (xstack[i] > xxvec[i]) xstack[i] = 1.0;
            else xstack[i] = 0.0;
	break;
    case '=':
	for (i=t1; i<=t2; i++) 
            if (floateq(xstack[i], xxvec[i])) xstack[i] = 1.0;
            else xstack[i] = 0.0;
	break;
    case '@':
	for (i=t1; i<=t2; i++) 
            if (floateq(xstack[i], xxvec[i])) xstack[i] = 0.0;
            else xstack[i] = 1.0;
	break;
    case '!':
	for (i=t1; i<=t2; i++)
	    if (floatneq(xxvec[i], 0.0)) xstack[i] = 0.0;
	else xstack[i] = 1.0;
	break;
    } /* end of operator switch */

    for (i=t1; i<=t2; i++) 
        if (na(xxvec[i]) || na(st2[i])) xstack[i] = NADBL;

    free(st2);
    return 0;
}

/* ........................................................  */

static int _domath (double *xxvec, const double *xmvec, const int nt,
		    const DATAINFO *pdinfo)
/* do math transformations and return result in xxvec */
{
    register int k, i;
    long int xint; 
    double xx, yy, *x;
    int t1 = pdinfo->t1, t2 = pdinfo->t2;

    /* xmvec contains vector of data to be transformed, result
       returned in xxvec */
    x = calloc(t2-t1+1, sizeof(double)); /* FIXME? */

    switch (nt) {

    case T_LOG:
    case T_LN:
	for (k=t1; k<=t2; k++) {
	    xx = xmvec[k];
	    if (na(xx)) {
		xxvec[k] = NADBL;
		continue;
	    }
	    else if (xx <= 0.0) {
		free(x);
		return E_LOGS;
	    }
	    xxvec[k] = log(xx);
	}
	break;

    case T_EXP:
	for (k=t1; k<=t2; k++) {
	    xx = xmvec[k];
	    if (na(xx)) {
                xxvec[k] = NADBL;
                continue;
	    }
	    else if (xx > HIGHVALU) {
		free(x);
		return E_HIGH;
	    }
	    xxvec[k] = exp(xx);
	}
	break;

    case T_SIN:
	for (k=t1; k<=t2; k++) {
	    xx = xmvec[k];
	    xxvec[k] = (na(xx))? NADBL: sin(xx);
	}
	break;

    case T_COS:
	for (k=t1; k<=t2; k++) {
	    xx = xmvec[k];
	    xxvec[k] = (na(xx))? NADBL: cos(xx);
	}
	break;

    case T_DIFF:
	for (k=t1+1; k<=t2; k++) {
	    xx = xmvec[k];
	    yy = xmvec[k-1];
	    xxvec[k] = (na(xx) || na(yy))? NADBL : xx - yy;
	}
	xxvec[t1] = NADBL;
	break;

    case T_LDIFF:
	for (k=t1+1; k<=t2; k++) {
	    xx = xmvec[k];
	    yy = xmvec[k-1];
	    if (na(xx) || na(yy)) {
		xxvec[k] = NADBL;
		continue;
	    }   
	    else if (xx <= 0.0 || yy <= 0.0) {
		free(x);
		return E_LOGS;
	    }
	    xxvec[k] = log(xx) - log(yy);
	}
	xxvec[t1] = NADBL;
	break;

    case T_MEAN: 
	i = -1;
	for (k=t1; k<=t2; k++) {
	    xx = xmvec[k];
	    if (na(xx)) continue;
	    i++; 
	    x[i] = xx;
	}
	xx = esl_mean(0, i, x); 
	for (k=t1; k<=t2; k++) xxvec[k] = xx;
	break;

    case T_MEDIAN:
	i = -1;
	for (k=t1; k<=t2; k++) {
	    xx = xmvec[k];
	    if (na(xx)) continue;
	    i++;
	    x[i] = xx;
	}
	qsort(x, i+1, sizeof(double), compare_doubles);
	xx = esl_median(x, i+1);
	for (k=t1; k<=t2; k++) xxvec[k] = xx;
	break;

    case T_SD:
	i = -1;
	for (k=t1; k<=t2; k++) {
	    xx = xmvec[k];
	    if (na(xx)) continue;
	    i++;
	    x[i] = xx;
	}
	xx = esl_stddev(0, i, x); 
	for (k=t1; k<=t2; k++) xxvec[k] = xx;
	break;

    case T_VAR:
	i = -1;
	for (k=t1; k<=t2; k++) {
	    xx = xmvec[k];
	    if (na(xx)) continue;
	    i++;
	    x[i] = xx;
	}
	xx = esl_variance(0, i, x); 
	for (k=t1; k<=t2; k++) xxvec[k] = xx;
	break;

    case T_SORT:
	i = -1;
	/*  x[0] = -999999.0; */
	for (k=t1; k<=t2; k++) {
	    xx = xmvec[k];
	    if (na(xx)) continue;
	    i++;
	    x[i] = xx;
	}
	/*  sort(i+1, x); */
	qsort(x, i+1, sizeof(double), compare_doubles);
	for (k=t1; k<=t2; k++) xxvec[k] = x[k-t1];
	break;

    case T_INT:
	for (k=t1; k<=t2; k++) {
	    xint = (int) (xmvec[k] + VSMALL);
	    if (xint == -999) {
		xxvec[k] = NADBL;
		continue;
	    }
	    xxvec[k] = (double) xint;
	}
	break;

    case T_ABS:
	for (k=t1; k<=t2; k++) {
	    xx = xmvec[k];
	    if (na(xx)) {
		xxvec[k] = NADBL;
		continue;
	    }
	    xxvec[k] = (xx<0.0)? -xx : xx;
	}
	break;

    case T_SQRT:
	for (k=t1; k<=t2; k++) {
	    xx = xmvec[k];
	    if (na(xx)) {
		xxvec[k] = NADBL;
		continue;
	    }
	    else if (xx < 0.0) {
		free(x);
		return E_SQRT;
	    }
	    xxvec[k] = sqrt(xx);
	}
	break;

    case T_SUM:
	i = -1;
	for (k=t1; k<=t2; k++) {
	    xx = xmvec[k];
	    if (na(xx)) continue;
	    i++;
	    x[i] = xx;
	}
	xx = esl_mean(0, i, x); 
	i++;
	for (k=t1; k<=t2; k++) xxvec[k] = xx*i;
	break;

    case T_CUM:  /* cumulate, with "cum" function */
	xxvec[t1] = (na(xmvec[t1])) ? 0.0 : xmvec[t1];
	for (k=t1+1; k<=t2; k++) {
	    if (na(xmvec[k])) xxvec[k] = xxvec[k-1];
	    else xxvec[k] = xxvec[k-1] + xmvec[k];
	}
	break;

    case T_MISSING:  /* check whether obs is missing or not */
	for (k=t1; k<=t2; k++) {
	    xxvec[k] = (na(xmvec[k])) ? 1.0 : 0.0;
	}
	break;

    case T_MISSZERO:  /* change missing obs to zero */
	for (k=t1; k<=t2; k++) {
	    xxvec[k] = (na(xmvec[k])) ? 0.0 : xmvec[k];
	}
	break;

    case T_ZEROMISS:  /* change zero to missing obs */
	for (k=t1; k<=t2; k++) {
	    xxvec[k] = (floateq(xmvec[k], 0.0)) ? NADBL : xmvec[k];
	}
	break;

    }
    free(x);
    return 0;
}

/* .....................................................*/

static int _evalexp (char *ss, double *xmvec, double *xxvec, 
		     const double *Z, const DATAINFO *pdinfo, 
		     const MODEL *pmod, GENERATE *genr)
{
    char s3[MAXLEN], op2, op3;
    int ig;

    /* evaluate expression inside parentheses and value in xxvec */
    op3 = '\0';
    do {
	_getvar(ss, s3, &op2);
	if (op2 == '\0' || is_operator(op2)) {
	    ig = _getxvec(s3, xmvec, Z, pdinfo, pmod, genr->errmsg);
	    if (ig != 0) return ig;
	    _cstack(xxvec, xmvec, op3, pdinfo, genr);
	    op3 = op2;
        }
    } while (strlen(ss) > 0);
    return 0;
}

/* ........................................................ */

static void _getvar (char *str, char *word, char *c)
     /*   scans string str for first occurrence of {}()+-*^/
     copies the character into c
     copies string to the left into word. deletes word from s
     if no occurrence, and sets word = str, str = '\0', and c = '\0'.
*/
{
    register int i;

    *word = '\0';
    for (i=0;  i < strlen(str); i++)  { 
	if (str[i] == '{' || str[i] == '}' || str[i] == '(' ||
	    str[i] == ')' || is_operator(str[i])) {
	    *c = str[i];
	    copy(str, 0, i, word);
	    delete(str, 0, i+1);
	    return;
	}
    }
    strcpy(word, str);
    *str = '\0';
    *c = '\0';
    return;
}

/* ...........................................................*/

static int check_modelstat (const MODEL *pmod, char *msg, int type1)
{
    if (pmod == NULL || pmod->list == NULL) {
	switch (type1) {
	case 'e':
	    strcpy(msg, "No $ess (error sum of squares) value is available.");
	    return 1;
	    break;
	case 'r':
	    strcpy(msg, "No $rsq (R-squared) value is available.");
	    return 1;
	    break;
	case 'q':
	    strcpy(msg, "No $trsq (T*R-squared) value is available.");
	    return 1;
	    break;
	case 'd':
	    strcpy(msg, "No $df (degrees of freedom) value is available.");
	    return 1;
	    break;
	case 's':
	    strcpy(msg, "No $sigma (std. err. of model) value is available.");
	    return 1;
	    break;
	case 'l':
	    strcpy(msg, "No $lnl (log-likelihood) value is available.");
	    return 1;
	    break;
	default:
	    return 0;
	    break;
	}
    }
    if (pmod != NULL && pmod->ci != LOGIT && pmod->ci != PROBIT &&
	type1 == 'l') {
	strcpy(msg, "$lnl (log-likelihood) is not available for the last "
	       "model.");
	return 1;
    }
    return 0;
}

/* ...........................................................*/

static int _getxvec (char *ss, double *xxvec, 
		     const double *Z, const DATAINFO *pdinfo, 
		     const MODEL *pmod, char *msg)
     /* calculate and return the xxvec vector of values */
{
    char type1;
    int v1, n = pdinfo->n;
    register int i;
    double value;

    type1 = _strtype(ss, pdinfo);

    if (check_modelstat(pmod, msg, type1)) return 1;
    if (pmod && (pmod->ci == LOGIT || pmod->ci == PROBIT) &&
	(type1 == 'r' || type1 == 'e' || type1 == 's' || type1 == 'q')) 
	return E_BADSTAT;

    switch (type1) {  

    case 'e':
	for (i=0; i<n; i++) xxvec[i] = pmod->ess;
	break;

    case 'o':
	for (i=0; i<n; i++) {
	    if (pmod->list) xxvec[i] = (double) pmod->nobs;
	    else xxvec[i] = (double) (pdinfo->t2 - pdinfo->t1 + 1);
	}
	break;

    case 'r':
	for (i=0; i<n; i++) xxvec[i] = pmod->rsq;
	break;

    case 'l':
	for (i=0; i<n; i++) xxvec[i] = pmod->lnL;
	break;

    case 's':
	if (pmod->nwt) 
	    for (i=0; i<n; i++) xxvec[i] = pmod->sigma_wt;
	else 
	    for (i=0; i<n; i++) xxvec[i] = pmod->sigma;
	break;

    case 'q':
	for (i=0; i<n; i++) xxvec[i] = pmod->nobs * pmod->rsq;
	break;

    case 'd':
	for (i=0; i<n; i++) xxvec[i] = (double) pmod->dfd;
	break;

    case 'n':
	value = atof(ss);
	for (i=0; i<n; i++) xxvec[i] = value; 
	break;

    case 'v':
	v1 = varindex(pdinfo, ss);
	if (v1 == UHATNUM) {
	    if (pmod->uhat == NULL) return 1;
	    if (pmod->t2 - pmod->t1 + 1 > n ||
		model_sample_issue(pmod, NULL, Z, pdinfo)) {
		strcpy(msg, "Can't retrieve uhat: data set has changed");
		return 1;
	    }	    
	    for (i=0; i<pmod->t1; i++) xxvec[i] = NADBL;
	    for (i=pmod->t1; i<=pmod->t2; i++) xxvec[i] = pmod->uhat[i]; 
	    for (i=pmod->t2 + 1; i<n; i++) xxvec[i] = NADBL;
	}
	else if (v1 == INDEXNUM) {
	    int k = genr_scalar_index(0, 0);

	    for (i=0; i<n; i++) xxvec[i] = (double) k;
	}
	else if (v1 == TNUM) { /* auto trend/index */
	    if (pdinfo->time_series && pdinfo->pd == 1) /* annual */ 
		for (i=0; i<n; i++) xxvec[i] = pdinfo->sd0 + i;
	    else if (pdinfo->time_series == 1 && 
		     (pdinfo->pd == 4 || pdinfo->pd == 12)) {
		char obsstr[8];
		
		for (i=0; i<n; i++) {
		    ntodate(obsstr, i, pdinfo);
		    xxvec[i] = atof(obsstr);
		}
	    } else
		for (i=0; i<n; i++) xxvec[i] = (double) (i + 1);
	} else 
	    for (i=0; i<n; i++) xxvec[i] = Z(v1, i);
	break;

    case 'u':  return 1;

    default:
	if (strlen(ss) != 0) {
	    sprintf(msg, "Undefined variable name '%s' in genr", ss);
	    return 1;
	}
	break;
    } 
    return 0;
}

/* ..................................................................*/

static void _lag (const char *ss, const int vi, double *xmvec, double *Z, 
		  const DATAINFO *pdinfo)
     /*  calculate lags and leads of variable  */
{
    register int i;
    int lg;
    int t1 = pdinfo->t1, t2 = pdinfo->t2, n = pdinfo->n;

    lg = atoi(ss);
    if (lg > 0) {
        for (i=t1; i<=t2-lg; i++) xmvec[i] = Z(vi, i+lg);
        for (i=1+t2-lg; i<=t2; i++) xmvec[i] = NADBL;
    }
    if (lg < 0)  {
        lg = -lg;
        for (i=t1+lg; i<=t2; i++) xmvec[i] = Z(vi, i-lg);
        for (i=t1; i<=t1+lg-1; i++) xmvec[i] = NADBL;
    }
}

/* ......................................................  */

static int _scanb (const char *ss, char *word)
     /*  scan string right to left for + - * / ^ ( 
    ss is string, n is no. of chars in string, return word to
    left of operator 
*/
{
    register int i;
    int n = strlen(ss);

    *word = '\0';
    i = n - 1;
    if (ss[i] == '(' || ss[i] == '\0' || is_operator(ss[i])) {
	word[0] = ss[n-1];
	word[1] = '\0';
	return 0;
    }
    for (i=n-1; i>=0; i--) {
	if (ss[i] == '(' || ss[i] == '\0' || is_operator(ss[i])) {
	    strcpy(word, ss+i+1);
	    return 1;
	}
    }
    if (i == -1) {
	strcpy(word, ss);
	if (ss[0] == '\0') return 0;
	else
	    return 1;
    }
    return 0;
}

/* ......................................................   */

static char _strtype (char *ss, const DATAINFO *pdinfo)
/*  checks whether ss is a number, variable name or transformation  
    returns 'n' for no., 'v' for var, 't' for trans and '0' for none
*/
{
    int i;

    if (ss[0] == '$') {
        lower(ss);
        if (strcmp(ss, "$ess") == 0)  
	    return 'e';
        if (strcmp(ss, "$nobs") == 0) 
	    return 'o';
        if (strcmp(ss, "$rsq") == 0)  
	    return 'r';
	if (strcmp(ss, "$sigma") == 0)  
	    return 's';
        if (strcmp(ss, "$df") == 0)   
	    return 'd';
        if (strcmp(ss, "$lnl") == 0)   
	    return 'l';
        if (strcmp(ss, "$nrsq") == 0 || strcmp(ss, "$trsq") == 0) 
	    return 'q';
    }

    if (isnumber(ss)) {
        i = strlen(ss) - 1;
        if (ss[i] == 'e') { /* FIXME puts() */
            puts("Scientific notation not allowed.  Use floating point");
            return 'u';
        }
        else return 'n';
    }

    for (i=0; ; i++)  {
	if (math[i] == NULL) break;
        if (strcmp(ss, math[i]) == 0) return 't';
    }

    i = varindex(pdinfo, ss);
    if (i < pdinfo->v || i == UHATNUM || i == TNUM ||
	i == INDEXNUM) return 'v'; 

    return '\0';
}

/* ........................................................  */

static int _whichtrans (const char *ss)
{
    register int i;

    for (i=0; ; i++) {
	if (math[i] == NULL) break;
        if (strcmp(ss, math[i]) == 0) return i+1;
    }
    return 0;
}

/* ......................................................  */

int dummy (double **pZ, DATAINFO *pdinfo)
/* creates periodic (usually seasonal) dummies */
{
    static char word[16];
    int vi, t, yy, pp, mm;
    int n = pdinfo->n, nvar = pdinfo->v;
    int ndummies = pdinfo->pd;
    double xx;

    if (ndummies == 1) return E_PDWRONG;
    if (grow_Z(ndummies, pZ, pdinfo)) return E_ALLOC;

    mm = (pdinfo->pd < 10)? 10 : 100;
    for (vi=1; vi<=ndummies; vi++) {
        sprintf(word, "dummy_%d", vi);
	strcpy(pdinfo->varname[nvar+vi-1], word);
	sprintf(pdinfo->label[nvar+vi-1], "%s = 1 if period is %d, "
		"0 otherwise", word, vi);
        for (t=0; t<pdinfo->n; t++) {
            xx = date(t, pdinfo->pd, pdinfo->sd0);
            yy = (int) xx;
            pp = (int) (mm * (xx - yy) + 0.5);
            (*pZ)[n*(nvar+vi-1) + t] = (pp == vi)? 1.0 : 0.0;
        }
    }
    return 0;
}

/* ......................................................  */

int paneldum (double **pZ, DATAINFO *pdinfo, int opt)
/* creates panel data dummies (unit and period) 
   opt = 0 for stacked time-series, 1 for stacked cross-section
*/
{
    static char word[16];
    int vi, t, yy, pp, mm;
    int n = pdinfo->n, nvar = pdinfo->v;
    int ndum, nudum, ntdum;
    double xx;

    ntdum = pdinfo->pd;
    if (ntdum == 1) return E_PDWRONG;
    nudum = pdinfo->n / pdinfo->pd;
    if (nudum == 1) return E_PDWRONG;

    ndum = ntdum + nudum;
    if (grow_Z(ndum, pZ, pdinfo)) return E_ALLOC;

    /* first generate the frequency-based dummies */
    mm = (pdinfo->pd < 10)? 10 : 100;
    for (vi=1; vi<=ntdum; vi++) {
	if (opt) sprintf(word, "du_%d", vi);
        else sprintf(word, "dt_%d", vi);
	strcpy(pdinfo->varname[nvar+vi-1], word);
	sprintf(pdinfo->label[nvar+vi-1], "%s = 1 if %s is %d, "
		"0 otherwise", word, (opt)? "unit": "period", vi);
        for (t=0; t<pdinfo->n; t++) {
            xx = date(t, pdinfo->pd, pdinfo->sd0);
            yy = (int) xx;
            pp = (int) (mm*(xx - yy) + 0.5);
            (*pZ)[n*(nvar+vi-1) + t] = (pp == vi)? 1.0 : 0.0;
        }
    }

    /* and then the block-based ones */
    for (vi=1; vi<=nudum; vi++) {
	if (opt) sprintf(word, "dt_%d", vi);
        else sprintf(word, "du_%d", vi);
	strcpy(pdinfo->varname[nvar+ntdum+vi-1], word);
	sprintf(pdinfo->label[nvar+ntdum+vi-1], "%s = 1 if %s is %d, "
		"0 otherwise", word, (opt)? "period": "unit", vi);
        for (t=0; t<pdinfo->n; t++) 
	    (*pZ)[n*(nvar+ntdum+vi-1) + t] = 0.0;
	for (t=(vi-1)*pdinfo->pd; t<vi*pdinfo->pd; t++) 
	    (*pZ)[n*(nvar+ntdum+vi-1) + t] = 1.0;
    }
    return 0;
}

/* ........................................................  */

static void _genrtime (DATAINFO *pdinfo, GENERATE *genr, int time)
/* create time trend variable */
{
    int t, n = pdinfo->n, v = pdinfo->v;

    if (time) t = varindex(pdinfo, "time");
    else t = varindex(pdinfo, "index");
    if (t < v) {
	genr->errcode = E_VAREXISTS;
        return;
    }
    if (time) {
	strcpy(genr->varname, "time");
	strcpy(genr->label,"time trend variable");
    } else {
	strcpy(genr->varname, "index");
	strcpy(genr->label,"data index variable");
    }
    genr->varnum = v;
    for (t=0; t<n; t++) genr->xvec[t] = (double) (t + 1);
    return;
}

/* ........................................................  */

int plotvar (double **pZ, DATAINFO *pdinfo, const char *period)
{
    int t, vi, y1, n = pdinfo->n, v = pdinfo->v;
    float rm;

    if ((vi = varindex(pdinfo, period)) < v) return 0;
    if (grow_Z(1, pZ, pdinfo)) return E_ALLOC;
    strcpy(pdinfo->varname[vi], period);

    y1 = (int) pdinfo->sd0;
    rm = pdinfo->sd0 - y1;

    switch(period[0]) {
    case 'a':
	strcpy(pdinfo->label[vi], "annual plotting variable"); 
	for (t=0; t<n; t++) 
	    (*pZ)[n*vi + t] = (double) (t + atoi(pdinfo->stobs));
	break;
    case 'q':
	strcpy(pdinfo->label[vi], "quarterly plotting variable");
	(*pZ)[n*vi + 0] = y1 + (10.0 * rm - 1.0)/4.0;
	for (t=1; t<n; t++) 
	    (*pZ)[n*vi + t] = (*pZ)[n*vi + t-1] + .25;
	break;
    case 'm':
	strcpy(pdinfo->label[vi], "monthly plotting variable");
	(*pZ)[n*vi + 0] = y1 + (100.0 * rm - 1.0)/12.0;
	for (t=1; t<n; t++) 
	    (*pZ)[n*vi + t] = (*pZ)[n*vi + t-1] + (1.0/12.0);
	break;
    case 'h':
	strcpy(pdinfo->label[vi], "hourly plotting variable");
	(*pZ)[n*vi + 0] = y1 + (100.0 * rm - 1.0)/24.0;
	for (t=1; t<n; t++) 
	    (*pZ)[n*vi + t] = (*pZ)[n*vi + t-1] + (1.0/24.0);
	break; 
    case 'i':
	strcpy(pdinfo->label[vi], "index variable");
	for (t=0; t<n; t++) (*pZ)[n*vi + t] = (double) (t + 1);
	break;
    case 't':
	strcpy(pdinfo->label[vi], "time trend variable");
	for (t=0; t<n; t++) (*pZ)[n*vi + t] = (double) (t + 1);
	break;
    default:
	break;
    }
    return 0;
}

/* ......................................................  */

int laggenr (const int iv, const int lag, const int opt, double **pZ, 
	     DATAINFO *pdinfo)
/*
    creates Z[iv][t-lagval] and prints label if opt != 0.
    aborts if a variable of the same name already exists
*/
{
    char word[32];
    char s[32];
    int t, t1, n = pdinfo->n, v = pdinfo->v;

    strcpy(s, pdinfo->varname[iv]);
    if (pdinfo->pd >=10) esl_trunc(s, 5);
    else esl_trunc(s, 6);
    sprintf(word, "_%d", lag);
    strcat(s, word);

    /* "s" should now contain the new variable name --
     check whether it already exists: if so, get out */
    if (varindex(pdinfo, s) < v) return 0;

    if (grow_Z(1, pZ, pdinfo)) return E_ALLOC;

    for (t=0; t<n; t++) (*pZ)[n*v + t] = NADBL;
    for (t=0; t<lag; t++) (*pZ)[n*v + t] = NADBL;
    t1 = (lag > pdinfo->t1)? lag : pdinfo->t1;
    for (t=t1; t<=pdinfo->t2; t++) {
        (*pZ)[n*v + t] = (*pZ)[iv*n + t-lag];
    }
    strcpy(pdinfo->varname[v], s);
    sprintf(pdinfo->label[v], "%s = %s(-%d)", s, 
	    pdinfo->varname[iv], lag);

    return 0;
}

/* ........................................................  */

static int _normal_dist (double *a, const int t1, const int t2) 
     /* Box and Muller method */
{
    int i;
    double xx, yy, scale = 1.0/RAND_MAX;

    for (i=t1; i<=t2; i++) {
	xx = (double) rand() * scale;
	yy = (double) rand() * scale;
	a[i] = sqrt(-2. * log(xx)) * cos(2. * M_PI * yy);
    }
    return 0;
}

/* ........................................................  */

static void _uniform (double *a, const int t1, const int t2) 
{
    int i;
    double scale = 100.0/RAND_MAX;

    for (i=t1; i<=t2; i++) 
	a[i] = (double) rand(); 
    for (i=t1; i<=t2; i++) 
	a[i] *= scale; 
}

/* .......................................................... */

void varlist (const DATAINFO *pdinfo, print_t *prn)
{
    register int i;
    int n = pdinfo->v;

    pprintf(prn, "Listing %d variables:\n", n);
    for (i=0; i<n; i++) {
	pprintf(prn, "%3d) %-10s", i, pdinfo->varname[i]);
	if ((i+1) % 5 == 0) 
	    pprintf(prn, "\n");
    }
    if (n%5) pprintf(prn, "\n");
}

/* .......................................................... */

int varindex (const DATAINFO *pdinfo, const char *varname)
/*  checks name of variable against list in datainfo.varname.
    Returns index number of variable or n + 1 if not in list. 
*/
{
    int i;

    if (strcmp(varname, "uhat") == 0) return UHATNUM; 
    if (strcmp(varname, "t") == 0) return TNUM;
    if (strcmp(varname, "i") == 0) return INDEXNUM;
    if (strcmp(varname, "const") == 0 || strcmp(varname, "CONST") == 0)
        return 0;

    for (i=0; i<pdinfo->v; i++) 
        if (strcmp(pdinfo->varname[i], varname) == 0)  
	    return i;

    return pdinfo->v;
}

/* ........................................................ */

static int _createvar (double *xxvec, char *snew, char *sleft, 
		       char *sright, int nv, int nvtmp, double **pZ, 
		       DATAINFO *pdinfo)
{
    static char ss[10];
    int mv, t1 = pdinfo->t1, t2 = pdinfo->t2, n = pdinfo->n;
    register int t;

    sprintf(ss, "q#$%d", nv + nvtmp); 
    mv = varindex(pdinfo, ss);

    if (grow_Z(1, pZ, pdinfo)) return E_ALLOC;

    strcpy(pdinfo->varname[mv], ss);
    for (t=t1; t<=t2; t++) (*pZ)[n*mv + t] = xxvec[t];
    /* return a new string with the temporary variable name in
       place of the calculated expression */
    strcpy(snew, sleft);
    strcat(snew, ss);
    strcat(snew, sright);

    return 0;
}

/* ........................................................ */

static void _genrfree (double **pZ, DATAINFO *pdinfo, GENERATE *genr,
		       double *mystack, double *mvec, const int nv)
{
    int s = pdinfo->v - nv;

    if (s > 0) shrink_Z(s, pZ, pdinfo);
    if (mystack != NULL) free(mystack);
    if (mvec != NULL) free(mvec);
    if (genr != NULL) free(genr->xvec);
    
}

/* ...................................................... */

int logs (const int *list, double **pZ, DATAINFO *pdinfo, char *msg)
/* generates logarithms for each var in list 
   returns number of vars generated, or -1 on failure */
{
    register int i;
    int j, t, v, nvar = pdinfo->v, n = pdinfo->n;
    int check, le_zero;
    int l0 = list[0];
    double xx;
    char s[32];

    if (grow_Z(l0, pZ, pdinfo)) return -1;

    j = 0;
    for (i=1; i<=list[0]; i++) {
	v = list[i];
	if (v == 0) continue; /* dont try to take log of constant */
	/* and don't try to take the log of a dummy variable */
	if (isdummy(v, pdinfo->t1, pdinfo->t2, *pZ, pdinfo->n))
	    continue;
	if (v < nvar)  { 
	    le_zero = 0;
	    for (t=0; t<n; t++) (*pZ)[n*(nvar+j) + t] = NADBL;
	    for (t=pdinfo->t1; t<=pdinfo->t2; t++) {
		xx = (*pZ)[n*v + t];
		if (xx <= 0.0) {
		    (*pZ)[n*(nvar+j) + t] = NADBL;
		    if (!na(xx)) {
			if (msg != NULL)
			    sprintf(msg, "Log error: Variable '%s', obs %d,"
				    " value = %g\n", pdinfo->varname[v],
				    t+1, xx);
#ifdef GENR_DEBUG
			fprintf(stderr, "Log error: Variable '%s', obs %d,"
				" value = %g\n", pdinfo->varname[v], t+1, xx);
#endif
			/* ran across a zero or negative number when
			   trying to take logs */
			le_zero = 1;
		    }
		}
		else (*pZ)[n*(nvar+j) + t] = log(xx); 
	    }
	    if (le_zero) continue;
	    strcpy(s, "l_");
	    strcat(s, pdinfo->varname[v]);
	    esl_trunc(s, 8);
	    strcpy(pdinfo->varname[nvar+j], s);
	    strcat(s, " = log of ");
	    strcat(s, pdinfo->varname[v]);
	    strcpy(pdinfo->label[nvar+j], s);
	    check = varindex(pdinfo, pdinfo->varname[j]);
	    if (check < nvar) {
		if (_identical(&(*pZ)[n*check], &(*pZ)[n*(nvar+j)], n)) {
		    j--;
		}
	    } else printf("label: %s\n", pdinfo->label[nvar+j]);
	} else _varerror(s);
	j++;
    }

    /* shrink Z if warranted (not all vars logged) */
    if (j < l0) shrink_Z(l0 - j, pZ, pdinfo);

    if (j == 0) j = -1;
    return j;
}

/* ...................................................... */

int lags (const int *list, double **pZ, DATAINFO *pdinfo)
/* generates lag variables for each var in list */
{
    int check, l, v, lv, opt = 1;
    
    for (v=1; v<=list[0]; v++) {
	lv = list[v];
	if (lv == 0) continue;
	for (l=1; l<=pdinfo->pd; l++) {
	    check = laggenr(lv, l, opt, pZ, pdinfo);
	    if (check) return 1;
	}
    }
    return 0;
}


/* ...................................................... */

int _parse_lagvar (const char *varname, LAGVAR *plagv, DATAINFO *pdinfo)
{
    int i, j;
    int l = 0, n = strlen(varname);
    int op;
    char testint[3];

    /*  fprintf(stderr, "_parse_lagvar: varname = %s\n", varname); */

    for (i=0; i<3; i++) testint[i] = '\0';
    for (i=0; i<n-3; i++) {
	if (varname[i] == '(') {
	    l = i;
	    if ((op = varname[i+1]) != '-') return 0;
	    for (i=l+2; i<n; i++) {
		if (varname[i] == ')') {
		    for (j=l+2; j<i; j++) {
			if (!isdigit((unsigned char) varname[j])) 
			    return 0;
			testint[j-(l+2)] = varname[j];
		    }
		    testint[2] = '\0';
		    if ((plagv->lag = atoi(testint))) {
			strncpy(plagv->varname, varname, l);
			plagv->varname[l] = '\0';
			/*  snprintf(plagv->varname, l+1, "%s", varname); */ 
			if ((n = varindex(pdinfo, plagv->varname)) 
			    < pdinfo->v) {
			    plagv->varnum = n;
			    return l;
			}
			else return 0;
		    } else return 0;
		}
	    }
	}
    }
    return 0;
}

/* ...................................................... */

int xpxgenr (const int *list, double **pZ, DATAINFO *pdinfo, 
	     const int opt, const int nodup)
/* Generates squares, cross products of the variables in "list".  
   If a variable is a dummy, square is ignored.
   if opt = 0, only square terms are created.
   if opt = 1, both squares and cross-products are created.
       Returns the number of new variables created, or -1 in case of
   failure.
       If nodup = 1, vars will not be created if they are already
   present in the data set.
*/
{
    int check, i, j, t, li, lj, l0 = list[0];
    int maxterms, terms, n = pdinfo->n, v = pdinfo->v;
    double zi, zj;
    char s[12], s1[9];

    /* maximum number of terms if none are "bad" */
    if (opt) maxterms = (l0*l0 + l0)/2;
    else maxterms = l0;
/*      fprintf(stderr, "xpxgenr: maxterms = %d\n", maxterms);   */
/*      printlist(list);   */

    if (grow_Z(maxterms, pZ, pdinfo)) return -1;

    terms = 0;
    for (i=1; i<=l0; i++) {
	li = list[i];
	if (!isdummy(li, 0, n-1, *pZ, n)) {
	    for (t=0; t<n; t++) (*pZ)[n*(v+terms) + t] = NADBL;
	    for (t=pdinfo->t1; t<=pdinfo->t2; t++) {
		zi = (*pZ)[n*li + t];
		if (na(zi)) (*pZ)[n*(v+terms) + t] = NADBL;
		else (*pZ)[n*(v+terms) + t] = zi * zi;
	    }
	    if (iszero(0, n-1, &(*pZ)[n*(v+terms)])) continue; 
	    /*
	      prefix varname by sq, truncate if too long and save under 
	      new varname; new label is "varname = oldname squared"
	    */
	    strcpy(s, "sq_");
	    strcat(s, pdinfo->varname[li]);
	    esl_trunc(s, 8);
	    strcpy(pdinfo->varname[v+terms], s);
	    /* check if an _identical variable exists? */
	    if (nodup) {
		check = varindex(pdinfo, pdinfo->varname[(v+terms)]);
		if (check < v) {
		    if (_identical(&(*pZ)[n*check], &(*pZ)[n*(v+terms)], n)) 
			continue;
		}
	    }
	    sprintf(pdinfo->label[v+terms], "%s = %s squared", s,
		    pdinfo->varname[li]);  
	    terms++;
	}
	/* also do cross-products if wanted */
	if (opt) {
	    for (j=i+1; j<=l0; j++) {
		lj = list[j];
		for (t=0; t<n; t++) (*pZ)[n*(v+terms) + t] = NADBL;
		for (t=pdinfo->t1; t<=pdinfo->t2; t++) {
		    zi = (*pZ)[n*li + t];
		    zj = (*pZ)[n*lj + t];
		    if (na(zi) || na(zj)) 
			(*pZ)[n*(v+terms) + t] = NADBL;
		    else (*pZ)[n*(v+terms) + t] = zi*zj;
		}
		if (iszero(0, n-1, &(*pZ)[n*(v+terms)])) continue;
		/*
		  trunc varname i and varname j if needed and cat them.
		  save as newvarname.  Also make label.
		*/
		strcpy(s, pdinfo->varname[li]);
		esl_trunc(s, 3);
		strcat(s, "_");
		strcpy(s1, pdinfo->varname[lj]);
		esl_trunc(s1, 4);
		strcat(s, s1);
		strcpy(pdinfo->varname[v+terms], s);
		sprintf(pdinfo->label[v+terms], "%s = %s times %s",
			s, pdinfo->varname[li], pdinfo->varname[lj]);
		terms++;
	    }
	}
    }

    if (terms < maxterms) shrink_Z(maxterms - terms, pZ, pdinfo);
    /*  fprintf(stderr, "xpxgenr: returning %d\n", terms); */
    return terms;
}

/* ...................................................... */

int rhodiff (char *param, const int *list, double **pZ, DATAINFO *pdinfo)
     /* creates rho-differenced counterparts of the variables in
	the input list and adds them to the data set */
{
    int i, j, maxlag, p, t, t1, nv, v = pdinfo->v, n = pdinfo->n;
    char s[64], parmbit[9];
    double xx, *rhot;

    /*  printf("rhodiff: param = %s\n", param); */
    maxlag = count_fields(param);
    rhot = malloc(maxlag * sizeof *rhot);
    if (rhot == NULL) return E_ALLOC;
    if (maxlag > pdinfo->t1) t1 = maxlag;
    else t1 = pdinfo->t1;

    /*  printf("rhodiff: maxlag = %d, t1 = %d\n", maxlag, t1); */

    /* parse "param" string */
    j = strlen(param);
    p = 0;
    for (i=0; i<j; i++) {
	if ((i == 0 || param[i] == ' ') && i < (j - 1)) {
	    sscanf(param + i + (i? 1: 0), "%8s", parmbit); 
	    /*  printf("rhodiff: parmbit = %s\n", parmbit); */
	    if (isalpha((unsigned char) parmbit[0])) {
		nv = varindex(pdinfo, parmbit);
		if (nv == v) {
		    free(rhot);
		    return E_UNKVAR;
		}
		rhot[p] = (*pZ)[n*nv + pdinfo->t1];
		/*  printf("rhodiff: rhot[%d] = %f\n", p, rhot[p]); */
	    } else {
		rhot[p] = atof(parmbit);
		/*  printf("rhodiff: rhot[%d] = %f\n", p, rhot[p]); */
	    }
	    p++;
	}
    }

    if (grow_Z(list[0], pZ, pdinfo)) return E_ALLOC;

    for (i=1; i<=list[0]; i++) {
	j = list[i];
	/*  printf("rhodiff: doing list[%d] = %d\n", i, list[i]); */
	/* make name and label */
	strcpy(s, pdinfo->varname[j]);
	esl_trunc(s, 7);
	strcat(s, "#");
	strcpy(pdinfo->varname[v+i-1], s);
	sprintf(pdinfo->label[v+i-1], "%s = rho-differenced %s", 
		pdinfo->varname[v+i-1], pdinfo->varname[j]);
	/* fill out values */
	for (t=0; t<n; t++) (*pZ)[n*(v+i-1) + t] = NADBL;
	for (t=t1; t<=pdinfo->t2; t++) {
	    xx = (*pZ)[n*j + t];
	    if (na(xx)) {
		(*pZ)[n*(v+i-1) + t] = NADBL;
		continue;
	    }
	    for (p=0; p<maxlag; p++) {
		if (na((*pZ)[n*j + t-p-1])) {
		    xx = NADBL;
		    break;
		}
		else xx -= rhot[p] * (*pZ)[n*j + t-p-1];
	    }
	    (*pZ)[n*(v+i-1) + t] = xx;
	}
    }
    free(rhot);
    return 0;
}

/* ...................................................... */

static double _genr_cov (const char *str, double **pZ, 
			 const DATAINFO *pdinfo)
{
    int i, n, n2, p, v1, v2;
    char v1str[9], v2str[9];

    n = strlen(str);
    if (n > 17) return NADBL;
    p = haschar(',', str);
    if (p < 0 || p > 8) return NADBL;
    n2 = n - p - 1;
    /* get first var name */
    for (i=0; i<p; i++) v1str[i] = str[i];
    v1str[p] = '\0';
    /* get second var name */
    for (i=0; i<n2; i++) v2str[i] = str[p+1+i];
    v2str[i] = '\0';
    /* and look up the two */
    v1 = varindex(pdinfo, v1str);
    v2 = varindex(pdinfo, v2str);
    if (v1 >= pdinfo->v || v2 >= pdinfo->v)
	return NADBL;

    n = pdinfo->n;
    return covar(pdinfo->t2 - pdinfo->t1 + 1,
		 &(*pZ)[v1*n + pdinfo->t1], 
		 &(*pZ)[v2*n + pdinfo->t1]);
}

/* ...................................................... */

static double _genr_corr (const char *str, double **pZ, 
			  const DATAINFO *pdinfo)
{
    int i, n, n2, p, v1, v2;
    char v1str[9], v2str[9];

    n = strlen(str);
    if (n > 17) return NADBL;
    p = haschar(',', str);
    if (p < 0 || p > 8) return NADBL;
    n2 = n - p - 1;
    /* get first var name */
    for (i=0; i<p; i++) v1str[i] = str[i];
    v1str[p] = '\0';
    /* get second var name */
    for (i=0; i<n2; i++) v2str[i] = str[p+1+i];
    v2str[i] = '\0';
    /* and look up the two */
    v1 = varindex(pdinfo, v1str);
    v2 = varindex(pdinfo, v2str);
    if (v1 >= pdinfo->v || v2 >= pdinfo->v)
	return NADBL;

    n = pdinfo->n;
    return corr(pdinfo->t2 - pdinfo->t1 + 1,
		&(*pZ)[v1*n + pdinfo->t1], &(*pZ)[v2*n + pdinfo->t1]);
}

/* ...................................................... */

static double _genr_vcv (const char *str, double **pZ, 
			 const DATAINFO *pdinfo, MODEL *pmod)
{
    int i, j, k, n, n2, nv, p, v1, v2, v1l, v2l;
    char v1str[9], v2str[9];

    if (pmod == NULL || pmod->list == NULL) return NADBL;

    n = strlen(str);
    if (n > 17) return NADBL;
    p = haschar(',', str);
    if (p < 0 || p > 8) return NADBL;
    n2 = n - p - 1;
    /* get first var name */
    for (i=0; i<p; i++) v1str[i] = str[i];
    v1str[p] = '\0';
    /* get second var name */
    for (i=0; i<n2; i++) v2str[i] = str[p+1+i];
    v2str[i] = '\0';
    /* are they valid? */
    v1 = varindex(pdinfo, v1str);
    v2 = varindex(pdinfo, v2str);
    if (v1 >= pdinfo->v || v2 >= pdinfo->v) return NADBL;
    /* check model list */
    v1l = _ismatch(v1, pmod->list);
    v2l = _ismatch(v2, pmod->list);
    if (!v1l || !v2l) return NADBL;
    /* model vcv matrix */
    if (pmod->vcv == NULL && makevcv(pmod)) return NADBL;
    /* now find the right entry */
    nv = pmod->list[0];
    if (v1l > v2l) {
	k = v1l;
	v1l = v2l;
	v2l = k;
    }
    k = 0;
    for (i=2; i<=nv; i++) {
	for (j=2; j<=nv; j++) {
	    if (j < i) continue;
	    if (i == v1l && j == v2l) return pmod->vcv[k];
	    k++;
	}
    }
    return NADBL;
}

/* ...................................................... */

static void _genr_msg (GENERATE *pgenr, const int nv)
{
	sprintf(pgenr->msg, "%s var. no. %d (%s)\n", 
		(pgenr->varnum < nv)? "Replaced" : "Generated", 
		 pgenr->varnum, pgenr->varname);
}

/* ......................................................  */

static int _ismatch (const int lv, const int *list)
{
    int n;

    for (n=1; n<=list[0]; n++) 
        if (lv == list[n]) return n;
    return 0;
}

/* .......................................................... */

static void _varerror (const char *ss)
/* print error message for variable not in name list */
{
    fprintf(stderr, "\nInvalid var. name (%s)\n", ss);
    if (strcmp(ss, "const") == 0) 
        fputs("const cannot be used to store values", stderr);
    else if (strcmp(ss, "uhat") == 0) 
        fputs("uhat can be used only in genr.  First use the command: "
	      "genr newname = uhat", stderr);
    else if (ss[0] == '$') 
	fprintf(stderr, "Reserved var. names starting with "
		"$ can be used only in genr.\nFirst use the "
		"command:  genr newname = %s\n", ss);
}

/* .......................................................... */

int simulate (char *cmd, double **pZ, DATAINFO *pdinfo, char *msg)
     /* for "sim" command */
{
    int f, i, t, t1, t2, m, n = pdinfo->n, nv, pv, *isconst;
    char varname[32], tmpstr[128], parm[9], **toks;
    double xx, *a;

    f = count_fields(cmd);
    m = f - 4;

    a = malloc(m * sizeof(double));
    isconst = malloc(m * sizeof(int));
    toks = malloc(f * 9);
    if (a == NULL || isconst == NULL || toks == NULL) return E_ALLOC;
    for (i=0; i<m; i++) isconst[i] = 1;

    strncpy(tmpstr, cmd, 127);
    strtok(tmpstr, " ");
    for (i=0; i<f-1; i++) {
	toks[i] = strtok(NULL, " ");
    }

    /* try getting valid obs from stobs and endobs */
    t1 = dateton(toks[0], pdinfo->pd, pdinfo->stobs, msg);
    t2 = dateton(toks[1], pdinfo->pd, pdinfo->stobs, msg);
    if (strlen(msg) || t1 < 0 || t1 >= t2 || t2 > pdinfo->n) {
	free(a);
	free(toks);
	return 1;
    }

    /* name of var to simulate */
    strcpy(varname, toks[2]);
    esl_trunc(varname, 8);
    nv = varindex(pdinfo, varname);
    if (nv == 0 || nv >= pdinfo->v) {
	sprintf(msg, (nv)? "For 'sim', the variable must already "
		"exist.\n" :
		"You can't use the constant for this purpose.\n");
	free(a);
	free(toks);
	return 1;
    }

    /* get the parameter terms */
    for (i=0; i<m; i++) {
	strcpy(parm, toks[i+3]);
	if (isalpha((unsigned char) parm[0])) {
	    pv = varindex(pdinfo, parm);
	    if (pv == 0 || pv >= pdinfo->v) {
		sprintf(msg, "bad varname in sim.\n");
		free(a);
		free(toks);
		return 1;
	    } else {
		isconst[i] = 0;
		/*  fprintf(fp, "param[%d] is a variable, %d\n", i, pv); */ 
		a[i] = (double) pv;
	    }
	}
	else {
	    a[i] = atof(parm);
	    /*  fprintf(fp, "param[%d] is a constant = %f\n", i, a[i]); */ 
	}
    }

    if (t1 < m - 1) t1 = m - 1;
    for (t=t1; t<=t2; t++) {
	xx = 0.;
	for (i=0; i<m; i++) {
	    if (isconst[i]) {
		if (i == 0) xx += a[i];
		else xx += a[i] * (*pZ)[n*nv + t-i];
	    } else {
		pv = (int) a[i];
		if (na((*pZ)[n*pv + t])) {
		    xx = NADBL;
		    break;
		}
		if (i == 0) xx += (*pZ)[n*pv + t];
		else xx += (*pZ)[n*pv + t] * (*pZ)[n*nv + t-i];
	    }
	}
	(*pZ)[n*nv + t] = xx;
    }

    free(a);
    free(isconst);
    free(toks);
    return 0;
}

/* .......................................................... */

int multiply (char *s, int *list, char *sfx, double **pZ,
	      DATAINFO *pdinfo)
{
    int i, t, v = 0, nv, n = pdinfo->n, lv, l0 = list[0];
    int slen;
    double m = 0;
    char tmp[9];

    /* parse s */
    if (isdigit((unsigned char) s[0])) m = atof(s);
    else {
	v = varindex(pdinfo, s);
	if (v == pdinfo->v) return E_UNKVAR; 
    }

    if (grow_Z(l0, pZ, pdinfo)) return E_ALLOC;
    slen = strlen(sfx);

    /* fill out values */
    for (i=1; i<=l0; i++) {
	nv = pdinfo->v - l0 - 1 + i;
	lv = list[i];
	for (t=0; t<n; t++) (*pZ)[n*nv + t] = NADBL;
	for (t=pdinfo->t1; t<=pdinfo->t2; t++) {
	    if (na((*pZ)[n*lv + t])) {
		(*pZ)[n*nv + t] = NADBL;
		continue;
	    }
	    if (v) {
		if (na((*pZ)[n*v + t]))
		    (*pZ)[n*nv + t] = NADBL;
		else (*pZ)[n*nv + t] = (*pZ)[n*v + t] * (*pZ)[n*lv + t];
	    } else (*pZ)[n*nv + t] = m * (*pZ)[n*lv + t];
	}
	/* do names and labels */
	strcpy(tmp, pdinfo->varname[lv]);
	esl_trunc(tmp, 8 - slen);
	strcat(tmp, sfx);
	strcpy(pdinfo->varname[nv], tmp);
	if (v) 
	    sprintf(pdinfo->label[nv], "%s = %s * %s",
		    pdinfo->varname[nv], pdinfo->varname[v], 
		    pdinfo->varname[lv]); 
	else 
	    sprintf(pdinfo->label[nv], "%s = %f * %s",
		    pdinfo->varname[nv], m, pdinfo->varname[lv]); 
    }

    return 0;
}

/* .......................................................... */

int genr_fit_resid (MODEL *pmod, double **pZ, DATAINFO *pdinfo,
		    int code, int undo)
{
    char vname[9], vlabel[MAXLABEL];
    int i, n, t, t1 = pmod->t1, t2 = pmod->t2;

    if (grow_Z(1, pZ, pdinfo)) return E_ALLOC;

    i = pdinfo->v - 1;
    n = pdinfo->n;

    for (t=0; t<t1; t++) (*pZ)[n*i + t] = NADBL;
    for (t=t2+1; t<n; t++) (*pZ)[n*i + t] = NADBL;

    if (code == 0) { /* residuals */
	sprintf(vname, "uhat%d", pmod->ID);
	sprintf(vlabel, "residual from model %d", pmod->ID);
	for (t=t1; t<=t2; t++) 
	    (*pZ)[n*i + t] = pmod->uhat[t];
    }
    else if (code == 1) { /* fitted values */
	sprintf(vname, "yhat%d", pmod->ID);
	sprintf(vlabel, "fitted value from model %d", pmod->ID);
	for (t=t1; t<=t2; t++) 
	    (*pZ)[n*i + t] = pmod->yhat[t];
    }
    else if (code == 2) { /* squared residuals */
	sprintf(vname, "usq%d", pmod->ID);
	sprintf(vlabel, "squared residual from model %d", pmod->ID);
	for (t=t1; t<=t2; t++) 
	    (*pZ)[n*i + t] = pmod->uhat[t] * pmod->uhat[t];
    }
    strcpy(pdinfo->varname[i], vname);

    if (!undo) 
	strcpy(pdinfo->label[i], vlabel);

    return 0;
}
