dnl wavpack

AC_ARG_WITH(wavpack, AS_HELP_STRING([--without-wavpack],
                                    [Compile without WavPack support]))

if test "x$with_wavpack" != "xno"
then
	PKG_CHECK_MODULES(WAVPACK, [wavpack >= 4.31],
			[AC_SUBST(WAVPACK_LIBS)
			AC_SUBST(WAVPACK_CFLAGS)
			want_wavpack="yes"
			DECODER_PLUGINS="$DECODER_PLUGINS wavpack"],
			[true])
fi

AM_CONDITIONAL([BUILD_wavpack], [test "$want_wavpack"])
AC_CONFIG_FILES([decoder_plugins/wavpack/Makefile])
