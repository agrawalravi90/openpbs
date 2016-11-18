/*
 * Copyright (C) 1994-2016 Altair Engineering, Inc.
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
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A
 * PARTICULAR PURPOSE.  See the GNU Affero General Public License for more details.
 *  
 * You should have received a copy of the GNU Affero General Public License along 
 * with this program.  If not, see <http://www.gnu.org/licenses/>.
 *  
 * Commercial License Information: 
 * 
 * The PBS Pro software is licensed under the terms of the GNU Affero General 
 * Public License agreement ("AGPL"), except where a separate commercial license 
 * agreement for PBS Pro version 14 or later has been executed in writing with Altair.
 *  
 * Altair’s dual-license business model allows companies, individuals, and 
 * organizations to create proprietary derivative works of PBS Pro and distribute 
 * them - whether embedded or bundled with other software - under a commercial 
 * license agreement.
 * 
 * Use of Altair’s trademarks, including but not limited to "PBS™", 
 * "PBS Professional®", and "PBS Pro™" and Altair’s logos is subject to Altair's 
 * trademark licensing policies.
 *
 */


/**
 * @file	ecl_verify.c
 *
 * @brief	The top level verification functionality
 *
 * @par		Functionality:
 *		Top level verification routines which in-turn call attribute
 *		level verification functions for datatype and value.
 */

#include <pbs_config.h>   /* the master config generated by configure */

#include <sys/types.h>
#include <stdlib.h>
#include <string.h>

#include "pbs_ifl.h"
#include "pbs_ecl.h"
#include "pbs_error.h"

#include "job.h"
#include "reservation.h"
#include "queue.h"
#include "pbs_nodes.h"
#include "server.h"
#include "libpbs.h"
#include "pbs_client_thread.h"

static enum batch_op seljobs_opstring_enums[] = {EQ, NE, GE, GT, LE, LT};
static int size_seljobs = sizeof(seljobs_opstring_enums)/sizeof(enum batch_op);

/* static function declarations */
static int
__pbs_verify_attributes(int connect, int batch_request,
	int parent_object, int command, struct attropl *attribute_list);
static int
__pbs_verify_attributes_dummy(int connect, int batch_request,
	int parent_object,  int command, struct attropl *attribute_list);
static struct ecl_attribute_def * ecl_findattr(int, struct attropl *);
static struct ecl_attribute_def * ecl_find_attr_in_def(
	struct ecl_attribute_def *, char *, int);
static int get_attr_type(struct ecl_attribute_def attr_def);

/* default function pointer assignments */
int (*pfn_pbs_verify_attributes)(int connect, int batch_request,
	int parent_object, int cmd, struct attropl *attribute_list)
= &__pbs_verify_attributes;


/**
 * @brief
 *	Bypass attribute verification on IFL API calls
 *
 * @par Functionality:
 *	This function resets the attribute verifcation function pointer to a
 *	dummy function, called from daemons, such that attribute verification is
 *	bypassed.
 *
 * @see
 *
 * @return void
 *
 * @par Side Effects:
 *	None
 *
 * @par MT-safe: No
 *
 */
void
set_no_attribute_verification(void)
{
	pfn_pbs_verify_attributes = &__pbs_verify_attributes_dummy;
}

/**
 * @brief
 *	The dummy verify attributes function
 *
 * @par Functionality:
 *	This is the function that gets called when IFL API is invoked by an
 *	application which has earlier called "set_no_attribute_verification"
 *
 * @see
 *
 * @param[in]	connect		-	Connection Identifier
 * @param[in]	batch_request	-	Batch Request Type
 * @param[in]	parent_object	-	Parent Object Type
 * @param[in]	cmd		-	Command Type
 * @param[in]	attribute_list	-	list of attributes
 *
 * @return	int
 * @retval	Zero
 *
 * @par Side Effects:
 *	None
 *
 * @par MT-safe: No
 *
 */
static int
__pbs_verify_attributes_dummy(int connect, int batch_request,
	int parent_object, int cmd, struct attropl *attribute_list)
{
	return 0;
}

