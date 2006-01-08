lib_LTLIBRARIES = libmp3_decoder.la
libdir = $(plugindir)/$(DECODER_PLUGIN_DIR)
libmp3_decoder_la_LDFLAGS = @PLUGIN_LDFLAGS@
libmp3_decoder_la_CFLAGS = -I$(top_srcdir)
libmp3_decoder_la_LIBADD = -lmad -lid3tag -lz $(RCC_LIBS)
libmp3_decoder_la_SOURCES = mp3.c xing.c xing.h
