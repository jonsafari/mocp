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

/* This is required to prevent an "empty translation unit" warning
   if neither strcasestr() nor clock_gettime() get defined. */
#if defined(HAVE_STRCASESTR) && defined(HAVE_CLOCK_GETTIME)
int compat_is_empty;
#endif
