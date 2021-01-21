/*
 * Copyright (C) 1994-2021 Altair Engineering, Inc.
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

/**
 * @file	pbs_log.c
 * @brief
 * pbs_log.c - contains functions to log error and event messages to
 *	the log file.
 *
 * @par Functions included are:
 *	log_open()
 *	log_open_main()
 *	log_err()
 *	log_joberr()
 *	log_record()
 *	log_close()
 *	log_add_debug_info()
 *	log_add_if_info()
 */


#include <pbs_config.h>   /* the master config generated by configure */

#include "portability.h"
#include "pbs_error.h"

#include <sys/stat.h>
#include <sys/param.h>
#include <sys/types.h>
#include <sys/time.h>
#include <limits.h>
#include <time.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <stdlib.h>
#include <pthread.h>
#include <signal.h>
#include <stddef.h>
#include <stdarg.h>

#include "log.h"
#include "pbs_ifl.h"
#include "libutil.h"
#include "pbs_version.h"
#if SYSLOG
#include <syslog.h>
#endif

/* Default to no locking. */

/* Global Data */

char log_buffer[LOG_BUF_SIZE];
char log_directory[_POSIX_PATH_MAX/2];

/*
 * PBS logging is not reentrant. Especially the log switch changes the
 * global log file pointer. Guard using a mutex.
 * Initialize the mutex once at log_open().
 */
static pthread_once_t log_once_ctl = PTHREAD_ONCE_INIT;
static pthread_key_t pbs_log_tls_key;
static pthread_mutex_t log_mutex;

char *msg_daemonname;

/* Local Data */

static int	     log_auto_switch = 0;
static int	     log_open_day;
static FILE	    *logfile;		/* open stream for log file */
static volatile int  log_opened = 0;
#if SYSLOG
static int	     syslogopen = 0;
#endif	/* SYSLOG */

/*
 * the order of these names MUST match the defintions of
 * PBS_EVENTCLASS_* in log.h
 */
static char *class_names[] = {
	"n/a",
	"Svr",
	"Que",
	"Job",
	"Req",
	"Fil",
	"Act",
	"Node",
	"Resv",
	"Sched",
	"Hook",
	"Resc",
	"TPP"
};

static char pbs_leaf_name[PBS_MAXHOSTNAME + 1] = "N/A";
static char pbs_mom_node_name[PBS_MAXHOSTNAME + 1] = "N/A";
static unsigned int locallog = 0;
static unsigned int syslogfac = 0;
static unsigned int syslogsvr = 3;
static unsigned int pbs_log_highres_timestamp = 0;

void
set_log_conf(char *leafname, char *nodename,
		unsigned int islocallog, unsigned int sl_fac, unsigned int sl_svr,
		unsigned int log_highres)
{
	if (leafname) {
		strncpy(pbs_leaf_name, leafname, PBS_MAXHOSTNAME);
		pbs_leaf_name[PBS_MAXHOSTNAME] = '\0';
	}

	if (nodename) {
		strncpy(pbs_mom_node_name, nodename, PBS_MAXHOSTNAME);
		pbs_mom_node_name[PBS_MAXHOSTNAME] = '\0';
	}

	locallog = islocallog;
	syslogfac = sl_fac;
	syslogsvr = sl_svr;
	pbs_log_highres_timestamp = log_highres;
}

#ifdef WIN32
/**
 * @brief
 *		gettimeofday - This function returns the current calendar
 *		time as the elapsed time since the epoch in the struct timeval
 *		structure indicated by tp
 *
 * @param[in] - tp - pointer to timeval struct
 * @param[in] - tzp - pointer to timezone struct (not used)
 * @return int
 * @retval -1 - failure
 * @retval 0 - success
 */
