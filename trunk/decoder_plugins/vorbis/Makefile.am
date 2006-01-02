lib_LTLIBRARIES = libvorbis_decoder.la
libdir = $(plugindir)/$(DECODER_PLUGIN_DIR)
libvorbis_decoder_la_LDFLAGS = @PLUGIN_LDFLAGS@
libvorbis_decoder_la_LIBADD = $(OGG_VORBIS_LIBS)
libvorbis_decoder_la_CFLAGS = $(OGG_VORBIS_CFLAGS) -I$(top_srcdir)
libvorbis_decoder_la_SOURCES = vorbis.c
