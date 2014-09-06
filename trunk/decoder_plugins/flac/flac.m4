dnl FLAC

AC_ARG_WITH(flac, AS_HELP_STRING([--without-flac],
                                 [Compile without FLAC support]))

if test "x$with_flac" != "xno"
then
	PKG_CHECK_MODULES(LIBFLAC, [flac >= 1.1.3],
			  [AC_SUBST(LIBFLAC_LIBS)
			   AC_SUBST(LIBFLAC_CFLAGS)
			   want_flac="yes"
			   DECODER_PLUGINS="$DECODER_PLUGINS flac"],
			  [true])
fi

AM_CONDITIONAL([BUILD_flac], [test "$want_flac"])
AC_CONFIG_FILES([decoder_plugins/flac/Makefile])