/**
 * @brief
 *	The real verify function called from most IFL API calls
 *
 * @par Functionality:
 *	1. Gets the attr_errlist from the TLS data, deallocates it, if already
 *         allocated and then allocates it again.\n
 *	2. Clears the connect context values from the TLS\n
 *	3. Calls verify_attributes to verify the list of attributes passed\n
 *
 * @see verify_attributes
 *
 * @param[in]	connect		-	Connection Identifier
 * @param[in]	batch_request	-	Batch Request Type
 * @param[in]	parent_object	-	Parent Object Type
 * @param[in]	cmd		-	Command Type
 * @param[in]	attribute_list	-	list of attributes
 *
 * @return	int
 * @retval 	0 - No failed attributes
 * @retval 	+n - Number of failed attributes (pbs_errno set to last error)
 * @retval 	-1 - System error verifying attributes (pbs_errno is set)
 *
 * @par		Side effects:
 *		Modifies the TLS data for this thread\n
 *		pbs_errno is set on encourtering error
 *
 * @par MT-safe: Yes
 */
static int
__pbs_verify_attributes(int connect, int batch_request,
	int parent_object, int cmd, struct attropl *attribute_list)
{
	struct pbs_client_thread_context *ptr;
	struct pbs_client_thread_connect_context *con;
	int rc;

	/* get error list from TLS */
	ptr = (struct pbs_client_thread_context *)
		pbs_client_thread_get_context_data();
	if (ptr == NULL) {
		/* very unlikely case */
		pbs_errno = PBSE_SYSTEM;
		return -1;
	}

	/* since api is going to reuse err_list, free it first */
	free_errlist(ptr->th_errlist);
	ptr->th_errlist = NULL;

	con = pbs_client_thread_find_connect_context(connect);
	if (con == NULL) {
		if ((con = pbs_client_thread_add_connect_context(connect))
			== NULL) {
			pbs_errno = PBSE_SYSTEM;
			return -1;
		}
	}

	/* clear the TLS error codes */
	con->th_ch_errno = 0;
	if (con->th_ch_errtxt)
		free(con->th_ch_errtxt);
	con->th_ch_errtxt = (char *) NULL;

	if (attribute_list == NULL)
		return 0;

	rc = verify_attributes(batch_request, parent_object, cmd,
		attribute_list,	&ptr->th_errlist);
	if (rc > 0) {
		/* also set the pbs error code */
		pbs_errno = ptr->th_errlist->ecl_attrerr[0].ecl_errcode;

		/* copy first error code into TLS connection context */
		con->th_ch_errno = ptr->th_errlist->ecl_attrerr[0].ecl_errcode;
		if (ptr->th_errlist->ecl_attrerr[0].ecl_errmsg) {
			con->th_ch_errtxt =
				strdup(ptr->th_errlist->ecl_attrerr[0].ecl_errmsg);
			if (con->th_ch_errtxt == NULL) {
				pbs_errno = PBSE_SYSTEM;
				return -1;
			}
		}
	}
	return rc;
}

/**
 * @brief
 *	Verify one attribute
 *
 * @par Functionality:
 *      1. Finds the attribute in the correct object attribute list\n
 *      2. Invokes the at_verify_datatype function to check datatype is good\n
 *      3. Invokes the at_verify_value function to check if the value is good\n
 *	4. This function is also called from the hooks verification functions,
 *	   "is_job_input_valid" and "is_resv_input_valid" from
 *	    lib\Libpython\pbs_python_svr_internal.c
 *
 * @see verify_attributes
 *
 * @param[in]	batch_request	-	Batch Request Type
 * @param[in]	parent_object	-	Parent Object Type
 * @param[in]	cmd		-	Command Type
 * @param[in]	pattr		-	list of attributes
 * @param[out]	verified	-	Whether verification was done
 * @param[out]	err_msg		-	Error message for attribute verification
 *					failure
 * @return	int
 * @retval	0   - Passed verification
 * @retval	> 0 - attribute failed verification (pbs error number returned)
 * @retval	-1  - Out of memory
 *
 * @par	verified:
 *	1 - if the verification could be done\n
 *	0 - No verification handlers present, verification not done\n
 *	This output parameter is primarily used by the hooks verification
 *	functions to figure out whether any attribute verification was really
 *	done. If not done (value was 0) then the hooks code calls the server
 *	decode functions in an attempt to verify the attribute values.
 *
 * @par	err_msg:
 *	If the attribute fails verification, the err_msg parameter is set
 *	to the reason of failure. \n
 *	The err_msg parameter is passed to all the attribute verifiction
 *	routines, such that if a need arises, it would be possible for the
 *	individual routines to set a custom error message. \n
 * 	If the called attribute verification routines do not set any custom
 *	verification error message, then this routine sets the error message
 *	by calling "pbse_to_txt" to convert the return error code to error msg.
 *
 * @par	Side effects:
 *	pbs_errno set on error
 *
 * @par MT-safe: Yes
 */
