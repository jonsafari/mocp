/*
 * MOC - music on console
 * Copyright (C) 2004-2005 Damian Pietras <daper@daper.net>
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
#include <stdint.h>
#include <string.h>
#include <ctype.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/utsname.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <time.h>
#include <locale.h>
#include <assert.h>
#include <popt.h>

#include "common.h"
#include "server.h"
#include "interface.h"
#include "options.h"
#include "protocol.h"
#include "log.h"
#include "decoder.h"
#include "lists.h"
#include "files.h"
#include "rcc.h"

static int mocp_argc;
static const char **mocp_argv;

#ifndef OPENWRT
static int popt_next_val = 1;
static char *render_popt_command_line ();
#endif

/* List of MOC-specific environment variables. */
static struct {
	const char *name;
	const char *desc;
} environment_variables[] = {
	{"MOCP_OPTS", "Additional command line options"},
	{"MOCP_POPTRC", "List of POPT configuration files"}
};

struct parameters
{
	char *config_file;
	int debug;
	int only_server;
	int foreground;
	int append;
	int enqueue;
	int clear;
	int play;
	int allow_iface;
	int stop;
	int exit;
	int pause;
	int unpause;
	int next;
	int previous;
	int get_file_info;
	int toggle_pause;
	int playit;
	int seek_by;
	char jump_type;
	int jump_to;
	char *formatted_info_param;
	int get_formatted_info;
	char *adj_volume;
	char *toggle;
	char *on;
	char *off;
};

/* Connect to the server, return fd of the socket or -1 on error. */
static int server_connect ()
{
	struct sockaddr_un sock_name;
	int sock;

	/* Create a socket.
	 * For reasons why AF_UNIX is the correct constant to use in both
	 * cases, see the commentary the SVN log for commit r2833. */
	if ((sock = socket (AF_UNIX, SOCK_STREAM, 0)) == -1)
		 return -1;

	sock_name.sun_family = AF_UNIX;
	strcpy (sock_name.sun_path, socket_name());

	if (connect(sock, (struct sockaddr *)&sock_name,
				SUN_LEN(&sock_name)) == -1) {
		close (sock);
		return -1;
	}

	return sock;
}

/* Ping the server.
 * Return 1 if the server responds with EV_PONG, otherwise 0. */
static int ping_server (int sock)
{
	int event;

	send_int(sock, CMD_PING); /* ignore errors - the server could have
				     already closed the connection and sent
				     EV_BUSY */
	if (!get_int(sock, &event))
		fatal ("Error when receiving pong response!");
	return event == EV_PONG ? 1 : 0;
}

/* Check if a directory ./.moc exists and create if needed. */
static void check_moc_dir ()
{
	char *dir_name = create_file_name ("");
	struct stat file_stat;

	/* strip trailing slash */
	dir_name[strlen(dir_name)-1] = 0;

	if (stat (dir_name, &file_stat) == -1) {
		if (errno != ENOENT)
			fatal ("Error trying to check for "CONFIG_DIR" directory: %s",
			        xstrerror (errno));

		if (mkdir (dir_name, 0700) == -1)
			fatal ("Can't create directory %s: %s",
					dir_name, xstrerror (errno));
	}
	else {
		if (!S_ISDIR(file_stat.st_mode) || access (dir_name, W_OK))
			fatal ("%s is not a writable directory!", dir_name);
	}
}

/* Run client and the server if needed. */
static void start_moc (const struct parameters *params, lists_t_strs *args)
{
	int server_sock;

	if (params->foreground) {
		set_me_server ();
		server_init (params->debug, params->foreground);
		server_loop ();
		return;
	}

	server_sock = server_connect ();

	if (server_sock != -1 && params->only_server)
		fatal ("Server is already running!");

	if (server_sock == -1) {
		int i = 0;
		int notify_pipe[2];
		ssize_t rc;

		printf ("Running the server...\n");

		/* To notify the client that the server socket is ready */
		if (pipe(notify_pipe))
			fatal ("pipe() failed: %s", xstrerror (errno));

		switch (fork()) {
		case 0: /* child - start server */
			set_me_server ();
			server_init (params->debug, params->foreground);
			rc = write (notify_pipe[1], &i, sizeof(i));
			if (rc < 0)
				fatal ("write() to notify pipe failed: %s", xstrerror (errno));
			close (notify_pipe[0]);
			close (notify_pipe[1]);
			server_loop ();
			options_free ();
			decoder_cleanup ();
			io_cleanup ();
			files_cleanup ();
			rcc_cleanup ();
			common_cleanup ();
			exit (EXIT_SUCCESS);
		case -1:
			fatal ("fork() failed: %s", xstrerror (errno));
		default:
			close (notify_pipe[1]);
			if (read(notify_pipe[0], &i, sizeof(i)) != sizeof(i))
				fatal ("Server exited!");
			close (notify_pipe[0]);
			server_sock = server_connect ();
			if (server_sock == -1) {
				perror ("server_connect()");
				fatal ("Can't connect to the server!");
			}
		}
	}

	if (params->only_server)
		send_int (server_sock, CMD_DISCONNECT);
	else {
		xsignal (SIGPIPE, SIG_IGN);
		if (!ping_server (server_sock))
			fatal ("Can't connect to the server!");

		init_interface (server_sock, params->debug, args);
		interface_loop ();
		interface_end ();
	}

	close (server_sock);
}

