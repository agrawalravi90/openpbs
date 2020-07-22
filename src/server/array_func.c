/*
 * Copyright (C) 1994-2020 Altair Engineering, Inc.
 * For more information, contact Altair at www.altair.com.
 *
 * This file is part of both the OpenPBS software ("OpenPBS")
 * and the PBS Professional ("PBS Pro") software.
 *
 * Open Source License Information:
 *
 * OpenPBS is free software. You can redistribute it and/or modify it under
 * the terms of the GNU Affero General Public License as published by the
 * Free Software Foundation, either version 3 of the License, or (at your
 * option) any later version.
 *
 * OpenPBS is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Affero General Public
 * License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * Commercial License Information:
 *
 * PBS Pro is commercially licensed software that shares a common core with
 * the OpenPBS software.  For a copy of the commercial license terms and
 * conditions, go to: (http://www.pbspro.com/agreement.html) or contact the
 * Altair Legal Department.
 *
 * Altair's dual-license business model allows companies, individuals, and
 * organizations to create proprietary derivative works of OpenPBS and
 * distribute them - whether embedded or bundled with other software -
 * under a commercial license agreement.
 *
 * Use of Altair's trademarks, including but not limited to "PBS™",
 * "OpenPBS®", "PBS Professional®", and "PBS Pro™" and Altair's logos is
 * subject to Altair's trademark licensing policies.
 */

/*
 * array_func.c - Functions which provide basic Job Array functions
 */

#include <pbs_config.h>   /* the master config generated by configure */

#include <unistd.h>
#include <sys/types.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <ctype.h>
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "pbs_ifl.h"
#include "libpbs.h"
#include "list_link.h"
#include "attribute.h"
#include "resource.h"
#include "server_limits.h"
#include "server.h"
#include "job.h"
#include "log.h"
#include "pbs_error.h"
#include "batch_request.h"
#include "pbs_nodes.h"
#include "svrfunc.h"
#include "svrjob.h"
#include "acct.h"
#include <sys/time.h>


/* External data */
extern char *msg_job_end_stat;
extern int   resc_access_perm;

/*
 * list of job attributes to copy from the parent Array job
 * when creating a sub job.
 */
static enum job_atr attrs_to_copy[] = {
	JOB_ATR_jobname,
	JOB_ATR_job_owner,
	JOB_ATR_resc_used,
	JOB_ATR_state,
	JOB_ATR_in_queue,
	JOB_ATR_at_server,
	JOB_ATR_account,
	JOB_ATR_ctime,
	JOB_ATR_errpath,
	JOB_ATR_grouplst,
	JOB_ATR_join,
	JOB_ATR_keep,
	JOB_ATR_mtime,
	JOB_ATR_mailpnts,
	JOB_ATR_mailuser,
	JOB_ATR_nodemux,
	JOB_ATR_outpath,
	JOB_ATR_priority,
	JOB_ATR_qtime,
	JOB_ATR_remove,
	JOB_ATR_rerunable,
	JOB_ATR_resource,
	JOB_ATR_session_id,
	JOB_ATR_shell,
	JOB_ATR_sandbox,
	JOB_ATR_jobdir,
	JOB_ATR_stagein,
	JOB_ATR_stageout,
	JOB_ATR_substate,
	JOB_ATR_userlst,
	JOB_ATR_variables,
	JOB_ATR_euser,
	JOB_ATR_egroup,
	JOB_ATR_hashname,
	JOB_ATR_hopcount,
	JOB_ATR_queuetype,
	JOB_ATR_security,
	JOB_ATR_etime,
	JOB_ATR_refresh,
	JOB_ATR_gridname,
	JOB_ATR_umask,
	JOB_ATR_cred,
	JOB_ATR_runcount,
	JOB_ATR_pset,
	JOB_ATR_eligible_time,
	JOB_ATR_sample_starttime,
	JOB_ATR_executable,
	JOB_ATR_Arglist,
	JOB_ATR_reserve_ID,
	JOB_ATR_project,
	JOB_ATR_run_version,
	JOB_ATR_tolerate_node_failures,
#if defined(PBS_SECURITY) && (PBS_SECURITY == KRB5)
	JOB_ATR_cred_id,
#endif
	JOB_ATR_submit_host,
	JOB_ATR_LAST /* This MUST be LAST	*/
};

/**
 * @brief
 * 			is_job_array - determines if the job id indicates
 *
 * @par	Functionality:
 * Note - subjob index or range may be invalid and not detected as such
 *
 * @param[in]	id - Job Id.
 *
 * @return      Job Type
 * @retval	 0  - A regular job
 * @retval	-1  - A ArrayJob
 * @retval	 2  - A single subjob
 * @retval	-3  - A range of subjobs
 */
int
is_job_array(char *id)
{
	char *pc;

	if ((pc = strchr(id, (int)'[')) == NULL)
		return IS_ARRAY_NO; /* not an ArrayJob nor a subjob (range) */
	if (*++pc == ']')
		return IS_ARRAY_ArrayJob;	/* an ArrayJob */

	/* know it is either a single subjob or an range there of */

	while (isdigit((int)*pc))
		++pc;
	if ((*pc == '-') || (*pc == ','))
		return IS_ARRAY_Range;	/* a range of subjobs */
	else
		return IS_ARRAY_Single;
}

/**
 * @brief
 * 		numindex_to_offset - return the offset into the table for a numerical
 *
 * @param[in]	parent - Pointer to to parent job structure.
 * @param[in]	iindx  - first number of range
 *
 *	@return	Sub job index.
 *	@retval -1	- on error.
 */
