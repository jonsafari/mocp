#ifndef UTF8_H
#define UTF8_H

#include <stdarg.h>

void utf8_init ();
void utf8_cleanup ();
int xwaddstr (WINDOW *win, const char *str);
int xwaddnstr (WINDOW *win, const char *str, const int n);
int xmvwaddstr (WINDOW *win, const int y, const int x, const char *str);
int xmvwaddnstr (WINDOW *win, const int y, const int x, const char *str,
		const int n);

#ifdef HAVE__ATTRIBUTE__
int xwprintw (WINDOW *win, const char *fmt, ...)
	__attribute__ ((format (printf, 2, 3)));
#else
int xwprintw (WINDOW *win, const char *fmt, ...);
#endif

size_t strwidth (const char *s);

#endif
