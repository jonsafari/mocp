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
		[AC_CHECK_PROG([FFMPEG_CONFIG], [ffmpeg-config], [yes])
		 if test "x$FFMPEG_CONFIG" = "xyes"
		 then
			 ffmpeg_CPPFLAGS=`ffmpeg-config --cflags`
			 ffmpeg_CFLAGS=`ffmpeg-config --cflags`
			 avformat_LIBS=`ffmpeg-config --plugin-libs avformat`
			 avcodec_LIBS=`ffmpeg-config --plugin-libs avcodec`
			 avutil_LIBS=`ffmpeg-config --plugin-libs avutil`
			 ffmpeg_LIBS="$avformat_LIBS $avcodec_LIBS $avutil_LIBS"
			 AC_SUBST(ffmpeg_CPPFLAGS)
			 AC_SUBST(ffmpeg_CFLAGS)
			 AC_SUBST(ffmpeg_LIBS)
			 want_ffmpeg="yes"
		 fi])
	if test "x$want_ffmpeg" = "xyes"
	then
		if $PKG_CONFIG --max-version 53.47.99 libavcodec
		then
			 DECODER_PLUGINS="$DECODER_PLUGINS ffmpeg/libav"
		elif test "`$PKG_CONFIG --modversion libavcodec | awk -F. '{ print $3; }'`" -gt 99
		then
			 DECODER_PLUGINS="$DECODER_PLUGINS ffmpeg"
			 AC_DEFINE([HAVE_FFMPEG], 1,
				[Define to 1 if you know you have FFmpeg.])
		else
			 DECODER_PLUGINS="$DECODER_PLUGINS ffmpeg(libav)"
			 AC_DEFINE([HAVE_LIBAV], 1,
				[Define to 1 if you know you have LibAV.])
		fi
		if ! $PKG_CONFIG --atleast-version 52.110.0 libavformat
		then
			FFMPEG_DEPRECATED="yes"
		fi
		save_CPPFLAGS="$CPPFLAGS"
		CPPFLAGS="$CPPFLAGS $ffmpeg_CPPFLAGS"
		save_CFLAGS="$CFLAGS"
		CFLAGS="$CFLAGS $ffmpeg_CFLAGS"
		save_LIBS="$LIBS"
		LIBS="$LIBS $ffmpeg_LIBS"
		AC_CHECK_HEADERS(ffmpeg/avformat.h libavformat/avformat.h)
		if test "x$ac_cv_header_ffmpeg_avformat_h" = "xyes"
		then
			AC_CHECK_MEMBERS([struct AVCodecContext.request_channels], [], [],
		                     [#include <ffmpeg/avcodec.h>])
		else
			AC_CHECK_MEMBERS([struct AVCodecContext.request_channels], [], [],
		                     [#include <libavcodec/avcodec.h>])
		fi
		AC_SEARCH_LIBS(avcodec_open2, avcodec,
			[AC_DEFINE([HAVE_AVCODEC_OPEN2], 1,
				[Define to 1 if you have the `avcodec_open2' function.])])
		AC_SEARCH_LIBS(avcodec_decode_audio2, avcodec,
			[AC_DEFINE([HAVE_AVCODEC_DECODE_AUDIO2], 1,
				[Define to 1 if you have the `avcodec_decode_audio2' function.])])
		AC_SEARCH_LIBS(avcodec_decode_audio3, avcodec,
			[AC_DEFINE([HAVE_AVCODEC_DECODE_AUDIO3], 1,
				[Define to 1 if you have the `avcodec_decode_audio3' function.])])
		AC_SEARCH_LIBS(avcodec_decode_audio4, avcodec,
			[AC_DEFINE([HAVE_AVCODEC_DECODE_AUDIO4], 1,
				[Define to 1 if you have the `avcodec_decode_audio4' function.])],
			[AX_FUNC_POSIX_MEMALIGN])
		AC_SEARCH_LIBS(avformat_open_input, avformat,
			[AC_DEFINE([HAVE_AVFORMAT_OPEN_INPUT], 1,
				[Define to 1 if you have the `avformat_open_input' function.])])
		AC_SEARCH_LIBS(avformat_close_input, avformat,
			[AC_DEFINE([HAVE_AVFORMAT_CLOSE_INPUT], 1,
				[Define to 1 if you have the `avformat_close_input' function.])])
		AC_SEARCH_LIBS(avformat_find_stream_info, avformat,
			[AC_DEFINE([HAVE_AVFORMAT_FIND_STREAM_INFO], 1,
				[Define to 1 if you have the `avformat_find_stream_info' function.])])
		AC_SEARCH_LIBS(avio_size, avformat,
			[AC_DEFINE([HAVE_AVIO_SIZE], 1,
				[Define to 1 if you have the `avio_size' function.])])
		AC_CHECK_MEMBERS([AVIOContext.seekable], , ,
		                 [#include <libavformat/avformat.h>])
		AC_SEARCH_LIBS(av_metadata_get, avformat,
			[AC_DEFINE([HAVE_AV_METADATA_GET], 1,
				[Define to 1 if you have the `av_metadata_get' function.])])
		AC_SEARCH_LIBS(av_dict_get, avutil,
			[AC_DEFINE([HAVE_AV_DICT_GET], 1,
				[Define to 1 if you have the `av_dict_get' function.])])
		AC_SEARCH_LIBS(av_get_channel_layout_nb_channels, avutil,
			[AC_DEFINE([HAVE_AV_GET_CHANNEL_LAYOUT_NB_CHANNELS], 1,
				[Define to 1 if you have the `av_get_channel_layout_nb_channels' function.])])
		AC_CHECK_DECLS([CODEC_ID_MP2], , ,
		                 [#include <libavcodec/avcodec.h>])
		AC_CHECK_DECLS([AV_CODEC_ID_MP2], , ,
		                 [#include <libavcodec/avcodec.h>])
		AC_CHECK_DECLS([CODEC_ID_OPUS], , ,
		                 [#include <libavcodec/avcodec.h>])
		AC_CHECK_DECLS([AV_CODEC_ID_OPUS], , ,
		                 [#include <libavcodec/avcodec.h>])
		AC_CHECK_DECLS([CODEC_ID_SPEEX], , ,
		                 [#include <libavcodec/avcodec.h>])
		AC_CHECK_DECLS([AV_CODEC_ID_SPEEX], , ,
		                 [#include <libavcodec/avcodec.h>])
		AC_CHECK_DECLS([CODEC_ID_THEORA], , ,
		                 [#include <libavcodec/avcodec.h>])
		AC_CHECK_DECLS([AV_CODEC_ID_THEORA], , ,
		                 [#include <libavcodec/avcodec.h>])
		AC_CHECK_DECLS([CODEC_ID_VORBIS], , ,
		                 [#include <libavcodec/avcodec.h>])
		AC_CHECK_DECLS([AV_CODEC_ID_VORBIS], , ,
		                 [#include <libavcodec/avcodec.h>])
		AC_SEARCH_LIBS(av_frame_alloc, avutil,
			[AC_DEFINE([HAVE_AV_FRAME_ALLOC], 1,
				[Define to 1 if you have the `av_frame_alloc' function.])])
		AC_SEARCH_LIBS(av_frame_unref, avutil,
			[AC_DEFINE([HAVE_AV_FRAME_UNREF], 1,
				[Define to 1 if you have the `av_frame_unref' function.])])
		AC_SEARCH_LIBS(av_frame_free, avutil,
			[AC_DEFINE([HAVE_AV_FRAME_FREE], 1,
				[Define to 1 if you have the `av_frame_free' function.])])
		AC_SEARCH_LIBS(avcodec_free_frame, avcodec,
			[AC_DEFINE([HAVE_AVCODEC_FREE_FRAME], 1,
				[Define to 1 if you have the `avcodec_free_frame' function.])])
		AC_CHECK_DECLS([CODEC_ID_PCM_S8], , ,
		                 [#include <libavcodec/avcodec.h>])
		AC_CHECK_DECLS([CODEC_ID_PCM_S8_PLANAR], , ,
		                 [#include <libavcodec/avcodec.h>])
		AC_CHECK_DECLS([AV_CODEC_ID_PCM_S8_PLANAR], , ,
		                 [#include <libavcodec/avcodec.h>])
		AC_CHECK_DECLS([CODEC_ID_PCM_U8], , ,
		                 [#include <libavcodec/avcodec.h>])
		AC_CHECK_DECLS([CODEC_ID_PCM_S16LE], , ,
		                 [#include <libavcodec/avcodec.h>])
		AC_CHECK_DECLS([CODEC_ID_PCM_S16LE_PLANAR], , ,
		                 [#include <libavcodec/avcodec.h>])
		AC_CHECK_DECLS([AV_CODEC_ID_PCM_S16LE_PLANAR], , ,
		                 [#include <libavcodec/avcodec.h>])
		AC_CHECK_DECLS([CODEC_ID_PCM_S16BE], , ,
		                 [#include <libavcodec/avcodec.h>])
		AC_CHECK_DECLS([CODEC_ID_PCM_U16LE], , ,
		                 [#include <libavcodec/avcodec.h>])
		AC_CHECK_DECLS([CODEC_ID_PCM_U16BE], , ,
		                 [#include <libavcodec/avcodec.h>])
		AC_CHECK_DECLS([CODEC_ID_PCM_S24LE], , ,
		                 [#include <libavcodec/avcodec.h>])
		AC_CHECK_DECLS([CODEC_ID_PCM_S24BE], , ,
		                 [#include <libavcodec/avcodec.h>])
		AC_CHECK_DECLS([CODEC_ID_PCM_U24LE], , ,
		                 [#include <libavcodec/avcodec.h>])
		AC_CHECK_DECLS([CODEC_ID_PCM_U24BE], , ,
		                 [#include <libavcodec/avcodec.h>])
		AC_CHECK_DECLS([CODEC_ID_PCM_S32LE], , ,
		                 [#include <libavcodec/avcodec.h>])
		AC_CHECK_DECLS([CODEC_ID_PCM_S32BE], , ,
		                 [#include <libavcodec/avcodec.h>])
		AC_CHECK_DECLS([CODEC_ID_PCM_U32LE], , ,
		                 [#include <libavcodec/avcodec.h>])
		AC_CHECK_DECLS([CODEC_ID_PCM_U32BE], , ,
		                 [#include <libavcodec/avcodec.h>])
		AC_CHECK_DECLS([AV_SAMPLE_FMT_U8], , ,
		                 [#include <libavcodec/avcodec.h>])
		AC_CHECK_DECLS([AV_SAMPLE_FMT_U8P], , ,
		                 [#include <libavcodec/avcodec.h>])
		AC_CHECK_DECLS([AV_SAMPLE_FMT_S16P], , ,
		                 [#include <libavcodec/avcodec.h>])
		AC_CHECK_DECLS([AV_SAMPLE_FMT_U16LE], , ,
		                 [#include <libavcodec/avcodec.h>])
		AC_CHECK_DECLS([AV_SAMPLE_FMT_U16BE], , ,
		                 [#include <libavcodec/avcodec.h>])
		AC_CHECK_DECLS([AV_SAMPLE_FMT_U24LE], , ,
		                 [#include <libavcodec/avcodec.h>])
		AC_CHECK_DECLS([AV_SAMPLE_FMT_U24BE], , ,
		                 [#include <libavcodec/avcodec.h>])
		AC_CHECK_DECLS([AV_SAMPLE_FMT_S32P], , ,
		                 [#include <libavcodec/avcodec.h>])
		AC_CHECK_DECLS([AV_SAMPLE_FMT_U32LE], , ,
		                 [#include <libavcodec/avcodec.h>])
		AC_CHECK_DECLS([AV_SAMPLE_FMT_U32BE], , ,
		                 [#include <libavcodec/avcodec.h>])
		AC_CHECK_DECLS([AV_SAMPLE_FMT_FLTP], , ,
		                 [#include <libavcodec/avcodec.h>])
		AC_CHECK_DECLS([CODEC_CAP_EXPERIMENTAL], , ,
		                 [#include <libavcodec/avcodec.h>])
		AC_SEARCH_LIBS(av_get_sample_fmt_name, avutil,
			[AC_DEFINE([HAVE_AV_GET_SAMPLE_FMT_NAME], 1,
				[Define to 1 if you have the `av_get_sample_fmt_name' function.])])
		AC_SEARCH_LIBS(av_lockmgr_register, avcodec,
			[AC_DEFINE([HAVE_LOCKMGR_REGISTER], 1,
				[Define to 1 if you have the `av_lockmgr_register' function.])])
		CPPFLAGS="$save_CPPFLAGS"
		CFLAGS="$save_CFLAGS"
		LIBS="$save_LIBS"
	fi
fi

AM_CONDITIONAL([BUILD_ffmpeg], [test "$want_ffmpeg"])
AC_CONFIG_FILES([decoder_plugins/ffmpeg/Makefile])