int
gettimeofday(struct timeval *tp, struct timezone *tzp)
{
	FILETIME file_time = {0};
	ULARGE_INTEGER large_int = {0};
	/*
 	 * Microsecond different from "January 1, 1601 (UTC)" to
  	 * "00:00:00 January 1, 1970" as Windows's FILESYSTEM is represents from
 	 * "January 1, 1601 (UTC)"
  	 */
	static const unsigned __int64 epoch = 116444736000000000ULL;

	GetSystemTimeAsFileTime(&file_time);
	large_int.LowPart = file_time.dwLowDateTime;
	large_int.HighPart = file_time.dwHighDateTime;
	tp->tv_sec = (time_t)((large_int.QuadPart - epoch) / 10000000L);
	tp->tv_usec = (time_t)((large_int.QuadPart - epoch) % 1000000L);
	return 0;
}
#endif


/* External functions called */

/**
 * @brief
 * set_msgdaemonname - set the variable msg_daemonname
 *			as per the daemon
 * @param[in] - ch - the string msg_daemonname to be set
 * @return int
 * @retval 1 - failure
 * @retval 0 - success
 */

int
set_msgdaemonname(const char *ch)
{
	if(!(msg_daemonname = strdup(ch))) {
		return 1;
	}
	return 0;
}

/**
 * @brief
 * set_logfile - set the logfile to stderr to log the message to stderr
 * @param[in] - fp - log file pointer
 * @return void
 */

void
set_logfile(FILE *fp)
{
	log_opened = 1;
	logfile = fp;
}



/*
 * @brief
 * 	mk_log_name - make the log name used by MOM
 *	based on the date: yyyymmdd
 *
 * @param[in] pbuf - buffer to hold log file name
 * @param[in] pbufsz - max size of buffer
 *
 * @return	string
 * @retval	log file name	success
 *
 */

static char *
mk_log_name(char *pbuf, size_t pbufsz)
{
#ifndef WIN32
	struct tm ltm;
#endif
	struct tm *ptm;
	time_t time_now;

	time_now = time(NULL);

#ifdef WIN32
	ptm = localtime(&time_now);
	(void)snprintf(pbuf, pbufsz, "%s\\%04d%02d%02d", log_directory,
		ptm->tm_year+1900, ptm->tm_mon+1, ptm->tm_mday);
#else
	ptm = localtime_r(&time_now, &ltm);
	(void)snprintf(pbuf, pbufsz, "%s/%04d%02d%02d", log_directory,
		ptm->tm_year+1900, ptm->tm_mon+1, ptm->tm_mday);
#endif
	log_open_day = ptm->tm_yday;	/* Julian date log opened */
	return (pbuf);
}

/**
 * @brief
 *	Return the address of the tls data related to pbs_log_tls_key
 *
 * @return tls data pointer
 *
 * @par Side Effects:
 *	None
 *
 * @par MT-safe: Yes
 *
 */
void *
log_get_tls_data(void)
{
	return pthread_getspecific(pbs_log_tls_key);
}

/**
 * @brief
 *	Lock the mutex associated with this log
 *
 * @return Error code
 * @retval -1 - failure
 * @retval  0 - success
 *
 * @par Side Effects:
 *	None
 *
 * @par MT-safe: Yes
 *
 */
int
log_mutex_lock()
{
	void *log_lock;
	if ((log_lock = pthread_getspecific(pbs_log_tls_key)) != NULL)
		return -1;

	if (pthread_mutex_lock(&log_mutex) != 0)
		return -1;

	/* use &log_lock for non-null value */
	log_lock = &log_lock;
	pthread_setspecific(pbs_log_tls_key, log_lock);

	return 0;
}

/**
 * @brief
 *	Unlock the mutex associated with this log
 *
 * @return Error code
 * @retval -1 - failure
 * @retval  0 - success
 *
 * @par Side Effects:
 *	None
 *
 * @par MT-safe: Yes
 *
 */
int
log_mutex_unlock()
{
	void *log_lock;
	if ((log_lock = pthread_getspecific(pbs_log_tls_key)) == NULL)
		return -1;

	if (pthread_mutex_unlock(&log_mutex) != 0)
		return -1;

	log_lock = NULL;
	pthread_setspecific(pbs_log_tls_key, log_lock);

	return 0;
}

