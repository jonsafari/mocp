lib_LTLIBRARIES = libogg_decoder.la
libdir = $(plugindir)/$(DECODER_PLUGIN_DIR)
libogg_decoder_la_LDFLAGS = @PLUGIN_LDFLAGS@
libogg_decoder_la_LIBADD = $(OGG_VORBIS_LIBS)
libogg_decoder_la_CFLAGS = $(OGG_VORBIS_CFLAGS) -I$(top_srcdir)
libogg_decoder_la_SOURCES = ogg.c
