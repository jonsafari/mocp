/*
 * MOC - music on console
 * Copyright (C) 2003,2004 Damian Pietras <daper@daper.net>
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
#include <sys/types.h>
#include <sys/socket.h>
#ifdef HAVE_SYS_SELECT_H
# include <sys/select.h>
#endif
#include <sys/time.h>
#include <sys/un.h>
#include <time.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <signal.h>
#include <errno.h>
#include <stdarg.h>
#include <pthread.h>

#include "log.h"
#include "protocol.h"
#include "main.h"
#include "audio.h"
#include "oss.h"
#include "options.h"

/* Log file. */
#define SERVER_LOG	"mocp_server_log"

#define PID_FILE	"pid"

/* Thread ID of the server thread. */
static pthread_t server_tid; /* Why? */

/* Set to 1 when a signal arrived causing the program to exit. */
static volatile int server_quit = 0;

static int client_sock = -1;
static pthread_mutex_t client_sock_mut = PTHREAD_MUTEX_INITIALIZER;

/* Information about currently played file */
static struct {
	int bitrate;
	int rate;
	int channels;
	int time; 
} sound_info = {
	-1,
	-1,
	-1,
	0
};

static void write_pid_file ()
{
	char *fname = create_file_name (PID_FILE);
	FILE *file;

	if ((file = fopen(fname, "w")) == NULL)
		fatal ("Can't write pid file.");
	fprintf (file, "%d\n", getpid());
	fclose (file);
}

/* Check if there is a pid file and if it is valid, return the pid, else 0 */
static int check_pid_file ()
{
	FILE *file;
	pid_t pid;
	char *fname = create_file_name (PID_FILE);

	if (fname == NULL)
		fatal ("Can't create pid file name.");

	/* Read the pid file */
	if ((file = fopen(fname, "r")) == NULL)
		return 0;
	if (fscanf(file, "%d", &pid) != 1) {
		fclose (file);
		return 0;
	}
	fclose (file);

	return pid;
}

static void sig_exit (int sig)
{
	logit ("Got signal %d", sig);
	server_quit = 1;

	/* FIXME: wake up the server thread with pthread_kill() when it's not
	 * the server thread */
}

/* Initialize the server - return fd of the listening socket or -1 on error */
int server_init (int debug, int foreground, const char *sound_driver)
{
	struct sockaddr_un sock_name;
	int server_sock;
	int pid;

	pid = check_pid_file ();
	if (pid) {
		fprintf (stderr, "Server already running with pid %d\n", pid);
		fatal ("Exiting.");
	}

	if (foreground)
		log_init_stream (stdout);	
	else if (debug) {
		FILE *logf;
		if (!(logf = fopen(SERVER_LOG, "a")))
			fatal ("Can't open log file.");
		log_init_stream (logf);
	}

	/* Create a socket */
	if ((server_sock = socket (PF_LOCAL, SOCK_STREAM, 0)) == -1)
		fatal ("Can't create socket: %s.", strerror(errno));
	sock_name.sun_family = AF_LOCAL;
	strcpy (sock_name.sun_path, socket_name());

	/* Bind to socket */
	if (bind(server_sock, (struct sockaddr *)&sock_name, SUN_LEN(&sock_name)) == -1)
		fatal ("Can't bind() to the socket: %s", strerror(errno));

	if (listen(server_sock, 1) == -1)
		fatal ("listen() failed: %s", strerror(errno));

	audio_init (sound_driver);

	server_tid = pthread_self ();
	signal (SIGTERM, sig_exit);
	signal (SIGINT, sig_exit);
	signal (SIGHUP, SIG_IGN);
	signal (SIGQUIT, sig_exit);
	signal (SIGPIPE, SIG_IGN);

	write_pid_file ();

	if (!foreground)
		setsid ();

	return server_sock;
}

/* Send EV_DATA and the integer value. Return 0 on error. */
static int send_data_int (const int data)
{
	LOCK (client_sock_mut);
	if (!send_int(client_sock, EV_DATA) || !send_int(client_sock, data)) {
		UNLOCK (client_sock_mut);
		return 0;
	}
	UNLOCK (client_sock_mut);

	return 1;
}

/* End playing and cleanup. */
static void server_shutdown ()
{
	logit ("Server exiting...");
	audio_exit ();
	unlink (socket_name());
	unlink (create_file_name(PID_FILE));

	logit ("Server exited");
}

