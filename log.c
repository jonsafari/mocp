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
#include <pthread.h>
#include <sys/time.h>
#include <time.h>
#include <errno.h>
#include <signal.h>

#include "common.h"
#include "lists.h"
#include "log.h"

static FILE *logfp = NULL; /* logging file stream */
static enum {
	UNINITIALISED,
	BUFFERING,
	LOGGING
} logging_state = UNINITIALISED;
static lists_t_strs *buffered_log = NULL;
static int log_records_spilt = 0;

static pthread_mutex_t logging_mutex = PTHREAD_MUTEX_INITIALIZER;

#ifndef NDEBUG
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
	{SIGWINCH, "SIGWINCH", 0, 0},
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

static inline void flush_log (void)
{
	int rc;

	if (logfp) {
		do {
			rc = fflush (logfp);
		} while (rc != 0 && errno == EINTR);
	}
}

static void locked_logit (const char *file, const int line,
                          const char *function, const char *msg)
{
	char time_str[20];
	struct timeval utc_time;
	time_t tv_sec;
	struct tm tm_time;
	const char fmt[] = "%s.%06u: %s:%d %s(): %s\n";

	gettimeofday (&utc_time, NULL);
	tv_sec = utc_time.tv_sec;
	localtime_r (&tv_sec, &tm_time);
	strftime (time_str, sizeof (time_str), "%b %e %T", &tm_time);

	if (logfp) {
		fprintf (logfp, fmt, time_str, (unsigned)utc_time.tv_usec,
		                     file, line, function, msg);
	}
	else if (logging_state == BUFFERING) {
		int len;
		char *str;

		len = snprintf (NULL, 0, fmt, time_str, (unsigned)utc_time.tv_usec,
		                              file, line, function, msg);
		str = xmalloc (len + 1);
		snprintf (str, len + 1, fmt, time_str, (unsigned)utc_time.tv_usec,
		                             file, line, function, msg);

		lists_strs_push (buffered_log, str);
	}
}

static void log_signals_raised (void)
{
#ifndef NDEBUG
	size_t ix;

    for (ix = 0; ix < ARRAY_SIZE(sig_info); ix += 1) {
		while (sig_info[ix].raised > sig_info[ix].logged) {
			locked_logit (__FILE__, __LINE__, __FUNCTION__, sig_info[ix].name);
			sig_info[ix].logged += 1;
		}
	}
#endif
}

/* Put something into the log */
void internal_logit (const char *file, const int line, const char *function,
		const char *format, ...)
{
	char *msg;
	va_list va;

	LOCK(logging_mutex);

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
	UNLOCK(logging_mutex);
}

/* fake logit() function for NDEBUG */
void fake_logit (const char *format ATTR_UNUSED, ...)
{
}

/* Initialize logging stream */
void log_init_stream (FILE *f, const char *fn)
{
	char *msg;

	LOCK(logging_mutex);

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

	msg = format_msg ("Writing log to: %s", fn);
	locked_logit (__FILE__, __LINE__, __FUNCTION__, msg);
	free (msg);

	if (log_records_spilt > 0) {
		msg = format_msg ("%d log records spilt", log_records_spilt);
		locked_logit (__FILE__, __LINE__, __FUNCTION__, msg);
		free (msg);
	}

	flush_log ();

	UNLOCK(logging_mutex);
}

void log_close ()
{
	LOCK(logging_mutex);
	if (!(logfp == stdout || logfp == stderr || logfp == NULL)) {
		fclose (logfp);
		logfp = NULL;
	}
	if (buffered_log) {
		lists_strs_free (buffered_log);
		buffered_log = NULL;
	}
	log_records_spilt = 0;
	UNLOCK(logging_mutex);
}
