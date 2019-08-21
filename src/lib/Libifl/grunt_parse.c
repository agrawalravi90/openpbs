/*
 * Copyright (C) 1994-2019 Altair Engineering, Inc.
 * For more information, contact Altair at www.altair.com.
 *
 * This file is part of the PBS Professional ("PBS Pro") software.
 *
 * Open Source License Information:
 *
 * PBS Pro is free software. You can redistribute it and/or modify it under the
 * terms of the GNU Affero General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option) any
 * later version.
 *
 * PBS Pro is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.
 * See the GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * Commercial License Information:
 *
 * For a copy of the commercial license terms and conditions,
 * go to: (http://www.pbspro.com/UserArea/agreement.html)
 * or contact the Altair Legal Department.
 *
 * Altair’s dual-license business model allows companies, individuals, and
 * organizations to create proprietary derivative works of PBS Pro and
 * distribute them - whether embedded or bundled with other software -
 * under a commercial license agreement.
 *
 * Use of Altair’s trademarks, including but not limited to "PBS™",
 * "PBS Professional®", and "PBS Pro™" and Altair’s logos is subject to Altair's
 * trademark licensing policies.
 *
 */
/**
 * @file	grunt_parse.c
 */


#include "pbs_config.h"   /* the master config generated by configure */
#include <ctype.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include "grunt.h"
#include "pbs_error.h"

#ifdef NAS /* localmod 082 */
#ifndef	MAX
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#endif
#endif /* localmod 082 */

/**
 * @brief
 * 	-parse_resc_equal_string - (thread safe) parse a string of the form:
 *		name1 = value1[,value2 ...][: name2 = value3[,value4 ...]]
 *	into <name1> <value1[,value2 ...>
 *	     <name2> <value3[,value4 ...>
 *
 * @par
 *	after call,
 *		*name will point to "name1"
 *		*value will point to "value1 ..." upto but not
 *			including the colon before "name2".
 *
 * @param[in] start - the start of the string to parse.
 * @param[in] name - point to "name1"
 * @param[in] value - will point to "value1 ..." upto but not
 *		      including the colon before "name2".
 * @param[in] last - points to where parsing stopped, use as "start" on
 *		     next call to continue.  last is set only if the function
 *		     return is "1".
 *
 * @return	int
 * @return 	int
 * @retval 	1 	if  name and value are found,
 * @retval	0 	if nothing (more) is parsed (null input)
 * @retval	-1 	if a syntax error was detected.
 *
 * @par
 *	each string is null terminated.
 */

static int
parse_resc_equal_string(char  *start, char **name, char **value, char **last)
{
	char	 *pc;
	char     *backup;
	int	  quoting = 0;

	if ((start==NULL) || (name==NULL) || (value==NULL) || (last==NULL))
		return -1;	/* error */

	pc = start;

	if (*pc == '\0') {
		*name = NULL;
		return (0);	/* already at end, return no strings */
	}

	/* strip leading spaces */

	while (isspace((int)*pc) && *pc)
		pc++;

	if (*pc == '\0') {
		*name = NULL;	/* null name */
		return (0);
	} else if (!isalpha((int)*pc))
		return (-1);	/* no name, return error */

	*name = pc;

	/* have found start of name, look for end of it */

	while (!isspace((int)*pc) && (*pc != '=') && *pc)
		pc++;

	/* now look for =, while stripping blanks between end of name and = */

	while (isspace((int)*pc) && *pc)
		*pc++ = '\0';
	if (*pc != '=')
		return (-1);	/* should have found a = as first non blank */
	*pc++ = '\0';

	/* that follows is the value string, skip leading white space */

	while (isspace((int)*pc) && *pc)
		pc++;

	/* is the value string to be quoted ? */

	if ((*pc == '"') || (*pc == '\''))
		quoting = (int)*pc++;  /* adv start of "value" past quote chr */
	*value = pc;

	/*
	 * now go to first colon, or if quoted, the colon sign
	 * after the close quote
	 */

	if (quoting) {
		while ((*pc != (char)quoting) && *pc)	/* look for matching */
			pc++;
		if (*pc) {
			char *pd;
			/* close string up over the trailing quote */
			pd = pc;
			while (*pd) {
				*pd = *(pd+1);
				pd++;
			}
		} else
			return (-1);
	}
	while ((*pc != ':') && *pc)
		pc++;

	if (*pc == '\0') {
		while (isspace((int)*--pc)) ;
		if (*pc == ',')	/* trailing comma is a no no */
			return (-1);
		*last = ++pc;
		return (1);	/* no equal, just end of line, stop here */
	}

	/* strip off any trailing white space */

	backup = pc++;
	*backup = '\0';			/* null the colon */

	while (isspace((int)*--backup))
		*backup = '\0';
	*last = pc;
	return (1);
}



