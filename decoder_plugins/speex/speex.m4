dnl speex

AC_ARG_WITH(speex, AS_HELP_STRING([--without-speex],
                                  [Compile without speex support]))

if test "x$with_speex" != "xno"
then
	PKG_CHECK_MODULES(speex, [ogg >= 1.0 speex >= 1.0.0],
			  [AC_SUBST(speex_LIBS)
			  AC_SUBST(speex_CFLAGS)
			  want_speex="yes"
			  DECODER_PLUGINS="$DECODER_PLUGINS speex"],
			  [true])
fi

AM_CONDITIONAL([BUILD_speex], [test "$want_speex"])
AC_CONFIG_FILES([decoder_plugins/speex/Makefile])
