lib_LTLIBRARIES = libsndfile_formats_decoder.la
libdir = $(plugindir)/$(DECODER_PLUGIN_DIR)
libsndfile_formats_decoder_la_LDFLAGS = @PLUGIN_LDFLAGS@
libsndfile_formats_decoder_la_LIBADD = $(sndfile_LIBS)
libsndfile_formats_decoder_la_CFLAGS = $(sndfile_CFLAGS) -I$(top_srcdir)
libsndfile_formats_decoder_la_SOURCES = sndfile_formats.c