/**
 * @brief
 *	-parse_node_resc_r - (thread safe) parse the node and resource string of the form:
 *	nodeA:resc1=value1:resc2=value2
 *
 * @param[in]	str - start of string to parse (string will be
 *                    munged, so make a copy before calling this
 *                    function)
 * @param[out]	nodep - pointer to node name
 * @param[out]	pnelm - number of used elements in key_valye_pair
 *                      array
 * @param[in][out] nl - total number of elements in key_value_pair
 *                      array
 * @param[in][out] kv - pointer to array of key_value_pair structures
 *			will be malloced if nl == 0, will grow if needed
 *			will not be freed by this routine
 *
 * @return  int
 * @retval  0 = ok
 * @retval  !0 = error
 *
 */
int
parse_node_resc_r(char *str, char **nodep, int *pnelem, int *nlkv, struct key_value_pair **kv)
{
	int	      i;
	int	      nelm = 0;
	char  *pc;
	char	     *word;
	char	     *value;
	char	     *last;


	if (str == NULL)
		return (PBSE_INTERNAL);

	if (*nlkv == 0) {
		*kv = (struct key_value_pair *) malloc(KVP_SIZE * sizeof(struct key_value_pair));
		if (*kv == NULL)
			return -1;
		*nlkv = KVP_SIZE;
	}
	for (i=0; i<*nlkv; i++) {
		(*kv)[i].kv_keyw = NULL;
		(*kv)[i].kv_val  = NULL;
	}

	pc = str;

	while (isspace((int)*pc))
		pc++;
	if (*pc == '\0') {
		*pnelem = nelm;
		return 0;
	}

	*nodep = pc;
	while ((*pc != ':') && !isspace((int)*pc) && *pc)
		pc++;

	if (pc == *nodep)
		return -1;	/* error - no node name */

	if (*pc == '\0') {
		*pnelem = nelm;	/* no resources */
		return 0;
	} else {
		while (*pc != ':' && *pc)
			*pc++ ='\0';
		if (*pc == ':')
			*pc++ = '\0';
	}

	/* process resource=value strings upto closing brace */

	if (*pc == '\0')
		return -1;

	i = parse_resc_equal_string(pc, &word, &value, &last);
	while (i == 1) {
		if (nelm >= *nlkv) {
			/* make more space in k_v table */
			struct key_value_pair *ttpkv;
			ttpkv = (struct key_value_pair *)realloc(*kv, (*nlkv+KVP_SIZE) * sizeof(struct key_value_pair));
			if (ttpkv == NULL)
				return PBSE_SYSTEM;
			*kv = ttpkv;
			*nlkv += KVP_SIZE;
		}
		(*kv)[nelm].kv_keyw = word;
		(*kv)[nelm].kv_val  = value;
		nelm++;

		i = parse_resc_equal_string(last, &word, &value, &last);
	}
	if (i == -1)
		return PBSE_BADATVAL;

	*pnelem = nelm;
	return 0;
}

/**
 * @brief
 *	-parse_node_resc - parse the node and resource string of the form:
 *	nodeA:resc1=value1:resc2=value2
 *
 *      @param		str	start of the string to parse
 *      @param[out]	nodep	pointer to node name
 *	@param[out]	nl      number of used data elements in
 *                             	key_value_pair array
 *      @param[out]	kv      pointer to array of key_value_pair structures
 *
 *      @return  int
 *      @retval  0 = ok
 *      @retval  !0 = error
 */
