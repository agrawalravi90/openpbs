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
 * @file    job_recov_db.c
 *
 * @brief
 * 		job_recov_db.c - This file contains the functions to record a job
 *		data struture to database and to recover it from database.
 *
 *		The data is recorded in the database
 *
 * Functions included are:
 *
 *	job_save_db()         -	save job to database
 *	job_or_resv_save_db() -	save to database (job/reservation)
 *	job_recov_db()        - recover(read) job from database
 *	job_or_resv_recov_db() -	recover(read) job/reservation from database
 *	svr_to_db_job		  -	Load a server job object to a database job object
 *	db_to_svr_job		  - Load data from database job object to a server job object
 *	svr_to_db_resv		  -	Load data from server resv object to a database resv object
 *	db_to_svr_resv		  -	Load data from database resv object to a server resv object
 *	resv_save_db		  -	Save resv to database
 *	resv_recov_db		  - Recover resv from database
 *
 */


#include <pbs_config.h>   /* the master config generated by configure */

#include <stdio.h>
#include <sys/types.h>

#ifndef WIN32
#include <sys/param.h>
#endif

#include "pbs_ifl.h"
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

#include <unistd.h>
#include "server_limits.h"
#include "list_link.h"
#include "attribute.h"

#ifdef WIN32
#include <sys/stat.h>
#include <io.h>
#include <windows.h>
#include "win.h"
#endif

#include "job.h"
#include "reservation.h"
#include "queue.h"
#include "log.h"
#include "pbs_nodes.h"
#include "svrfunc.h"
#include <memory.h>
#include "libutil.h"
#include "pbs_db.h"


#define MAX_SAVE_TRIES 3

#ifndef PBS_MOM
extern pbs_db_conn_t	*svr_db_conn;
extern pbs_list_head svr_queues;
#endif

#ifdef NAS /* localmod 005 */
/* External Functions Called */
extern int save_attr_db(pbs_db_conn_t *conn, pbs_db_attr_info_t *p_attr_info,
	struct attribute_def *padef, struct attribute *pattr,
	int numattr, int newparent);
extern int recov_attr_db(pbs_db_conn_t *conn,
	void *parent,
	pbs_db_attr_info_t *p_attr_info,
	struct attribute_def *padef,
	struct attribute *pattr,
	int limit,
	int unknown);
#endif /* localmod 005 */

/* global data items */
extern time_t time_now;

#ifndef PBS_MOM

extern job *refresh_job(pbs_db_job_info_t *dbjob, int *refreshed);
extern resc_resv *refresh_resv(char *);
extern pbs_list_head	svr_allresvs;

/**
 * @brief
 *		Load a server job object to a database job object
 *
 * @see
 * 		job_save_db
 *
 * @param[in]	pjob - Address of the job in the server
 * @param[out]	dbjob - Address of the database job object
 * @param[in]   updatetype - Quick or full update
 *
 * @retval   !=0  Failure
 * @retval   0    Success
 */
static int
svr_to_db_job(job *pjob, pbs_db_job_info_t *dbjob, int updatetype)
{
	memset(dbjob, 0, sizeof(pbs_db_job_info_t));
	strcpy(dbjob->ji_jobid, pjob->ji_qs.ji_jobid);
	dbjob->ji_state     = pjob->ji_qs.ji_state;
	dbjob->ji_substate  = pjob->ji_qs.ji_substate;
	dbjob->ji_svrflags  = pjob->ji_qs.ji_svrflags;
	dbjob->ji_numattr   = pjob->ji_qs.ji_numattr;
	dbjob->ji_ordering  = pjob->ji_qs.ji_ordering;
	dbjob->ji_priority  = pjob->ji_qs.ji_priority;
	dbjob->ji_stime     = pjob->ji_qs.ji_stime;
	dbjob->ji_endtBdry  = pjob->ji_qs.ji_endtBdry;
	strcpy(dbjob->ji_queue, pjob->ji_qs.ji_queue);
	strcpy(dbjob->ji_destin, pjob->ji_qs.ji_destin);
	dbjob->ji_un_type   = pjob->ji_qs.ji_un_type;
	if (pjob->ji_qs.ji_un_type == JOB_UNION_TYPE_NEW) {
		dbjob->ji_fromsock  = pjob->ji_qs.ji_un.ji_newt.ji_fromsock;
		dbjob->ji_fromaddr  = pjob->ji_qs.ji_un.ji_newt.ji_fromaddr;
	} else if (pjob->ji_qs.ji_un_type == JOB_UNION_TYPE_EXEC) {
		dbjob->ji_momaddr   = pjob->ji_qs.ji_un.ji_exect.ji_momaddr;
		dbjob->ji_momport   = pjob->ji_qs.ji_un.ji_exect.ji_momport;
		dbjob->ji_exitstat  = pjob->ji_qs.ji_un.ji_exect.ji_exitstat;
	} else if (pjob->ji_qs.ji_un_type == JOB_UNION_TYPE_ROUTE) {
		dbjob->ji_quetime   = pjob->ji_qs.ji_un.ji_routet.ji_quetime;
		dbjob->ji_rteretry  = pjob->ji_qs.ji_un.ji_routet.ji_rteretry;
	} else if (pjob->ji_qs.ji_un_type == JOB_UNION_TYPE_MOM) {
		dbjob->ji_exitstat  = pjob->ji_qs.ji_un.ji_momt.ji_exitstat;
	}

	/* extended portion */
	strcpy(dbjob->ji_4jid, pjob->ji_extended.ji_ext.ji_4jid);
	strcpy(dbjob->ji_4ash, pjob->ji_extended.ji_ext.ji_4ash);
	dbjob->ji_credtype  = pjob->ji_extended.ji_ext.ji_credtype;
	dbjob->ji_qrank = pjob->ji_wattr[(int)JOB_ATR_qrank].at_val.at_long;

	if (updatetype != PBS_UPDATE_DB_QUICK) {
		if ((encode_attr_db(job_attr_def,
			pjob->ji_wattr,
			(int)JOB_ATR_LAST, &dbjob->attr_list, 0)) != 0)
			return -1;
	}

	return 0;
}