int
numindex_to_offset(job *parent, int iindx)
{
	struct ajtrkhd *ptbl = parent->ji_ajtrk;
	int i;

	if (ptbl == NULL || ((iindx - ptbl->tkm_start) % ptbl->tkm_step) != 0 || ptbl->tkm_start > iindx || iindx > ptbl->tkm_end)
		return -1;

	i = (int)((iindx - ptbl->tkm_start) / ptbl->tkm_step);
	/*
	 * ensure we got correct offset by doing
	 * reverse lookup of offset to subjob index
	 */
	if (SJ_TBLIDX_2_IDX(parent, i) != iindx)
		return -1;
	return i;
}

/**
 * @brief
 * 		subjob_index_to_offset - return the offset into the table for an array
 *
 * @param[in]	parent - Pointer to to parent job structure.
 * @param[in]	index  - first number of range
 *
 *	@return	external Sub job index string.
 *	@retval -1	- on error.
 */
int
subjob_index_to_offset(job *parent, char *index)
{
	long nidx;

	if (parent == NULL || parent->ji_ajtrk == NULL || index == NULL || *index == '\0')
		return -1;

	nidx = atoi(index);
	return numindex_to_offset(parent, nidx);
}
/**
 * @brief
 * 		get_index_from_jid - from a subjob job id string
 *
 * @param[in]	newjid - New job id.
 *
 *	@return	index substring
 *	@retval NULL - on error.
 *	@retval ptr - ptr to static char array containing index string if found
 *
 *	@par	MT-safe: No - uses static variables - index, indexlen.
 */
char *
get_index_from_jid(char *newjid)
{
	int           i;
	char         *pcb;
	char         *pce;
	char	     *pnew;
	size_t	      t;
	static char  *index;
	static size_t indexlen = 0;;

	if ((pcb = strchr(newjid, (int)'[')) == NULL)
		return NULL;
	if ((pce = strchr(newjid, (int)']')) == NULL)
		return NULL;
	if (pce <= pcb)
		return NULL;

	if (indexlen == 0) {
		indexlen = pce - pcb;
		index = (char *)malloc(indexlen);
	} else if (indexlen < (pce - pcb)) {
		t = pce - pcb;
		pnew = realloc(index, t);
		if (pnew == NULL)
			return NULL;
		index = pnew;
		indexlen = t;
	}
	if (index == NULL)
		return NULL;


	i = 0;
	while (++pcb < pce)
		index[i++] = *pcb;
	index[i] = '\0';
	return index;
}

/**
 * @brief
 * 		get_queued_subjobs_ct	-	get the number of queued subjobs if pjob is job array else return 1
 *
 * @param[in]	pjob	-	pointer to job structure
 *
 * @return	int
 * @retval	-1	: parse error
 * @retval	positive	: count of subjobs in JOB_ATR_array_indices_remaining if job array else 1
 */
int
get_queued_subjobs_ct(job *pjob)
{
	if (NULL == pjob)
		return -1;

	if (pjob->ji_qs.ji_svrflags & JOB_SVFLG_ArrayJob) {
		if (NULL == pjob->ji_ajtrk)
			return -1;

		return pjob->ji_ajtrk->tkm_subjsct[JOB_STATE_QUEUED];
	}

	return 1;
}
/**
 * @brief
 * 		find_arrayparent - find and return a pointer to the job that is or will be
 * 		the parent of the subjob id string
 *
 * @param[in]	subjobid - sub job id.
 *
 *	@return	parent job
 */
job *
find_arrayparent(char *subjobid)
{
	int   i;
	char  idbuf[PBS_MAXSVRJOBID+1];
	char *pc;

	for (i=0; i<PBS_MAXSVRJOBID; i++) {
		idbuf[i] = *(subjobid + i);
		if (idbuf[i] == '[')
			break;
	}
	idbuf[++i] = ']';
	idbuf[++i] = '\0';
	pc = strchr(subjobid, (int)'.');
	if (pc)
		strcat(idbuf, pc);
	return (find_job(idbuf));
}
/**
 * @brief
 * 		set_subjob_tblstate - set the subjob tracking table state field for
 *		the "offset" entry
 *
 * @param[in]	parent - pointer to parent job.
 * @param[in]	offset - sub job index.
 * @param[in]	newstate - newstate of the sub job.
 *
 *	@return	void
 */
void
set_subjob_tblstate(job *parent, int offset, int newstate)
{
	int  		 oldstate;
	struct ajtrkhd	*ptbl;

	if (offset == -1)
		return;

	if (parent == NULL)
		return;

	ptbl = parent->ji_ajtrk;
	if (ptbl == NULL)
		return;

	oldstate =  ptbl->tkm_tbl[offset].trk_status;
	if (oldstate == newstate)
		return;		/* nothing to do */

	ptbl->tkm_tbl[offset].trk_status = newstate;

	ptbl->tkm_subjsct[oldstate]--;
	ptbl->tkm_subjsct[newstate]++;

	/* set flags in attribute so stat_job will update the attr string */
	ptbl->tkm_flags |= TKMFLG_REVAL_IND_REMAINING;

}
/**
 * @brief
 * 		update_array_indices_remaining_attr - updates array_indices_remaining attribute
 *
 * @param[in,out]	parent - pointer to parent job.
 *
 * @return	void
 */
