lib_LTLIBRARIES = libsndfile_decoder.la
libdir = $(plugindir)/$(DECODER_PLUGIN_DIR)
libsndfile_decoder_la_LDFLAGS = @PLUGIN_LDFLAGS@
libsndfile_decoder_la_LIBADD = $(sndfile_LIBS)
libsndfile_decoder_la_CFLAGS = $(sndfile_CFLAGS) -I$(top_srcdir)
libsndfile_decoder_la_SOURCES = sndfile.c