/* Send commands requested in params to the server. */
static void server_command (struct parameters *params, lists_t_strs *args)
{
	int sock;

	if ((sock = server_connect()) == -1)
		fatal ("The server is not running!");

	xsignal (SIGPIPE, SIG_IGN);
	if (!ping_server (sock))
		fatal ("Can't connect to the server!");

	if (params->playit)
		interface_cmdline_playit (sock, args);
	if (params->clear)
		interface_cmdline_clear_plist (sock);
	if (params->append)
		interface_cmdline_append (sock, args);
	if (params->enqueue)
		interface_cmdline_enqueue (sock, args);
	if (params->play)
		interface_cmdline_play_first (sock);
	if (params->get_file_info)
		interface_cmdline_file_info (sock);
	if (params->seek_by)
		interface_cmdline_seek_by (sock, params->seek_by);
	if (params->jump_type=='%')
		interface_cmdline_jump_to_percent (sock,params->jump_to);
	if (params->jump_type=='s')
		interface_cmdline_jump_to (sock,params->jump_to);
	if (params->get_formatted_info)
		interface_cmdline_formatted_info (sock, params->formatted_info_param);
	if (params->adj_volume)
		interface_cmdline_adj_volume (sock, params->adj_volume);
	if (params->toggle)
		interface_cmdline_set (sock, params->toggle, 2);
	if (params->on)
		interface_cmdline_set (sock, params->on, 1);
	if (params->off)
		interface_cmdline_set (sock, params->off, 0);
	if (params->exit) {
		if (!send_int(sock, CMD_QUIT))
			fatal ("Can't send command!");
	}
	else if (params->stop) {
		if (!send_int(sock, CMD_STOP) || !send_int(sock, CMD_DISCONNECT))
			fatal ("Can't send commands!");
	}
	else if (params->pause) {
		if (!send_int(sock, CMD_PAUSE) || !send_int(sock, CMD_DISCONNECT))
			fatal ("Can't send commands!");
	}
	else if (params->next) {
		if (!send_int(sock, CMD_NEXT) || !send_int(sock, CMD_DISCONNECT))
			fatal ("Can't send commands!");
	}
	else if (params->previous) {
		if (!send_int(sock, CMD_PREV) || !send_int(sock, CMD_DISCONNECT))
			fatal ("Can't send commands!");
	}
	else if (params->unpause) {
		if (!send_int(sock, CMD_UNPAUSE) || !send_int(sock, CMD_DISCONNECT))
			fatal ("Can't send commands!");
	}
	else if (params->toggle_pause) {
		int state, ev, cmd = -1;

		if (!send_int(sock, CMD_GET_STATE))
			fatal ("Can't send commands!");
		if (!get_int(sock, &ev) || ev != EV_DATA || !get_int(sock, &state))
			fatal ("Can't get data from the server!");

		if (state == STATE_PAUSE)
			cmd = CMD_UNPAUSE;
		else if (state == STATE_PLAY)
			cmd = CMD_PAUSE;

		if (cmd != -1 && !send_int(sock, cmd))
			fatal ("Can't send commands!");
		if (!send_int(sock, CMD_DISCONNECT))
			fatal ("Can't send commands!");
	}

	close (sock);
}

static void show_version ()
{
	int rc;
	struct utsname uts;

	putchar ('\n');
	printf ("          This is : %s\n", PACKAGE_NAME);
	printf ("          Version : %s\n", PACKAGE_VERSION);

#ifdef PACKAGE_REVISION
	printf ("         Revision : %s\n", PACKAGE_REVISION);
#endif

	/* Show build time */
#ifdef __DATE__
	printf ("            Built : %s", __DATE__);
# ifdef __TIME__
	printf (" %s", __TIME__);
# endif
	putchar ('\n');
#endif

	/* Show compiled-in components */
	printf ("    Compiled with :");
#ifdef HAVE_OSS
	printf (" OSS");
#endif
#ifdef HAVE_SNDIO
	printf (" SNDIO");
#endif
#ifdef HAVE_ALSA
	printf (" ALSA");
#endif
#ifdef HAVE_JACK
	printf (" JACK");
#endif
#ifndef NDEBUG
	printf (" DEBUG");
#endif
#ifdef HAVE_CURL
	printf (" Network streams");
#endif
#ifdef HAVE_SAMPLERATE
	printf (" resample");
#endif
	putchar ('\n');

	rc = uname (&uts);
	if (rc == 0)
		printf ("       Running on : %s %s %s\n", uts.sysname, uts.release,
	                                                           uts.machine);

	printf ("           Author : Damian Pietras\n");
	printf ("         Homepage : %s\n", PACKAGE_URL);
	printf ("           E-Mail : %s\n", PACKAGE_BUGREPORT);
	printf ("        Copyright : (C) 2003-2016 Damian Pietras and others\n");
	printf ("          License : GNU General Public License, version 2 or later\n");
	putchar ('\n');
}