int
verify_an_attribute(int batch_request, int parent_object, int cmd,
	struct attropl *pattr,
	int *verified,
	char **err_msg)
{
	ecl_attribute_def * p_eclattr = NULL;
	int err_code = PBSE_NONE;
	char *p;

	*verified = 1; /* set to verified */

	/* skip check when dealing with a "resource" parent object */
	if (parent_object == MGR_OBJ_RSC)
		return PBSE_NONE;

	if ((p_eclattr = ecl_findattr(parent_object, pattr)) == NULL) {
		err_code = PBSE_NOATTR;
		goto err;
	}

	if (pattr->value == NULL || pattr->value[0] == '\0') {

		/* allow empty/null values for unset/delete of pbs_manager */
		if ((batch_request == PBS_BATCH_Manager) &&
			(cmd == MGR_CMD_UNSET || cmd == MGR_CMD_DELETE))
			return PBSE_NONE;

		/* for the following stat calls, the value can be null/empty */
		if (batch_request == PBS_BATCH_StatusJob ||
			batch_request == PBS_BATCH_StatusQue ||
			batch_request == PBS_BATCH_StatusSvr ||
			batch_request == PBS_BATCH_StatusNode ||
			batch_request == PBS_BATCH_StatusRsc ||
			batch_request == PBS_BATCH_StatusHook ||
			batch_request == PBS_BATCH_StatusResv ||
			batch_request == PBS_BATCH_StatusSched)
			return PBSE_NONE;
	}

	/* for others, value shouldn't be null */
	if (pattr->value == NULL) {
		err_code = PBSE_BADATVAL;
		goto err;
	}

	/*
	 * When using ifl library directly, there is a possibility where resource is passed as NULL
	 * Check this variable for NULL and send error if it is NULL.
	 */
	if (strcasecmp(pattr->name, ATTR_l) == 0) {
		if (pattr->resource == NULL) {
			err_code = PBSE_UNKRESC;
			goto err;
		}
	}


	if (p_eclattr->at_verify_datatype) {
		if ((err_code = p_eclattr->at_verify_datatype(pattr, err_msg)))
			goto err;
	}

	if (p_eclattr->at_verify_value) {
		if ((err_code = p_eclattr->at_verify_value(batch_request,
			parent_object, cmd, pattr, err_msg)))
			goto err;
	}

	if (p_eclattr->at_verify_value == NULL) /* no verify func */
		*verified = 0;

	return PBSE_NONE;

err:
	if ((err_code !=0) && (*err_msg == NULL)) {
		/* find err_msg and update it */
		p = pbse_to_txt(err_code);
		if (p) {
			*err_msg = strdup(p);
			if (*err_msg == NULL) {
				err_code = PBSE_SYSTEM;
				return -1;
			}
		}
	}
	return err_code;
}

/**
 * @brief
 *	Duplicate an attribute structure
 *
 * @par Functionality:
 *	Helper routine to safely duplicate a attribute structure
 *	frees if allocation fails anywhere.
 * @see
 *
 * @param[in]	pattr	-	list of attributes
 *
 * @return	Pointer to the duplicated attribute structure
 * @retval	address of the duplicated attribute (failure)
 * @retval	NULL (failure)
 *
 * @par Side Effects: None
 *
 * @par MT-safe: Yes
 */