void
update_array_indices_remaining_attr(job *parent)
{
	struct ajtrkhd	*ptbl = parent->ji_ajtrk;

	if (ptbl->tkm_flags & TKMFLG_REVAL_IND_REMAINING) {
		attribute *premain = &parent->ji_wattr[(int)JOB_ATR_array_indices_remaining];
		char *pnewstr = cvt_range(parent, JOB_STATE_QUEUED);
		if ((pnewstr == NULL) || (*pnewstr == '\0'))
			pnewstr = "-";
		job_attr_def[JOB_ATR_array_indices_remaining].at_free(premain);
		job_attr_def[JOB_ATR_array_indices_remaining].at_decode(premain, 0, 0, pnewstr);
		/* also update value of attribute "array_state_count" */
		update_subjob_state_ct(parent);
		ptbl->tkm_flags &= ~TKMFLG_REVAL_IND_REMAINING;
	}
}
/**
 * @brief
 * 		chk_array_doneness - check if all subjobs are expired and if so,
 *		purge the Array Job itself
 *
 * @param[in,out]	parent - pointer to parent job.
 *
 *	@return	void
 */
void
chk_array_doneness(job *parent)
{
	char acctbuf[40];
	int e;
	int i;
	struct ajtrkhd	*ptbl = parent->ji_ajtrk;

	if (ptbl == NULL)
		return;

	if (ptbl->tkm_flags & (TKMFLG_NO_DELETE | TKMFLG_CHK_ARRAY))
		return;	/* delete of subjobs in progress, or re-entering, so return here */

	if (ptbl->tkm_subjsct[JOB_STATE_QUEUED] + ptbl->tkm_subjsct[JOB_STATE_RUNNING]
			+ ptbl->tkm_subjsct[JOB_STATE_HELD] + ptbl->tkm_subjsct[JOB_STATE_EXITING] == 0) {

		/* Array Job all done, do simple eoj processing */

		for (e=i=0; i<ptbl->tkm_ct; ++i) {
			if (ptbl->tkm_tbl[i].trk_error > 0)
				e = 1;
			else if (ptbl->tkm_tbl[i].trk_error < 0) {
				e = 2;
				break;
			}
		}
		parent->ji_qs.ji_un_type = JOB_UNION_TYPE_EXEC;
		parent->ji_qs.ji_un.ji_exect.ji_momaddr = 0;
		parent->ji_qs.ji_un.ji_exect.ji_momport = 0;
		parent->ji_qs.ji_un.ji_exect.ji_exitstat = e;

		check_block(parent, "");
		if (parent->ji_qs.ji_state == JOB_STATE_BEGUN) {
			/* if BEGUN, issue 'E' account record */
			sprintf(acctbuf, msg_job_end_stat, e);
			account_job_update(parent, PBS_ACCT_LAST);
			set_attr_rsc_used_acct(parent);
			account_jobend(parent, acctbuf, PBS_ACCT_END);

			svr_mailowner(parent, MAIL_END, MAIL_NORMAL, acctbuf);
		}
		if (parent->ji_wattr[(int)JOB_ATR_depend].at_flags & ATR_VFLAG_SET)
			depend_on_term(parent);

		/*
		 * Check if the history of the finished job can be saved or it needs to be purged .
		 */
		ptbl->tkm_flags |= TKMFLG_CHK_ARRAY;

		svr_saveorpurge_finjobhist(parent);
	} else {
		/* Before we do a full save of parent, recalculate "JOB_ATR_array_indices_remaining" here*/
		update_array_indices_remaining_attr(parent);
		job_save_db(parent);
	}
}
/**
 * @brief
 * 		update_subjob_state - update the subjob state in the table entry for
 * 		the subjob and the total counts for each state.
 * 		If job going into EXPIRED state, the job exitstatus is saved in the tbl
 *
 * @param[in]	pjob - pointer to the actual subjob job entry
 * @param[in]	newstate - newstate of the sub job.
 *
 *	@return	void
 */
void
update_subjob_state(job *pjob, int newstate)
{
	int		 len;
	job		*parent;
	char		*pc;
	struct ajtrkhd	*ptbl;

	parent = pjob->ji_parentaj;
	if (parent == NULL)
		return;
	ptbl   = parent->ji_ajtrk;
	if (ptbl == NULL)
		return;

	/* verify that parent job is in fact the parent Array Job */
	pc  = strchr(pjob->ji_qs.ji_jobid, (int)'[');
	len = pc - pjob->ji_qs.ji_jobid - 1;
	if ((strncmp(pjob->ji_qs.ji_jobid, parent->ji_qs.ji_jobid, len) != 0) ||
		(ptbl == NULL))
		return;	/* nope, not the parent */

	set_subjob_tblstate(parent, pjob->ji_subjindx, newstate);
	if (newstate == JOB_STATE_EXPIRED) {
		ptbl->tkm_tbl[pjob->ji_subjindx].trk_error =
			pjob->ji_qs.ji_un.ji_exect.ji_exitstat;

		if (svr_chk_history_conf()) {
			if ((pjob->ji_wattr[(int)JOB_ATR_stageout_status].at_flags) & ATR_VFLAG_SET) {
				ptbl->tkm_tbl[pjob->ji_subjindx].trk_stgout =
					pjob->ji_wattr[(int)JOB_ATR_stageout_status].at_val.at_long;
			}
			if ((pjob->ji_wattr[(int)JOB_ATR_exit_status].at_flags) & ATR_VFLAG_SET) {
				ptbl->tkm_tbl[pjob->ji_subjindx].trk_exitstat = 1;
			}
		}
		ptbl->tkm_tbl[pjob->ji_subjindx].trk_substate = pjob->ji_qs.ji_substate;
	}
	chk_array_doneness(parent);
}
/**
 * @brief
 * 		get_subjob_discarding - return the discarding flag of a subjob given by the parent job
 * 		and integer index into the table for the subjob
 *
 * @param[in]	parent - pointer to the parent job
 * @param[in]	iindx - first number of range
 *
 * @return	status
 * @retval	-1	-  error
 */
