lib_LTLIBRARIES = libflac_decoder.la
libdir = $(plugindir)/$(DECODER_PLUGIN_DIR)
libflac_decoder_la_CFLAGS = $(LIBFLAC_CFLAGS) -I$(top_srcdir)
libflac_decoder_la_LDFLAGS = @PLUGIN_LDFLAGS@
libflac_decoder_la_LIBADD = $(LIBFLAC_LIBS)
libflac_decoder_la_SOURCES = flac.c