/* Accept connection and send EV_BUSY message. */
static void busy (int list_sock)
{
	int sock;
	struct sockaddr_un client_name;
	socklen_t name_len = sizeof (client_name);

	logit ("Another client connected, we are busy.");

	sock = accept (list_sock, (struct sockaddr *)&client_name,
			&name_len);
	if (sock == -1)
		logit ("accept() on busy socket failed");
	else {
		send_int (sock, EV_BUSY);
		close (sock);
	}
}

/* Handle CMD_LIST_ADD, return 0 if ok or EOF on error. */
static int req_list_add ()
{
	char *file;

	file = get_str (client_sock);
	if (!file)
		return EOF;

	logit ("Adding '%s' to the list", file);
	
	audio_plist_add (file);
	free (file);

	return 0;
}

/* Handle CMD_PLAY, return 0 if ok or EOF on error. */
static int req_play ()
{
	int num;

	if (!get_int(client_sock, &num))
		return EOF;

	logit ("Playing %d", num);
	audio_play (num);
	
	return 0;
}

/* Handle CMD_SEEK, return 0 if ok or EOF on error */
static int req_seek ()
{
	int sec;

	if (!get_int(client_sock, &sec))
		return EOF;

	logit ("Seeking %ds", sec);
	audio_seek (sec);

	return 0;
}

/* Report an error logging it and sending a message to the client. */
void error (const char *format, ...)
{
	char msg[256];
	va_list va;
	
	va_start (va, format);
	vsnprintf (msg, sizeof(msg), format, va);
	msg[sizeof(msg) - 1] = 0;
	va_end (va);

	logit ("%s", msg);
	/* FIXME: send EV_ERROR */
}

/* Send the song name to the client. Return 0 on error. */
static int send_sname ()
{
	int status = 1;
	char *sname = audio_get_sname ();
	
	LOCK (client_sock_mut);
	if (!send_int(client_sock, EV_DATA) || !send_str(client_sock,
				sname ? sname : ""))
		status = 0;
	free (sname);
	UNLOCK (client_sock_mut);

	return status;
}

/* Return 0 if an option is valid when getting/setting with the client. */
static int valid_sync_option (const char *name)
{
	return !strcasecmp(name, "ShowStreamErrors")
		|| !strcasecmp(name, "Repeat")
		|| !strcasecmp(name, "Shuffle")
		|| !strcasecmp(name, "AutoNext");
}

/* Send requested option valur to the client. Return 1 if OK. */
static int send_option ()
{
	char *name;
	
	if (!(name = get_str(client_sock)))
		return 0;

	/* We can send only a few options, other make no sense here */
	if (!valid_sync_option(name)) {
		logit ("Client wantetd to get not supported option '%s'",
				name);
		free (name);
		return 0;
	}

	/* All supported options are integer type. */
	if (!send_data_int(options_get_int(name))) {
		free (name);
		return 0;
	}
	
	free (name);
	return 1;
}

/* Get and set an option from the client. Return 1 on error. */
static int get_set_option ()
{
	char *name;
	int val;
	
	if (!(name = get_str(client_sock)))
		return 0;
	if (!valid_sync_option(name)) {
		logit ("Client requested setting invalid option '%s'", name);
		return 0;
	}
	if (!get_int(client_sock, &val)) {
		free (name);
		return 0;
	}

	option_set_int (name, val);
	
	free (name);
	return 1;
}

/* Reveive a command from the client and execute it.
 * Return EOF on EOF or error, 1 if the client wants the server to exit,
 * otherwise 0.
 */
static int handle_command ()
{
	int cmd;
	int status = 0;

	if (!get_int(client_sock, &cmd)) {
		logit ("Failed to get command from the client");
		return EOF;
	}

	switch (cmd) {
		case CMD_QUIT:
			logit ("Exit request from the client");
			status = 1;
			break;
		case CMD_LIST_CLEAR:
			logit ("Clearing the list");
			audio_plist_clear ();
			break;
		case CMD_LIST_ADD:
			status = req_list_add ();
			break;
		case CMD_PLAY:
			status = req_play ();
			break;
		case CMD_DISCONNECT:
			status = EOF;
			break;
		case CMD_PAUSE:
			audio_pause ();
			break;
		case CMD_UNPAUSE:
			audio_unpause ();
			break;
		case CMD_STOP:
			audio_stop ();
			break;
		case CMD_GET_CTIME:
			if (!send_data_int(audio_get_time()))
				status = EOF;
			break;
		case CMD_SEEK:
			if (req_seek())
				status = EOF;
			break;
		case CMD_GET_STIME:
			if (!send_data_int(sound_info.time))
				status = EOF;
			break;
		case CMD_GET_SNAME:
			if (!send_sname())
				status = EOF;
			break;
		case CMD_GET_STATE:
			if (!send_data_int(audio_get_state()))
				status = EOF;
			break;
		case CMD_GET_BITRATE:
			if (!send_data_int(sound_info.bitrate))
				status = EOF;
			break;
		case CMD_GET_RATE:
			if (!send_data_int(sound_info.rate))
				status = EOF;
			break;
		case CMD_GET_CHANNELS:
			if (!send_data_int(sound_info.channels))
				status = EOF;
			break;
		case CMD_NEXT:
			audio_next ();
			break;
		case CMD_PING:
			if (!send_int(client_sock, EV_PONG))
				status = EOF;
			break;
		case CMD_GET_OPTION:
			if (!send_option())
				status = EOF;
			break;
		case CMD_SET_OPTION:
			if (!get_set_option())
				status = EOF;
			break;
		default:
			logit ("Bad command (0x%x) from the client.", cmd);
			status = EOF;
	}

	return status;
}

