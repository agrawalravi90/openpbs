/*
 * Copyright (C) 1994-2018 Altair Engineering, Inc.
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
 * @file	pbs_alterjob.c
 * @brief
 * Send the Alter Job request to the server --
 * really an instance of the "manager" request.
 */

#include <pbs_config.h>   /* the master config generated by configure */

#include <stdio.h>
#include <stdlib.h>
#include "libpbs.h"

/**
 * @brief	Convenience function to create attropl list from attrl (shallow copy)
 *
 * @param[in]	attrib - the list to copy
 *
 * @return struct attropl
 * @retval newly allocated attropl list
 * @retval NULL for malloc error
 */
static struct attropl *
attrl_to_attropl(struct attrl *attrib)
{
	struct attropl *ap = NULL;
	struct attropl *ap1 = NULL;

	/* copy the attrl to an attropl */
	while (attrib != NULL) {
		if (ap == NULL) {
			ap1 = ap = MH(struct attropl);
		} else {
			ap->next = MH(struct attropl);
			ap = ap->next;
		}
		if (ap == NULL) {
			pbs_errno = PBSE_SYSTEM;
			return NULL;
		}
		ap->name = attrib->name;
		ap->resource = attrib->resource;
		ap->value = attrib->value;
		ap->op = SET;
		ap->next = NULL;
		attrib = attrib->next;
	}

	return ap1;
}


/**
 * @brief	Convenience function to shallow-free oplist
 *
 * @param[out]	oplist - the list to free
 *
 * @return void
 */
static void
__free_attropl(struct attropl *oplist)
{
	struct attropl *ap = NULL;

	while (oplist != NULL) {
		ap = oplist->next;
		free(oplist);
		oplist = ap;
	}
}

/**
 * @brief
 *	-Send the Alter Job request to the server --
 *	really an instance of the "manager" request.
 *
 * @param[in] c - connection handle
 * @param[in] jobid- job identifier
 * @param[in] attrib - pointer to attribute list
 * @param[in] extend - extend string for encoding req
 *
 * @return	int
 * @retval	0	success
 * @retval	!0	error
 *
 */
int
__pbs_alterjob(int c, char *jobid, struct attrl *attrib, char *extend)
{
	struct attropl *attrib_opl = NULL;
	int i;

	if ((jobid == NULL) || (*jobid == '\0'))
		return (pbs_errno = PBSE_IVALREQ);

	attrib_opl = attrl_to_attropl(attrib);

	i = PBSD_manager(c,
		PBS_BATCH_ModifyJob,
		MGR_CMD_SET,
		MGR_OBJ_JOB,
		jobid,
		attrib_opl,
		extend);

	/* free up the attropl we just created */
	__free_attropl(attrib_opl);

	return i;
}


/**
 * @brief	Send Alter Job request to the server, Asynchronously
 *
 * @param[in] c - connection handle
 * @param[in] jobid- job identifier
 * @param[in] attrib - pointer to attribute list
 * @param[in] extend - extend string for encoding req
 *
 * @return	int
 * @retval	0	success
 * @retval	!0	error
 *
 */
int
pbs_asyalterjob(int c, char *jobid, struct attrl *attrib, char *extend)
{
	struct attropl *attrib_opl = NULL;
	int i;

	if ((jobid == NULL) || (*jobid == '\0'))
		return (pbs_errno = PBSE_IVALREQ);

	/* initialize the thread context data, if not initialized */
	if (pbs_client_thread_init_thread_context() != 0)
		return pbs_errno;

	/* lock pthread mutex here for this connection */
	/* blocking call, waits for mutex release */
	if (pbs_client_thread_lock_connection(c) != 0)
		return pbs_errno;


	/* send the manage request with modifyjob async */
	attrib_opl = attrl_to_attropl(attrib);
	i = PBSD_mgr_put(c, PBS_BATCH_ModifyJob_Async, MGR_CMD_SET, MGR_OBJ_JOB, jobid, attrib_opl, extend, 0, NULL);
	__free_attropl(attrib_opl);

	if (i) {
		(void)pbs_client_thread_unlock_connection(c);
		return i;
	}

	/* unlock the thread lock and update the thread context data */
	if (pbs_client_thread_unlock_connection(c) != 0)
		return pbs_errno;

	return i;

}
