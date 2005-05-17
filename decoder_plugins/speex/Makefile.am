lib_LTLIBRARIES = libspeex_decoder.la
libdir = $(plugindir)/$(DECODER_PLUGIN_DIR)
libspeex_decoder_la_LDFLAGS = @PLUGIN_LDFLAGS@
libspeex_decoder_la_LIBADD = $(speex_LIBS)
libspeex_decoder_la_CFLAGS = $(speex_CFLAGS) -I$(top_srcdir)
libspeex_decoder_la_SOURCES = speex.c