static struct attropl * duplicate_attr(struct attropl * pattr)
{
	struct attropl *new_attr = (struct attropl *)
		calloc(1, sizeof(struct attropl));
	if (new_attr == NULL)
		return NULL;
	if (pattr->name)
		if ((new_attr->name = strdup(pattr->name)) == NULL)
			goto err;
	if (pattr->resource)
		if ((new_attr->resource = strdup(pattr->resource)) == NULL)
			goto err;
	if (pattr->value)
		if ((new_attr->value = strdup(pattr->value)) == NULL)
			goto err;
	return new_attr;

err:
	free(new_attr->name);
	free(new_attr->resource);
	free(new_attr->value);
	free(new_attr);
	return NULL;
}

/**
 * @brief
 *	Loops through the attribute list and verifies each attribute
 *
 * @par	Functionality:
 *	1. Calls verify_an_attribute to verify each attribute in a loop\n
 *	2. Adjusts the attribute_list by expanding it appropriately
 *
 * @see
 *	__pbs_verify_attributes\n
 *	verify_an_attribute
 *
 * @param[in]	batch_request	-	Batch Request Type
 * @param[in]	parent_object	-	Parent Object Type
 * @param[in]	cmd		-	Command Type
 * @param[in]	attribute_list	-	list of attributes
 * @param[out]	arg_err_list	-	list holding attribute errors
 *
 * @return	int
 * @retval	0 - No failed attributes
 * @retval	+n - Number of failed attributes (pbs_errno set to last error)
 * @retval	-1 - System error verifying attributes (pbs_errno is set)
 *
 * @par	Side effects:
 *	 pbs_errno set on error
 *
 * @par MT-safe: Yes
 */
int
verify_attributes(int batch_request, int parent_object, int cmd,
	struct attropl *attribute_list,
	struct ecl_attribute_errors **arg_err_list)
{

	struct attropl *pattr = NULL;
	int failure_count = 0;
	int cur_size = 0;
	int err_code = 0;
	struct ecl_attribute_errors *err_list = NULL;
	struct ecl_attrerr *temp = NULL;
	char *msg=NULL;
	int i;
	int verified;
	struct attropl *new_attr;

	err_list = (struct ecl_attribute_errors *)
		malloc(sizeof(struct ecl_attribute_errors));
	if (err_list == NULL) {
		err_code = PBSE_SYSTEM;
		return -1;
	}
	err_list->ecl_numerrors = 0;
	err_list->ecl_attrerr = NULL;

	if ((parent_object == MGR_OBJ_SITE_HOOK) || (parent_object == MGR_OBJ_PBS_HOOK)) {
		/* exempt from attribute checks */
		*arg_err_list = err_list;
		return 0;
	}

	for (pattr = attribute_list; pattr; pattr = pattr->next) {

		err_code = verify_an_attribute(batch_request, parent_object,
			cmd, pattr, &verified, &msg);

		/* now check the op value, for selectjob api*/
		if ((err_code == 0) &&
			(batch_request == PBS_BATCH_SelectJobs)) {
			for (i = 0; i< size_seljobs; i++)
				if (pattr->op == seljobs_opstring_enums[i])
					break;
			if (i == sizeof(seljobs_opstring_enums))
				err_code = PBSE_BADATVAL;
		}

		if (err_code != 0) {
			if (cur_size - failure_count < 1) {
				cur_size += SLOT_INCR_SIZE;
				temp = (struct ecl_attrerr *)
					realloc(err_list->ecl_attrerr,
					cur_size * sizeof(struct ecl_attrerr));
				if (temp == NULL) {
					free_errlist(err_list);
					pbs_errno = PBSE_SYSTEM;
					return -1;
				}
				err_list->ecl_attrerr = temp;
			}
			failure_count++;

			/* keep a copy of the whole attribute, incase attribute
			 * was allocated from stack by caller etc, it might be
			 * lost, and a pointer alone would be of no good
			 */
			new_attr = duplicate_attr(pattr);
			if (new_attr == NULL) {
				free_errlist(err_list);
				pbs_errno = PBSE_SYSTEM;
				return -1;
			}
			err_list->ecl_attrerr[failure_count - 1].ecl_attribute
			= (struct attropl *) new_attr;
			err_list->ecl_attrerr[failure_count - 1].ecl_errcode
			= err_code;
			err_list->ecl_attrerr[failure_count - 1].ecl_errmsg
			= NULL;
			if (msg != NULL) {
				if ((err_list->ecl_attrerr[failure_count - 1].
					ecl_errmsg = strdup(msg)) == NULL) {
					pbs_errno = PBSE_SYSTEM;
					free_errlist(err_list);
					free(msg);
					msg = NULL;
					return -1;
				}
				free(msg);
				msg = NULL;
			}
		}
	}

