/*
 * MOC - music on console
 * Copyright (C) 2004 Damian Pietras <daper@daper.net>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 */

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <signal.h>
#define _GNU_SOURCE
#include <getopt.h>
#include <errno.h>
#include <unistd.h>
#include <time.h>
#include "server.h"
#include "main.h"
#include "interface.h"
#include "options.h"
#include "protocol.h"
#include "log.h"
#include "file_types.h"

#define CONFIG_FILE	"config"

struct parameters
{
	int debug;
	int only_server;
	int foreground;
};

/* End program with a message. Use when an error occurs and we can't recover. */
void fatal (const char *format, ...)
{
	va_list va;
	char msg[256];
	
	va_start (va, format);
	vsnprintf (msg, sizeof(msg), format, va);
	msg[sizeof(msg) - 1] = 0;
	fprintf (stderr, "\nFATAL_ERROR: %s\n\n", msg);
	logit ("FATAL ERROR: %s", msg);
	va_end (va);

	exit (EXIT_FATAL);
}

void *xmalloc (const size_t size)
{
	void *p;

	if ((p = malloc(size)) == NULL)
		fatal ("Can't allocate memory!");
	return p;
}

void *xrealloc (void *ptr, const size_t size)
{
	void *p;

	if ((p = realloc(ptr, size)) == NULL)
		fatal ("Can't allocate memory!");
	return p;

}

char *xstrdup (const char *s)
{
	char *n;

	if (s && (n = strdup(s)) == NULL)
		fatal ("Can't allocate memory!");

	return s ? n : NULL;
}

/* Return path to a file in MOC config directory. NOT THREAD SAFE */
char *create_file_name (const char *file)
{
	char *home_dir = getenv("HOME");
	static char fname[PATH_MAX];
	int len = strlen(home_dir) + strlen(CONFIG_DIR) + strlen(file) + 3;

	if (len > PATH_MAX)
		return NULL;

	sprintf (fname, "%s/%s/%s", home_dir, CONFIG_DIR, file);
	return fname;
}

/* Ping the server. return 1 if the server respond with EV_PONG, otherwise 1. */
static int ping_server (int sock)
{
	int event;
	
	send_int(sock, CMD_PING); /* ignore errors - the server could have
				     already closed the connection and sent
				     EV_BUSY */
	if (!get_int(sock, &event))
		fatal ("Error when receiving pong response.");
	return event == EV_PONG ? 1 : 0;
}

/* Check if a directory ./.moc exists and create if needed. */
static void check_moc_dir ()
{
	char *dir_name = create_file_name ("");
	struct stat file_stat;

	/* strip trailing slash */
	dir_name[strlen(dir_name)-1] = 0;

	if (stat(dir_name, &file_stat) == -1) {
		if (errno == ENOENT) {
			if (mkdir(dir_name, 0700) == -1)
				fatal ("Can't create directory %s, %s",
						strerror(errno));
		}
		else
			fatal ("Error trying to check for "CONFIG_DIR
					" directory: %s", strerror(errno));
	}
}

/* Run client and the server if needed. */
static void start_moc (const struct parameters *params, char **args,
		const int arg_num)
{
	int list_sock;
	int server_sock = -1;

	check_moc_dir ();

	options_parse (create_file_name(CONFIG_FILE));
	file_types_init ();
	srand (time(NULL));

	if (!params->foreground && (server_sock = server_connect()) == -1) {
		int notify_pipe[2];
		int i = 0;

		printf ("Running the server...\n");

		/* To notify the client that the server socket is ready */
		if (pipe(notify_pipe))
			fatal ("pipe() failed: %s", strerror(errno));

		switch (fork()) {
			case 0: /* child - start server */
				list_sock = server_init (params->debug,
						params->foreground);
				write (notify_pipe[1], &i, sizeof(i));
				close (notify_pipe[0]);
				close (notify_pipe[1]);
				server_loop (list_sock);
				options_free ();
				exit (0);
			case -1:
				fatal ("fork() failed");
			default:
				close (notify_pipe[1]);
				if (read(notify_pipe[0], &i, sizeof(i))
						!= sizeof(i))
					fatal ("Server exited");
				close (notify_pipe[0]);
				if ((server_sock = server_connect()) == -1) {
					perror ("server_connect()");
					fatal ("Can't connect to the server");
				}
		}
	}
	else if (!params->foreground && params->only_server)
		fatal ("Server is already running");
	else if (params->foreground && params->only_server) {
		list_sock = server_init (params->debug, params->foreground);
		server_loop (list_sock);
	}

	if (!params->only_server) {
		signal (SIGPIPE, SIG_IGN);
		if (ping_server(server_sock)) {
			init_interface (server_sock, params->debug,
					args, arg_num);
			interface_loop ();
			interface_end ();
		}
		else
			fatal ("The server is busy (another client is "
					"connected)");
	}

	options_free ();
}