/**
 * @brief
 *		Load data from database job object to a server job object
 *
 * @see
 * 		job_recov_db
 *
 * @param[out]	pjob - Address of the job in the server
 * @param[in]	dbjob - Address of the database job object
 *
 * @retval   !=0  Failure
 * @retval   0    Success
 */
static int
db_to_svr_job(job *pjob,  pbs_db_job_info_t *dbjob)
{
	/* Variables assigned constant values are not stored in the DB */
	pjob->ji_qs.ji_jsversion = JSVERSION;
	strcpy(pjob->ji_savetm, dbjob->ji_savetm);
	strcpy(pjob->ji_qs.ji_jobid, dbjob->ji_jobid);
	pjob->ji_qs.ji_state = dbjob->ji_state;
	pjob->ji_qs.ji_substate = dbjob->ji_substate;
	/* dbjob->ji_svrflags fetching wrong value from database, thus commented out.
	 *  will fix it proper later.
	 */
	//pjob->ji_qs.ji_svrflags = dbjob->ji_svrflags;
	pjob->ji_qs.ji_numattr = dbjob->ji_numattr ;
	pjob->ji_qs.ji_ordering = dbjob->ji_ordering;
	pjob->ji_qs.ji_priority = dbjob->ji_priority;
	pjob->ji_qs.ji_stime = dbjob->ji_stime;
	pjob->ji_qs.ji_endtBdry = dbjob->ji_endtBdry;
	strcpy(pjob->ji_qs.ji_queue, dbjob->ji_queue);
	strcpy(pjob->ji_qs.ji_destin, dbjob->ji_destin);
	pjob->ji_qs.ji_fileprefix[0] = 0;
	pjob->ji_qs.ji_un_type = dbjob->ji_un_type;
	if (pjob->ji_qs.ji_un_type == JOB_UNION_TYPE_NEW) {
		pjob->ji_qs.ji_un.ji_newt.ji_fromsock = dbjob->ji_fromsock;
		pjob->ji_qs.ji_un.ji_newt.ji_fromaddr = dbjob->ji_fromaddr;
		pjob->ji_qs.ji_un.ji_newt.ji_scriptsz = 0;
	} else if (pjob->ji_qs.ji_un_type == JOB_UNION_TYPE_EXEC) {
		pjob->ji_qs.ji_un.ji_exect.ji_momaddr = dbjob->ji_momaddr;
		pjob->ji_qs.ji_un.ji_exect.ji_momport = dbjob->ji_momport;
		pjob->ji_qs.ji_un.ji_exect.ji_exitstat = dbjob->ji_exitstat;
	} else if (pjob->ji_qs.ji_un_type == JOB_UNION_TYPE_ROUTE) {
		pjob->ji_qs.ji_un.ji_routet.ji_quetime = dbjob->ji_quetime;
		pjob->ji_qs.ji_un.ji_routet.ji_rteretry = dbjob->ji_rteretry;
	} else if (pjob->ji_qs.ji_un_type == JOB_UNION_TYPE_MOM) {
		pjob->ji_qs.ji_un.ji_momt.ji_svraddr = 0;
		pjob->ji_qs.ji_un.ji_momt.ji_exitstat = dbjob->ji_exitstat;
		pjob->ji_qs.ji_un.ji_momt.ji_exuid = 0;
		pjob->ji_qs.ji_un.ji_momt.ji_exgid = 0;
	}

	/* extended portion */
#if defined(__sgi)
	pjob->ji_extended.ji_ext.ji_jid = 0;
	pjob->ji_extended.ji_ext.ji_ash = 0;
#else
	strcpy(pjob->ji_extended.ji_ext.ji_4jid, dbjob->ji_4jid);
	strcpy(pjob->ji_extended.ji_ext.ji_4ash, dbjob->ji_4ash);
#endif
	pjob->ji_extended.ji_ext.ji_credtype = dbjob->ji_credtype;

	if ((decode_attr_db(pjob, &dbjob->attr_list, job_attr_def,
				pjob->ji_wattr,
				(int)JOB_ATR_LAST, (int) JOB_ATR_UNKN)) != 0)
		return -1;

	return 0;
}

