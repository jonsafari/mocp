lib_LTLIBRARIES = libsidplay2_decoder.la
libdir = $(plugindir)/$(DECODER_PLUGIN_DIR)
libsidplay2_decoder_la_LDFLAGS = @PLUGIN_LDFLAGS@ $(sidplay2_LDFLAGS)
libsidplay2_decoder_la_LIBADD = $(sidplay2_LIBS) $(sidutils_LIBS)
libsidplay2_decoder_la_CFLAGS = $(sidplay2_CFLAGS) $(sidutils_CFLAGS) -I$(top_srcdir)
libsidplay2_decoder_la_CXXFLAGS = $(sidplay2_CFLAGS) $(sidutils_CFLAGS) -I$(top_srcdir)
libsidplay2_decoder_la_SOURCES = sidplay2.cc sidplay2.h
