dnl ffmpeg/libav

AC_ARG_WITH(ffmpeg, AS_HELP_STRING([--without-ffmpeg],
                                   [Compile without ffmpeg/libav]))

if test "x$with_ffmpeg" != "xno"
then
	PKG_CHECK_MODULES(ffmpeg, libavutil libavcodec libavformat,
		[ffmpeg_CPPFLAGS=`$PKG_CONFIG --cflags-only-I libavutil libavcodec libavformat`
		 AC_SUBST(ffmpeg_CPPFLAGS)
		 AC_SUBST(ffmpeg_CFLAGS)
		 AC_SUBST(ffmpeg_LIBS)
		 want_ffmpeg="yes"],
		[true])
	if test "x$want_ffmpeg" = "xyes"
	then
		if $PKG_CONFIG --max-version 53.47.99 libavcodec
		then
			AC_MSG_ERROR([You need FFmpeg/LibAV of at least release 1.0/10.0.])
		fi
		if test "`$PKG_CONFIG --modversion libavcodec | awk -F. '{ print $3; }'`" -gt 99
		then
			if ! $PKG_CONFIG --atleast-version 54.59.100 libavcodec
			then
				AC_MSG_ERROR([You need FFmpeg of at least release 1.0.])
			fi
			DECODER_PLUGINS="$DECODER_PLUGINS ffmpeg"
			AC_DEFINE([HAVE_FFMPEG], 1,
			          [Define to 1 if you know you have FFmpeg.])
		else
			if ! $PKG_CONFIG --atleast-version 55.34.1 libavcodec
			then
				AC_MSG_ERROR([You need LibAV of at least release 10.0.])
			fi
			DECODER_PLUGINS="$DECODER_PLUGINS ffmpeg(libav)"
			AC_DEFINE([HAVE_LIBAV], 1,
			          [Define to 1 if you know you have LibAV.])
		fi
		save_CPPFLAGS="$CPPFLAGS"
		CPPFLAGS="$CPPFLAGS $ffmpeg_CPPFLAGS"
		save_CFLAGS="$CFLAGS"
		CFLAGS="$CFLAGS $ffmpeg_CFLAGS"
		save_LIBS="$LIBS"
		LIBS="$LIBS $ffmpeg_LIBS"
		AC_CHECK_MEMBERS([struct AVProbeData.mime_type], [], [],
	                     [#include <libavformat/avformat.h>])
		AC_CHECK_HEADERS([libavutil/channel_layout.h])
		AC_SEARCH_LIBS(av_packet_alloc, avcodec,
			[AC_DEFINE([HAVE_AV_PACKET_FNS], 1,
				[Define to 1 if you have the `av_packet_*' functions.])])
		AC_SEARCH_LIBS(av_frame_alloc, avutil,
			[AC_DEFINE([HAVE_AV_FRAME_FNS], 1,
				[Define to 1 if you have the `av_frame_*' functions.])])
        CPPFLAGS="$save_CPPFLAGS"
        CFLAGS="$save_CFLAGS"
        LIBS="$save_LIBS"
    fi
fi

AM_CONDITIONAL([BUILD_ffmpeg], [test "$want_ffmpeg"])
AC_CONFIG_FILES([decoder_plugins/ffmpeg/Makefile])
