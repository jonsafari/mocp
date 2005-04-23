SUBDIRS = themes decoder_plugins
AM_CPPFLAGS = -DSYSTEM_THEMES_DIR=\"$(pkgdatadir)/themes\" \
	      -DPLUGIN_DIR=\"$(plugindir)/$(DECODER_PLUGIN_DIR)\"

bin_PROGRAMS = mocp
mocp_SOURCES = log.c \
	       log.h \
	       protocol.h \
	       protocol.c \
	       server.c \
	       server.h \
	       main.c \
	       main.h \
	       playlist.c \
	       playlist.h \
	       fifo_buf.c \
	       fifo_buf.h \
	       out_buf.c \
	       out_buf.h \
	       audio.c \
	       audio.h \
	       decoder.c \
	       decoder.h \
       	       interface.c \
	       interface.h \
	       menu.c \
	       menu.h \
	       files.c \
	       files.h \
	       options.c \
	       options.h \
	       player.c \
	       player.h \
	       playlist_file.c \
	       playlist_file.h \
	       themes.c \
	       themes.h \
	       keys.c \
	       keys.h \
	       ltdl.c \
	       ltdl.h \
	       io.c \
	       io.h \
	       compat.c \
	       compat.h
EXTRA_mocp_SOURCES = gnugetopt.h \
		     getopt.c \
		     getopt1.c \
		     null_out.c \
		     null_out.h \
		     oss.c \
		     oss.h \
		     alsa.c \
		     alsa.h \
		     io_curl.c \
		     io_curl.h \
		     jack.c \
		     jack.h
man_MANS = mocp.8
mocp_LDADD = @EXTRA_OBJS@
mocp_DEPENDENCIES = @EXTRA_OBJS@
mocp_LDFLAGS = @EXTRA_LIBS@
EXTRA_DIST = config.example mocp.8 THANKS keymap.example Doxyfile

docdir = $(datadir)/doc/$(PACKAGE)
doc_DATA = config.example THANKS README keymap.example
