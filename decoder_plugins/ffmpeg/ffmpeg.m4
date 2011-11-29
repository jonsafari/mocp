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
	save_LIBS="$LIBS"
	AC_SEARCH_LIBS(avcodec_decode_audio2, avcodec,
		[AC_DEFINE([HAVE_AVCODEC_DECODE_AUDIO2], 1,
			[Define to 1 if you have the `avcodec_decode_audio2' function.])])
	AC_SEARCH_LIBS(avcodec_decode_audio3, avcodec,
		[AC_DEFINE([HAVE_AVCODEC_DECODE_AUDIO3], 1,
			[Define to 1 if you have the `avcodec_decode_audio3' function.])])
	AC_SEARCH_LIBS(avformat_open_input, avformat,
		[AC_DEFINE([HAVE_AVFORMAT_OPEN_INPUT], 1,
			[Define to 1 if you have the `avformat_open_input' function.])])
	AC_SEARCH_LIBS(av_metadata_get, avformat,
		[AC_DEFINE([HAVE_AV_METADATA_GET], 1,
			[Define to 1 if you have the `av_metadata_get' function.])])
	AC_SEARCH_LIBS(av_dict_get, avutil,
		[AC_DEFINE([HAVE_AV_DICT_GET], 1,
			[Define to 1 if you have the `av_dict_get' function.])])
	LIBS="$save_LIBS"
fi

AM_CONDITIONAL([BUILD_ffmpeg], [test "$want_ffmpeg"])
AC_CONFIG_FILES([decoder_plugins/ffmpeg/Makefile])