int
get_subjob_discarding(job *parent, int iindx)
{
	if (iindx == -1)
		return -1;
	return (parent->ji_ajtrk->tkm_tbl[iindx].trk_discarding);
}
/**
 * @brief
 * 		get_subjob_state - return the state of a subjob given by the parent job
 * 		and integer index into the table for the subjob
 *
 * @param[in]	parent - pointer to the parent job
 * @param[in]	iindx - first number of range
 *
 * @return	status
 * @retval	-1	-  error
 */
int
get_subjob_state(job *parent, int iindx)
{
	if (iindx == -1)
		return -1;
	return (parent->ji_ajtrk->tkm_tbl[iindx].trk_status);
}
/**
 * @brief
 * 		update_subjob_state_ct - update the "array_state_count" attribute of an
 * 		array job
 *
 * @param[in]	pjob - pointer to the job
 *
 * @return	void
 */
void
update_subjob_state_ct(job *pjob)
{
	char *buf;
	static char *statename[] = {
		"Transit", "Queued", "Held", "Waiting", "Running",
		"Exiting", "Expired", "Beginning", "Moved", "Finished" };


	buf = malloc(150);
	if (buf == NULL)
		return;
	buf[0] = '\0';
	sprintf(buf+strlen(buf), "%s:%d ", statename[JOB_STATE_QUEUED],
		pjob->ji_ajtrk->tkm_subjsct[JOB_STATE_QUEUED]);
	sprintf(buf+strlen(buf), "%s:%d ", statename[JOB_STATE_RUNNING],
		pjob->ji_ajtrk->tkm_subjsct[JOB_STATE_RUNNING]);
	sprintf(buf+strlen(buf), "%s:%d ", statename[JOB_STATE_EXITING],
		pjob->ji_ajtrk->tkm_subjsct[JOB_STATE_EXITING]);
	sprintf(buf+strlen(buf), "%s:%d ", statename[JOB_STATE_EXPIRED],
		pjob->ji_ajtrk->tkm_subjsct[JOB_STATE_EXPIRED]);
	if (pjob->ji_wattr[(int)JOB_ATR_array_state_count].at_val.at_str)
		free(pjob->ji_wattr[(int)JOB_ATR_array_state_count].at_val.at_str);

	pjob->ji_wattr[(int)JOB_ATR_array_state_count].at_val.at_str = buf;
	pjob->ji_wattr[(int)JOB_ATR_array_state_count].at_flags |= ATR_SET_MOD_MCACHE;
}
/**
 * @brief
 * 		subst_array_index - Substitute the actual index into the file name
 * 		if this is a sub job and if the array index substitution
 * 		string is in the specified file path.  If, not the original string
 * 		is returned unchanged.
 *
 * @param[in]	pjob - pointer to the job
 * @param[in]	path - name of local or destination
 *
 * @return	path
 */
char *
subst_array_index(job *pjob, char *path)
{
	char *pindorg;
	const char *cvt;
	char trail[MAXPATHLEN + 1];
	job *ppjob = pjob->ji_parentaj;

	if (ppjob == NULL)
		return path;
	if ((pindorg = strstr(path, PBS_FILE_ARRAY_INDEX_TAG)) == NULL)
		return path; /* unchanged */

	cvt = uLTostr(SJ_TBLIDX_2_IDX(ppjob, pjob->ji_subjindx), 10);
	*pindorg = '\0';
	strcpy(trail, pindorg + strlen(PBS_FILE_ARRAY_INDEX_TAG));
	strcat(path, cvt);
	strcat(path, trail);
	return path;
}
/**
 * @brief
 * 		mk_subjob_index_tbl - make the subjob index tracking table
 *		(struct ajtrkhd) based on the number of indexes in the "range"
 *
 * @param[in]	range - subjob index range
 * @param[in]	initialstate - job state
 * @param[out]	pbserror - PBSError to return
 * @param[in]	mode - "actmode" parameter to action function of "array_indices_submitted"
 *
 * @return	ptr to table
 * @retval  NULL	- error
 */
