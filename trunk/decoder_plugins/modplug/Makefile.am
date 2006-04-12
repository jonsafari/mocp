lib_LTLIBRARIES = libmodplug_decoder.la
libdir = $(plugindir)/$(DECODER_PLUGIN_DIR)
libmodplug_decoder_la_LDFLAGS = @PLUGIN_LDFLAGS@
libmodplug_decoder_la_LIBADD = $(modplug_LIBS)
libmodplug_decoder_la_CFLAGS = $(modplug_CFLAGS) -I$(top_srcdir)
libmodplug_decoder_la_SOURCES = modplug.c
