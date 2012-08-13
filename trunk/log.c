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
#include <pthread.h>
#include <sys/time.h>
#include <time.h>

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

/* Put something into the log */
void internal_logit (const char *file, const int line, const char *function,
		const char *format, ...)
{
	int len;
	char *msg, time_str[20];
	struct timeval utc_time;
	va_list va;
	struct tm tm_time;
	const char fmt[] = "%s.%06u: %s:%d %s(): %s\n";

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
			UNLOCK(logging_mutex);
			return;
		}
	}

	va_start (va, format);
	len = vsnprintf (NULL, 0, format, va) + 1;
	va_end (va);
	msg = xmalloc (len);
	va_start (va, format);
	vsnprintf (msg, len, format, va);
	va_end (va);

	gettimeofday (&utc_time, NULL);
	localtime_r (&utc_time.tv_sec, &tm_time);
	strftime (time_str, sizeof (time_str), "%b %e %T", &tm_time);

	if (logfp) {
		fprintf (logfp, fmt, time_str, (unsigned)utc_time.tv_usec,
		                     file, line, function, msg);
		fflush (logfp);
	}
	else {
		char *str;

		len = snprintf (NULL, 0, fmt, time_str, (unsigned)utc_time.tv_usec,
		                              file, line, function, msg);
		str = xmalloc (len + 1);
		snprintf (str, len + 1, fmt, time_str, (unsigned)utc_time.tv_usec,
		                             file, line, function, msg);

		lists_strs_push (buffered_log, str);
	}

	UNLOCK(logging_mutex);

	free (msg);
}

/* fake logit() function for NDEBUG */
void fake_logit (const char *format ATTR_UNUSED, ...)
{
}

/* Initialize logging stream */
void log_init_stream (FILE *f, const char *fn)
{
	logfp = f;

	LOCK(logging_mutex);

	if (logging_state == BUFFERING) {
		if (logfp) {
			int ix;

			for (ix = 0; ix < lists_strs_size (buffered_log); ix += 1)
				fprintf (logfp, "%s", lists_strs_at (buffered_log, ix));

			fflush (logfp);
		}
		lists_strs_free (buffered_log);
		buffered_log = NULL;
	}

	logging_state = LOGGING;

	UNLOCK(logging_mutex);

	logit ("Writing log to: %s", fn);
	if (log_records_spilt > 0)
		logit ("%d log records spilt", log_records_spilt);
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
