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
	       player.h
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
		     ogg.h
mocp_LDADD = @EXTRA_OBJS@
mocp_DEPENDENCIES = @EXTRA_OBJS@
man_MANS = mocp.8
EXTRA_DIST = config.example mocp.8 THANKS