int
parse_node_resc(char *str, char **nodep, int *nl, struct key_value_pair **kv)
{
	int	      i;
	int	      nelm = 0;
	static int    nkvelements = 0;
	static struct key_value_pair *tpkv = NULL;

	if (str == NULL)
		return (PBSE_INTERNAL);

	i = parse_node_resc_r(str, nodep, &nelm, &nkvelements, &tpkv);

	*nl = nelm;
	*kv = tpkv;
	return i;
}

/**
 * @brief
 *	-parse_chunk_r - (thread safe) decode a chunk of a selection specification string,
 *
 *	Chunk is of the form: [#][:word=value[:word=value...]]
 *
 * @param str    = string to parse (will be munged) (input)
 #ifdef NAS localmod 082
 * @param[in] extra	= number of extra entries to reserve in pkv (input)
 #endif localmod 082
 * @param[in] nchk   = number of chunks, "#" (output)
 * @param[in] pnelem = of active data elements in key_value_pair
 *                     array (output)
 * @param[in] nkve   = total number of elements (size) in the
 *                     key_value_pair array (input/output)
 * @param[in] pkv    = pointer to array of key_value_pair (input/output)
 * @param[in] dflt   = upon receiving a select specification with
 *                     no number of chunks factor, we default to a nchk
 *                     factor of 1.  The new resource default_chunk.nchunk
 *                     controls the value of this chunk factor when it is
 *                     not set.  The dflt argument specifies whether the
 *                     number of nchk was provided on the select line or not
 *                     such that at a later time we can determine if
 *                     the default_chunk.nchunk resource should be
 *                     applied or not (see make_schedselect) (output)
 *
 * @par	Note:
 *	the key_value_pair array, rtn, will be grown if additional
 *	space is needed,  it is not freed by this routine
 *
 * @return 	int
 * @retval 	0 	if ok
 * @retval 	!0 	on error
 *
 */