static struct ajtrkhd *
mk_subjob_index_tbl(char *range, int initalstate, int *pbserror, int mode)
{
	int i;
	int j;
	int limit;
	int start;
	int end;
	int step;
	int count;
	char *eptr;
	struct ajtrkhd *trktbl;
	size_t sz;

	i = parse_subjob_index(range, &eptr, &start, &end, &step, &count);
	if (i != 0) {
		*pbserror = PBSE_BADATVAL;
		return NULL; /* parse error */
	}

	if ((mode == ATR_ACTION_NEW) || (mode == ATR_ACTION_ALTER)) {
		if (server.sv_attr[(int) SVR_ATR_maxarraysize].at_flags & ATR_VFLAG_SET)
			limit = server.sv_attr[(int) SVR_ATR_maxarraysize].at_val.at_long;
		else
			limit = PBS_MAX_ARRAY_JOB_DFL; /* default limit 10000 */

		if (count > limit) {
			*pbserror = PBSE_MaxArraySize;
			return NULL; /* parse error */
		}
	}

	sz = sizeof(struct ajtrkhd) + ((count - 1) * sizeof(struct ajtrk));
	trktbl = (struct ajtrkhd *) malloc(sz);

	if (trktbl == NULL) {
		*pbserror = PBSE_SYSTEM;
		return NULL;
	}
	trktbl->tkm_ct = count;
	trktbl->tkm_start = start;
	trktbl->tkm_end = end;
	trktbl->tkm_step = step;
	trktbl->tkm_size = sz;
	trktbl->tkm_flags = 0;
	for (i = 0; i < PBS_NUMJOBSTATE; i++)
		trktbl->tkm_subjsct[i] = 0;
	trktbl->tkm_subjsct[JOB_STATE_QUEUED] = count;
	trktbl->tkm_dsubjsct = 0;
	j = 0;
	for (i = start; i <= end; i += step, j++) {
		trktbl->tkm_tbl[j].trk_status = initalstate;
		trktbl->tkm_tbl[j].trk_error = 0;
		trktbl->tkm_tbl[j].trk_discarding = 0;
		trktbl->tkm_tbl[j].trk_substate = JOB_SUBSTATE_FINISHED;
		trktbl->tkm_tbl[j].trk_stgout = -1;
		trktbl->tkm_tbl[j].trk_exitstat = 0;
		trktbl->tkm_tbl[j].trk_psubjob = NULL;
	}
	return trktbl;
}
/**
 * @brief
 * 		setup_arrayjob_attrs - set up the special attributes of an Array Job
 *		Called as "action" routine for the attribute array_indices_submitted
 *
 * @param[in]	pattr - pointer to special attributes of an Array Job
 * @param[in]	pobj -  pointer to job structure
 * @param[in]	mode -  actmode
 *
 * @return	PBS error
 * @retval  0	- success
 */
int
setup_arrayjob_attrs(attribute *pattr, void *pobj, int mode)
{
	job *pjob = pobj;

	/* set attribute "array" True  and clear "array_state_count" */

	pjob->ji_wattr[(int)JOB_ATR_array].at_val.at_long = 1;
	pjob->ji_wattr[(int)JOB_ATR_array].at_flags = ATR_SET_MOD_MCACHE;
	job_attr_def[(int)JOB_ATR_array_state_count].at_free(&pjob->ji_wattr[(int)JOB_ATR_array_state_count]);

	if ((mode == ATR_ACTION_NEW) || (mode == ATR_ACTION_RECOV)) {
		int pbs_error = PBSE_BADATVAL;
		if (pjob->ji_ajtrk)
			free(pjob->ji_ajtrk);
		if ((pjob->ji_ajtrk = mk_subjob_index_tbl(pjob->ji_wattr[(int)JOB_ATR_array_indices_submitted].at_val.at_str,
			                                      JOB_STATE_QUEUED, &pbs_error, mode)) == NULL)
			return pbs_error;
	}

	if (mode == ATR_ACTION_RECOV) {
		/* set flags in attribute so stat_job will update the attr string */
		pjob->ji_ajtrk->tkm_flags |= TKMFLG_REVAL_IND_REMAINING;

		return (PBSE_NONE);
	}

	if ((mode != ATR_ACTION_ALTER) && (mode != ATR_ACTION_NEW))
		return PBSE_BADATVAL;

	if (is_job_array(pjob->ji_qs.ji_jobid) != IS_ARRAY_ArrayJob)
		return PBSE_BADATVAL;	/* not an Array Job */

	if (mode == ATR_ACTION_ALTER) {
		if (pjob->ji_qs.ji_state != JOB_STATE_QUEUED)
			return PBSE_MODATRRUN;	/* cannot modify once begun */

		/* clear "array_indices_remaining" so can be reset */

		job_attr_def[(int)JOB_ATR_array_indices_remaining].at_free(&pjob->ji_wattr[(int)JOB_ATR_array_indices_remaining]);
	}

	/* set "array_indices_remaining" if not already set */
	if ((pjob->ji_wattr[(int)JOB_ATR_array_indices_remaining].at_flags & ATR_VFLAG_SET) == 0)
		job_attr_def[(int)JOB_ATR_array_indices_remaining].at_decode(&pjob->ji_wattr[(int)JOB_ATR_array_indices_remaining], NULL, NULL, pattr->at_val.at_str);


	/* set other Array related fields in the job structure */

	pjob->ji_qs.ji_svrflags |= JOB_SVFLG_ArrayJob;


	return (PBSE_NONE);
}
/**
 * @brief
 * 		fixup_arrayindicies - set state of subjobs based on array_indicies_remaining
 * @par	Functionality:
 * 		This is used when a job is being qmoved into this server.
 * 		It is necessary that the indices_submitted be first to cause the
 * 		creation of the tracking tbl. If the job is created here, no need of fix indicies
 *
 * @param[in]	pattr - pointer to special attributes of an Array Job
 * @param[in]	pobj -  pointer to job structure
 * @param[in]	mode -  actmode
 * @return	PBS error
 * @retval  0	- success
 */
