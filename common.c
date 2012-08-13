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
# undef malloc
#endif

#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <assert.h>
#include <unistd.h>
#include <sys/types.h>
#include <pwd.h>
#include <errno.h>
#ifdef HAVE_SYSLOG
#include <syslog.h>
#endif

#include "common.h"
#include "server.h"
#include "interface.h"
#include "interface_elements.h"
#include "log.h"
#include "options.h"

static int im_server = 0; /* Am I the server? */

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

/* End program with a message. Use when an error occurs and we can't recover.
 * If we're the server, then also log the message to the system log. */
void internal_fatal (const char *file ATTR_UNUSED, int line ATTR_UNUSED,
                 const char *function ATTR_UNUSED, const char *format, ...)
{
	va_list va;
	char msg[256];

	windows_reset ();

	va_start (va, format);
	vsnprintf (msg, sizeof(msg), format, va);
	msg[sizeof(msg) - 1] = 0;
	fprintf (stderr, "\nFATAL_ERROR: %s\n\n", msg);
#ifndef NDEBUG
	internal_logit (file, line, function, "FATAL ERROR: %s", msg);
#endif
	va_end (va);

	log_close ();

#ifdef HAVE_SYSLOG
	if (im_server)
		syslog (LOG_USER|LOG_ERR, "%s", msg);
#endif

	exit (EXIT_FATAL);
}

void *xmalloc (size_t size)
{
	void *p;

#ifndef HAVE_MALLOC
	size = MAX(1, size);
#endif

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

char *str_repl (char *target, const char *oldstr, const char *newstr)
{
	size_t oldstr_len = strlen(oldstr);
	size_t newstr_len = strlen(newstr);
	size_t target_len = strlen(target);
	size_t target_max = target_len;
	size_t s, p;
	char *needle;
	for (s = 0; (needle = strstr(target + s, oldstr)) != NULL; s = p + newstr_len) {
		target_len += newstr_len - oldstr_len;
		p = needle - target;
		if (target_len + 1 > target_max) {
			target_max = MAX(target_len + 1, target_max * 2);
			target = xrealloc(target, target_max);
		}
		memmove(target + p + newstr_len, target + p + oldstr_len, target_len - p - newstr_len + 1);
		memcpy(target + p, newstr, newstr_len);
	}
	target = xrealloc(target, target_len + 1);
	return target;
}

/* Extract a substring starting at 'src' for length 'len' and remove
 * any leading and trailing whitespace.  Return NULL if unable.  */
char *trim (const char *src, size_t len)
{
	char *result;
	const char *first, *last;

	for (last = &src[len - 1]; last >= src; last -= 1) {
		if (!isspace (*last))
			break;
	}
	if (last < src)
		return NULL;

	for (first = src; first <= last; first += 1) {
		if (!isspace (*first))
			break;
	}
	if (first > last)
		return NULL;

	last += 1;
	result = xcalloc (last - first + 1, sizeof (char));
	strncpy (result, first, last - first);
	result[last - first] = 0x00;

	return result;
}

/* Return true iff the argument would be a syntactically valid symbol.
 * (Note that the so-called "peculiar indentifiers" are disallowed here.) */
bool is_valid_symbol (const char *candidate)
{
	size_t len;
	bool result;
	const char *first = "+-.0123456789@";
	const char *valid = "abcdefghijklmnopqrstuvwxyz"
	                    "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
	                    "0123456789"
	                    "@?!.+-*/<=>:$%^&_~";

	result = false;
	len = strlen (candidate);
	if (len > 0 && len == strspn (candidate, valid) &&
	               strchr (first, candidate[0]) == NULL)
		result = true;

	return result;
}

/* Return path to a file in MOC config directory. NOT THREAD SAFE */
char *create_file_name (const char *file)
{
	static char fname[PATH_MAX];
	char *moc_dir = options_get_str ("MOCDir");

	if (moc_dir[0] == '~') {
		if (snprintf(fname, sizeof(fname), "%s/%s/%s", get_home (),
				(moc_dir[1] == '/') ? moc_dir + 2 : moc_dir + 1,
				file)
				>= (int)sizeof(fname))
			fatal ("Path too long!");
	}
	else if (snprintf(fname, sizeof(fname), "%s/%s", moc_dir, file)
			>= (int)sizeof(fname))
		fatal ("Path too long!");

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

/* Determine and return the path of the user's home directory. */
const char *get_home ()
{
	static const char *home = NULL;
	struct passwd *passwd;

	if (home == NULL) {
		home = xstrdup (getenv ("HOME"));
		if (home == NULL) {
			errno = 0;
			passwd = getpwuid (geteuid ());
			if (passwd)
				home = xstrdup (passwd->pw_dir);
			else
				if (errno != 0)
					logit ("getpwuid(%d): %s", geteuid (), strerror (errno));
		}
	}

	return home;
}
