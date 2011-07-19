DECODER_PLUGIN_DIR=decoder_plugins
AC_SUBST([DECODER_PLUGIN_DIR])

dnl libid3tag (with zlib)
AC_CHECK_LIB(z, gzopen, [HAVE_ZLIB=yes],)
AC_CHECK_LIB(id3tag, id3_file_open, [HAVE_LIBID3TAG=yes],)
AC_CHECK_HEADER([id3tag.h],[HAVE_LIBID3TAG_H=yes],)

if test "x$HAVE_ZLIB" = "xyes" -a "x$HAVE_LIBID3TAG" = "xyes" \
	-a "x$HAVE_LIBID3TAG_H" = "xyes"
then
	HAVE_ID3TAG=yes
else
	HAVE_ID3TAG=no
fi

m4_include(decoder_plugins/aac/aac.m4)
m4_include(decoder_plugins/ffmpeg/ffmpeg.m4)
m4_include(decoder_plugins/flac/flac.m4)
m4_include(decoder_plugins/modplug/modplug.m4)
m4_include(decoder_plugins/mp3/mp3.m4)
m4_include(decoder_plugins/musepack/musepack.m4)
m4_include(decoder_plugins/sidplay2/sidplay2.m4)
m4_include(decoder_plugins/sndfile/sndfile.m4)
m4_include(decoder_plugins/speex/speex.m4)
m4_include(decoder_plugins/timidity/timidity.m4)
m4_include(decoder_plugins/vorbis/vorbis.m4)
m4_include(decoder_plugins/wavpack/wavpack.m4)

AC_CONFIG_FILES([decoder_plugins/Makefile])
