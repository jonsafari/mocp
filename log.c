/*
 * MOC - music on console
 * Copyright (C) 2004 Damian Pietras <daper@daper.net>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 */

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <stdio.h>
#include <stdarg.h>
#include <stdint.h>
#include <assert.h>
#include <pthread.h>
#include <time.h>
#include <errno.h>
#include <signal.h>

#include "common.h"
#include "lists.h"
#include "log.h"
#include "options.h"

#ifndef NDEBUG
static FILE *logfp = NULL; /* logging file stream */

static enum {
	UNINITIALISED,
	BUFFERING,
	LOGGING
} logging_state = UNINITIALISED;

static lists_t_strs *buffered_log = NULL;
static int log_records_spilt = 0;

static lists_t_strs *circular_log = NULL;
static int circular_ptr = 0;

static pthread_mutex_t logging_mtx = PTHREAD_MUTEX_INITIALIZER;

static struct {
	int sig;
	const char *name;
	volatile uint64_t raised;
	uint64_t logged;
} sig_info[] = {
	{SIGINT, "SIGINT", 0, 0},
	{SIGHUP, "SIGHUP", 0, 0},
	{SIGQUIT, "SIGQUIT", 0, 0},
	{SIGTERM, "SIGTERM", 0, 0},
	{SIGCHLD, "SIGCHLD", 0, 0},
#ifdef SIGWINCH
	{SIGWINCH, "SIGWINCH", 0, 0},
#endif
	{0, "SIG other", 0, 0}
};
#endif

#ifndef NDEBUG
void log_signal (int sig)
{
	int ix = 0;

	while (sig_info[ix].sig && sig_info[ix].sig != sig)
		ix += 1;

	sig_info[ix].raised += 1;
}
#endif

#ifndef NDEBUG
static inline void flush_log (void)
{
	int rc;

	if (logfp) {
		do {
			rc = fflush (logfp);
		} while (rc != 0 && errno == EINTR);
	}
}
#endif

#ifndef NDEBUG
static void locked_logit (const char *file, const int line,
                          const char *function, const char *msg)
{
	int len;
	char *str, time_str[20];
	struct timespec utc_time;
	time_t tv_sec;
	struct tm tm_time;
	const char fmt[] = "%s.%06ld: %s:%d %s(): %s\n";

	assert (logging_state == BUFFERING || logging_state == LOGGING);
	assert (logging_state != BUFFERING || !logfp);
	assert (logging_state != BUFFERING || !circular_log);
	assert (logging_state != LOGGING || logfp || !circular_log);

	if (logging_state == LOGGING && !logfp)
		return;

	clock_gettime (CLOCK_REALTIME, &utc_time);
	tv_sec = utc_time.tv_sec;
	localtime_r (&tv_sec, &tm_time);
	strftime (time_str, sizeof (time_str), "%b %e %T", &tm_time);

	if (logfp && !circular_log) {
		fprintf (logfp, fmt, time_str, utc_time.tv_nsec / 1000L,
		                     file, line, function, msg);
		return;
	}

	len = snprintf (NULL, 0, fmt, time_str, utc_time.tv_nsec / 1000L,
	                              file, line, function, msg);
	str = xmalloc (len + 1);
	snprintf (str, len + 1, fmt, time_str, utc_time.tv_nsec / 1000L,
	                             file, line, function, msg);

	if (logging_state == BUFFERING) {
		lists_strs_push (buffered_log, str);
		return;
	}

	assert (circular_log);

	if (circular_ptr == lists_strs_capacity (circular_log))
		circular_ptr = 0;
	if (circular_ptr < lists_strs_size (circular_log))
		free (lists_strs_swap (circular_log, circular_ptr, str));
	else
		lists_strs_push (circular_log, str);
	circular_ptr += 1;
}
#endif

#ifndef NDEBUG
static void log_signals_raised (void)
{
	size_t ix;

    for (ix = 0; ix < ARRAY_SIZE(sig_info); ix += 1) {
		while (sig_info[ix].raised > sig_info[ix].logged) {
			locked_logit (__FILE__, __LINE__, __func__, sig_info[ix].name);
			sig_info[ix].logged += 1;
		}
	}
}
#endif

/* Put something into the log.  If built with logging disabled,
 * this function is provided as a stub so independant plug-ins
 * configured with logging enabled can still resolve it. */