int
fixup_arrayindicies(attribute *pattr, void *pobj, int mode)
{
	int i;
	int start;
	int end;
	int step;
	int count;
	char *ep;
	char *str;
	job *pjob = pobj;

	if (!pjob || !(pjob->ji_qs.ji_svrflags & JOB_SVFLG_ArrayJob) || !pjob->ji_ajtrk)
		return PBSE_BADATVAL;

	if ((pjob->ji_qs.ji_svrflags & JOB_SVFLG_HERE) && mode == ATR_ACTION_NEW)
		return PBSE_NONE;

	/* set all sub jobs expired, then reset queued the ones in "remaining" */
	for (i = 0; i < pjob->ji_ajtrk->tkm_ct; i++)
		set_subjob_tblstate(pjob, i, JOB_STATE_EXPIRED);

	str = pattr->at_val.at_str;
	while (1) {
		if (parse_subjob_index(str, &ep, &start, &end, &step, &count) != 0)
			break;
		for (i = start; i <= end; i += step)
			set_subjob_tblstate(pjob, numindex_to_offset(pjob, i), JOB_STATE_QUEUED);
		str = ep;
	}

	return (PBSE_NONE);
}
/**
 * @brief
 * 		create_subjob - create a Subjob from the parent Array Job
 * 		Certain attributes are changed or left out
 * @param[in]	parent - pointer to parent Job
 * @param[in]	newjid -  new job id
 * @param[in]	rc -  return code
 * @return	pointer to new job
 * @retval  NULL	- error
 */
job *
create_subjob(job *parent, char *newjid, int *rc)
{
	pbs_list_head  attrl;
	int	   i;
	int	   j;
	int	   indx;
	char	  *index;
	attribute_def *pdef;
	attribute *ppar;
	attribute *psub;
	svrattrl  *psatl;
	job 	  *subj;
	long	   eligibletime;
	long	    time_msec;
	struct timeval	    tval;
	char tmp_path[MAXPATHLEN + 1];

	if ((parent->ji_qs.ji_svrflags & JOB_SVFLG_ArrayJob) == 0) {
		*rc = PBSE_IVALREQ;
		return NULL;	/* parent not an array job */
	}

	/* find and copy the index */

	if ((index = get_index_from_jid(newjid)) == NULL) {
		*rc = PBSE_IVALREQ;
		return NULL;
	}
	if ((indx = subjob_index_to_offset(parent, index)) == -1) {
		*rc = PBSE_UNKJOBID;
		return NULL;
	}
	if (parent->ji_ajtrk->tkm_tbl[indx].trk_status != JOB_STATE_QUEUED) {
		*rc = PBSE_BADSTATE;
		return NULL;
	}

	/*
	 * allocate and clear basic structure
	 * cannot copy job attributes because cannot share strings and other
	 * malloc-ed data,  so copy ji_qs as a whole and then copy the
	 * non-saved items before ji_qs.
	 */

	if ((subj = job_alloc()) == NULL) {
		*rc = PBSE_SYSTEM;
		return NULL;
	}
	subj->ji_qs = parent->ji_qs;	/* copy the fixed save area */
	parent->ji_ajtrk->tkm_tbl[indx].trk_psubjob = subj;
	subj->ji_qhdr     = parent->ji_qhdr;
	subj->ji_myResv   = parent->ji_myResv;
	subj->ji_parentaj = parent;
	strcpy(subj->ji_qs.ji_jobid, newjid);	/* replace job id */
	*subj->ji_qs.ji_fileprefix = '\0';
	subj->ji_subjindx = indx;

	/*
	 * now that is all done, copy the required attributes by
	 * encoding and then decoding into the new array.  Then add the
	 * subjob specific attributes.
	 */

	resc_access_perm = ATR_DFLAG_ACCESS;
	CLEAR_HEAD(attrl);
	for (i = 0; attrs_to_copy[i] != JOB_ATR_LAST; i++) {
		j    = (int)attrs_to_copy[i];
		ppar = &parent->ji_wattr[j];
		psub = &subj->ji_wattr[j];
		pdef = &job_attr_def[j];

		if (pdef->at_encode(ppar, &attrl, pdef->at_name, NULL,
			ATR_ENCODE_MOM, &psatl) > 0) {
			for (psatl = (svrattrl *)GET_NEXT(attrl); psatl;
				psatl = ((svrattrl *)GET_NEXT(psatl->al_link))) {
				pdef->at_decode(psub, psatl->al_name, psatl->al_resc,
					psatl->al_value);
			}
			/* carry forward the default bit if set */
			psub->at_flags |= (ppar->at_flags & ATR_VFLAG_DEFLT);
			free_attrlist(&attrl);
		}
	}

	psub = &subj->ji_wattr[(int)JOB_ATR_array_id];
	job_attr_def[(int)JOB_ATR_array_id].at_decode(psub, NULL, NULL,
		parent->ji_qs.ji_jobid);

	psub = &subj->ji_wattr[(int)JOB_ATR_array_index];
	job_attr_def[(int)JOB_ATR_array_index].at_decode(psub, NULL, NULL, index);

	/* Lastly, set or clear a few flags and link in the structure */

	subj->ji_qs.ji_svrflags &= ~JOB_SVFLG_ArrayJob;
	subj->ji_qs.ji_svrflags |=  JOB_SVFLG_SubJob;
	subj->ji_qs.ji_substate = JOB_SUBSTATE_TRANSICM;
	svr_setjobstate(subj, JOB_STATE_QUEUED, JOB_SUBSTATE_QUEUED);
	subj->ji_wattr[(int)JOB_ATR_state].at_flags    |= ATR_VFLAG_SET;
	subj->ji_wattr[(int)JOB_ATR_substate].at_flags |= ATR_VFLAG_SET;

	/* subjob needs to borrow eligible time from parent job array.
	 * expecting only to accrue eligible_time and nothing else.
	 */
	if (server.sv_attr[(int)SVR_ATR_EligibleTimeEnable].at_val.at_long == 1) {

		eligibletime = parent->ji_wattr[(int)JOB_ATR_eligible_time].at_val.at_long;

		if (parent->ji_wattr[(int)JOB_ATR_accrue_type].at_val.at_long == JOB_ELIGIBLE)
			eligibletime += subj->ji_wattr[(int)JOB_ATR_sample_starttime].at_val.at_long - parent->ji_wattr[(int)JOB_ATR_sample_starttime].at_val.at_long;

		subj->ji_wattr[(int)JOB_ATR_eligible_time].at_val.at_long = eligibletime;
		subj->ji_wattr[(int)JOB_ATR_eligible_time].at_flags |= ATR_SET_MOD_MCACHE;

	}

	gettimeofday(&tval, NULL);
	time_msec = (tval.tv_sec * 1000L) + (tval.tv_usec/1000L);
	/* set the queue rank attribute */
	subj->ji_wattr[(int)JOB_ATR_qrank].at_val.at_long = time_msec;
	subj->ji_wattr[(int)JOB_ATR_qrank].at_flags |= ATR_SET_MOD_MCACHE;
	if (svr_enquejob(subj) != 0) {
		job_purge(subj);
		*rc = PBSE_IVALREQ;
		return NULL;
	}

	psub = &subj->ji_wattr[JOB_ATR_outpath];
	snprintf(tmp_path, MAXPATHLEN + 1, "%s", psub->at_val.at_str);
	job_attr_def[JOB_ATR_outpath].at_decode(psub, NULL, NULL,
		subst_array_index(subj, tmp_path));

	psub = &subj->ji_wattr[JOB_ATR_errpath];
	snprintf(tmp_path, MAXPATHLEN + 1, "%s", psub->at_val.at_str);
	job_attr_def[JOB_ATR_errpath].at_decode(psub, NULL, NULL,
		subst_array_index(subj, tmp_path));

	*rc = PBSE_NONE;
	return subj;
}