/**
 * @brief
 *		Load data from server resv object to a database resv object
 *
 * @see
 * 		resv_save_db
 *
 * @param[in]	presv - Address of the resv in the server
 * @param[out]  dbresv - Address of the database resv object
 *
 * @retval   !=0  Failure
 * @retval   0    Success
 */
static int
svr_to_db_resv(resc_resv *presv,  pbs_db_resv_info_t *dbresv, int updatetype)
{
	memset(dbresv, 0, sizeof(pbs_db_resv_info_t));
	strcpy(dbresv->ri_resvid, presv->ri_qs.ri_resvID);
	strcpy(dbresv->ri_queue, presv->ri_qs.ri_queue);
	dbresv->ri_duration = presv->ri_qs.ri_duration;
	dbresv->ri_etime = presv->ri_qs.ri_etime;
	dbresv->ri_un_type = presv->ri_qs.ri_un_type;
	if (dbresv->ri_un_type == RESV_UNION_TYPE_NEW) {
		dbresv->ri_fromaddr = presv->ri_qs.ri_un.ri_newt.ri_fromaddr;
		dbresv->ri_fromsock = presv->ri_qs.ri_un.ri_newt.ri_fromsock;
	}
	dbresv->ri_numattr = presv->ri_qs.ri_numattr;
	dbresv->ri_resvTag = presv->ri_qs.ri_resvTag;
	dbresv->ri_state = presv->ri_qs.ri_state;
	dbresv->ri_stime = presv->ri_qs.ri_stime;
	dbresv->ri_substate = presv->ri_qs.ri_substate;
	dbresv->ri_svrflags = presv->ri_qs.ri_svrflags;
	dbresv->ri_tactive = presv->ri_qs.ri_tactive;
	dbresv->ri_type = presv->ri_qs.ri_type;

	if (updatetype != PBS_UPDATE_DB_QUICK) {
		if ((encode_attr_db(resv_attr_def,
				presv->ri_wattr,
				(int)RESV_ATR_LAST, &dbresv->attr_list, 0)) != 0)
			return -1;
	}
	return 0;
}

/**
 * @brief
 *		Load data from database resv object to a server resv object
 *
 * @see
 * 		resv_recov_db
 *
 * @param[out]	presv - Address of the resv in the server
 * @param[in]	dbresv - Address of the database resv object
 *
 * @retval   !=0  Failure
 * @retval   0    Success
 */
static int
db_to_svr_resv(resc_resv *presv, pbs_db_resv_info_t *pdresv)
{
	strcpy(presv->ri_qs.ri_resvID, pdresv->ri_resvid);
	strcpy(presv->ri_qs.ri_queue, pdresv->ri_queue);
	presv->ri_qs.ri_duration = pdresv->ri_duration;
	presv->ri_qs.ri_etime = pdresv->ri_etime;
	presv->ri_qs.ri_un_type = pdresv->ri_un_type;
	if (pdresv->ri_un_type == RESV_UNION_TYPE_NEW) {
		presv->ri_qs.ri_un.ri_newt.ri_fromaddr = pdresv->ri_fromaddr;
		presv->ri_qs.ri_un.ri_newt.ri_fromsock = pdresv->ri_fromsock;
	}
	presv->ri_qs.ri_numattr = pdresv->ri_numattr;
	presv->ri_qs.ri_resvTag = pdresv->ri_resvTag;
	presv->ri_qs.ri_state = pdresv->ri_state;
	presv->ri_qs.ri_stime = pdresv->ri_stime;
	presv->ri_qs.ri_substate = pdresv->ri_substate;
	presv->ri_qs.ri_svrflags = pdresv->ri_svrflags;
	presv->ri_qs.ri_tactive = pdresv->ri_tactive;
	presv->ri_qs.ri_type = pdresv->ri_type;
	strcpy(presv->ri_savetm, pdresv->ri_savetm);

	if ((decode_attr_db(presv, &pdresv->attr_list, resv_attr_def,
		presv->ri_wattr,
		(int) RESV_ATR_LAST, (int) RESV_ATR_UNKN)) != 0)
		return -1;

	return 0;

}

