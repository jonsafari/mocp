SUBDIRS = themes
AM_CPPFLAGS = -DSYSTEM_THEMES_DIR=\"$(pkgdatadir)/themes\"

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
	       buf.c \
	       buf.h \
	       audio.c \
	       audio.h \
	       file_types.c \
	       file_types.h \
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
	       keys.h
EXTRA_mocp_SOURCES = gnugetopt.h \
		     getopt.c \
		     getopt1.c \
		     mp3.c \
		     mp3.h \
		     xing.c \
		     xing.h \
		     null_out.c \
		     null_out.h \
		     oss.c \
		     oss.h \
		     ogg.c \
		     ogg.h \
		     alsa.c \
		     alsa.h \
		     flac.c \
		     flac.h \
		     sndfile_formats.c \
		     sndfile_formats.h
mocp_LDADD = @EXTRA_OBJS@
mocp_DEPENDENCIES = @EXTRA_OBJS@
man_MANS = mocp.8
EXTRA_DIST = config.example mocp.8 THANKS
		     config.example

docdir = $(datadir)/doc/$(PACKAGE)
doc_DATA = config.example THANKS README
