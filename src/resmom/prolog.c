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

#include <pbs_config.h>   /* the master config generated by configure */
/**
 * @file prolog.c
 */
#define PBS_MOM 1
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/types.h>
#ifdef	WIN32
#include <process.h>
#else
#include <unistd.h>
#include <pwd.h>
#include <sys/wait.h>
#endif
#include <sys/stat.h>
#include "libpbs.h"
#include "list_link.h"
#include "server_limits.h"
#include "attribute.h"
#include "job.h"
#include "log.h"
#include "mom_mach.h"
#include "mom_func.h"


#define PBS_PROLOG_TIME 30

unsigned int pe_alarm_time = PBS_PROLOG_TIME;
static pid_t	child;
static int	run_exit;
#ifdef	WIN32
static HANDLE	pelog_handle = INVALID_HANDLE_VALUE;
#endif

extern int pe_input(char *jobid);

#ifdef	WIN32
static void
pelog_timeout(void)
{
	if (pelog_handle != INVALID_HANDLE_VALUE) {
		if (!TerminateJobObject(pelog_handle, 2))
			log_err(-1, __func__, "TerminateJobObject failed: Could not terminate pelog object");
		log_eventf(PBSEVENT_DEBUG, PBS_EVENTCLASS_JOB, LOG_INFO, __func__, "terminated pelog object");
	}
}
#endif

/**
 * @brief
 *	convert resources_[list or used] values to a single string that are
 *	comma-separated.
 *
 * @param[in] pattr - the attribute to convert.
 * @param[in][out] buf - the buffer into which to convert.
 * @param[in] buflen - the length of the above buffer.
 *
 * @return pointer to 'buf', and also a modified 'buf'.
 * @retval = string
 * @retval = empty buffer on malloc failure under windows only
 *
 * @note     	This function may not concatenate all resources and their values as one string if buf size is insufficient.
 *		That mean returned buf value may have less resources list compared to actual list.
 *		This effect may occur in both windows and Linux. No indication of this error given.
 *		Also, for windows only, this function returns empty buf if malloc fails.
 *
 */

static char *
resc_to_string(pattr, buf, buflen)
attribute *pattr;	/* the attribute to convert */
char      *buf;		/* the buffer into which to convert */
int	   buflen;	/* the length of the above buffer */
{
	int       need;
	svrattrl *patlist;
	pbs_list_head svlist;
#ifdef WIN32
	int tmp_buflen = buflen;
#endif

	CLEAR_HEAD(svlist);
	*buf = '\0';

	if (encode_resc(pattr, &svlist, "x", NULL, ATR_ENCODE_CLIENT, NULL) <=0)
		return (buf);

	patlist = (svrattrl *)GET_NEXT(svlist);
	while (patlist) {
		need = strlen(patlist->al_resc) + strlen(patlist->al_value) + 3;
		if (need < buflen) {
			(void)strcat(buf, patlist->al_resc);
			(void)strcat(buf, "=");
			(void)strcat(buf, patlist->al_value);
			buflen -= need;
		}
		patlist = (svrattrl *)GET_NEXT(patlist->al_link);
		if (patlist)
			(void)strcat(buf, ",");
	}
#ifdef WIN32
	if ((buf[0] != '\0') && buflen >= 3) {
		char *buf2=(char*)malloc(tmp_buflen * sizeof(char));
		if (buf2 == NULL) {
			log_err(errno, __func__, "malloc failure");
			buf[0] = '\0';
		} else {
			snprintf(buf2, tmp_buflen, "\"%s\"", buf);
			(void)strcpy(buf, buf2);
			free(buf2);
		}
	}
#endif
	return (buf);
}

/**
 * @brief
 * 	pelog_err - record error for run_pelog()
 *
 * @param[in] pjob - pointer to job structure
 * @param[in] file - file name
 * @param[in] text - error message
 *
 * @return int
 * @retval error number
 *
 */

static int
pelog_err(pjob, file, n, text)
job  *pjob;
char *file;
int   n;
char *text;
{
	sprintf(log_buffer, "pro/epilogue failed, file: %s, exit: %d, %s",
		file, n, text);
	log_event(PBSEVENT_ERROR, PBS_EVENTCLASS_JOB, LOG_WARNING,
		pjob->ji_qs.ji_jobid, log_buffer);
	return (n);
}

