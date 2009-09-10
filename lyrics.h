#ifndef  LYRICS_H
#define  LYRICS_H

#ifdef HAVE_NCURSESW_H
# include <ncursesw/curses.h>
#elif HAVE_NCURSES_H
# include <ncurses.h>
#elif HAVE_CURSES_H
# include <curses.h>
#endif

void lyrics_cleanup (const unsigned int n);
char **get_lyrics_text (const WINDOW*, const char*, int*);

#endif