/**
 * @brief
 *	 	Duplicate the existing batch request for a running subjob
 *
 * @param[in]	opreq	- the batch status request structure to duplicate
 * @param[in]	pjob	- the parent job structure of the subjob
 * @param[in]	func	- the function to call after duplicating the batch
 *			   structure.
 * @par
 *		1. duplicate the batch request
 *		2. replace the job id with the one from the running subjob
 *		3. link the new batch request to the original and incr its ref ct
 *		4. call the "func" with the new batch request and job
 * @note
 *		Currently, this is called in PBS_Batch_DeleteJob, PBS_Batch_SignalJob,
 *		PBS_Batch_Rerun, and PBS_Batch_RunJob subjob requests.
 *		For any other request types, be sure to add another switch case below
 *		(matching request type).
 */
void
dup_br_for_subjob(struct batch_request *opreq, job *pjob, void (*func)(struct batch_request *, job *))
{
	struct batch_request  *npreq;

	npreq = alloc_br(opreq->rq_type);
	if (npreq == NULL)
		return;

	npreq->rq_perm    = opreq->rq_perm;
	npreq->rq_fromsvr = opreq->rq_fromsvr;
	npreq->rq_conn = opreq->rq_conn;
	npreq->rq_orgconn = opreq->rq_orgconn;
	npreq->rq_time    = opreq->rq_time;
	strcpy(npreq->rq_user, opreq->rq_user);
	strcpy(npreq->rq_host, opreq->rq_host);
	npreq->rq_extend  = opreq->rq_extend;
	npreq->rq_reply.brp_choice = BATCH_REPLY_CHOICE_NULL;
	npreq->rq_refct   = 0;

	/* for each type, update the job id with the one from the new job */

	switch (opreq->rq_type) {
		case PBS_BATCH_DeleteJob:
			npreq->rq_ind.rq_delete = opreq->rq_ind.rq_delete;
			strcpy(npreq->rq_ind.rq_delete.rq_objname,
				pjob->ji_qs.ji_jobid);
			break;
		case PBS_BATCH_SignalJob:
			npreq->rq_ind.rq_signal = opreq->rq_ind.rq_signal;
			strcpy(npreq->rq_ind.rq_signal.rq_jid,
				pjob->ji_qs.ji_jobid);
			break;
		case PBS_BATCH_Rerun:
			strcpy(npreq->rq_ind.rq_rerun,
				pjob->ji_qs.ji_jobid);
			break;
		case PBS_BATCH_RunJob:
			npreq->rq_ind.rq_run = opreq->rq_ind.rq_run;
			strcpy(npreq->rq_ind.rq_run.rq_jid,
				pjob->ji_qs.ji_jobid);
			break;
		default:
			delete_link(&npreq->rq_link);
			free(npreq);
			return;
	}

	npreq->rq_parentbr = opreq;
	opreq->rq_refct++;

	func(npreq, pjob);
}

/**
 * @brief
 * 		mk_subjob_id - make (in a static array) a jobid for a subjob based on
 * 		the parent jobid and the offset into the tracking table for the subjob
 * @param[in]	parent - pointer to parent Job
 * @param[in]	offset -  sub job index.
 * @return	job id
 * @par	MT-safe: No - uses a global buffer, "jid".
 */
