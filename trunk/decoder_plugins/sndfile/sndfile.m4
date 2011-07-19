dnl libsndfile

AC_ARG_WITH(sndfile, AS_HELP_STRING([--without-sndfile],
                                    [Compile without libsndfile]))

if test "x$with_sndfile" != "xno"
then
	PKG_CHECK_MODULES(sndfile, sndfile >= 1.0.0,
			   [AC_SUBST(sndfile_LIBS)
			   AC_SUBST(sndfile_CFLAGS)
			   want_sndfile="yes"
			   DECODER_PLUGINS="$DECODER_PLUGINS sndfile"],
			   [true])
fi

AM_CONDITIONAL([BUILD_sndfile], [test "$want_sndfile"])
AC_CONFIG_FILES([decoder_plugins/sndfile/Makefile])