#ifndef WIN32

/**
 * @brief
 *	wrapper function for log_mutex_lock().
 *
 */
void
log_atfork_prepare()
{
	log_mutex_lock();
}

/**
 * @brief
 *	wrapper function for log_mutex_unlock().
 *
 */
void
log_atfork_parent()
{
	log_mutex_unlock();
}

/**
 * @brief
 *	wrapper function for log_mutex_unlock().
 *
 */
void
log_atfork_child()
{
	log_mutex_unlock();
}
#endif

/**
 * @brief
 *	Initialize the log mutex and tls
 *
 * @par Side Effects:
 *	None
 *
 * @par MT-safe: Yes
 *
 */
void
log_init(void)
{
	if (pthread_key_create(&pbs_log_tls_key, NULL) != 0) {
		fprintf(stderr, "log tls key creation failed\n");
		return;
	}

	if (pthread_mutex_init(&log_mutex, NULL) != 0) {
		fprintf(stderr, "log mutex init failed\n");
		return;
	}

#ifndef WIN32
	/* for unix, set a pthread_atfork handler */
	if (pthread_atfork(log_atfork_prepare, log_atfork_parent, log_atfork_child) != 0) {
		fprintf(stderr, "log mutex atfork handler failed\n");
		return;
	}
#endif
}

/**
 * @brief
 *	Add general debugging information in log
 *
 * @par Side Effects:
 * 	None
 *
 * @par MT-safe: Yes
 *
 */
void
log_add_debug_info()
{
	char dest[LOG_BUF_SIZE] = {'\0'};
	char temp[PBS_MAXHOSTNAME + 1] = {'\0'};
	char host[PBS_MAXHOSTNAME + 1] = "N/A";

	/* Set hostname */
	if (!gethostname(temp, (sizeof(temp) - 1))) {
		snprintf(host, sizeof(host), "%s", temp);
		if (!get_fullhostname(temp, temp, (sizeof(temp) - 1)))
			/* Overwrite if full hostname is available */
			snprintf(host, sizeof(host), "%s", temp);
	}
	/* Record to log */
	snprintf(dest, sizeof(dest),
		"hostname=%s;pbs_leaf_name=%s;pbs_mom_node_name=%s",
		host, pbs_leaf_name, pbs_mom_node_name);
	log_record(PBSEVENT_SYSTEM, PBS_EVENTCLASS_SERVER, LOG_INFO,
		msg_daemonname, dest);
	return;
}

/**
 * @brief
 *	Add supported authentication method to log
 *
 * @param[in]	supported_auth_methods - An array of supported authentication method
 *
 * @return void
 *
 */
void
log_supported_auth_methods(char **supported_auth_methods)
{
	if (supported_auth_methods) {
		int i = 0;
		while (supported_auth_methods[i]) {
			log_eventf(PBSEVENT_FORCE, PBS_EVENTCLASS_SERVER, LOG_INFO, msg_daemonname,
					"Supported authentication method: %s", supported_auth_methods[i]);
			i++;
		}
	}
}

/**
 * @brief
 *	Add interface information to log
 *
 * @par Side Effects:
 *	None
 *
 * @par MT-safe: Yes
 *
 */

void
log_add_if_info()
{
	char tbuf[LOG_BUF_SIZE];
	char msg[LOG_BUF_SIZE];
	char temp[LOG_BUF_SIZE];
	int i;
	char dest[LOG_BUF_SIZE * 2];
	struct log_net_info *ni, *curr;

	memset(msg, '\0', sizeof(msg));
	ni = get_if_info(msg);
	if (msg[0] != '\0') {
		/* Adding error message to log */
		snprintf(tbuf, sizeof(tbuf), "%s", msg);
		log_record(PBSEVENT_SYSTEM, PBS_EVENTCLASS_SERVER, LOG_INFO, msg_daemonname, tbuf);
	}
	if (!ni)
		return;

	/* Add info to log */
	for (curr = ni; curr; curr = curr->next) {
		snprintf(tbuf, sizeof(tbuf), "%s interface %s: ",
			(curr->iffamily) ? curr->iffamily : "NULL",
			(curr->ifname) ? curr->ifname : "NULL");
		for (i = 0; curr->ifhostnames[i]; i++) {
			snprintf(temp, sizeof(temp), "%s ", curr->ifhostnames[i]);
			snprintf(dest, sizeof(dest), "%s%s", tbuf, temp);
		}
		log_record(PBSEVENT_SYSTEM, PBS_EVENTCLASS_SERVER, LOG_INFO, msg_daemonname, dest);
	}

	free_if_info(ni);
}