/**
 * @brief
 *		Save job to database
 *
 * @param[in]	pjob - The job to save
 * @param[in]   updatetype:
 *		SAVEJOB_QUICK - Quick update, save only quick save area
 *		SAVEJOB_FULL  - Update along with attributes
 *		SAVEJOB_NEW   - Create new job in database (insert)
 *		SAVEJOB_FULLFORCE - Same as SAVEJOB_FULL
 *
 * @return      Error code
 * @retval	 0 - Success
 * @retval	-1 - Failure
 * @retval	 1 - Jobid clash, retry with new jobid
 *
 */
int
job_save_db(job *pjob, int updatetype)
{
	pbs_db_job_info_t dbjob;
	pbs_db_obj_info_t obj;
	pbs_db_conn_t *conn = svr_db_conn;
	int savetype = PBS_UPDATE_DB_FULL;
	int rc;


	/*
	 * if job has new_job flag set, then updatetype better be SAVEJOB_NEW
	 * If not, ignore and return success
	 * This is to avoid saving the job at several places even before the job
	 * is initially created in the database in req_commit
	 * We reset the flag ji_newjob in req_commit (server only)
	 * after we have successfully created the job in the database
	 */
	if (pjob->ji_newjob == 1 && updatetype != SAVEJOB_NEW)
		return (0);

	/* if ji_modified is set, ie an attribute changed, then update mtime */
	if (pjob->ji_modified) {
		pjob->ji_wattr[JOB_ATR_mtime].at_val.at_long = time_now;
		pjob->ji_wattr[JOB_ATR_mtime].at_flags |= ATR_VFLAG_MODCACHE;
	}

	if (pjob->ji_qs.ji_jsversion != JSVERSION) {
		/* version of job structure changed, force full write */
		pjob->ji_qs.ji_jsversion = JSVERSION;
		updatetype = SAVEJOB_FULLFORCE;
	}

	if (updatetype == SAVEJOB_NEW)
		savetype = PBS_INSERT_DB;
	else if (updatetype == SAVEJOB_QUICK)
		savetype = PBS_UPDATE_DB_QUICK;
	else
		savetype = PBS_UPDATE_DB_FULL;

	if (svr_to_db_job(pjob, &dbjob, savetype) != 0)
		goto db_err;

	obj.pbs_db_obj_type = PBS_DB_JOB;
	obj.pbs_db_un.pbs_db_job = &dbjob;

	if (updatetype == SAVEJOB_QUICK) {

		if (pbs_db_begin_trx(conn, 0, conn->conn_trx_async) != 0)
			goto db_err;

		/* update database */
		if (pbs_db_save_obj(conn, &obj, PBS_UPDATE_DB_QUICK) != 0)
			goto db_err;

		if (pbs_db_end_trx(conn, PBS_DB_COMMIT) != 0)
			goto db_err;

	} else {
		/*
		 * write the whole structure to the database.
		 * The update has five parts:
		 * (1) the job structure,
		 * (2) the extended area,
		 * (3) if a Array Job, the index tracking table
		 * (4) the attributes in the "encoded "external form, and last
		 * (5) the dependency list.
		 */

		if (pbs_db_begin_trx(conn, 0, conn->conn_trx_async) != 0)
			goto db_err;

		rc = pbs_db_save_obj(conn, &obj, savetype);
		if (rc != 0) {
			if (updatetype == SAVEJOB_NEW && strstr(conn->conn_db_err, "duplicate key value")) {
				/* new job has a jobid clash, allow retry with a new jobid */
				pbs_db_reset_obj(&obj);
				if (pbs_db_end_trx(conn, PBS_DB_COMMIT) != 0)
					goto db_err;

				return (1);
			}
			goto db_err;
		}

	}

	if (pbs_db_end_trx(conn, PBS_DB_COMMIT) != 0)
		goto db_err;

	strcpy(pjob->ji_savetm, dbjob.ji_savetm); /* update savetm when we save a job, so that we do not save multiple times */

	pbs_db_reset_obj(&obj);
	pjob->ji_modified = 0;
	pjob->ji_newjob = 0; /* reset dontsave - job is now saved */

	return (0);
db_err:
	pbs_db_reset_obj(&obj);
	sprintf(log_buffer, "Failed to save job %s ", pjob->ji_qs.ji_jobid);
	if (conn->conn_db_err != NULL)
		strncat(log_buffer, conn->conn_db_err, LOG_BUF_SIZE - strlen(log_buffer) - 1);
	log_err(-1, "job_save", log_buffer);
	(void) pbs_db_end_trx(conn, PBS_DB_ROLLBACK);
	if (updatetype == SAVEJOB_NEW) {
		/* database save failed for new job, stay up, */
		return (-1); /* return without calling panic_stop_db */
	}
	panic_stop_db(log_buffer);
	return (-1);
}

