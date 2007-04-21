lib_LTLIBRARIES = libwavpack_decoder.la
libdir = $(plugindir)/$(DECODER_PLUGIN_DIR)
libwavpack_decoder_la_LDFLAGS = @PLUGIN_LDFLAGS@
libwavpack_decoder_la_LIBADD = $(WAVPACK_LIBS)
libwavpack_decoder_la_CFLAGS = $(WAVPACK_CFLAGS) -I$(top_srcdir)
libwavpack_decoder_la_SOURCES = wavpack.c