/* Show program banner. */
static void show_banner ()
{
	printf ("%s (version %s", PACKAGE_NAME, PACKAGE_VERSION);
#ifdef PACKAGE_REVISION
	printf (", revision %s", PACKAGE_REVISION);
#endif
	printf (")\n");
}

static const char mocp_summary[] = "[OPTIONS] [FILE|DIR ...]";

/* Show program usage. */
static void show_usage (poptContext ctx)
{
	show_banner ();
	poptSetOtherOptionHelp (ctx, mocp_summary);
	poptPrintUsage (ctx, stdout, 0);
}

/* Show program help. */
static void show_help (poptContext ctx)
{
	size_t ix;

	show_banner ();
	poptSetOtherOptionHelp (ctx, mocp_summary);
	poptPrintHelp (ctx, stdout, 0);

	printf ("\nEnvironment variables:\n\n");
	for (ix = 0; ix < ARRAY_SIZE(environment_variables); ix += 1)
		printf ("  %-34s%s\n", environment_variables[ix].name,
		                       environment_variables[ix].desc);
	printf ("\n");
}

/* Show POPT-interpreted command line arguments. */
#ifndef OPENWRT
static void show_args ()
{
	if (mocp_argc > 0) {
		char *str;

		str = getenv ("MOCP_POPTRC");
		if (str)
			printf ("MOCP_POPTRC='%s' ", str);

		str = getenv ("MOCP_OPTS");
		if (str)
			printf ("MOCP_OPTS='%s' ", str);

		str = render_popt_command_line ();
		printf ("%s\n", str);
		free (str);
	}
}
#endif

/* Disambiguate the user's request. */
static void show_misc_cb (poptContext ctx,
                          enum poptCallbackReason unused1 ATTR_UNUSED,
                          const struct poptOption *opt,
                          const char *unused2 ATTR_UNUSED,
                          void *unused3 ATTR_UNUSED)
{
	switch (opt->shortName) {
	case 'V':
		show_version ();
		break;
	case 'h':
		show_help (ctx);
		break;
	case 0:
#ifndef OPENWRT
		if (!strcmp (opt->longName, "echo-args"))
			show_args ();
		else
#endif
		if (!strcmp (opt->longName, "usage"))
			show_usage (ctx);
		break;
	}

	exit (EXIT_SUCCESS);
}

enum {
	CL_HANDLED = 0,
	CL_NOIFACE,
	CL_SDRIVER,
	CL_MUSICDIR,
	CL_THEME,
	CL_SETOPTION,
	CL_MOCDIR,
	CL_SYNCPL,
	CL_NOSYNC,
	CL_ASCII,
	CL_JUMP,
	CL_GETINFO
};

static struct parameters params;

static struct poptOption general_opts[] = {
#ifndef NDEBUG
	{"debug", 'D', POPT_ARG_NONE, &params.debug, CL_HANDLED,
			"Turn on logging to a file", NULL},
#endif
	{"moc-dir", 'M', POPT_ARG_STRING, NULL, CL_MOCDIR,
			"Use the specified MOC directory instead of the default", "DIR"},
	{"music-dir", 'm', POPT_ARG_NONE, NULL, CL_MUSICDIR,
			"Start in MusicDir", NULL},
	{"config", 'C', POPT_ARG_STRING, &params.config_file, CL_HANDLED,
			"Use the specified config file instead of the default", "FILE"},
	{"set-option", 'O', POPT_ARG_STRING, NULL, CL_SETOPTION,
			"Override the configuration option NAME with VALUE", "'NAME=VALUE'"},
	{"foreground", 'F', POPT_ARG_NONE, &params.foreground, CL_HANDLED,
			"Run the server in foreground (logging to stdout)", NULL},
	{"server", 'S', POPT_ARG_NONE, &params.only_server, CL_HANDLED,
			"Only run the server", NULL},
	{"sound-driver", 'R', POPT_ARG_STRING, NULL, CL_SDRIVER,
			"Use the first valid sound driver", "DRIVERS"},
	{"ascii", 'A', POPT_ARG_NONE, NULL, CL_ASCII,
			"Use ASCII characters to draw lines", NULL},
	{"theme", 'T', POPT_ARG_STRING, NULL, CL_THEME,
			"Use the selected theme file (read from ~/.moc/themes if the path is not absolute)", "FILE"},
	{"sync", 'y', POPT_ARG_NONE, NULL, CL_SYNCPL,
			"Synchronize the playlist with other clients", NULL},
	{"nosync", 'n', POPT_ARG_NONE, NULL, CL_NOSYNC,
			"Don't synchronize the playlist with other clients", NULL},
	POPT_TABLEEND
};