/**
 * @brief
 *	Utility function called inside job_recov_db
 *
 * @param[in]	dbjob - Pointer to the database structure of a job
 *
 * @retval	 NULL - Failure
 * @retval	!NULL - Success, pointer to job structure recovered
 *
 */
job *
job_recov_db_spl(pbs_db_job_info_t *dbjob)
{
	job		*pj;

	pj = job_alloc();	/* allocate & initialize job structure space */
	if (pj == (job *)0) {
		return ((job *)0);
	}

	if (db_to_svr_job(pj, dbjob) != 0)
		goto db_err;

	return (pj);
db_err:
	if (pj)
		job_free(pj);

	snprintf(log_buffer, LOG_BUF_SIZE, "Failed to recover job %s", dbjob->ji_jobid);
	log_err(-1, "job_recov", log_buffer);

	return (NULL);
}

/**
 * @brief
 *	Refresh/retrieve job from database and add it into AVL tree if not present
 *
 *	@param[in]	dbjob - The pointer to the wrapper job object of type pbs_db_job_info_t
 *  @param[in]  refreshed - To count the no. of jobs refreshed
 *
 * @return	The recovered job
 * @retval	NULL - Failure
 * @retval	!NULL - Success, pointer to job structure recovered
 *
 */
job *
refresh_job(pbs_db_job_info_t *dbjob, int *refreshed) 
{
	int i;
	char comment_buffer[LOG_BUF_SIZE] = {'\0'}; /* for backing up the comment */
	int cmnt_flag = 0;
	job *pj = NULL;
	
	*refreshed = 0;

	/* get the old pointer of the job, if job is in AVL tree */
	if (!(pj = find_job_avl(dbjob->ji_jobid))) {
		if (!(pj = job_recov_db_spl(dbjob))) /* if job is not in AVL tree, load the job from database */
			goto err;
		
		svr_enquejob(pj); /* add job into AVL tree */

		*refreshed = 1;
		
	} else if (strcmp(dbjob->ji_savetm, pj->ji_savetm) != 0) { /* if the job had really changed in the DB */

		/* job comment backup */
		if (pj->ji_wattr[JOB_ATR_Comment].at_val.at_str != NULL) {
			sprintf(comment_buffer, "%s", pj->ji_wattr[JOB_ATR_Comment].at_val.at_str);
			cmnt_flag = 1;
		}
		/* remove any malloc working job attribute space */
		for (i=0; i < (int)JOB_ATR_LAST; i++)
			job_attr_def[i].at_free(&pj->ji_wattr[i]);

		/* db_to_svr_job makes call to decode_attr_db which further calls setup_arrayjob_attrs through action function
		 * and there we are freeing the structure stale_job_ptr->ji_ajtrk
		 * resulting parent looses his control on it's subjobs
		 * Added a hack in setup_arrayjob_attrs to fix the problem for time being only
		 */

		/* refresh all the job attributes */
		if (db_to_svr_job(pj, dbjob) != 0)
			goto err;

		/* assigned job comment again to the job */
		if(cmnt_flag)
			job_attr_def[(int)JOB_ATR_Comment].at_decode(&pj->ji_wattr[(int)JOB_ATR_Comment], NULL, NULL, comment_buffer);

		*refreshed = 1;
	}

	return pj;

err:
	snprintf(log_buffer, LOG_BUF_SIZE, "Failed to refresh job attribute %s", dbjob->ji_jobid);
	log_err(-1, __func__, log_buffer);
	return NULL;
}

/**
 * @brief
 *	Refresh/retrieve queue from database and add it into AVL tree if not present
 *
 *	@param[in]	dbque - The pointer to the wrapper queue object of type pbs_db_que_info_t
 *  @param[in]  refreshed - To count the no. of queues refreshed
 *
 * @return	The recovered queue
 * @retval	NULL - Failure
 * @retval	!NULL - Success, pointer to queue structure recovered
 *
 */