	if ((failure_count > 0) && (failure_count != cur_size)) {
		temp = (struct ecl_attrerr *)
			realloc(err_list->ecl_attrerr, failure_count *
			sizeof(struct ecl_attrerr));
		if (temp == NULL) {
			free_errlist(err_list);
			pbs_errno = PBSE_SYSTEM;
			return -1;
		}
		err_list->ecl_attrerr = temp;
	}

	err_list->ecl_numerrors = failure_count;
	*arg_err_list = err_list;
	return failure_count;
}

/**
 * @brief
 *	Name: ecl_findattr
 *
 * @par		Functionality:
 *		1. Find the attribute in the list associated with the
 *		parent_object by calling ecl_find_attr_in_def().
 *
 * @see
 *
 * @param[in]	parent_object	-	Parent Object Type
 * @param[in]	pattr		-	list of attributes
 *
 * @return	pointer to the ecl_attribute_def structure
 * @retval	Return value: Address of the ecl_attribute_def structure
 *		associated with the given attribute, NULL if not found
 *
 * @par		Side effects:
 *		None
 *
 * @par MT-safe: Yes
 */
static struct ecl_attribute_def * ecl_findattr(int parent_object,
	struct attropl *pattr)
{
	switch (parent_object) {
		case MGR_OBJ_JOB:
			return (ecl_find_attr_in_def(ecl_job_attr_def, pattr->name,
				ecl_job_attr_size));
		case MGR_OBJ_SERVER:
			return (ecl_find_attr_in_def(ecl_svr_attr_def, pattr->name,
				ecl_svr_attr_size));
		case MGR_OBJ_SCHED:
			return (ecl_find_attr_in_def(ecl_sched_attr_def, pattr->name,
				ecl_sched_attr_size));
		case MGR_OBJ_QUEUE:
			return (ecl_find_attr_in_def(ecl_que_attr_def, pattr->name,
				ecl_que_attr_size));
		case MGR_OBJ_NODE:
		case MGR_OBJ_HOST:
			return (ecl_find_attr_in_def(ecl_node_attr_def, pattr->name,
				ecl_node_attr_size));
		case MGR_OBJ_RESV:
			return (ecl_find_attr_in_def(ecl_resv_attr_def, pattr->name,
				ecl_resv_attr_size));
	}
	return NULL;
}

/**
 * @brief
 * 	find_attr - find attribute definition by name
 *
 * @see
 *
 * @par
 *	Searches array of attribute definition strutures to find one whose name
 *	matches the requested name.
 *
 * @param[in]	attr_def	-	ptr to attribute definition
 * @param[in]	name		-	attribute name to find
 * @param[in]	limit		-	limit on size of defintion array
 *
 * @return	ecl_attribute_def  - ptr to attribute defintion
 * @retval	>= pointer to attribute (success case)
 * @retval	NULL - if didn't find matching name (failed)
 *
 * @par Side Effects:
 *	None
 *
 * @par MT-safe: Yes
 */
static struct ecl_attribute_def * ecl_find_attr_in_def(
	struct ecl_attribute_def *attr_def,
	char *name, int limit)
{
	int index;

	if (attr_def) {
		for (index = 0; index < limit; index++) {
			char *pc = NULL;

			if (strncasecmp(name, attr_def[index].at_name,
					strlen(attr_def[index].at_name)) == 0) {
				pc = name + strlen(attr_def[index].at_name);
				if ((*pc == '\0') || (*pc == '.') || (*pc == ','))
					return &(attr_def[index]);
			}
		}
	}
	return NULL;
}