static struct poptOption server_opts[] = {
	{"pause", 'P', POPT_ARG_NONE, &params.pause, CL_NOIFACE,
			"Pause", NULL},
	{"unpause", 'U', POPT_ARG_NONE, &params.unpause, CL_NOIFACE,
			"Unpause", NULL},
	{"toggle-pause", 'G', POPT_ARG_NONE, &params.toggle_pause, CL_NOIFACE,
			"Toggle between playing and paused", NULL},
	{"stop", 's', POPT_ARG_NONE, &params.stop, CL_NOIFACE,
			"Stop playing", NULL},
	{"next", 'f', POPT_ARG_NONE, &params.next, CL_NOIFACE,
			"Play the next song", NULL},
	{"previous", 'r', POPT_ARG_NONE, &params.previous, CL_NOIFACE,
			"Play the previous song", NULL},
	{"seek", 'k', POPT_ARG_INT, &params.seek_by, CL_NOIFACE,
			"Seek by N seconds (can be negative)", "N"},
	{"jump", 'j', POPT_ARG_STRING, NULL, CL_JUMP,
			"Jump to some position in the current track", "N{%,s}"},
	{"volume", 'v', POPT_ARG_STRING, &params.adj_volume, CL_NOIFACE,
			"Adjust the PCM volume", "[+,-]LEVEL"},
	{"exit", 'x', POPT_ARG_NONE, &params.exit, CL_NOIFACE,
			"Shutdown the server", NULL},
	{"append", 'a', POPT_ARG_NONE, &params.append, CL_NOIFACE,
			"Append the files/directories/playlists passed in "
			"the command line to playlist", NULL},
	{"recursively", 'e', POPT_ARG_NONE, &params.append, CL_NOIFACE,
			"Alias for --append", NULL},
	{"enqueue", 'q', POPT_ARG_NONE, &params.enqueue, CL_NOIFACE,
			"Add the files given on command line to the queue", NULL},
	{"clear", 'c', POPT_ARG_NONE, &params.clear, CL_NOIFACE,
			"Clear the playlist", NULL},
	{"play", 'p', POPT_ARG_NONE, &params.play, CL_NOIFACE,
			"Start playing from the first item on the playlist", NULL},
	{"playit", 'l', POPT_ARG_NONE, &params.playit, CL_NOIFACE,
			"Play files given on command line without modifying the playlist", NULL},
	{"toggle", 't', POPT_ARG_STRING, &params.toggle, CL_NOIFACE,
			"Toggle a control (shuffle, autonext, repeat)", "CONTROL"},
	{"on", 'o', POPT_ARG_STRING, &params.on, CL_NOIFACE,
			"Turn on a control (shuffle, autonext, repeat)", "CONTROL"},
	{"off", 'u', POPT_ARG_STRING, &params.off, CL_NOIFACE,
			"Turn off a control (shuffle, autonext, repeat)", "CONTROL"},
	{"info", 'i', POPT_ARG_NONE, &params.get_file_info, CL_NOIFACE,
			"Print information about the file currently playing", NULL},
	{"format", 'Q', POPT_ARG_STRING, &params.formatted_info_param, CL_GETINFO,
			"Print formatted information about the file currently playing", "FORMAT"},
	POPT_TABLEEND
};

static struct poptOption misc_opts[] = {
	{NULL, 0, POPT_ARG_CALLBACK,
	       (void *) (uintptr_t) show_misc_cb, 0, NULL, NULL},
	{"version", 'V', POPT_ARG_NONE, NULL, 0,
			"Print version information", NULL},
#ifndef OPENWRT
	{"echo-args", 0, POPT_ARG_NONE, NULL, 0,
			"Print POPT-interpreted arguments", NULL},
#endif
	{"usage", 0, POPT_ARG_NONE, NULL, 0,
			"Print brief usage", NULL},
	{"help", 'h', POPT_ARG_NONE, NULL, 0,
			"Print extended usage", NULL},
	POPT_TABLEEND
};