pbs_queue *
refresh_queue(pbs_db_que_info_t *dbque, int *refreshed) {

	*refreshed = 0;
	char  *pc;
	pbs_queue *pque = NULL;
	char   qname[PBS_MAXDEST + 1];

	(void)strncpy(qname, dbque->qu_name, PBS_MAXDEST);
	qname[PBS_MAXDEST] ='\0';
	pc = strchr(qname, (int)'@');	/* strip off server (fragment) */
	if (pc)
		*pc = '\0';
	/* get the old pointer of the queue, if queue is already in memory */
	pque = (pbs_queue *)GET_NEXT(svr_queues);
	while (pque != NULL) {
		if (strcmp(qname, pque->qu_qs.qu_name) == 0)
			break;
		pque = (pbs_queue *)GET_NEXT(pque->qu_link);
	}
	if (pc)
		*pc = '@';	/* restore '@' server portion */

	if (pque) {
		if (strcmp(dbque->qu_savetm, pque->qu_savetm) != 0) {
			/* if queue had changed in db */
			*refreshed = 1;
			return que_recov_db(dbque->qu_name, pque, 0);
		}
	} else {
		/* if queue is not in memory, fetch it from db */
		*refreshed = 1;
		return que_recov_db(dbque->qu_name, pque, 0);
	}
	return pque;
}


/**
 * @brief
 *	Refresh/retrieve reservation from database and add it into AVL tree if not present
 *
 * @param[in]	resvid - Reservation id to refresh
 *
 * @return	The recovered reservation
 * @retval	NULL - Failure
 * @retval	!NULL - Success, pointer to resv structure recovered
 *
 */
resc_resv *
refresh_resv(char *resvid) {
	int i;
	resc_resv *new_resv_ptr = NULL;
	resc_resv *stale_resv_ptr = NULL;
	pbs_db_conn_t *conn = svr_db_conn;
	pbs_db_obj_info_t obj;
	pbs_db_resv_info_t dbresv;

	/* get the old pointer of the resv, if resv is in svr list */
	char *at;
	if ((at = strchr(resvid, (int)'@')) != 0)
		*at = '\0';	/* strip of @server_name */
	stale_resv_ptr = (resc_resv *)GET_NEXT(svr_allresvs);
	while (stale_resv_ptr != NULL) {
		if (!strcmp(resvid, stale_resv_ptr->ri_qs.ri_resvID))
			break;
		stale_resv_ptr = (resc_resv *)GET_NEXT(stale_resv_ptr->ri_allresvs);
	}

	if (at)
		*at = '@';	/* restore @server_name */

	if(stale_resv_ptr == NULL) {
		/* if resv is not in list, load the resv from database */
		new_resv_ptr = resv_recov_db(resvid, stale_resv_ptr, 0);
		if (new_resv_ptr == NULL) {
			snprintf(log_buffer, LOG_BUF_SIZE, "Failed to recover reservation from db %s", resvid);
			log_err(-1, "refresh_resv", log_buffer);
			return NULL;
		}
		/* add resv to server list */
		append_link(&svr_allresvs, &new_resv_ptr->ri_allresvs, new_resv_ptr);
		return new_resv_ptr;
	} else {
		strcpy(dbresv.ri_resvid, resvid);
		obj.pbs_db_obj_type = PBS_DB_RESV;
		obj.pbs_db_un.pbs_db_resv = &dbresv;

		if (pbs_db_load_obj(conn, &obj, 0) != 0) {
			snprintf(log_buffer, LOG_BUF_SIZE, "Failed to load resv %s", dbresv.ri_resvid);
			log_err(-1, "refresh_resv", log_buffer);
			pbs_db_reset_obj(&obj);
			return NULL;
		}

		/* remove any malloc working resv attribute space */
		for (i=0; i < (int)RESV_ATR_LAST; i++) {
			resv_attr_def[i].at_free(&stale_resv_ptr->ri_wattr[i]);
		}

		/* refresh all the resv attributes */
		if (db_to_svr_resv(stale_resv_ptr, &dbresv) != 0) {
			snprintf(log_buffer, LOG_BUF_SIZE, "Failed to refresh resv attribute %s", dbresv.ri_resvid);
			log_err(-1, "refresh_resv", log_buffer);
			pbs_db_reset_obj(&obj);
			return NULL;
		}
		pbs_db_reset_obj(&obj);
		return stale_resv_ptr;
	}
}

