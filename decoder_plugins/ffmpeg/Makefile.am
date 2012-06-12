lib_LTLIBRARIES = libffmpeg_decoder.la
libdir = $(plugindir)/$(DECODER_PLUGIN_DIR)
libffmpeg_decoder_la_LDFLAGS = @PLUGIN_LDFLAGS@
libffmpeg_decoder_la_LIBADD = $(ffmpeg_LIBS)
libffmpeg_decoder_la_CPPFLAGS = $(ffmpeg_CPPFLAGS) -I$(top_srcdir)
libffmpeg_decoder_la_CFLAGS = $(ffmpeg_CFLAGS) -I$(top_srcdir)
libffmpeg_decoder_la_SOURCES = ffmpeg.c