void internal_logit (const char *file LOGIT_ONLY,
                     const int line LOGIT_ONLY,
                     const char *function LOGIT_ONLY,
                     const char *format LOGIT_ONLY, ...)
{
#ifndef NDEBUG
	int saved_errno = errno;
	char *msg;
	va_list va;

	LOCK(logging_mtx);

	if (!logfp) {
		switch (logging_state) {
		case UNINITIALISED:
			buffered_log = lists_strs_new (128);
			logging_state = BUFFERING;
			break;
		case BUFFERING:
			/* Don't let storage run away on us. */
			if (lists_strs_size (buffered_log) < lists_strs_capacity (buffered_log))
				break;
			log_records_spilt += 1;
		case LOGGING:
			goto end;
		}
	}

	log_signals_raised ();

	va_start (va, format);
	msg = format_msg_va (format, va);
	va_end (va);
	locked_logit (file, line, function, msg);
	free (msg);

	flush_log ();

	log_signals_raised ();

end:
	UNLOCK(logging_mtx);

	errno = saved_errno;
#endif
}

/* Initialize logging stream */
void log_init_stream (FILE *f LOGIT_ONLY, const char *fn LOGIT_ONLY)
{
#ifndef NDEBUG
	char *msg;

	LOCK(logging_mtx);

	logfp = f;

	if (logging_state == BUFFERING) {
		if (logfp) {
			int ix;

			for (ix = 0; ix < lists_strs_size (buffered_log); ix += 1)
				fprintf (logfp, "%s", lists_strs_at (buffered_log, ix));
		}
		lists_strs_free (buffered_log);
		buffered_log = NULL;
	}

	logging_state = LOGGING;
	if (!logfp)
		goto end;

	msg = format_msg ("Writing log to: %s", fn);
	locked_logit (__FILE__, __LINE__, __func__, msg);
	free (msg);

	if (log_records_spilt > 0) {
		msg = format_msg ("%d log records spilt", log_records_spilt);
		locked_logit (__FILE__, __LINE__, __func__, msg);
		free (msg);
	}

	flush_log ();

end:
	UNLOCK(logging_mtx);
#endif
}

/* Start circular logging (if enabled). */
void log_circular_start ()
{
#ifndef NDEBUG
	int circular_size;

	assert (logging_state == LOGGING);
	assert (!circular_log);

	if (!logfp)
		return;

	circular_size = options_get_int ("CircularLogSize");
	if (circular_size > 0) {
		LOCK(logging_mtx);

		circular_log = lists_strs_new (circular_size);
		circular_ptr = 0;

		UNLOCK(logging_mtx);
	}
#endif
}

/* Internal circular log reset. */
#ifndef NDEBUG
static inline void locked_circular_reset ()
{
	lists_strs_clear (circular_log);
	circular_ptr = 0;
}
#endif

/* Reset the circular log (if enabled). */
void log_circular_reset ()
{
#ifndef NDEBUG
	assert (logging_state == LOGGING);

	if (!circular_log)
		return;

	LOCK(logging_mtx);

	locked_circular_reset ();

	UNLOCK(logging_mtx);
#endif
}

/* Write circular log (if enabled) to the log file. */
void log_circular_log ()
{
#ifndef NDEBUG
	int ix;

	assert (logging_state == LOGGING && (logfp || !circular_log));

	if (!circular_log)
		return;

	LOCK(logging_mtx);

	fprintf (logfp, "\n* Circular Log Starts *\n\n");

	for (ix = circular_ptr; ix < lists_strs_size (circular_log); ix += 1)
		fprintf (logfp, "%s", lists_strs_at (circular_log, ix));

	fflush (logfp);

	for (ix = 0; ix < circular_ptr; ix += 1)
		fprintf (logfp, "%s", lists_strs_at (circular_log, ix));

	fprintf (logfp, "\n* Circular Log Ends *\n\n");

	fflush (logfp);

	locked_circular_reset ();

	UNLOCK(logging_mtx);
#endif
}

/* Stop circular logging (if enabled). */
void log_circular_stop ()
{
#ifndef NDEBUG
	assert (logging_state == LOGGING);

	if (!circular_log)
		return;

	LOCK(logging_mtx);

	lists_strs_free (circular_log);
	circular_log = NULL;
	circular_ptr = 0;

	UNLOCK(logging_mtx);
#endif
}

void log_close ()
{
#ifndef NDEBUG
	LOCK(logging_mtx);

	if (!(logfp == stdout || logfp == stderr || logfp == NULL)) {
		fclose (logfp);
		logfp = NULL;
	}

	if (buffered_log) {
		lists_strs_free (buffered_log);
		buffered_log = NULL;
	}

	log_records_spilt = 0;

	UNLOCK(logging_mtx);
#endif
}