char *
mk_subjob_id(job *parent, int offset)
{
	static char jid[PBS_MAXSVRJOBID+1];
	char        hold[PBS_MAXSVRJOBID+1];
	char        index[20];
	char       *pb;

	sprintf(index, "%d", SJ_TBLIDX_2_IDX(parent, offset));
	strcpy(jid, parent->ji_qs.ji_jobid);

	pb = strchr(jid, (int)']');
	strcpy(hold, pb);		/* "].hostname" section */

	pb = strchr(jid, (int)'[');	/* "seqnum[" section */
	*(pb+1) = '\0';
	strcat(jid, index);
	strcat(jid, hold);
	return jid;
}
/**
 * @brief
 * 		cvt-range - convert entries in subjob index table which are in "state"
 * 		to a range of indices of subjobs.  range will be of form:
 * 		X,X-Y:Z,...
 * @param[in]	pjob - job pointer
 * @param[in]	state -  job state.
 * @return	Pointer to static buffer
 * @par	MT-safe: No - uses a global buffer, "buf" and "buflen".
 */
char *
cvt_range(job *pjob, int state)
{
	unsigned int first; /* first of a pair or range   */
	unsigned int next;  /* next one we are looking at */
	unsigned int last;
	int pcomma = 0;
	static char *buf = NULL;
	static size_t buflen = 0;
	struct ajtrkhd *trktbl = pjob->ji_ajtrk;

	if (trktbl == NULL)
		return NULL;

	if (buf == NULL) {
		buflen = 1000;
		if ((buf = (char *) malloc(buflen)) == NULL)
			return NULL;
	}
	*buf = '\0'; /* initialize buf to empty */
	first = 0;
	while (first < trktbl->tkm_ct) {

		if ((buflen - strlen(buf)) < 20) {
			char *tmpbuf;
			/* expand buf */
			buflen += 500;
			tmpbuf = realloc(buf, buflen);
			if (tmpbuf == NULL)
				return NULL;
			buf = tmpbuf;
		}

		/* find first incompleted entry */
		if (trktbl->tkm_tbl[first].trk_status == state) {
			last = first;
			next = first + 1;
			/* add "first" or ",first" */
			if (pcomma)
				sprintf(buf + strlen(buf), ",");
			else
				pcomma = 1;

			sprintf(buf + strlen(buf), "%d", SJ_TBLIDX_2_IDX(pjob, first));

			/* find next incomplete entry */

			while (next < trktbl->tkm_ct) {
				if (trktbl->tkm_tbl[next].trk_status == state) {
					last = next++;
				} else {
					break;
				}
			}
			if (last > (first + 1)) {
				if (trktbl->tkm_step > 1)
					sprintf(buf + strlen(buf), "-%d:%d", SJ_TBLIDX_2_IDX(pjob, last), trktbl->tkm_step);
				else
					sprintf(buf + strlen(buf), "-%d", SJ_TBLIDX_2_IDX(pjob, last));
			} else if (last > first) {
				sprintf(buf + strlen(buf), ",%d", SJ_TBLIDX_2_IDX(pjob, last));
			}
			first = last + 1;
		} else {
			first++;
		}
	}

	return buf;
}
/**
 * @brief
 *		parse_subjob_index - parse a subjob index range of the form:
 *		START[-END[:STEP]][,...]
 *		Each call parses up to the first comma or if no comma the end of
 *		the string or a ']'
 * @param[in]	pc	-	range of sub jobs
 * @param[out]	ep	-	ptr to character that terminated scan (comma or new-line)
 * @param[out]	pstart	-	first number of range
 * @param[out]	pend	-	maximum value in range
 * @param[out]	pstep	-	stepping factor
 * @param[out]	pcount -	number of entries in this section of the range
 *
 * @return	integer
 * @retval	0	- success
 * @retval	1	- no (more) indices are found
 * @retval	-1	- parse/format error
 */
int
parse_subjob_index(char *pc, char **ep, int *pstart, int *pend, int *pstep, int *pcount)
{
	int start;
	int end;
	int step;
	char *eptr;

	while (isspace((int) *pc) || (*pc == ','))
		pc++;
	if ((*pc == '\0') || (*pc == ']')) {
		*pcount = 0;
		*ep = pc;
		return (1);
	}

	if (!isdigit((int) *pc)) {
		/* Invalid format, 1st char not digit */
		return (-1);
	}
	start = (int) strtol(pc, &eptr, 10);
	pc = eptr;
	while (isspace((int) *pc))
		pc++;
	if ((*pc == ',') || (*pc == '\0') || (*pc == ']')) {
		/* "X," or "X" case */
		end = start;
		step = 1;
		if (*pc == ',')
			pc++;
	} else {
		/* should be X-Y[:Z] case */
		if (*pc != '-') {
			/* Invalid format, not in X-Y format */
			*pcount = 0;
			return (-1);
		}
		end = (int) strtol(++pc, &eptr, 10);
		pc = eptr;
		if (isspace((int) *pc))
			pc++;
		if ((*pc == '\0') || (*pc == ',') || (*pc == ']')) {
			step = 1;
		} else if (*pc++ != ':') {
			/* Invalid format, not in X-Y:z format */
			*pcount = 0;
			return (-1);
		} else {
			while (isspace((int) *pc))
				pc++;
			step = (int) strtol(pc, &eptr, 10);
			pc = eptr;
			while (isspace((int) *pc))
				pc++;
			if (*pc == ',')
				pc++;
		}

		/* y must be greater than x for a range and z must be greater 0 */
		if ((start >= end) || (step < 1))
			return (-1);
	}

	*ep = pc;
	/* now compute the number of extires ((end + 1) - start + (step - 1)) / step = (end - start + step) / step */
	*pcount = (end - start + step) / step;
	*pstart = start;
	*pend = end;
	*pstep = step;
	return (0);
}
