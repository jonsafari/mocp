dnl @synopsis MP_WITH_CURSES
dnl
dnl Detect SysV compatible curses, such as ncurses.
dnl
dnl Defines HAVE_CURSES_H or HAVE_NCURSES_H if curses is found.
dnl CURSES_LIB is also set with the required libary, but is not appended
dnl to LIBS automatically. If no working curses libary is found CURSES_LIB
dnl will be left blank.
dnl
dnl This macro adds the option "--with-ncurses" to configure which can
dnl force the use of ncurses or nothing at all.
dnl
dnl @version $Id: mp_with_curses.m4,v 1.2 2002/09/12 21:48:39 guidod Exp $
dnl @author Mark Pulford <mark@kyne.com.au>
dnl
AC_DEFUN([MP_WITH_CURSES],
  [AC_ARG_WITH(ncurses, [  --with-ncurses          Force the use of ncurses over curses],,)
   mp_save_LIBS="$LIBS"
   CURSES_LIB=""
   if test "$with_ncurses" != yes
   then
     AC_CACHE_CHECK([for working curses], mp_cv_curses,
       [LIBS="$LIBS -lcurses"
        AC_TRY_LINK(
          [#include <curses.h>],
          [chtype a; int b=A_STANDOUT, c=KEY_LEFT; initscr(); ],
          mp_cv_curses=yes, mp_cv_curses=no)])
     if test "$mp_cv_curses" = yes
     then
       AC_DEFINE(HAVE_CURSES_H, 1, [Define if you have curses.h])
       CURSES_LIB="-lcurses"
     fi
   fi
   if test ! "$CURSES_LIB"
   then
     AC_CACHE_CHECK([for working ncurses], mp_cv_ncurses,
       [LIBS="$mp_save_LIBS -lncurses"
        AC_TRY_LINK(
          [#include <ncurses.h>],
          [chtype a; int b=A_STANDOUT, c=KEY_LEFT; initscr(); ],
          mp_cv_ncurses=yes, mp_cv_ncurses=no)])
     if test "$mp_cv_ncurses" = yes
     then
       AC_DEFINE(HAVE_NCURSES_H, 1, [Define if you have ncurses.h])
       CURSES_LIB="-lncurses"
     fi
   fi
   LIBS="$mp_save_LIBS"
])dnl