/**
 *
 * @brief
 *	Calls log_open_main() in non-silent mode.
 *
 * @param[in]	filename - the log filename passed to log_open_main().
 * @param[in]	log_directory -  The directory name passed to log_open_main().
 *
 * @return int	- return value of log_open_main().
 *
 */
int
log_open(char *filename, char *directory)
{
	return (log_open_main(filename, directory, 0));
}

/**
 *
 * @brief
 * 	Open the log file for append.
 *
 * @par
 *	Opens a (new) log file.
 *	If a log file is already open, and the new file is successfully opened,
 *	the old file is closed.  Otherwise the old file is left open.

 * @param[in]	filename - if non-NULL or non-empty string, then this must be
 *			   an absolute pathname, which is opened and made as
 *			   the log file.
 *			 - if NULL or empty string, then calls mk_log_name()
 *			   to create a log file named after the current date
 *			   yymmdd, which is made into the log file.
 * @param[in]	log_directory -  The directory used by mk_log_name()
 *				 as the log directory for the generated
 *				 log filename.
 * @param[in]	silent - if set to 1, then extra messages such as
 *			"Log opened", "pbs_version=", "pbs_build="
 *			are not printed out on the log file.
 *
 * @return int
 * @retval 0	for success
 * @retval != 0 for failure
 */
