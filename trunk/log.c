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
#include <time.h>
#include "main.h"

static FILE *logfp = NULL; /* logging file stream */

/* Put something into the log */
void internal_logit (const char *file, const int line, const char *function,
		const char *format, ...)
{
	char msg[256];
	char time_str[20];
	time_t utc_time;
	va_list va;
	struct tm tm_time;

	if (!logfp)
		return;

	va_start (va, format);
	vsnprintf (msg, sizeof(msg), format, va);
	msg[sizeof(msg) - 1] = 0;

	time (&utc_time);
	localtime_r (&utc_time, &tm_time);
	strftime (time_str, sizeof(time_str), "%b %e %T", &tm_time);

	fprintf (logfp, "%s: %s:%d %s(): %s\n", time_str, file, line, function,
			msg);
	fflush (logfp);

	va_end (va);
}

/* fake logit() function for NDEBUG */
void fake_logit (const char *format ATTR_UNUSED, ...)
{
}

/* Initialize logging stream */
void log_init_stream (FILE *f)
{
	logfp = f;
}

void log_close ()
{
	if (!(logfp == stdout || logfp == stderr || logfp == NULL))
		fclose (logfp);
}