static struct poptOption mocp_opts[] = {
	{NULL, 0, POPT_ARG_INCLUDE_TABLE, general_opts, 0, "General options:", NULL},
	{NULL, 0, POPT_ARG_INCLUDE_TABLE, server_opts, 0, "Server commands:", NULL},
	{NULL, 0, POPT_ARG_INCLUDE_TABLE, misc_opts, 0, "Miscellaneous options:", NULL},
	POPT_AUTOALIAS
	POPT_TABLEEND
};

/* Read the POPT configuration files as given in MOCP_POPTRC. */
static void read_mocp_poptrc (poptContext ctx, const char *env_poptrc)
{
	int ix, rc, count;
	lists_t_strs *files;

	files = lists_strs_new (4);
	count = lists_strs_split (files, env_poptrc, ":");

	for (ix = 0; ix < count; ix += 1) {
		const char *fn;

		fn = lists_strs_at (files, ix);
		if (!strlen (fn))
			continue;

		if (!is_secure (fn))
			fatal ("POPT config file is not secure: %s", fn);

		rc = poptReadConfigFile (ctx, fn);
		if (rc < 0)
			fatal ("Error reading POPT config file '%s': %s",
			        fn, poptStrerror (rc));
	}

	lists_strs_free (files);
}

/* Check that the ~/.popt file is secure. */
static void check_popt_secure ()
{
	int len;
	const char *home, dot_popt[] = ".popt";
	char *home_popt;

	home = get_home ();
	len = strlen (home) + strlen (dot_popt) + 2;
	home_popt = xcalloc (len, sizeof (char));
	snprintf (home_popt, len, "%s/%s", home, dot_popt);
	if (!is_secure (home_popt))
		fatal ("POPT config file is not secure: %s", home_popt);
	free (home_popt);
}

/* Read the default POPT configuration file. */
static void read_default_poptrc (poptContext ctx)
{
	int rc;

	check_popt_secure ();
	rc = poptReadDefaultConfig (ctx, 0);

	if (rc == POPT_ERROR_ERRNO) {
		int saved_errno = errno;

		fprintf (stderr, "\n"
		         "WARNING: The following fatal error message may be bogus!\n"
		         "         If you have an empty /etc/popt.d directory, try\n"
		         "         adding an empty file to it.  If that does not fix\n"
		         "         the problem then you have a genuine error.\n");

		errno = saved_errno;
	}

	if (rc != 0)
		fatal ("Error reading default POPT config file: %s",
		        poptStrerror (rc));
}

/* Read the POPT configuration files(s). */
static void read_popt_config (poptContext ctx)
{
	const char *env_poptrc;

	env_poptrc = getenv ("MOCP_POPTRC");
	if (env_poptrc)
		read_mocp_poptrc (ctx, env_poptrc);
	else
		read_default_poptrc (ctx);
}

/* Prepend MOCP_OPTS to the command line. */
static void prepend_mocp_opts (poptContext ctx)
{
	int rc;
	const char *env_opts;

	env_opts = getenv ("MOCP_OPTS");
	if (env_opts && strlen (env_opts)) {
		int env_argc;
		const char **env_argv;

		rc = poptParseArgvString (env_opts, &env_argc, &env_argv);
		if (rc < 0)
			fatal ("Error parsing MOCP_OPTS: %s", poptStrerror (rc));

		rc = poptStuffArgs (ctx, env_argv);
		if (rc < 0)
			fatal ("Error prepending MOCP_OPTS: %s", poptStrerror (rc));

		free (env_argv);
	}
}

/* Return a copy of the POPT option table structure which is suitable
 * for rendering the POPT expansions of the command line. */
#ifndef OPENWRT
struct poptOption *clone_popt_options (struct poptOption *opts)
{
	size_t count, ix, iy = 0;
	struct poptOption *result;
	const struct poptOption specials[] = {POPT_AUTOHELP
	                                      POPT_AUTOALIAS
	                                      POPT_TABLEEND};

	assert (opts);

	for (count = 1;
	     memcmp (&opts[count - 1], &specials[2], sizeof (struct poptOption));
	     count += 1);

	result = xcalloc (count, sizeof (struct poptOption));

	for (ix = 0; ix < count; ix += 1) {
		if (opts[ix].argInfo == POPT_ARG_CALLBACK)
			continue;

		if (!memcmp (&opts[ix], &specials[0], sizeof (struct poptOption)))
			continue;

		if (!memcmp (&opts[ix], &specials[1], sizeof (struct poptOption)))
			continue;

		memcpy (&result[iy], &opts[ix], sizeof (struct poptOption));

		if (!memcmp (&opts[ix], &specials[2], sizeof (struct poptOption)))
			continue;

		if (opts[ix].argInfo == POPT_ARG_INCLUDE_TABLE) {
			result[iy++].arg = clone_popt_options (opts[ix].arg);
			continue;
		}

		switch (result[iy].argInfo) {
		case POPT_ARG_STRING:
		case POPT_ARG_INT:
		case POPT_ARG_LONG:
		case POPT_ARG_FLOAT:
		case POPT_ARG_DOUBLE:
			result[iy].argInfo = POPT_ARG_STRING;
			break;
		case POPT_ARG_VAL:
			result[iy].argInfo = POPT_ARG_NONE;
			break;
		case POPT_ARG_NONE:
			break;
		default:
			fatal ("Unknown POPT option table argInfo type: %d",
			                                result[iy].argInfo);
		}

		result[iy].arg = NULL;
		result[iy++].val = popt_next_val++;
	}