int
log_open_main(char *filename, char *directory, int silent)
{
	char  buf[_POSIX_PATH_MAX];
	int   fds;

	/*providing temporary buffer, tbuf, for forming pbs_version
	 *and pbs_build messages that get written on logfile open.
	 *Using the usual buffer, log_buffer, that one sees in calls
	 *to log_event() will result in clobbering the first message
	 *after midnight:  log_event(), calls log_record(), calls
	 *log_close() followed by log_open() - so a write into "log_buffer"
	 *inside log_open() obliterates the message that would have been
	 *placed in the newly opened, after mignight, server logfile.
	 */
	char  tbuf[LOG_BUF_SIZE];

	pthread_once(&log_once_ctl, log_init); /* initialize mutex once */

	if (log_opened > 0)
		return (-1);	/* already open */

	if (locallog != 0 || syslogfac == 0) {

		/* open PBS local logging */

		if (strcmp(log_directory, directory) != 0)
			(void)strncpy(log_directory, directory, _POSIX_PATH_MAX/2-1);

		if ((filename == NULL) || (*filename == '\0')) {
			filename = mk_log_name(buf, _POSIX_PATH_MAX);
			log_auto_switch = 1;
		}
#ifdef WIN32
		else if (*filename != '\\' && (strlen(filename) > 1 && \
				*(filename+1) != ':') ) {
			return (-1);	/* must be absolute path */
		}
#else
		else if (*filename != '/') {
			return (-1);	/* must be absolute path */
		}
#endif

#ifdef WIN32
		if ((fds = open(filename, O_CREAT|O_WRONLY|O_APPEND, S_IREAD | S_IWRITE)) < 0)
#elif defined (O_LARGEFILE )
		if ((fds = open(filename, O_CREAT|O_WRONLY|O_APPEND|O_LARGEFILE, 0644)) < 0)
#else
			if ((fds = open(filename, O_CREAT|O_WRONLY|O_APPEND, 0644)) < 0)
#endif
			{
				log_opened = -1;	/* note that open failed */
				return (-1);
			}

#ifdef WIN32
		secure_file2(filename, "Administrators",
			READS_MASK|WRITES_MASK|STANDARD_RIGHTS_REQUIRED,
			"Everyone", READS_MASK | READ_CONTROL);
#endif
		DBPRT(("Opened log file %s\n", filename))
		if (fds < 3) {

			log_opened = fcntl(fds, F_DUPFD, 3);	/* overload variable */
			if (log_opened < 0)
				return (-1);
			(void)close(fds);
			fds = log_opened;
		}
		logfile = fdopen(fds, "a");

#ifdef WIN32
		(void)setvbuf(logfile, NULL, _IONBF, 0);	/* no buffering to get instant log */
#else
		(void)setvbuf(logfile, NULL, _IOLBF, 0);	/* set line buffering */
#endif
		log_opened = 1;			/* note that file is open */

		if (!silent) {
			log_record(PBSEVENT_SYSTEM, PBS_EVENTCLASS_SERVER, LOG_INFO, "Log", "Log opened");
			snprintf(tbuf, LOG_BUF_SIZE, "pbs_version=%s", PBS_VERSION);
			log_record(PBSEVENT_SYSTEM, PBS_EVENTCLASS_SERVER, LOG_INFO, msg_daemonname, tbuf);
			snprintf(tbuf, LOG_BUF_SIZE, "pbs_build=%s", PBS_BUILD);
			log_record(PBSEVENT_SYSTEM, PBS_EVENTCLASS_SERVER, LOG_INFO, msg_daemonname, tbuf);

			log_add_debug_info();
			log_add_if_info();
		}
	}
#if SYSLOG
	if (syslogopen == 0 && syslogfac > 0 && syslogfac < 10) {
		/*
		 * We do not assume that the log facilities are defined sequentially.
		 * That is why we reference them each by name.
		 */
		switch (syslogfac) {
			case 2:
				syslogopen = LOG_LOCAL0;
				break;
			case 3:
				syslogopen = LOG_LOCAL1;
				break;
			case 4:
				syslogopen = LOG_LOCAL2;
				break;
			case 5:
				syslogopen = LOG_LOCAL3;
				break;
			case 6:
				syslogopen = LOG_LOCAL4;
				break;
			case 7:
				syslogopen = LOG_LOCAL5;
				break;
			case 8:
				syslogopen = LOG_LOCAL6;
				break;
			case 9:
				syslogopen = LOG_LOCAL7;
				break;
			case 1:
			default:
				syslogopen = LOG_DAEMON;
				break;
		}
		openlog(msg_daemonname, LOG_NOWAIT, syslogopen);
		DBPRT(("Syslog enabled, facility = %d\n", syslogopen))
		if (syslogsvr != 0) {
			/* set min priority of what gets logged via syslog */
			setlogmask(LOG_UPTO(syslogsvr));
			DBPRT(("Syslog mask set to 0x%x\n", syslogsvr))
		}
	}
#endif


	return (0);
}

/**
 * @brief
 * 	log_err - log an internal error
 *	The error is recorded to the pbs log file and to syslogd if it is
 *	available.  If the error file has not been opened and if syslog is
 *	not defined, then the console is opened.
 *
 * @param[in] errnum - error number
 * @param[in] routine - error in which routine
 * @param[in] text - text to be logged
 *
 */