/**
 * @brief 	Return the type of attribute (public, invisible or read-only)
 *
 * @param[in]	attr_def	-	the attribute to check
 *
 * @return	int
 * @retval	TYPE_ATTR_PUBLIC if the attribute is public
 * @retval	TYPE_ATTR_INVISIBLE if the attribute is a SvWR or SvRD (invisible)
 * @retval	TYPE_ATTR_READONLY otherwise
 *
 * @par Side Effects:
 *	None
 *
 * @par MT-safe: Yes
 */
static int
get_attr_type(struct ecl_attribute_def attr_def)
{
	/*
	 * Consider an attr def public if it has any of the write flags set
	 */
	if (attr_def.at_flags & (ATR_DFLAG_SvWR | ATR_DFLAG_SvRD))
		return TYPE_ATTR_INVISIBLE;
	else if (attr_def.at_flags & (ATR_DFLAG_USWR | ATR_DFLAG_OPWR | ATR_DFLAG_MGWR))
		return TYPE_ATTR_PUBLIC;
	else
		return TYPE_ATTR_READONLY;

}


/**
 * @brief
 *	find_resc_def - find the resource_def structure for a resource
 *	with a given name.
 *
 * @see
 *
 * @param[in]	rscdf		-	ptr to attribute definition strcture
 * @param[in]	name		-	name of resouce
 * @param[in] 	limit		-	number of members in resource_def array
 *
 * @return 	ecl_attribute_def - ptr to attribute definition
 * @retval	pointer to the found resource structure (success)
 * @retval	NULL(failure)
 *
 * @par Side Effects:
 *	None
 *
 * @par MT-safe: Yes
 */
struct ecl_attribute_def *ecl_find_resc_def(
	struct ecl_attribute_def *rscdf, char *name, int limit)
{
	while (limit--) {
		if (strcasecmp(rscdf->at_name, name) == 0)
			return (rscdf);
		rscdf++;
	}
	return NULL;
}

/**
 * @brief
 * 	Returns TRUE if the name passed in is an attribute.
 *
 * @note
 * 	This must not be called with object of type MGR_OBJ_SITE_HOOK or MGR_OBJ_PBS_HOOK.
 *
 * @param[in]	object - type of object
 * @param[in]	name  - name of the attribute
 * @param[in]	attr_type  - type of the attribute
 *
 * @eturn int
 * @retval	TRUE - means if the input is an attribute of the given 'object' type
 *        	    and 'attr_type'.
 * @retval	FALSE - otherwise.
 *
 */
int
is_attr(int object, char *name, int attr_type)
{
	struct ecl_attribute_def *attr_def = NULL;

	if ((object == MGR_OBJ_SITE_HOOK) || (object == MGR_OBJ_PBS_HOOK)) {
		return FALSE;
	}

	else if (object == MGR_OBJ_RSC) {
		return TRUE;
	}

	if ((attr_def = ecl_find_attr_in_def(ecl_svr_attr_def, name, ecl_svr_attr_size)) != NULL) {
		/* Make sure that the attribute types match */
		if (get_attr_type(*attr_def) & attr_type)
			return TRUE;
		else
			return FALSE;
	} else if ((attr_def = ecl_find_attr_in_def(ecl_node_attr_def, name, ecl_node_attr_size)) != NULL) {
		/* Make sure that the attribute types match */
		if (get_attr_type(*attr_def) & attr_type)
			return TRUE;
		else
			return FALSE;
	} else if ((attr_def = ecl_find_attr_in_def(ecl_que_attr_def, name, ecl_que_attr_size)) != NULL) {
		/* Make sure that the attribute types match */
		if (get_attr_type(*attr_def) & attr_type)
			return TRUE;
		else
			return FALSE;
	} else if ((attr_def = ecl_find_attr_in_def(ecl_sched_attr_def, name, ecl_sched_attr_size)) != NULL) {
		/* Make sure that the attribute types match */
		if (get_attr_type(*attr_def) & attr_type)
			return TRUE;
		else
			return FALSE;
	}

	return FALSE;
}