#ifndef WIN32
/**
 * @brief
 *	pelogalm() - alarm handler for run_pelog()
 *
 * @param[in] sig - signal number
 *
 * @return Void
 *
 */
static void
pelogalm(sig)
int sig;
{
	run_exit = -4;
}
#endif

/**
 * @brief
 *	run_pelog() - Run the Prologue/Epilogue script
 * @par
 *	Script is run under uid of root, prologue and the epilogue have:
 *		- argv[1] is the jobid
 *		- argv[2] is the user's name
 *		- argv[3] is the user's group name
 *		- the input file is a architecture dependent file
 *		- the output and error are the job's output and error
 #ifdef NAS localmod 095
 *	The prologue also has:
 *		- argv[4] is the list of resource limits specified
 #endif localmod 095
 *	The epilogue also has:
 *		- argv[4] is the job name
 *		- argv[5] is the session id
 *		- argv[6] is the list of resource limits specified
 *		- argv[7] is the list of resources used
 *		- argv[8] is the queue in which the job resides
 *		- argv[9] is the Account under which the job run
 *		- argv[10] is the job exit code
 *
 * @param[in] which - Script type (PE_PROLOGUE or PE_EPILOGUE)
 * @param[in] pelog - Path to the script file
 * @param[in] pjob - Pointer to the associated job structure
 * @param[in] pe_io_type - Output specifier (PE_IO_TYPE_NULL, PE_IO_TYPE_ASIS, or PE_IO_TYPE_STD)
 *
 * @return - Exit code
 * @retval  -2 - script not found
 * @retval  -1 - permission error
 * @retval   0 - success
 * @retval  >0 - exit status returned from script
 *
 */