void
log_err(int errnum, const char *routine, const char *text)
{
	char buf[LOG_BUF_SIZE], *errmsg;
	int  i;

	if (errnum == -1) {

#ifdef WIN32
		LPVOID	lpMsgBuf;
		DWORD	err = GetLastError();
		int		len;

		snprintf(buf, LOG_BUF_SIZE, "Err(%lu): ", err);
		FormatMessage(
			FORMAT_MESSAGE_ALLOCATE_BUFFER |
			FORMAT_MESSAGE_FROM_SYSTEM |
			FORMAT_MESSAGE_IGNORE_INSERTS,
			NULL, err,
			MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
			(LPTSTR)&lpMsgBuf, 0, NULL);
		strncat(buf, lpMsgBuf, LOG_BUF_SIZE - (int)strlen(buf) - 1);
		LocalFree(lpMsgBuf);
		buf[sizeof(buf)-1] = '\0';
		len = strlen(buf);
		if (buf[len-1] == '\n')
			len--;
		if (buf[len-1] == '.')
			len--;
		buf[len-1] = '\0';
#else
		buf[0] = '\0';
#endif
	} else {
		if (((errmsg = pbse_to_txt(errnum)) == NULL) &&
			((errmsg = strerror(errnum)) == NULL))
				errmsg = "";
		(void)snprintf(buf, LOG_BUF_SIZE, "%s (%d) in ", errmsg, errnum);
	}
	(void)strcat(buf, routine);
	(void)strcat(buf, ", ");
	i = LOG_BUF_SIZE - (int)strlen(buf) - 2;
	(void)strncat(buf, text, i);
	buf[LOG_BUF_SIZE -1] = '\0';

	if (log_opened == 0)
		(void)log_open("/dev/console", log_directory);

	if (isatty(2)) {
		if (msg_daemonname == NULL) {
			(void)fprintf(stderr, "%s\n", buf);
		} else {
			(void)fprintf(stderr, "%s: %s\n", msg_daemonname, buf);
		}
	}

	(void)log_record(PBSEVENT_ERROR | PBSEVENT_FORCE, PBS_EVENTCLASS_SERVER,
		LOG_ERR, msg_daemonname, buf);
}

/**
 * @brief
 * 	log_errf - a combination of log_err() and printf()
 *	The error is recorded to the pbs log file and to syslogd if it is
 *	available.  If the error file has not been opened and if syslog is
 *	not defined, then the console is opened.
 *
 * @param[in] errnum - error number
 * @param[in] routine - error in which routine
 * @param[in] fmt - format string
 * @param[in] ... - arguments to format string * 
 *
 */

void
log_errf(int errnum, const char *routine, const char *fmt, ...)
{
	va_list args;
	int len;
	char logbuf[LOG_BUF_SIZE];
	char *buf;

	va_start(args, fmt);

	len = vsnprintf(logbuf, sizeof(logbuf), fmt, args);

	if (len >= sizeof(logbuf)) {
		buf = pbs_asprintf_format(len, fmt, args);
		if (buf == NULL) {
			va_end(args);
			return;
		}
	} else
		buf = logbuf;

	log_err(errnum, routine, buf);

	if (len >= sizeof(logbuf))
		free(buf);
	va_end(args);
}

/**
 * @brief
 * 	log_joberr- log an internal, job-related error
 *	The error is recorded to the pbs log file and to syslogd if it is
 *	available.  If the error file has not been opened and if syslog is
 *	not defined, then the console is opened.  The record written into
 *	the log will be of type PBS_EVENTCLASS_JOB
 *
 * @param[in] errnum - error number
 * @param[in] routine - error in which routine
 * @param[in] text - text to be logged
 * @param[in] pjid - job id which logged error
 *
 * @return	void
 *
 */

