dnl libtimidity

AC_ARG_WITH(timidity, AS_HELP_STRING([--without-timidity],
                                     [Compile without libtimidity]))

if test "x$with_timidity" != "xno"
then
	PKG_CHECK_MODULES(timidity, libtimidity >= 0.1.0,
			   [AC_SUBST(timidity_LIBS)
			   AC_SUBST(timidity_CFLAGS)
			   want_timidity="yes"
			   DECODER_PLUGINS="$DECODER_PLUGINS timidity"],
			   [true])
fi

AM_CONDITIONAL([BUILD_timidity], [test "$want_timidity"])
AC_CONFIG_FILES([decoder_plugins/timidity/Makefile])