int
run_pelog(which, pelog, pjob, pe_io_type)
int   which;
char *pelog;
job  *pjob;
int   pe_io_type;
{
	char		*arg[12];
	char		exitcode[20];
	char		resc_list[2048];
	char		resc_used[2048];
	struct stat	sbuf;
	char		sid[20];
#ifdef WIN32
	int		 fd_out, fd_err;
	HANDLE		 hOut;
	HANDLE		 hErr;
	STARTUPINFO		si = { 0 };
	PROCESS_INFORMATION	pi = { 0 };
	int		flags = CREATE_DEFAULT_ERROR_MODE|CREATE_NEW_CONSOLE|
		CREATE_NEW_PROCESS_GROUP;
	int		rc, run_exit;
	char		cmd_line[PBS_CMDLINE_LENGTH] = {'\0'};
	char		action_name[_MAX_PATH+1];
	char            cmd_shell[MAX_PATH+1] = {'\0'};
#else
	struct sigaction act;
	int		fd_input;
	int		waitst;
	char	 buf[MAXNAMLEN + MAXPATHLEN + 2];
#endif

	if (stat(pelog, &sbuf) == -1) {
		if (errno == ENOENT)
			return (0);
		else
			return (pelog_err(pjob, pelog, errno, "cannot stat"));
	}
#ifdef WIN32
	else if (chk_file_sec(pelog, 0, 0, WRITES_MASK^FILE_WRITE_EA, 0))
#else
	else if ((sbuf.st_uid != 0) ||
		(! S_ISREG(sbuf.st_mode)) ||
		((sbuf.st_mode & (S_IRUSR|S_IXUSR)) !=
		(S_IRUSR|S_IXUSR)) ||
		(sbuf.st_mode & (S_IWGRP|S_IWOTH)))
#endif
		return (pelog_err(pjob, pelog, -1, "Permission Error"));

#ifdef WIN32

	/*
	 * if pe_io_type == PE_IO_TYPE_NULL, No Output, force to /dev/null
	 * otherwise, default to /dev/null in case of errors.
	 */
	hOut = INVALID_HANDLE_VALUE;
	hErr = INVALID_HANDLE_VALUE;
	if (pe_io_type == PE_IO_TYPE_STD) {
		int mode = O_CREAT | O_WRONLY | O_APPEND;
		/* open job standard out/error */
		fd_out = open_std_file(pjob, StdOut, O_APPEND|O_WRONLY,
			pjob->ji_qs.ji_un.ji_momt.ji_exgid);
		if (fd_out != -1) {
			hOut = (HANDLE)_get_osfhandle(fd_out);
			DWORD dwPtr = SetFilePointer(hOut, (LONG)NULL, (PLONG)NULL, FILE_END);
			if (dwPtr == INVALID_SET_FILE_POINTER)
				log_err(-1, __func__, "SetFilePointer failed for out file handle");
		}
		fd_err = open_std_file(pjob, StdErr, O_APPEND|O_WRONLY,
			pjob->ji_qs.ji_un.ji_momt.ji_exgid);
		if (fd_err != -1) {
			hErr = (HANDLE)_get_osfhandle(fd_err);
			DWORD dwPtr = SetFilePointer(hErr, (LONG)NULL, (PLONG)NULL, FILE_END);
			if (dwPtr == INVALID_SET_FILE_POINTER)
				log_err(-1, __func__, "SetFilePointer failed for error file handle");
		}
		if (fd_out == -1 || fd_err == -1) {
			log_event(PBSEVENT_ERROR, PBS_EVENTCLASS_JOB, LOG_WARNING,
				pjob->ji_qs.ji_jobid, "problem opening job output file(s)");
		}
	} else if (pe_io_type == PE_IO_TYPE_ASIS) {
		/* If PE_IO_TYPE_ASIS, setup in finish_exec */
		extern	int	script_out;
		extern	int	script_err;

		if (script_out != -1) {
			hOut = (HANDLE)_get_osfhandle(script_out);
			if (hOut == INVALID_HANDLE_VALUE)
				log_err(errno, __func__, "_get_osfhandle failed for out file handle");
		}
		if (script_err != -1) {
			hErr = (HANDLE)_get_osfhandle(script_err);
			if (hErr == INVALID_HANDLE_VALUE)
				log_err(errno, __func__, "_get_osfhandle failed for error file handle");
		}
	}

	/* for both prologue and epilogue */
	arg[0] = pelog;
	arg[1] = pjob->ji_qs.ji_jobid;
	arg[2] = pjob->ji_wattr[(int)JOB_ATR_euser].at_val.at_str;
	arg[3] = pjob->ji_wattr[(int)JOB_ATR_egroup].at_val.at_str;

	/* for epilogue only */
	if (which == PE_EPILOGUE) {
		arg[4] = pjob->ji_wattr[(int)JOB_ATR_jobname].at_val.at_str;
		sprintf(sid, "%ld", pjob->ji_wattr[(int)JOB_ATR_session_id].at_val.at_long);
		arg[5] = sid;
		arg[6] = resc_to_string(&pjob->ji_wattr[(int)JOB_ATR_resource], resc_list, 2048);
		arg[7] = resc_to_string(&pjob->ji_wattr[(int)JOB_ATR_resc_used], resc_used, 2048);
		arg[8] = pjob->ji_wattr[(int)JOB_ATR_in_queue].at_val.at_str;
		if (pjob->ji_wattr[(int)JOB_ATR_account].at_flags & ATR_VFLAG_SET)
			arg[9] = pjob->ji_wattr[(int)JOB_ATR_account].at_val.at_str;
		else
			arg[9] = "null";
		sprintf(exitcode, "%d", pjob->ji_qs.ji_un.ji_momt.ji_exitstat);
		arg[10] = exitcode;
		arg[11] = NULL;

	} else {
#ifdef NAS /* localmod 095 */
		arg[4] = resc_to_string(&pjob->ji_wattr[(int)JOB_ATR_resource], resc_list, 2048);
		arg[5] = NULL;
#else
		arg[4] = NULL;
#endif /* localmod 095 */
	}


	si.cb = sizeof(si);
	si.lpDesktop = "";
	si.dwFlags = STARTF_USESTDHANDLES;
	si.hStdInput = INVALID_HANDLE_VALUE;
	si.hStdOutput = hOut;
	si.hStdError = hErr;

	/* If we fail to get cmd shell(unlikely), use "cmd.exe" as shell */
	if (0 != get_cmd_shell(cmd_shell, sizeof(cmd_shell)))
		(void)snprintf(cmd_shell, sizeof(cmd_shell) - 1, "cmd.exe");
	(void)snprintf(cmd_line, PBS_CMDLINE_LENGTH - 1, "%s /c", cmd_shell);

	for (rc=0; arg[rc]; rc++) {
		strcat(cmd_line, " ");
		strcat(cmd_line, replace_space(arg[rc], ""));
	}

	sprintf(action_name, "pbs_pelog%d_%d", which, _getpid());
	pelog_handle = CreateJobObject(NULL, action_name);

	if ((pelog_handle == INVALID_HANDLE_VALUE) || (pelog_handle == NULL)) {
		run_exit = 254;
		(void)pelog_err(pjob, pelog, run_exit, "nonzero p/e exit status");
		return run_exit;
	}

	/* temporary add PBS_JOBDIR to the current process environement */
	if (pjob->ji_grpcache) {
		if ((pjob->ji_wattr[(int)JOB_ATR_sandbox].at_flags & ATR_VFLAG_SET) && (strcasecmp(pjob->ji_wattr[JOB_ATR_sandbox].at_val.at_str, "PRIVATE") == 0)) {
			/* set PBS_JOBDIR to the per-job staging and */
			/* execution directory*/
			if (!SetEnvironmentVariable("PBS_JOBDIR",
				jobdirname(pjob->ji_qs.ji_jobid,
				pjob->ji_grpcache->gc_homedir)))
				log_err(-1, __func__, "Unable to set environment variable PBS_JOBDIR for sandbox=PRIVATE");
		} else {
			/* set PBS_JOBDIR to user HOME*/
			if (!SetEnvironmentVariable("PBS_JOBDIR", pjob->ji_grpcache->gc_homedir))
				log_err(-1, __func__, "Unable to set environment variable PBS_JOBDIR to user HOME");
		}
	}

	/* in Windows, created process does not need to be unprotected */
	/* it doesn't inherit the protection value from the parent     */
	rc = CreateProcess(NULL, cmd_line,
		NULL, NULL, TRUE, flags,
		NULL, NULL, &si, &pi);

	/* could be sitting on a user's working directory (epilogue) */
	if ((rc == 0) && (GetLastError() == ERROR_ACCESS_DENIED)) {
		char    current_dir[MAX_PATH+1];
		char    *temp_dir = NULL;

		current_dir[0] = '\0';
		_getcwd(current_dir, MAX_PATH+1);

		temp_dir = get_saved_env("SYSTEMROOT");
		chdir(temp_dir?temp_dir:"C:\\");

		rc = CreateProcess(NULL, cmd_line,
			NULL, NULL, TRUE, flags,
			NULL, NULL, &si, &pi);

		/* restore current working directory */
		chdir(current_dir);
	}

	/* remove PBS_JOBDIR from the current process environement */
	if (!SetEnvironmentVariable("PBS_JOBDIR", NULL))
		log_err(-1, __func__, "unset environment variable PBS_JOBDIR");

	if (pe_io_type == PE_IO_TYPE_STD) {
		if (fd_out != -1)
			close(fd_out);
		if (fd_err != -1)
			close(fd_err);
	}

	run_exit = 255;
	if (rc == 0) {
		log_err(-1, __func__, "CreateProcess failed");
	} else {
		sprintf(log_buffer, "running %s",
			which == PE_PROLOGUE ? "prologue" : "epilogue");
		log_event(PBSEVENT_DEBUG, PBS_EVENTCLASS_JOB, LOG_INFO,
			pjob->ji_qs.ji_jobid, log_buffer);

		(void)win_alarm(pe_alarm_time, pelog_timeout);

		if (!AssignProcessToJobObject(pelog_handle, pi.hProcess))
			log_err(-1, __func__, "AssignProcessToJobObject");

		if (WaitForSingleObject(pi.hProcess, INFINITE) == WAIT_OBJECT_0) {
			if (!GetExitCodeProcess(pi.hProcess, &run_exit))
				log_err(-1, __func__, "GetExitCodeProcess");
		}
		else
			log_err(-1, __func__, "WaitForSingleObject");

		CloseHandle(pi.hProcess);
		CloseHandle(pi.hThread);
		if (pelog_handle != INVALID_HANDLE_VALUE) {
			CloseHandle(pelog_handle);
			pelog_handle = INVALID_HANDLE_VALUE;
		}

		(void)win_alarm(0, NULL);
	}

#else	/* Non-Windows code follows. */

	fd_input = pe_input(pjob->ji_qs.ji_jobid);
	if (fd_input < 0) {
		return (pelog_err(pjob, pelog, -2,
			"no pro/epilogue input file"));
	}

	run_exit = 0;
	child = fork();
	if (child > 0) {	/* parent */
		(void)close(fd_input);
		sprintf(log_buffer, "running %s",
			which == PE_PROLOGUE ? "prologue" : "epilogue");
		log_event(PBSEVENT_DEBUG, PBS_EVENTCLASS_JOB, LOG_INFO,
			pjob->ji_qs.ji_jobid, log_buffer);

		act.sa_handler = pelogalm;
		sigemptyset(&act.sa_mask);
		act.sa_flags = 0;
		sigaction(SIGALRM, &act, 0);
		alarm(pe_alarm_time);
		while (wait(&waitst) < 0) {
			if (errno != EINTR) {	/* continue loop on signal */
				run_exit = -3;
				break;
			}
			kill(-child, SIGKILL);
		}
		alarm(0);
		act.sa_handler = SIG_DFL;
		sigaction(SIGALRM, &act, 0);
		kill(-child, SIGKILL);
		if (run_exit == 0) {
			if (WIFEXITED(waitst)) {
				run_exit = WEXITSTATUS(waitst);
			} else if (WIFSIGNALED(waitst)) {
				run_exit = -3;
			}
		} else {
			sprintf(log_buffer, "completed %s, exit=%d",
				which == PE_PROLOGUE ? "prologue" : "epilogue",
				run_exit);
			log_event(PBSEVENT_DEBUG, PBS_EVENTCLASS_JOB, LOG_INFO,
				pjob->ji_qs.ji_jobid, log_buffer);
		}

	} else {		/* child */
		/*
		 * For sanity sake we make sure the following are defined.
		 * They should have been defined in unistd.h
		 */
#ifndef STDIN_FILENO
#define STDIN_FILENO 0
#endif
#ifndef STDOUT_FILENO
#define STDOUT_FILENO 1
#endif
#ifndef STDERR_FILENO
#define STDERR_FILENO 2
#endif

		/*
		 ** As these fd variabes (fds1(2)) are used in the child
		 ** process, define here only.
		 */
		int	fds1 = -1;
		int	fds2 = -1;

		if (fd_input != 0) {
			(void)close(STDIN_FILENO);
			(void)dup(fd_input);
			(void)close(fd_input);
		}

		/* unprotect from kernel killers (such as oom) */
		daemon_protect(0, PBS_DAEMON_PROTECT_OFF);

		/*
		 * If PE_IO_TYPE_ASIS, leave stdout/stderr alone as they
		 * are already open to job. Otherwise, set FDs 1 and 2
		 * appropriately being careful to join them where
		 * necessary.
		 */
		if (pe_io_type == PE_IO_TYPE_NULL) {
			/* Close any existing stdout/stderr. */
			(void)close(STDOUT_FILENO);
			(void)close(STDERR_FILENO);
			/* No output, force to /dev/null */
			fds1 = open("/dev/null", O_WRONLY, 0600);
			fds2 = dup(fds1);
		} else if (pe_io_type == PE_IO_TYPE_STD) {
			int join_method;
			/* Close any existing stdout/stderr. */
			(void)close(STDOUT_FILENO);
			(void)close(STDERR_FILENO);
			/*
			 * Do not open an output file unless it will be used.
			 * Otherwise, it will be left behind in spool.
			 */
			join_method = is_joined(pjob);
			/* Open job stdout/stderr. */
			if (join_method < 0) {		/* joined as stderr */
				fds1 = open("/dev/null", O_WRONLY, 0600);
				fds2 = open_std_file(pjob, StdErr, O_WRONLY | O_APPEND,
					pjob->ji_qs.ji_un.ji_momt.ji_exgid);
				(void)close(fds1);
				fds1 = dup(fds2);
			} else if (join_method > 0) {	/* joined as stdout */
				fds1 = open_std_file(pjob, StdOut, O_WRONLY | O_APPEND,
					pjob->ji_qs.ji_un.ji_momt.ji_exgid);
				fds2 = dup(fds1);
			} else {			/* not joined */
				fds1 = open_std_file(pjob, StdOut, O_WRONLY | O_APPEND,
					pjob->ji_qs.ji_un.ji_momt.ji_exgid);
				fds2 = open_std_file(pjob, StdErr, O_WRONLY | O_APPEND,
					pjob->ji_qs.ji_un.ji_momt.ji_exgid);
			}
			if (fds1 == -1 || fds2 == -1) {
				log_event(PBSEVENT_ERROR, PBS_EVENTCLASS_JOB, LOG_WARNING,
					pjob->ji_qs.ji_jobid, "problem opening job output file(s)");
			}
		}

		/* for both prologue and epilogue */

		arg[0] = pelog;
		arg[1] = pjob->ji_qs.ji_jobid;
		arg[2] = pjob->ji_wattr[(int)JOB_ATR_euser].at_val.at_str;
		arg[3] = pjob->ji_wattr[(int)JOB_ATR_egroup].at_val.at_str;

		/* for epilogue only */

		if (which == PE_EPILOGUE) {
			arg[4] = pjob->ji_wattr[(int)JOB_ATR_jobname].at_val.at_str;
			sprintf(sid, "%ld", pjob->ji_wattr[(int)JOB_ATR_session_id].at_val.at_long);
			arg[5] = sid;
			arg[6] = resc_to_string(&pjob->ji_wattr[(int)JOB_ATR_resource], resc_list, 2048);
			arg[7] = resc_to_string(&pjob->ji_wattr[(int)JOB_ATR_resc_used], resc_used, 2048);
			arg[8] = pjob->ji_wattr[(int)JOB_ATR_in_queue].at_val.at_str;
			if ((pjob->ji_wattr[(int)JOB_ATR_account].at_flags & ATR_VFLAG_SET) && (strlen(pjob->ji_wattr[(int)JOB_ATR_account].at_val.at_str) > 0))
				arg[9] = pjob->ji_wattr[(int)JOB_ATR_account].at_val.at_str;
			else
				arg[9] = "null";
			sprintf(exitcode, "%d", pjob->ji_qs.ji_un.ji_momt.ji_exitstat);
			arg[10] = exitcode;
			arg[11] = 0;

		} else {
#ifdef NAS /* localmod 095 */
			arg[4] = resc_to_string(&pjob->ji_wattr[(int)JOB_ATR_resource], resc_list, 2048);
			arg[5] = NULL;
#else
			arg[4] = NULL;
#endif /* localmod 095 */
		}

		(void)setsid();

		/* Add PBS_JOBDIR to the current process environement */
		if (pjob->ji_grpcache) {
			if ((pjob->ji_wattr[(int)JOB_ATR_sandbox].at_flags & ATR_VFLAG_SET) && (strcasecmp(pjob->ji_wattr[JOB_ATR_sandbox].at_val.at_str, "PRIVATE") == 0)) {
				/* set PBS_JOBDIR to the per-job staging and execution directory*/
				sprintf(buf, "PBS_JOBDIR=%s",
					jobdirname(pjob->ji_qs.ji_jobid,
					pjob->ji_grpcache->gc_homedir));
			} else {
				/* set PBS_JOBDIR to user HOME*/
				sprintf(buf, "PBS_JOBDIR=%s", pjob->ji_grpcache->gc_homedir);
			}
			if (setenv("PBS_JOBDIR", pjob->ji_grpcache->gc_homedir, 1) != 0)
				log_err(-1, "run_pelog", "set environment variable PBS_JOBDIR");
		}

		execv(pelog, arg);

		log_err(errno, "run_pelog", "execle of prologue failed");
		exit(255);
	}

#endif	/* WIN32 */

	if (run_exit)
		(void)pelog_err(pjob, pelog, run_exit, "nonzero p/e exit status");

	return (run_exit);
}