/**
 * @brief
 *	Recover job from database
 *
 * @param[in]	jid - Job id of job to recover
 *
 * @return      The recovered job
 * @retval	 NULL - Failure
 * @retval	!NULL - Success, pointer to job structure recovered
 *
 */
job *
job_recov_db(char *jid, job *pjob, int lock)
{
	job		*pj = NULL;
	pbs_db_job_info_t dbjob;
	pbs_db_obj_info_t obj;
	int rc = 0;
	pbs_db_conn_t *conn = svr_db_conn;

	strcpy(dbjob.ji_jobid, jid);
	if (pjob)
		strcpy(dbjob.ji_savetm, pjob->ji_savetm);
	else
		dbjob.ji_savetm[0] = '\0';
	
	obj.pbs_db_obj_type = PBS_DB_JOB;
	obj.pbs_db_un.pbs_db_job = &dbjob;

	/* read in job fixed sub-structure */
	if (pbs_db_load_obj(conn, &obj, 0) != 0)
		goto db_err;
	
	if (rc == -2)
		return pjob; /* no change in job, return the same job */

	pj = job_recov_db_spl(&dbjob);
	if (!pj)
		goto db_err;

	pbs_db_reset_obj(&obj);

	return (pj);

db_err:
	if (pj)
		job_free(pj);

	return (NULL);
}

/**
 * @brief
 *	Save resv to database
 *
 * @see
 * 		job_or_resv_save_db, svr_migrate_data_from_fs
 *
 * @param[in]	presv - The resv to save
 * @param[in]   updatetype:
 *		SAVERESV_QUICK - Quick update without attributes
 *		SAVERESV_FULL  - Full update with attributes
 *		SAVERESV_NEW   - New resv, insert into database
 *
 * @return      Error code
 * @retval	 0 - Success
 * @retval	-1 - Failure
 *
 */
int
resv_save_db(resc_resv *presv, int updatetype)
{
	pbs_db_resv_info_t dbresv;
	pbs_db_obj_info_t obj;
	pbs_db_conn_t *conn = svr_db_conn;
	int savetype = PBS_UPDATE_DB_FULL;
	int rc;

	/* if ji_modified is set, ie an attribute changed, then update mtime */
	if (presv->ri_modified) {
		presv->ri_wattr[RESV_ATR_mtime].at_val.at_long = time_now;
		presv->ri_wattr[RESV_ATR_mtime].at_val.at_long |= ATR_VFLAG_MODCACHE;
	}

	if (svr_to_db_resv(presv, &dbresv, savetype) != 0)
		goto db_err;

	obj.pbs_db_obj_type = PBS_DB_RESV;
	obj.pbs_db_un.pbs_db_resv = &dbresv;
	if (pbs_db_begin_trx(conn, 0, conn->conn_trx_async) !=0)
		goto db_err;
	if (updatetype == SAVERESV_QUICK) {
		/* update database */
		if (pbs_db_save_obj(conn, &obj, savetype) != 0)
			goto db_err;
	} else {

		/*
		 * write the whole structure to database.
		 * The file is updated in four parts:
		 * (1) the resv structure,
		 * (2) the extended area,
		 * (3) the attributes in the "encoded "external form, and last
		 * (4) the dependency list.
		 */

		if (updatetype == SAVERESV_NEW)
			savetype = PBS_INSERT_DB;

		rc = pbs_db_save_obj(conn, &obj, savetype);
		if (rc != 0) {
			if (updatetype == SAVERESV_NEW && strstr(conn->conn_db_err, "duplicate key value")) {
				/* new id clash, allow retry with a new */
				pbs_db_reset_obj(&obj);
				resv_attr_def[(int)RESV_ATR_queue].at_free(
						&presv->ri_wattr[(int)RESV_ATR_queue]);
				if (pbs_db_end_trx(conn, PBS_DB_COMMIT) != 0)
					goto db_err;

				return (1);
			}
			goto db_err;
		}
	}
	presv->ri_modified = 0;
	pbs_db_reset_obj(&obj);
	if (pbs_db_end_trx(conn, PBS_DB_COMMIT) != 0)
		goto db_err;

	return (0);
db_err:
	sprintf(log_buffer, "Failed to save resv %s ", presv->ri_qs.ri_resvID);
	if (conn->conn_db_err != NULL)
		strncat(log_buffer, conn->conn_db_err, LOG_BUF_SIZE - strlen(log_buffer) - 1);
	log_err(-1, "resv_save", log_buffer);
	(void) pbs_db_end_trx(conn, PBS_DB_ROLLBACK);
	if (updatetype == SAVERESV_NEW) {
		/* database save failed for new resv, stay up, */
		return (-1); /* return without calling panic_stop_db */
	}
	panic_stop_db(log_buffer);
	return (-1);
}