void
log_joberr(int errnum, const char *routine, const char *text, const char *pjid)
{
	char buf[LOG_BUF_SIZE], *errmsg;
	int  i;

	if (errnum == -1) {

#ifdef WIN32
		LPVOID	lpMsgBuf;
		DWORD	err = GetLastError();
		int		len;
		snprintf(buf, LOG_BUF_SIZE, "Err(%lu): ", err);
		FormatMessage(
			FORMAT_MESSAGE_ALLOCATE_BUFFER |
			FORMAT_MESSAGE_FROM_SYSTEM |
			FORMAT_MESSAGE_IGNORE_INSERTS,
			NULL, err,
			MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
			(LPTSTR)&lpMsgBuf, 0, NULL);
		strncat(buf, lpMsgBuf, LOG_BUF_SIZE - (int)strlen(buf) - 1);
		LocalFree(lpMsgBuf);
		buf[sizeof(buf)-1] = '\0';
		len = strlen(buf);
		if (buf[len-1] == '\n')
			len--;
		if (buf[len-1] == '.')
			len--;
		buf[len-1] = '\0';
#else
		buf[0] = '\0';
#endif
	} else {

		if (((errmsg = pbse_to_txt(errnum)) == NULL) &&
			((errmsg = strerror(errnum)) == NULL))
				errmsg = "";
		(void)snprintf(buf, LOG_BUF_SIZE, "%s (%d) in ", errmsg, errnum);
	}
	(void)strcat(buf, routine);
	(void)strcat(buf, ", ");
	i = LOG_BUF_SIZE - (int)strlen(buf) - 2;
	(void)strncat(buf, text, i);
	buf[LOG_BUF_SIZE -1] = '\0';

	if (log_opened == 0)
		(void)log_open("/dev/console", log_directory);

	if (isatty(2))
		(void)fprintf(stderr, "%s: %s\n", msg_daemonname, buf);

	(void)log_record(PBSEVENT_ERROR | PBSEVENT_FORCE, PBS_EVENTCLASS_JOB,
		LOG_ERR, pjid, buf);
}

/**
 * @brief
 * 	log_suspect_file - log security information about a file/directory
 *
 * @param[in] func - function id
 * @param[in] text - text to be logged
 * @param[in] file - file path
 * @param[in] sb - status of file
 *
 * @return	Void
 *
 */

void
log_suspect_file(const char *func, const char *text, const char *file, struct stat *sb)
{
	char buf[LOG_BUF_SIZE];

	snprintf(buf, LOG_BUF_SIZE, "Security issue from %s: %s, inode %lu, mode %#lx, uid %ld, gid %ld, ctime %#lx",
		func,
		text,
		(unsigned long)sb->st_ino,
		(unsigned long)sb->st_mode,
		(long)sb->st_uid,
		(long)sb->st_gid,
		(unsigned long)sb->st_ctime
		);
	/*
	 * Log the data.  Note that we swap the text and file name order
	 * because the text is more important in case msg is truncated.
	 */
	log_record(PBSEVENT_SECURITY, PBS_EVENTCLASS_FILE, LOG_CRIT, buf, file);
}

/**
 * @brief
 * 	log_record - log a message to the log file
 *	The log file must have been opened by log_open().
 *
 *	The caller should ensure proper formating of the message if "text"
 *	is to contain "continuation lines".
 *
 * @param[in] eventtype - event type
 * @param[in] objclass - event object class
 * @param[in] sev - indication for whether to syslogging enabled or not
 * @param[in] objname - object name stating log msg related to which object
 * @param[in] text - log msg to be logged.
 *
 *	Note, "sev" (for severity) is used  only if syslogging is enabled.
 *	See syslog(3) for details.
 */

