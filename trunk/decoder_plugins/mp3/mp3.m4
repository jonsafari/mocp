dnl libmad (mp3)

AC_ARG_WITH(mp3, AS_HELP_STRING([--without-mp3],
                                [Compile without mp3 support (libmad)]))

if test "x$with_mp3" != "xno"
then
	AC_CHECK_LIB(mad, mad_stream_init, [
		AC_CHECK_HEADER([mad.h], ,)])

	if test "$ac_cv_lib_mad_mad_stream_init" = "yes" -a "$HAVE_ID3TAG" = "yes"
	then
		want_mp3="yes"
		DECODER_PLUGINS="$DECODER_PLUGINS mp3"
	fi
fi

AM_CONDITIONAL([BUILD_mp3], [test "$want_mp3"])
AC_CONFIG_FILES([decoder_plugins/mp3/Makefile])