	return result;
}
#endif

/* Free a copied POPT option table structure. */
#ifndef OPENWRT
void free_popt_clone (struct poptOption *opts)
{
	int ix;
	const struct poptOption table_end = POPT_TABLEEND;

	assert (opts);

	for (ix = 0; memcmp (&opts[ix], &table_end, sizeof (table_end)); ix += 1) {
		if (opts[ix].argInfo == POPT_ARG_INCLUDE_TABLE)
			free_popt_clone (opts[ix].arg);
	}

	free (opts);
}
#endif

/* Return a pointer to the copied POPT option table entry for which the
 * 'val' field matches 'wanted'.  */
#ifndef OPENWRT
struct poptOption *find_popt_option (struct poptOption *opts, int wanted)
{
	int ix = 0;
	const struct poptOption table_end = POPT_TABLEEND;

	assert (opts);
	assert (LIMIT(wanted, popt_next_val));

	while (1) {
		struct poptOption *result;

		if (!memcmp (&opts[ix], &table_end, sizeof (table_end)))
			break;

		assert (opts[ix].argInfo != POPT_ARG_CALLBACK);

		if (opts[ix].val == wanted)
			return &opts[ix];

		switch (opts[ix].argInfo) {
		case POPT_ARG_INCLUDE_TABLE:
			result = find_popt_option (opts[ix].arg, wanted);
			if (result)
				return result;
		case POPT_ARG_STRING:
		case POPT_ARG_INT:
		case POPT_ARG_LONG:
		case POPT_ARG_FLOAT:
		case POPT_ARG_DOUBLE:
		case POPT_ARG_VAL:
		case POPT_ARG_NONE:
			ix += 1;
			break;
		default:
			fatal ("Unknown POPT option table argInfo type: %d",
			                                opts[ix].argInfo);
		}
	}

	return NULL;
}
#endif

/* Render the command line as interpreted by POPT. */
#ifndef OPENWRT
static char *render_popt_command_line ()
{
	int rc;
	lists_t_strs *cmdline;
	char *result;
	const char **rest;
	poptContext ctx;
	struct poptOption *null_opts;

	null_opts = clone_popt_options (mocp_opts);

	ctx = poptGetContext ("mocp", mocp_argc, mocp_argv, null_opts,
	                       POPT_CONTEXT_NO_EXEC);

	read_popt_config (ctx);
	prepend_mocp_opts (ctx);

	cmdline = lists_strs_new (mocp_argc * 2);
	lists_strs_append (cmdline, mocp_argv[0]);

	while (1) {
		size_t len;
		char *str;
		const char *arg;
		struct poptOption *opt;

		rc = poptGetNextOpt (ctx);
		if (rc == -1)
			break;

		if (rc == POPT_ERROR_BADOPT) {
			lists_strs_append (cmdline, poptBadOption (ctx, 0));
			continue;
		}

		opt = find_popt_option (null_opts, rc);
		if (!opt) {
			result = xstrdup ("Couldn't find option in copied option table!");
			goto err;
		}

		arg = poptGetOptArg (ctx);

		if (opt->longName) {
			len = strlen (opt->longName) + 3;
			if (arg)
				len += strlen (arg) + 3;
			str = xmalloc (len);

			if (arg)
				snprintf (str, len, "--%s='%s'", opt->longName, arg);
			else
				snprintf (str, len, "--%s", opt->longName);
		}
		else {
			len = 3;
			if (arg)
				len += strlen (arg) + 3;
			str = xmalloc (len);

			if (arg)
				snprintf (str, len, "-%c '%s'", opt->shortName, arg);
			else
				snprintf (str, len, "-%c", opt->shortName);
		}

		lists_strs_push (cmdline, str);
		free ((void *) arg);
	}

	rest = poptGetArgs (ctx);
	if (rest)
		lists_strs_load (cmdline, rest);

	result = lists_strs_fmt (cmdline, "%s ");

err:
	poptFreeContext (ctx);
	free_popt_clone (null_opts);
	lists_strs_free (cmdline);

	return result;
}
#endif