void
log_record(int eventtype, int objclass, int sev, const char *objname, const char *text)
{
	time_t now = 0;
	struct tm *ptm;
	int    rc = 0;
	FILE  *savlog;
	char slogbuf[LOG_BUF_SIZE];
	struct timeval tp;
	char microsec_buf[8] = {0};
#ifndef WIN32
	struct tm ltm;
	sigset_t block_mask;
	sigset_t old_mask;

	/* Block all signals to the process to make the function async-safe */
	sigfillset(&block_mask);
	sigprocmask(SIG_BLOCK, &block_mask, &old_mask);
#endif

#if SYSLOG
	if (syslogopen != 0) {
		snprintf(slogbuf, LOG_BUF_SIZE,
			"%s;%s;%s\n",
			class_names[objclass],
			objname,
			text);
		syslog(sev, "%s", slogbuf);
	}
#endif  /* SYSLOG */

	if (log_opened <= 0)
		goto sigunblock;

	if ((text == NULL) || (objname == NULL))
		goto sigunblock;

	/* if gettimeofday() fails, log messages will be printed at the epoch */
	if (gettimeofday(&tp, NULL) != -1) {
		now = tp.tv_sec;

		if (pbs_log_highres_timestamp)
			snprintf(microsec_buf, sizeof(microsec_buf), ".%06ld", (long)tp.tv_usec);
	}

#ifdef WIN32
	ptm = localtime(&now);
#else
	ptm = localtime_r(&now, &ltm);
#endif

	/* lock the log mutex */
	if (log_mutex_lock() != 0)
		goto sigunblock;

	/* Do we need to switch the log? */
	if (log_auto_switch && (ptm->tm_yday != log_open_day)) {
		log_close(1);
		log_open(NULL, log_directory);
	}

	if (log_opened < 1) {
		log_mutex_unlock();
		rc = errno;
		logfile = fopen("/dev/console", "w");
		if (logfile != NULL) {
			log_err(rc, "log_record", "PBS cannot open its log");
			fclose(logfile);
		}
		goto sigunblock;
	}

	if (locallog != 0 || syslogfac == 0) {
		rc = fprintf(logfile,
			     "%02d/%02d/%04d %02d:%02d:%02d%s;%04x;%s;%s;%s;%s\n",
			     ptm->tm_mon + 1, ptm->tm_mday, ptm->tm_year + 1900,
			     ptm->tm_hour, ptm->tm_min, ptm->tm_sec, microsec_buf,
			     eventtype & ~PBSEVENT_FORCE, msg_daemonname,
			     class_names[objclass], objname, text);

		(void)fflush(logfile);
		if (rc < 0) {
			rc = errno;
			clearerr(logfile);
			savlog = logfile;
			logfile = fopen("/dev/console", "w");

			if (logfile != NULL) {
				log_err(rc, "log_record", "PBS cannot write to its log");
				fclose(logfile);
			}
			logfile = savlog;
		}
	}

	if (log_mutex_unlock() != 0) {
		/* if the unlock fails, rarely, its a dangerous situation
		 * since other threads will stay hung waiting for a lock
		 * while logging, effectively hanging the entire application.
		 * Since we cannot notify this in the log, open the console
		 * and write a message for the administrator to hopefully notice.
		 */
		logfile = fopen("/dev/console", "w");
		if (logfile != NULL) {
			log_err(rc, "log_record", "PBS cannot unlock its log");
			fclose(logfile);
		}
	}

sigunblock:
#ifndef WIN32
	sigprocmask(SIG_SETMASK, &old_mask, NULL);
#else
	return;
#endif
}

/**
 * @brief
 * 	log_close - close the current open log file
 *
 * @param[in] msg - indicating whether to log a message of closing log file before closing it
 *
 * @return	Void
 *
 */

void
log_close(int msg)
{
	if (log_opened == 1) {
		log_auto_switch = 0;
		if (msg) {
			log_record(PBSEVENT_SYSTEM, PBS_EVENTCLASS_SERVER,
				LOG_INFO, "Log", "Log closed");
		}
		(void)fclose(logfile);
		log_opened = 0;
	}
#if SYSLOG
	if (syslogopen) {
		closelog();
		syslogopen = 0;
	}
#endif	/* SYSLOG */
}

/**
 * @brief
 *	Function to set the comm related log levels to event types
 *	on which pbs log mask works.
 *
 * @param[in]	level - The error level as per syslog
 *
 * @return - event type
 *
 */
int
log_level_2_etype(int level)
{
	int etype = PBSEVENT_DEBUG3 | PBSEVENT_DEBUG4;

	if (level == LOG_ERR)
		etype |= PBSEVENT_ERROR;
	else if (level == LOG_CRIT)
		etype |= PBSEVENT_SYSTEM | PBSEVENT_ADMIN | PBSEVENT_FORCE;
	else if (level == LOG_WARNING)
		etype |= PBSEVENT_SYSTEM | PBSEVENT_ADMIN;
	else if (level == LOG_NOTICE || level == LOG_INFO)
		etype |= PBSEVENT_DEBUG | PBSEVENT_DEBUG2;

	return etype;
}
