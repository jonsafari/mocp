#ifndef UTF8_H
#define UTF8_H

#ifdef HAVE_NCURSESW_H
# include <ncursesw/curses.h>
#elif HAVE_NCURSES_H
# include <ncurses.h>
#else
# include <curses.h>
#endif

#include <stdarg.h>
#ifdef HAVE_ICONV
# include <iconv.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* parameter passed to wcswidth() as a maximum width */
#define WIDTH_MAX	2048

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
char *xstrtail (const char *str, const int len);

char *iconv_str (const iconv_t desc, const char *str);
char *files_iconv_str (const char *str);
char *xterm_iconv_str (const char *str);

#ifdef __cplusplus
}
#endif

#endif
