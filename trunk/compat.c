/*
 * MOC - music on console
 * Copyright (C) 2005 Damian Pietras <daper@daper.net>
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

#include <pthread.h>
#include <string.h>
#include <errno.h>
#include <assert.h>

#ifndef HAVE_CLOCK_GETTIME
# include <sys/time.h>
#endif

#include "common.h"
#include "log.h"

/* Various functions which some systems lack. */

#ifndef HAVE_STRCASESTR
/* Case insensitive version of strstr(). */
char *strcasestr (const char *haystack, const char *needle)
{
	char *haystack_i, *needle_i;
	char *c;
	char *res;

	haystack_i = xstrdup (haystack);
	needle_i = xstrdup (needle);

	c = haystack_i;
	while (*c) {
		*c = tolower (*c);
		c++;
	}

	c = needle_i;
	while (*c) {
		*c = tolower (*c);
		c++;
	}

	res = strstr (haystack_i, needle_i);
	free (haystack_i);
	free (needle_i);
	return res ? res - haystack_i + (char *)haystack : NULL;
}
#endif

#ifndef HAVE_STRERROR_R
static pthread_mutex_t strerror_r_mtx = PTHREAD_MUTEX_INITIALIZER;
#endif

#ifndef HAVE_STRERROR_R
int strerror_r (int errnum, char *buf, size_t n)
{
	char *err_str;
	int ret_val = 0;

	LOCK (strerror_r_mtx);

	err_str = strerror (errnum);
	if (strlen (err_str) >= n) {
		errno = ERANGE;
		ret_val = -1;
	}
	else
		strcpy (buf, err_str);

	UNLOCK (strerror_r_mtx);

	return ret_val;
}
#endif

/* OSX doesn't provide clock_gettime(3) so fall back to gettimeofday(2). */
#ifndef HAVE_CLOCK_GETTIME
int clock_gettime (int clk_id ASSERT_ONLY, struct timespec *ts)
{
	int result;
	struct timeval tv;

	assert (clk_id == CLOCK_REALTIME);

	result = gettimeofday (&tv, NULL);
	ts->tv_sec = tv.tv_sec;
	ts->tv_nsec = tv.tv_usec * 1000L;

	return result;
}
#endif

void compat_cleanup ()
{
#ifndef HAVE_STRERROR_R
	int rc;

	rc = pthread_mutex_destroy (&strerror_r_mtx);
	if (rc != 0)
		logit ("Can't destroy strerror_r_mtx: %s", strerror (rc));
#endif
}