static void show_version () {
	printf (PACKAGE_STRING" ");

	/* Show build time */
#ifdef __DATE__
	printf ("Build: "__DATE__" ");
# ifdef __TIME__
	printf (__TIME__);
# endif
#endif
	
	putchar ('\n');

	/* Show compiled-in components */
	printf ("Compiled with:");
#ifdef HAVE_MAD
	printf (" MP3");
#endif
#ifdef HAVE_VORBIS
	printf (" OGG");
#endif
#ifdef HAVE_OSS
	printf (" OSS");
#endif
#ifndef NDEBUG
	printf (" DEBUG");
#endif
	
	putchar ('\n');
	
}

/* Show program usage and exit */
static void show_usage (const char *prg_name) {
	printf (PACKAGE_STRING"\n"
"Usage:\n"
"%s [OPTIONS]... [FILE]...\n"
"-V --version		Show program version and exit.\n"
"-h --help		Show usage and exit.\n"
"-D --debug		Turn on logging to a file.\n"
"-S --server		Run only the server.\n"
"-F --foreground	Run server in foreground, log to stdout.\n"
"-R --sound-driver NAME	Use the specified sound driver (oss, null).\n"
"-m --music-dir		Start in MusicDir.\n"
, prg_name);
}

/* Check if the sound driver string presents a proper driver. */
int proper_sound_driver (const char *driver)
{
#ifdef HAVE_OSS
	if (!strcasecmp(driver, "oss"))
		return 1;
#endif
#ifndef NDEBUG
	if (!strcasecmp(driver, "null"))
		return 1;
#endif

	return 0;
}

int main (int argc, char *argv[])
{
	struct option long_options[] = {
		{ "version",		0, NULL, 'V' },
		{ "help",		0, NULL, 'h' },
		{ "debug",		0, NULL, 'D' },
		{ "server",		0, NULL, 'S' },
		{ "foreground",		0, NULL, 'F' },
		{ "sound-driver",	1, NULL, 'R' },
		{ "music-dir",		0, NULL, 'm' },
		{ 0, 0, 0, 0 }
	};
	int ret, opt_index = 0;
	struct parameters params = {
		/* debug */		0,
		/* only_server */	0,
		/* foreground */	0
	};

	options_init ();

	while ((ret = getopt_long(argc, argv, "VhDSFR:m", long_options,
					&opt_index)) != -1) {
		switch (ret) {
			case 'V':
				show_version ();
				return 0;
			case 'h':
				show_usage (argv[0]);
				return 0;
			case 'D':
				params.debug = 1;
				break;
			case 'S':
				params.only_server = 1;
				break;
			case 'F':
				params.foreground = 1;
				break;
			case 'R':
				if (!check_str_option("SoundDriver", optarg))
					fatal ("No such sound driver");
				option_set_str ("SoundDriver", optarg);
				option_ignore_config ("SoundDriver");
				break;
			case 'm':
				option_set_int ("StartInMusicDir", 1);
				option_ignore_config ("StartInMusicDir");
				break;
			default:
				show_usage (argv[0]);
				return 1;
		}
	}
	
	if (params.foreground && !params.only_server)
		fatal ("Can't use --foreground without --server");

	start_moc (&params, argv + optind, argc - optind);

	return 0;
}