/**
 * @brief
 *	Recover resv from database
 *
 * @param[in]	resvid - Resv id to recover
 *
 * @return      The recovered reservation
 * @retval	 NULL - Failure
 * @retval	!NULL - Success, pointer to resv structure recovered
 *
 */
resc_resv *
resv_recov_db(char *resvid, resc_resv  *presv, int lock)
{
	//resc_resv               *presv;
	pbs_db_resv_info_t	dbresv;
	pbs_db_obj_info_t       obj;
	pbs_db_conn_t *conn = svr_db_conn;
	int rc = 0;
	int i;
	int oldresv = 1;

	strcpy(dbresv.ri_resvid, resvid);
	obj.pbs_db_obj_type = PBS_DB_RESV;
	obj.pbs_db_un.pbs_db_resv = &dbresv;

	if (presv)
		strcpy(dbresv.ri_savetm, presv->ri_savetm);
	else
		dbresv.ri_savetm[0] = '\0';

	if (!presv) {
		presv = resc_resv_alloc();
		oldresv = 0;
		if (presv == NULL) {
			log_err(-1, "resv_recov", "resc_resv_alloc failed");
			return NULL;
		}
	}

	/* read in resv fixed sub-structure */
	rc = pbs_db_load_obj(conn, &obj, lock);
	if (rc == -1)
		goto db_err;

	if (rc == -2)
		return presv;

	if (oldresv) {
		/* remove any malloc working attribute space */
		for (i = 0; i < (int)RESV_ATR_LAST; i++) {
			resv_attr_def[i].at_free(&presv->ri_wattr[i]);
		}
	}

	if (db_to_svr_resv(presv, &dbresv) != 0)
		goto db_err;

	pbs_db_reset_obj(&obj);

	return (presv);
db_err:
	if (presv)
		resv_free(presv);

	sprintf(log_buffer, "Failed to recover resv %s", resvid);
	log_err(-1, "resv_recov", log_buffer);

	return NULL;
}

/**
 * @brief
 *	Save job or reservation to database
 *
 * @param[in]	pobj - Address of job or reservation
 * @param[in]   updatetype - Type of update, see descriptions of job_save_db
 *			     and resv_save_db
 *				0=quick, 1=full existing, 2=full new
 * @param[in]	objtype	- Type of the object, job or resv
 *			JOB_OBJECT, RESC_RESV_OBJECT, RESV_JOB_OBJECT
 *
 * @return      Error code
 * @retval	 0 - Success
 * @retval	-1 - Failure
 *
 */
int
job_or_resv_save_db(void *pobj, int updatetype, int objtype)
{
	int rc = 0;

	if (objtype == RESC_RESV_OBJECT || objtype == RESV_JOB_OBJECT) {
		resc_resv *presv;
		presv = (resc_resv *) pobj;

		/* call resv_save */
		rc = resv_save_db(presv, updatetype);
		if (rc)
			return (rc);
	} else if (objtype == JOB_OBJECT) {
		job *pj = (job *) pobj;
		if (pj->ji_resvp) {
			if (updatetype == SAVEJOB_QUICK)
				rc = job_or_resv_save((void *) pj->ji_resvp,
					SAVERESV_QUICK,
					RESC_RESV_OBJECT);
			else if ((updatetype == SAVEJOB_FULL) ||
				(updatetype == SAVEJOB_FULLFORCE) ||
				(updatetype == SAVEJOB_NEW))
				rc = job_or_resv_save((void *) pj->ji_resvp,
					SAVERESV_FULL,
					RESC_RESV_OBJECT);
			if (rc)
				return (rc);
		}
		rc = job_save_db(pj, updatetype);
		if (rc)
			return (rc);
	} else {
		/*Don't expect to get here; incorrect object type*/
		return (-1);
	}
	return (0);
}

/**
 * @brief
 *		Recover job or reservation from database
 *
 * @see
 * 		pbsd_init
 *
 * @param[in]	id	- Id of the reservation/job to recover
 * @param[in]	objtype	- Type of the object, job or resv
 *
 * @return       The recovered job or resv
 * @retval	  NULL - Failure
 * @retval	 !NULL - Success - job/resv object returned
 *
 */
void*
job_or_resv_recov_db(char *id, int objtype)
{
	if (objtype == RESC_RESV_OBJECT) {
		return (resv_recov_db(id, NULL, 0));
	} else {
		return (job_recov_db(id, NULL, 0));
	}
}
#endif