/* Handle client requests, return != 0 when the client wants the server to exit. */
static int handle_client (int list_sock)
{
	fd_set fds;
	int res;
	int client_status = 0;
	int max_fd = (list_sock > client_sock ? list_sock : client_sock) + 1;
	int last_song_time = 0;

	while (client_status == 0) {
		struct timeval timeout;
		int song_time;

		FD_ZERO (&fds);
		FD_SET (list_sock, &fds);
		FD_SET (client_sock, &fds);
		timeout.tv_sec = 0;
		timeout.tv_usec = 50000;

		if (!server_quit)
			res = select (max_fd, &fds, NULL, NULL, &timeout);
		else
			res = 0;
		
		if (server_quit)
			client_status = 1;
		else if (res == -1) {
			logit ("select() failed, %s", strerror(errno));
			client_status = EOF;
		}
		else {
			if (FD_ISSET(list_sock, &fds))
				busy (list_sock);
			if (FD_ISSET(client_sock, &fds)) 
				client_status = handle_command ();

			song_time = audio_get_time ();
			if (song_time != last_song_time && !client_status) {
				last_song_time = song_time;
				LOCK (client_sock_mut);
				if (!send_int(client_sock, EV_CTIME))
					client_status = EOF;
				UNLOCK (client_sock_mut);
				/*logit ("Time: %d", song_time);*/
			}
		}
	}
	
	return client_status == 1 ? 1 : 0;
}

/* Handle incomming connections */
void server_loop (int list_sock)
{
	struct sockaddr_un client_name;
	socklen_t name_len = sizeof (client_name);
	int end = 0;

	logit ("MOC server started, pid: %d", getpid());

	do {
		fd_set fds;
		FD_ZERO (&fds);
		FD_SET (list_sock, &fds);
		int res;
		
		if (!server_quit)
			res = select (list_sock+1, &fds, NULL, NULL, NULL);
		else
			res = 0;

		if (res == -1 && !server_quit) {
			logit ("select() failed: %s", strerror(errno));
			fatal ("select() failed");
		}
		else if (!server_quit) {
			client_sock = accept (list_sock,
					(struct sockaddr *)&client_name,
					&name_len);

			if (client_sock == -1)
				fatal ("accept() failed");
			logit ("Incoming connection");
			end = handle_client (list_sock);
			
			LOCK (client_sock_mut);
			if (server_quit) {
				logit ("Exiting due to signal");
				send_int (client_sock, EV_EXIT);
			}
			close (client_sock);
			client_sock = -1;
			UNLOCK (client_sock_mut);
			
			logit ("Connection closed");
		}
	} while (!end && !server_quit);
	
	close (list_sock);
	server_shutdown ();
}

static void send_event (const int event)
{
	LOCK (client_sock_mut);
	if (client_sock != -1)
		send_int (client_sock, event);
	UNLOCK (client_sock_mut);
}

/* Don't allow events to be send to quickly. Return 1 if the event can be
 * send. */
static int event_throttle ()
{
	static int last_time = 0;

	if (last_time != time(NULL)) {
		last_time = time(NULL);
		return 1;
	}
	
	return 0;
}

void set_info_bitrate (const int bitrate)
{
	sound_info.bitrate = bitrate;
	if (event_throttle() || bitrate <= 0)
		send_event (EV_BITRATE);
}

void set_info_time (const int time)
{
	sound_info.time = time;
	send_event (EV_STATE);
}

void set_info_channels (const int channels)
{
	sound_info.channels = channels;
	send_event (EV_CHANNELS);
}

void set_info_rate (const int rate)
{
	sound_info.rate = rate;
	send_event (EV_RATE);
}

/* Notify the client about change of the player state. */
void state_change ()
{
	send_event (EV_STATE);
}