#ifdef NAS /* localmod 082 */
int
parse_chunk_r(char *str, int extra, int *nchk, int *pnelem, int *nkve, struct key_value_pair **pkv, int *dflt)
#else
int
parse_chunk_r(char *str, int *nchk, int *pnelem, int *nkve, struct key_value_pair **pkv, int *dflt)
#endif /* localmod 082 */
{
	int   i;
	int   nchunk=1; 		/* default number of chucks */
	int   setbydefault=1;
	int   nelem = 0;
	char *pc;
	char *ps;
	char *word;
	char *value;
	char *last;

	if (str == NULL)
		return (PBSE_INTERNAL);

	if (*nkve== 0) {
		/* malloc room for array of key_value_pair structure */
#ifdef NAS /* localmod 082 */
		i = MAX(extra, KVP_SIZE);
		*pkv = (struct key_value_pair *)malloc(i * sizeof(struct key_value_pair));
#else
		*pkv = (struct key_value_pair *)malloc(KVP_SIZE * sizeof(struct key_value_pair));
#endif /* localmod 082 */
		if (*pkv == NULL)
			return PBSE_SYSTEM;
#ifdef NAS /* localmod 082 */
		*nkve = i;
#else
		*nkve = KVP_SIZE;
#endif /* localmod 082 */
	}
	for (i=0; i<*nkve; ++i) {
		(*pkv)[i].kv_keyw  = NULL;
		(*pkv)[i].kv_val   = NULL;
	}

	pc = str;

	/* start of chunk */
	while (isspace((int)*pc))
		++pc;

	/* first word must start with number or letter */

	ps = pc;
	if (! isalnum((int)*pc))
		return (PBSE_BADATVAL);

	if (isdigit((int)*pc)) {
		/* leading count, should be followed by ':' or '\0' */
		++pc;
		while (isdigit((int)*pc))
			++pc;
		nchunk = atoi(ps);
		setbydefault = 0;
		while (isspace((int)*pc))
			++pc;
		if (*pc != '\0') {
			if (*pc != ':')
				return (PBSE_BADATVAL);
			++pc;
		}
	}

	/* next comes "resc=value" pairs */

	i = parse_resc_equal_string(pc, &word, &value, &last);
	while (i == 1) {
#ifdef NAS /* localmod 082 */
		while (nelem + extra >= *nkve) {
#else
		if (nelem >= *nkve) {
#endif /* localmod 082 */
			/* make more space in k_v table */
			struct key_value_pair *ttpkv;
			ttpkv = realloc(*pkv, (*nkve+KVP_SIZE)*sizeof(struct key_value_pair));
			if (ttpkv == NULL)
				return PBSE_SYSTEM;
			*pkv = ttpkv;
			for (i=*nkve; i<*nkve+KVP_SIZE; ++i) {
				(*pkv)[i].kv_keyw  = NULL;
				(*pkv)[i].kv_val   = NULL;
			}
			*nkve += KVP_SIZE;
		}
		(*pkv)[nelem].kv_keyw  = word;
		(*pkv)[nelem].kv_val   = value;
		nelem++;
		/* continue with next resc=value pair          */

		i = parse_resc_equal_string(last, &word, &value, &last);
	}
	if (i == -1)
		return PBSE_BADATVAL;

	*pnelem = nelem;
	*nchk = nchunk;
	if (dflt)
		*dflt = setbydefault;

	return 0;
}

/**
 * @brief
 * 	parse_chunk - (not thread safe) decode a chunk of a selection specification
 *	string,
 *
 * @par
 *	Chunk is of the form: [#][:word=value[:word=value...]]
 *
 * @param[in]  str  = string to parse
 #ifdef NAS localmod 082
 * @param[in]	extra = number of extra slots to allocate in rtn
 #endif localmod 082
 * @param[in]	nchk = number of chunks, "#"
 * @param[in]	nrtn = number of active (used) word=value pairs in the
 *		       key_value_pair array
 * @param[in]	rtn  = pointer to static array of key_value_pair
 * @param[in]  dflt = the nchk value was set to 1 by default
 *
 * @return 	int
 * @retval 	0 	if ok
 * @retval 	!0 	on error
 *
 */

#ifdef NAS /* localmod 082 */
int
parse_chunk(char *str, int extra, int *nchk, int *nrtn, struct key_value_pair **rtn, int *setbydflt)
#else
int
parse_chunk(char *str, int *nchk, int *nrtn, struct key_value_pair **rtn, int *setbydflt)
#endif /* localmod 082 */
{
	int   i;
	int   nelm = 0;

	static int   nkvelements = 0;
	static struct key_value_pair *tpkv = NULL;

	if (str == NULL)
		return (PBSE_INTERNAL);

#ifdef NAS /* localmod 082 */
	i = parse_chunk_r(str, extra, nchk, &nelm, &nkvelements, &tpkv, setbydflt);
#else
	i = parse_chunk_r(str, nchk, &nelm, &nkvelements, &tpkv, setbydflt);
#endif /* localmod 082 */
	*nrtn = nelm;
	*rtn  = tpkv;
	return i;
}

/**
 * @brief
 *	parse_plus_spec_r - (thread safe)
 * @par
 *	Called with "str" set for start of string of a set of plus connnected
 *	substrings "substring1+substring2+...";
 *
 * @param[in]	selstr - string to parse, continue to parse
 * @param[in]	last   - pointer to place to resume parsing
 * @param[in]	hp     - set based on finding '(' or ')'
 *			 > 0 = found '(' at start of substring
 *			 = 0 = no parens or found both in one substring
 *			 < 0 = found ')' at end of substring
 *
 * @par
 *	IMPORTANT: the input string will be munged by the various
 *	parsing routines, if you need an untouched original,  pass
 *	in a pointer to a copy.
 *
 * @return         A pointer to next substring
 * @retval         next substring (char *)
 * @retval         NULL if end of the spec
 *
 */
char *
parse_plus_spec_r(char *selstr, char **last, int *hp)
{
	int		haveparen = 0;
	char    *pe;
	char    *ps;

	if ((selstr == NULL) || (strlen(selstr)) == 0)
		return NULL;

	ps = selstr;

	while (isspace((int)*ps))
		++ps;
	if (*ps == '(') {
		haveparen++;
		ps++;		/* skip over the ( */
	}

	pe = ps;
	while (*pe != '\0') {
		if ((*pe == '"') || (*pe == '\'')) {
			char quote;

			quote = *pe;
			pe++;
			while (*pe != '\0' && *pe != quote)
				pe++;
			if (*pe == quote)
				pe++;
		} else if (*pe != '+' && *pe != ')')
			pe++;
		else
			break;
	}

	if (*pe) {
		if (*pe == ')') {
			*pe++ = '\0';	/* null the )		*/
			haveparen--;
		}
		if (*pe != '\0')
			*pe++ = '\0';	/* null the following +	*/
	}

	if (*ps) {
		if (last != NULL)
			*last = pe;
		if (hp != NULL)
			*hp = haveparen;
		return ps;
	} else
		return NULL;
}

/**
 * @brief
 *	parse_plus_spec - not thread safe
 * @par
 *	Called with "str" set for start of string of a set of plus connnected
 *	substrings "substring1+substring2+...";  OR
 *	called with null to continue where left off.
 *
 * @param[in] selstr - string holding select specs
 * @param[in] rc - flag
 *
 * @return 	A pointer to next substring
 * @retval	next substring (char *)
 * @retval	NULL if end of the spec
 *
 * @par
 *	IMPORTANT: the "selstr" is copied into a locally allocated "static"
 *	char array for parsing.  The orignal string is untouched.  The array
 *	is grown as need to hold "selstr".
 */
char *
parse_plus_spec(char *selstr, int *rc)
{
	int		hp;	/* value returned by parse_pluse_spec ignored */
	size_t		len;
	static char    *pe;
	char           *ps;
	static char    *parsebuf = NULL;
	static int	parsebufsz = 0;

	*rc = 0;
	if (selstr) {

		if ((len = strlen(selstr)) == 0)
			return NULL;
		else if (len >= parsebufsz) {
			if (parsebuf)
				free(parsebuf);
			parsebufsz = len * 2;
			parsebuf = (char *)malloc(parsebufsz);
			if (parsebuf == NULL) {
				parsebufsz = 0;
				*rc = errno;
				return NULL;
			}
		}

		(void)strcpy(parsebuf, selstr);
		ps = parsebuf;
	} else
		ps = pe;

	if (*ps == '+') {
		/* invalid string, starts with + */
		*rc = PBSE_BADNODESPEC;
		return NULL;
	}

	return (parse_plus_spec_r(ps, &pe, &hp));
}

/**
 * @brief
 *	parse_plus_spec - thread safe
 * @par
 *	Called with "str" set for start of string of a set of plus connnected
 *	substrings "substring1+substring2+...";  OR
 *	called with null to continue where left off.
 *
 * @param[in] selstr - string holding select specs
 * @param[in,out] tailptr - pointer to the end of the last substring
 * @param[in] rc - flag
 *
 * @return 	A pointer to next substring
 * @retval	next substring (char *)
 * @retval	NULL if end of the spec
 *
 * @par
 *	IMPORTANT: the "selstr" is copied into a locally allocated
 *	char array for parsing.  The orignal string is untouched.  The array
 *	is grown as need to hold "selstr".
 */
char *
parse_plus_spec_mt_safe(char *selstr, char **tailptr, int *rc)
{
	size_t len;
	char *substr = NULL;
	char *ret = NULL;
	char *spec = NULL;
	char *ptr = NULL;

	*rc = PBSE_NONE;

	if (selstr == NULL && (tailptr == NULL || *tailptr == NULL)) {
		*rc = PBSE_INTERNAL;
		return NULL;
	}

	if (*tailptr == NULL) { /* dealing with this string first time */
		if (*selstr == '+') {
			/* invalid string, starts with + */
			*rc = PBSE_BADNODESPEC;
			return NULL;
		}
		ptr = selstr;
		*tailptr = selstr;
	} else if (*tailptr == '\0') {	/* Reached end of selstr */
		return NULL;
	} else
		ptr = *tailptr;

	/* Calculate length of next substring */
	len = 0;
	for (; *ptr != '+' && *ptr != '\0'; ptr++, len++)
		;

	if (len == 0)
		return NULL;

	substr = strndup(*tailptr, len);
	if (substr == NULL) {
		*rc = PBSE_SYSTEM;
		return NULL;
	}

	*tailptr = ptr;
	if (**tailptr == '+')
		*tailptr += 1;

	ret = parse_plus_spec_r(substr, NULL, NULL);
	spec = strdup(ret);
	if (spec == NULL) {
		*rc = PBSE_SYSTEM;
		return NULL;
	}
	free(substr);

	return spec;
}