static void override_config_option (const char *arg, lists_t_strs *deferred)
{
	int len;
	bool append;
	char *ptr, *name, *value;
	enum option_type type;

	assert (arg != NULL);

	ptr = strchr (arg, '=');
	if (ptr == NULL)
		goto error;

	/* Allow for list append operator ("+="). */
	append = (ptr > arg && *(ptr - 1) == '+');

	name = trim (arg, ptr - arg - (append ? 1 : 0));
	if (!name || !name[0])
		goto error;
	type = options_get_type (name);

	if (type == OPTION_LIST) {
		if (deferred) {
			lists_strs_append (deferred, arg);
			free (name);
			return;
		}
	}
	else if (append)
		goto error;

	value = trim (ptr + 1, strlen (ptr + 1));
	if (!value || !value[0])
		goto error;

	if (value[0] == '\'' || value[0] == '"') {
		len = strlen (value);
		if (value[0] != value[len - 1])
			goto error;
		if (strlen (value) < 2)
			goto error;
		memmove (value, value + 1, len - 2);
		value[len - 2] = 0x00;
	}

	if (!options_set_pair (name, value, append))
		goto error;
	options_ignore_config (name);

	free (name);
	free (value);
	return;

error:
	fatal ("Malformed override option: %s", arg);
}

static long get_num_param (const char *p,const char ** last)
{
	char *e;
	long val;

	val = strtol (p, &e, 10);
	if ((*e&&last==NULL)||e==p)
		fatal ("The parameter should be a number!");

	if (last)
		*last=e;
	return val;
}

/* Process the command line options. */
static void process_options (poptContext ctx, lists_t_strs *deferred)
{
	int rc;

	while ((rc = poptGetNextOpt (ctx)) >= 0) {
		const char *jump_type, *arg;

		arg = poptGetOptArg (ctx);

		switch (rc) {
		case CL_SDRIVER:
			if (!options_check_list ("SoundDriver", arg))
				fatal ("No such sound driver: %s", arg);
			options_set_list ("SoundDriver", arg, false);
			options_ignore_config ("SoundDriver");
			break;
		case CL_MUSICDIR:
			options_set_bool ("StartInMusicDir", true);
			options_ignore_config ("StartInMusicDir");
			break;
		case CL_NOIFACE:
			params.allow_iface = 0;
			break;
		case CL_THEME:
			options_set_str ("ForceTheme", arg);
			break;
		case CL_SETOPTION:
			override_config_option (arg, deferred);
			break;
		case CL_MOCDIR:
			options_set_str ("MOCDir", arg);
			options_ignore_config ("MOCDir");
			break;
		case CL_SYNCPL:
			options_set_bool ("SyncPlaylist", true);
			options_ignore_config ("SyncPlaylist");
			break;
		case CL_NOSYNC:
			options_set_bool ("SyncPlaylist", false);
			options_ignore_config ("SyncPlaylist");
			break;
		case CL_ASCII:
			options_set_bool ("ASCIILines", true);
			options_ignore_config ("ASCIILines");
			break;
		case CL_JUMP:
			arg = poptGetOptArg (ctx);
			params.jump_to = get_num_param (arg, &jump_type);
			if (*jump_type)
				if (!jump_type[1])
					if (*jump_type == '%' || tolower (*jump_type) == 's') {
						params.jump_type = tolower (*jump_type);
						params.allow_iface = 0;
						break;
					}
			//TODO: Add message explaining the error
			show_usage (ctx);
			exit (EXIT_FAILURE);
		case CL_GETINFO:
			params.get_formatted_info = 1;
			params.allow_iface = 0;
			break;
		default:
			show_usage (ctx);
			exit (EXIT_FAILURE);
		}

		free ((void *) arg);
	}

	if (rc < -1) {
		const char *opt, *alias, *reason;

		opt = poptBadOption (ctx, 0);
		alias = poptBadOption (ctx, POPT_BADOPTION_NOALIAS);
		reason = poptStrerror (rc);

#ifdef OPENWRT
		if (!strcmp (opt, "--echo-args"))
			reason = "Not available on OpenWRT";
#endif

		/* poptBadOption() with POPT_BADOPTION_NOALIAS fails to
		 * return the correct option if poptStuffArgs() was used. */
		if (!strcmp (opt, alias) || getenv ("MOCP_OPTS"))
			fatal ("%s: %s", opt, reason);
		else
			fatal ("%s (aliased by %s): %s", opt, alias, reason);
	}
}

/* Process the command line options and arguments. */
static lists_t_strs *process_command_line (lists_t_strs *deferred)
{
	const char **rest;
	poptContext ctx;
	lists_t_strs *result;

	assert (deferred != NULL);

	ctx = poptGetContext ("mocp", mocp_argc, mocp_argv, mocp_opts, 0);

	read_popt_config (ctx);
	prepend_mocp_opts (ctx);
	process_options (ctx, deferred);

	if (params.foreground)
		params.only_server = 1;

	result = lists_strs_new (4);
	rest = poptGetArgs (ctx);
	if (rest)
		lists_strs_load (result, rest);

	poptFreeContext (ctx);

	return result;
}

