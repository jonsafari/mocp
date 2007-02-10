lib_LTLIBRARIES = libtimidity_decoder.la
libdir = $(plugindir)/$(DECODER_PLUGIN_DIR)
libtimidity_decoder_la_LDFLAGS = @PLUGIN_LDFLAGS@
libtimidity_decoder_la_LIBADD = $(timidity_LIBS)
libtimidity_decoder_la_CFLAGS = $(timidity_CFLAGS) -I$(top_srcdir)
libtimidity_decoder_la_SOURCES = timidity.c
