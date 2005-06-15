lib_LTLIBRARIES = libmusepack_decoder.la
libdir = $(plugindir)/$(DECODER_PLUGIN_DIR)
libmusepack_decoder_la_LDFLAGS = @PLUGIN_LDFLAGS@
libmusepack_decoder_la_LIBADD = $(MUSEPACK_LIBS) $(TAGLIB_LIBS)
libmusepack_decoder_la_CFLAGS = $(MUSEPACK_CFLAGS) $(TAGLIB_CFLAGS) \
				-I$(top_srcdir)
libmusepack_decoder_la_SOURCES = musepack.c