static void process_deferred_overrides (lists_t_strs *deferred)
{
	int ix;
	bool cleared;
	const char marker[] = "*Marker*";
	char **config_decoders;
	lists_t_strs *decoders_option;

	/* We need to shuffle the PreferredDecoders list into the
	 * right order as we load any deferred overriding options. */

	decoders_option = options_get_list ("PreferredDecoders");
	lists_strs_reverse (decoders_option);
	config_decoders = lists_strs_save (decoders_option);
	lists_strs_clear (decoders_option);
	lists_strs_append (decoders_option, marker);

	for (ix = 0; ix < lists_strs_size (deferred); ix += 1)
		override_config_option (lists_strs_at (deferred, ix), NULL);

	cleared = lists_strs_empty (decoders_option) ||
	          strcmp (lists_strs_at (decoders_option, 0), marker) != 0;
	lists_strs_reverse (decoders_option);
	if (!cleared) {
		char **override_decoders;

		free (lists_strs_pop (decoders_option));
		override_decoders = lists_strs_save (decoders_option);
		lists_strs_clear (decoders_option);
		lists_strs_load (decoders_option, (const char **)config_decoders);
		lists_strs_load (decoders_option, (const char **)override_decoders);
		free (override_decoders);
	}
	free (config_decoders);
}

static void log_environment_variables ()
{
#ifndef NDEBUG
	size_t ix;

	for (ix = 0; ix < ARRAY_SIZE(environment_variables); ix += 1) {
		char *str;

		str = getenv (environment_variables[ix].name);
		if (str)
			logit ("%s='%s'", environment_variables[ix].name, str);
	}
#endif
}

/* Log the command line which launched MOC. */
static void log_command_line ()
{
#ifndef NDEBUG
	lists_t_strs *cmdline;
	char *str;

	cmdline = lists_strs_new (mocp_argc);
	if (lists_strs_load (cmdline, mocp_argv) > 0)
		str = lists_strs_fmt (cmdline, "%s ");
	else
		str = xstrdup ("No command line available");
	logit ("%s", str);
	free (str);
	lists_strs_free (cmdline);
#endif
}

/* Log the command line as interpreted by POPT. */
static void log_popt_command_line ()
{
#if !defined(NDEBUG) && !defined(OPENWRT)
	if (mocp_argc > 0) {
		char *str;

		str = render_popt_command_line ();
		logit ("%s", str);
		free (str);
	}
#endif
}

int main (int argc, const char *argv[])
{
	lists_t_strs *deferred_overrides, *args;

	assert (argc >= 0);
	assert (argv != NULL);
	assert (argv[argc] == NULL);

	mocp_argc = argc;
	mocp_argv = argv;

#ifdef PACKAGE_REVISION
	logit ("This is Music On Console (revision %s)", PACKAGE_REVISION);
#else
	logit ("This is Music On Console (version %s)", PACKAGE_VERSION);
#endif

#ifdef CONFIGURATION
	logit ("Configured:%s", CONFIGURATION);
#endif

#if !defined(NDEBUG)
	{
		int rc;
		struct utsname uts;

		rc = uname (&uts);
		if (rc == 0)
			logit ("Running on: %s %s %s", uts.sysname, uts.release, uts.machine);
	}
#endif

	files_init ();

	if (get_home () == NULL)
		fatal ("Could not determine user's home directory!");

	memset (&params, 0, sizeof(params));
	params.allow_iface = 1;
	options_init ();
	deferred_overrides = lists_strs_new (4);

	/* set locale according to the environment variables */
	if (!setlocale(LC_ALL, ""))
		logit ("Could not set locale!");

	log_environment_variables ();
	log_command_line ();
	args = process_command_line (deferred_overrides);
	log_popt_command_line ();

	if (!params.allow_iface && params.only_server)
		fatal ("Server command options can't be used with --server!");

	if (!params.config_file)
		params.config_file = create_file_name ("config");
	options_parse (params.config_file);

	process_deferred_overrides (deferred_overrides);
	lists_strs_free (deferred_overrides);
	deferred_overrides = NULL;

	check_moc_dir ();

	io_init ();
	rcc_init ();
	decoder_init (params.debug);
	srand (time(NULL));

	if (params.allow_iface)
		start_moc (&params, args);
	else
		server_command (&params, args);

	lists_strs_free (args);
	options_free ();
	decoder_cleanup ();
	io_cleanup ();
	rcc_cleanup ();
	files_cleanup ();
	common_cleanup ();

	return EXIT_SUCCESS;
}
