/*
 * MOC - music on console
 * Copyright (C) 2004 - 2005 Damian Pietras <daper@daper.net>
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
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include "server.h"
#include "interface.h"
#include "log.h"
#include "common.h"
#include "options.h"

static int im_server = 0; /* Em I the server? */

static int count_str(const char *src, const char *str);

void error (const char *format, ...)
{
	va_list va;
	char msg[256];
	
	va_start (va, format);
	vsnprintf (msg, sizeof(msg), format, va);
	msg[sizeof(msg) - 1] = 0;
	va_end (va);
	
	if (im_server)
		server_error (msg);
	else
		interface_error (msg);
}

/* End program with a message. Use when an error occurs and we can't recover. */
void fatal (const char *format, ...)
{
	va_list va;
	char msg[256];
	
	va_start (va, format);
	vsnprintf (msg, sizeof(msg), format, va);
	msg[sizeof(msg) - 1] = 0;
	fprintf (stderr, "\nFATAL_ERROR: %s\n\n", msg);
	logit ("FATAL ERROR: %s", msg);
	va_end (va);

	exit (EXIT_FATAL);
}

void *xmalloc (const size_t size)
{
	void *p;

	if ((p = malloc(size)) == NULL)
		fatal ("Can't allocate memory!");
	return p;
}

void *xcalloc (size_t nmemb, size_t size)
{
	void *p;

	if ((p = calloc(nmemb, size)) == NULL)
		fatal ("Can't allocate memory!");
	return p;
}

void *xrealloc (void *ptr, const size_t size)
{
	void *p;

	if ((p = realloc(ptr, size)) == NULL && size != 0)
		fatal ("Can't allocate memory!");
	return p;

}

char *xstrdup (const char *s)
{
	char *n;

	if (s && (n = strdup(s)) == NULL)
		fatal ("Can't allocate memory!");

	return s ? n : NULL;
}

void set_me_server ()
{
	im_server = 1;
}

static int count_str (const char *src, const char *str)
{
	size_t str_len = strlen(str);
	size_t src_len = strlen(src);
	if (str_len > src_len)
		return 0;
	const char *s, *p;
	s = src;
	int count = 0;
	while (s != NULL) {
		p = strstr(s, str);
		if (p == NULL)
			break;
		else
			count++;
		if ((int)strlen(s) - (int)str_len < 0)
			break;
		s = p + str_len;
	}

	return count;
}

char *str_repl (char *target, const char *oldstr, const char *newstr)
{
	size_t oldstr_len = strlen(oldstr);
	size_t newstr_len = strlen(newstr);
	size_t target_len = strlen(target);
	if (oldstr_len > target_len)
		return target;
	int hits = count_str(target, oldstr);
	if (hits == 0)
		return target;
	char *s, *p;

	if (oldstr_len != newstr_len)
		target = xrealloc(target,
		                  target_len - hits*oldstr_len + hits*newstr_len + 1);

	s = target;
	while (s != NULL) {
		p = strstr(s, oldstr);
		if (p == NULL)
			return target;
		memmove(p + newstr_len, p + oldstr_len, strlen(p + oldstr_len) + 1);
		memcpy(p, newstr, newstr_len);
		s = p + newstr_len;
	}
	return target;
}


/* Return path to a file in MOC config directory. NOT THREAD SAFE */
char *create_file_name (const char *file)
{
	char *home_dir;
	static char fname[PATH_MAX];
	char *moc_dir = options_get_str ("MOCDir");
	
	if (moc_dir[0] == '~') {
		if (!(home_dir = getenv("HOME")))
			fatal ("No HOME environmential variable.");
		if (snprintf(fname, sizeof(fname), "%s/%s/%s", home_dir,
				(moc_dir[1] == '/') ? moc_dir + 2 : moc_dir + 1,
				file)
				>= (int)sizeof(fname))
			fatal ("Path too long.");
	}
	else if (snprintf(fname, sizeof(fname), "%s/%s", moc_dir, file)
			>= (int)sizeof(fname))
		fatal ("Path too long.");

	return fname;
}

/* Convert time in second to min:sec text format. buff must be 6 chars long. */
void sec_to_min (char *buff, const int seconds)
{
	assert (seconds >= 0);

	if (seconds < 6000) {

		/* the time is less than 99:59 */
		int min, sec;
		
		min = seconds / 60;
		sec = seconds % 60;

		snprintf (buff, 6, "%02d:%02d", min, sec);
	}
	else if (seconds < 10000 * 60) 

		/* the time is less than 9999 minutes */
		snprintf (buff, 6, "%4dm", seconds/60);
	else
		strcpy (buff, "!!!!!");
}
