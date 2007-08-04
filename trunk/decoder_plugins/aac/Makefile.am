lib_LTLIBRARIES = libaac_decoder.la
libdir = $(plugindir)/$(DECODER_PLUGIN_DIR)
libaac_decoder_la_LDFLAGS = @PLUGIN_LDFLAGS@
libaac_decoder_la_LIBADD = -lid3tag -lz $(FAAD2_LIBS)
libaac_decoder_la_CFLAGS = $(FAAD2_CFLAGS) -I$(top_srcdir)
libaac_decoder_la_SOURCES = aac.c
