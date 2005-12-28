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

#include <stdarg.h>
#ifdef HAVE_ICONV
# include <iconv.h>
#endif
#ifdef HAVE_NL_TYPES_H
# include <nl_types.h>
#endif
#ifdef HAVE_LANGINFO_H
# include <langinfo.h>
#endif
#ifdef HAVE_NCURSESW_H
# include <ncursesw/curses.h>
#elif HAVE_NCURSES_H
# include <ncurses.h>
#else
# include <curses.h>
#endif
#include <assert.h>
#include <string.h>
#include <errno.h>
#include <wchar.h>

#include "common.h"
#include "log.h"
#include "utf8.h"

static char *terminal_charset = NULL;
static int using_utf8 = 0;

#ifdef HAVE_ICONV
static iconv_t iconv_desc = (iconv_t)(-1);
#endif

/* Return a malloc()ed string converted using iconv().
 * if for_file_name is not 0, uses the conversion defined for file names.
 * For NULL returns NULL. */
static char *iconv_str (const char *str)
{
#ifdef HAVE_ICONV
	char buf[512];
	char *inbuf, *outbuf;
	char *str_copy;
	size_t inbytesleft, outbytesleft;
	char *converted;

	if (!str)
		return NULL;
	if (iconv_desc == (iconv_t)(-1))
		return xstrdup (str);

	str_copy = inbuf = xstrdup (str);
	outbuf = buf;
	inbytesleft = strlen(inbuf);
	outbytesleft = sizeof(buf) - 1;

	iconv (iconv_desc, NULL, NULL, NULL, NULL);
	
	while (inbytesleft) {
		if (iconv(iconv_desc, &inbuf, &inbytesleft, &outbuf,
					&outbytesleft)
				== (size_t)(-1)) {
			if (errno == EILSEQ) {
				inbuf++;
				inbytesleft--;
				if (!--outbytesleft) {
					*outbuf = 0;
					break;
				}
				*(outbuf++) = '#';
			}
			else if (errno == EINVAL) {
				*(outbuf++) = '#';
				*outbuf = 0;
				break;
			}
			else if (errno == E2BIG) {
				outbuf[sizeof(buf)-1] = 0;
				break;
			}
		}
	}

	*outbuf = 0;
	converted = xstrdup (buf);
	free (str_copy);
	
	return converted;
#else /* HAVE_ICONV */
	return xstrdup (str); /* TODO: we should strip unicode (non-ASCII)
				 characters here */
#endif
}

int xwaddstr (WINDOW *win, const char *str)
{
	int res;
	
	if (using_utf8)
		res = waddstr (win, str);
	else {
		char *lstr = iconv_str (str);

		res = waddstr (win, lstr);
		free (lstr);
	}

	return res;
}

int xwaddnstr (WINDOW *win, const char *str, const int n)
{
	int res;
	
	if (using_utf8)
		res = waddnstr (win, str, n);
	else {
		char *lstr = iconv_str (str);

		res = waddnstr (win, lstr, n);
		free (lstr);
	}

	return res;
}

int xmvwaddstr (WINDOW *win, const int y, const int x, const char *str)
{
	int res;
	
	if (using_utf8)
		res = mvwaddstr (win, y, x, str);
	else {
		char *lstr = iconv_str (str);

		res = mvwaddstr (win, y, x, lstr);
		free (lstr);
	}

	return res;
}

int xmvwaddnstr (WINDOW *win, const int y, const int x, const char *str,
		const int n)
{
	int res;
	
	if (using_utf8)
		res = mvwaddnstr (win, y, x, str, n);
	else {
		char *lstr = iconv_str (str);

		res = mvwaddnstr (win, y, x, lstr, n);
		free (lstr);
	}

	return res;
}

int xwprintw (WINDOW *win, const char *fmt, ...)
{
	va_list va;
	int res;
	char buf[1024];

	va_start (va, fmt);
	vsnprintf (buf, sizeof(buf), fmt, va);
	buf[sizeof(buf)-1] = 0;
	va_end (va);

	if (using_utf8)
		res = waddstr (win, buf);
	else {
		char *lstr = iconv_str (buf);

		res = waddstr (win, lstr);
		free (lstr);
	}

	return res;
}

static void iconv_cleanup ()
{
#ifdef HAVE_ICONV
	if (iconv_desc != (iconv_t)(-1)
			&& iconv_close(iconv_desc) == -1)
		logit ("iconv_close() failed: %s", strerror(errno));
#endif
}

void utf8_init ()
{
#ifdef HAVE_NL_LANGINFO_CODESET
#ifdef HAVE_NL_LANGINFO
	terminal_charset = xstrdup (nl_langinfo(CODESET));
	assert (terminal_charset != NULL);

	if (!strcmp(terminal_charset, "UTF-8")) {
#ifdef HAVE_NCURSESW
		logit ("Using UTF8 output");
		using_utf8 = 1;
#else /* HAVE_NCURSESW */
		terminal_charset = xstrdup ("US-ASCII");
		logit ("Using US-ASCII conversion - compiled without "
				"libncursesw");
#endif /* HAVE_NCURSESW */
	}
	else
		logit ("Terminal character set: %s", terminal_charset);
#else /* HAVE_NL_LANGINFO */
	terminal_charset = xstrdup ("US-ASCII");
	logit ("Assuming US-ASCII terminal character set");
#endif /* HAVE_NL_LANGINFO */
#endif /* HAVE_NL_LANGINFO_CODESET */

#ifdef HAVE_ICONV
	if (!using_utf8 && terminal_charset) {
		iconv_desc = iconv_open (terminal_charset, "UTF-8");
		if (iconv_desc == (iconv_t)(-1))
			logit ("iconv_open() failed: %s", strerror(errno));
	}
#endif
}

void utf8_cleanup ()
{
	if (terminal_charset)
		free (terminal_charset);
	iconv_cleanup ();
}

/* Return the number of columns the string takes when displayed. */
size_t strwidth (const char *s)
{
	return strlen (s); // temporary
	// TODO: consider also UTF8 -> ASCII conversion
}
