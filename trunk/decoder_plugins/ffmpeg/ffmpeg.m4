dnl ffmpeg

AC_ARG_WITH(ffmpeg, AS_HELP_STRING([--without-ffmpeg],
                                   [Compile without ffmpeg]))

if test "x$with_ffmpeg" != "xno"
then
	PKG_CHECK_MODULES(libavformat, libavformat,
		[AC_SUBST(libavformat_CFLAGS)
		 AC_SUBST(libavformat_LIBS)
		 want_ffmpeg="yes"
		 DECODER_PLUGINS="$DECODER_PLUGINS ffmpeg"],
		[AC_CHECK_PROG([FFMPEG_CONFIG], [ffmpeg-config], [yes])
		 if test "x$FFMPEG_CONFIG" = "xyes"
		 then
			 libavformat_CFLAGS=`ffmpeg-config --cflags`
			 libavformat_LIBS=`ffmpeg-config --plugin-libs avformat`
			 AC_SUBST(libavformat_CFLAGS)
			 AC_SUBST(libavformat_LIBS)
			 want_ffmpeg="yes"
			 DECODER_PLUGINS="$DECODER_PLUGINS ffmpeg"
		 fi])
	AC_CHECK_HEADERS(ffmpeg/avformat.h libavformat/avformat.h)
fi

AM_CONDITIONAL([BUILD_ffmpeg], [test "$want_ffmpeg"])
AC_CONFIG_FILES([decoder_plugins/ffmpeg/Makefile])
