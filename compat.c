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

/* Various functions which some systems lack. */

#ifndef HAVE_STRCASESTR
#include <string.h>
#include <ctype.h>

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

/* OSX doesn't provide clock_gettime(3) so fall back to gettimeofday(2). */
#ifndef HAVE_CLOCK_GETTIME
#include <sys/time.h>
#include <assert.h>

#include "common.h"

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

/* This is required to prevent an "empty translation unit" warning
   if neither strcasestr() nor clock_gettime() get defined. */
#if defined(HAVE_STRCASESTR) && defined(HAVE_CLOCK_GETTIME)
int compat_is_empty;
#endif
