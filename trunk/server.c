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
#include <assert.h>

/*#define DEBUG*/

#include "log.h"
#include "protocol.h"
#include "main.h"
#include "audio.h"
#include "oss.h"
#include "options.h"
#include "server.h"

#define SERVER_LOG	"mocp_server_log"
#define PID_FILE	"pid"

#define CLIENTS_MAX	10
#define EVENTS_MAX	10

struct events
{
	int queue[EVENTS_MAX];
	int num;
	pthread_mutex_t mutex;
};

struct client
{
	int socket; 		/* -1 if inactive */
	int wants_events;	/* requested events? */
	struct events events;
};

static struct client clients[CLIENTS_MAX];
	
/* Thread ID of the server thread. */
static pthread_t server_tid;

/* Set to 1 when a signal arrived causing the program to exit. */
static volatile int server_quit = 0;

static char err_msg[265] = "";

/* Information about currently played file */
static struct {
	int bitrate;
	int rate;
	int channels;
} sound_info = {
	-1,
	-1,
	-1
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

	if (server_tid != pthread_self())
		pthread_kill (server_tid, sig);
}

static void sig_wake_up (int sig ATTR_UNUSED)
{
	debug ("got wake up signal");
}

static void clients_init ()
{
	int i;

	for (i = 0; i < CLIENTS_MAX; i++) {
		clients[i].socket = -1;
		pthread_mutex_init (&clients[i].events.mutex, NULL);
	}
}

static void clients_cleanup ()
{
	int i;

	for (i = 0; i < CLIENTS_MAX; i++) {
		clients[i].socket = -1;
		if (pthread_mutex_destroy(&clients[i].events.mutex))
			logit ("Can't destroy events mutex: %s",
					strerror(errno));

	}
}

/* Add a client to the list, return 1 if ok, 0 on error (max clients exceeded) */
static int add_client (int sock)
{
	int i;

	for (i = 0; i < CLIENTS_MAX; i++)
		if (clients[i].socket == -1) {
			clients[i].wants_events = 0;
			clients[i].events.num = 0;
			clients[i].socket = sock;
			return 1;
		}

	return 0;
}

static void del_client (struct client *cli)
{
	cli->socket = -1;
}

/* CHeck if the process with ginen PID exists. Return != 0 if so. */
static int valid_pid (const int pid)
{
	return kill(pid, 0) == 0 ? 1 : 0;
}

static void wake_up_server ()
{
	debug ("Waking up the server");
	if (pthread_kill(server_tid, SIGUSR1))
		logit ("Can't wake up the server: %s", strerror(errno));
}

/* Initialize the server - return fd of the listening socket or -1 on error */
int server_init (int debug, int foreground)
{
	struct sockaddr_un sock_name;
	int server_sock;
	int pid;

	pid = check_pid_file ();
	if (pid && valid_pid(pid)) {
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

	unlink (socket_name());

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

	audio_init ();
	clients_init ();

	server_tid = pthread_self ();
	signal (SIGTERM, sig_exit);
	signal (SIGINT, foreground ? sig_exit : SIG_IGN);
	signal (SIGHUP, SIG_IGN);
	signal (SIGQUIT, sig_exit);
	signal (SIGPIPE, SIG_IGN);
	signal (SIGUSR1, sig_wake_up);

	write_pid_file ();

	if (!foreground)
		setsid ();

	return server_sock;
}

/* Send EV_DATA and the integer value. Return 0 on error. */
static int send_data_int (const struct client *cli, const int data)
{
	assert (cli->socket != -1);

	if (!send_int(cli->socket, EV_DATA) || !send_int(cli->socket, data))
		return 0;

	return 1;
}

/* Send EV_DATA and the string value. Return 0 on error. */
static int send_data_str (const struct client *cli, const char *str) {
	if (!send_int(cli->socket, EV_DATA) || !send_str(cli->socket, str))
		return 0;
	return 1;
}

/* Add event to the queue */
static void add_event (struct events *ev, const int event)
{
	int i;

	LOCK (ev->mutex);
	for (i = 0; i < ev->num; i++)
		if (ev->queue[i] == event) {
			UNLOCK (ev->mutex);
			debug ("Not adding event 0x%02x: already in the queue",
					event);
			return;
		}
	
	assert (ev->num < EVENTS_MAX);

	ev->queue[ev->num++] = event;
	UNLOCK (ev->mutex);
}

static void add_event_all (const int event)
{
	int i;
	int added = 0;

	for (i = 0; i < CLIENTS_MAX; i++)
		if (clients[i].socket != -1 && clients[i].wants_events) {
			add_event (&clients[i].events, event);
			added++;
		}

	if (added)
		wake_up_server ();
	else
		debug ("No events were added because there are no clients.");
}

/* Send events from the queue. Return 0 on error. */
static int flush_events (struct client *cli)
{
	int i;

	LOCK (cli->events.mutex);
	for (i = 0; i < cli->events.num; i++)
		if (!send_int(cli->socket, cli->events.queue[i])) {
			UNLOCK (cli->events.mutex);
			return 0;
		}
	
	cli->events.num = 0;
	UNLOCK (cli->events.mutex);

	return 1;
}

/* Send events to clients that are ready to write. */
static void send_events (fd_set *fds)
{
	int i;

	for (i = 0; i < CLIENTS_MAX; i++)
		if (clients[i].socket != -1 && clients[i].wants_events
				&& FD_ISSET(clients[i].socket, fds)) {
			debug ("Flushing events for client %d", i);
			if (!flush_events(&clients[i])) {
				close (clients[i].socket);
				del_client (&clients[i]);
			}
		}
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

/* Send EV_BUSY message and close the connection. */
static void busy (int sock)
{
	logit ("Closing connection due to maximum number of clients reached.");
	send_int (sock, EV_BUSY);
	close (sock);
}

/* Handle CMD_LIST_ADD, return 1 if ok or 0 on error. */
static int req_list_add (struct client *cli)
{
	char *file;

	file = get_str (cli->socket);
	if (!file)
		return 0;

	logit ("Adding '%s' to the list", file);
	
	audio_plist_add (file);
	free (file);

	return 1;
}

/* Handle CMD_PLAY, return 1 if ok or 0 on error. */
static int req_play (struct client *cli)
{
	char *file;

	if (!(file = get_str(cli->socket)))
		return 0;

	logit ("Playing %s", *file ? file : "first element on the list");
	audio_play (file);
	free (file);
	
	return 1;
}

/* Handle CMD_SEEK, return 1 if ok or 0 on error */
static int req_seek (struct client *cli)
{
	int sec;

	if (!get_int(cli->socket, &sec))
		return 0;

	logit ("Seeking %ds", sec);
	audio_seek (sec);

	return 1;
}

/* Report an error logging it and sending a message to the client. */
void error (const char *format, ...)
{
	va_list va;
	
	va_start (va, format);
	vsnprintf (err_msg, sizeof(err_msg), format, va);
	err_msg[sizeof(err_msg) - 1] = 0;
	va_end (va);

	logit ("ERROR: %s", err_msg);

	add_event_all (EV_ERROR);
}

/* Send the song name to the client. Return 0 on error. */
static int send_sname (struct client *cli)
{
	int status = 1;
	char *sname = audio_get_sname ();
	
	if (!send_data_str(cli, sname ? sname : ""))
		status = 0;
	free (sname);

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

/* Send requested option value to the client. Return 1 if OK. */
static int send_option (struct client *cli)
{
	char *name;
	
	if (!(name = get_str(cli->socket)))
		return 0;

	/* We can send only a few options, other make no sense here */
	if (!valid_sync_option(name)) {
		logit ("Client wantetd to get not supported option '%s'",
				name);
		free (name);
		return 0;
	}

	/* All supported options are integer type. */
	if (!send_data_int(cli, options_get_int(name))) {
		free (name);
		return 0;
	}
	
	free (name);
	return 1;
}

/* Get and set an option from the client. Return 1 on error. */
static int get_set_option (struct client *cli)
{
	char *name;
	int val;
	
	if (!(name = get_str(cli->socket)))
		return 0;
	if (!valid_sync_option(name)) {
		logit ("Client requested setting invalid option '%s'", name);
		return 0;
	}
	if (!get_int(cli->socket, &val)) {
		free (name);
		return 0;
	}

	option_set_int (name, val);
	
	free (name);
	return 1;
}

/* Set the mixer to the value provided by the client. Return 0 on error. */
static int set_mixer (struct client *cli)
{
	int val;

	if (!get_int(cli->socket, &val))
		return 0;

	audio_set_mixer (val);
	return 1;
}

/* Delete an item from the playlist. Return 0 on error. */
static int delete_item (struct client *cli)
{
	char *file;
	
	if (!(file = get_str(cli->socket)))
		return 0;

	audio_plist_delete (file);
	free (file);
	return 1;
}

/* Reveive a command from the client and execute it. */
static void handle_command (struct client *cli)
{
	int cmd;
	int err = 0;

	if (!get_int(cli->socket, &cmd)) {
		logit ("Failed to get command from the client");
		close (cli->socket);
		del_client (cli);
		return;
	}

	switch (cmd) {
		case CMD_QUIT:
			logit ("Exit request from the client");
			server_quit = 1;
			break;
		case CMD_LIST_CLEAR:
			logit ("Clearing the list");
			audio_plist_clear ();
			break;
		case CMD_LIST_ADD:
			if (!req_list_add(cli))
				err = 1;
			break;
		case CMD_PLAY:
			if (!req_play(cli))
				err = 1;
			break;
		case CMD_DISCONNECT:
			logit ("Client disconnected");
			close (cli->socket);
			del_client (cli);
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
			if (!send_data_int(cli, audio_get_time()))
				err = 1;
			break;
		case CMD_SEEK:
			if (!req_seek(cli))
				err = 1;
			break;
		case CMD_GET_SNAME:
			if (!send_sname(cli))
				err = 1;
			break;
		case CMD_GET_STATE:
			if (!send_data_int(cli, audio_get_state()))
				err = 1;
			break;
		case CMD_GET_BITRATE:
			if (!send_data_int(cli, sound_info.bitrate))
				err = 1;
			break;
		case CMD_GET_RATE:
			if (!send_data_int(cli, sound_info.rate))
				err = 1;
			break;
		case CMD_GET_CHANNELS:
			if (!send_data_int(cli, sound_info.channels))
				err = 1;
			break;
		case CMD_NEXT:
			audio_next ();
			break;
		case CMD_PING:
			if (!send_int(cli->socket, EV_PONG))
				err = 1;
			break;
		case CMD_GET_OPTION:
			if (!send_option(cli))
				err = 1;
			break;
		case CMD_SET_OPTION:
			if (!get_set_option(cli))
				err = 1;
			break;
		case CMD_GET_MIXER:
			if (!send_data_int(cli, audio_get_mixer()))
				err = 1;
			break;
		case CMD_SET_MIXER:
			if (!set_mixer(cli))
				err = 1;
			break;
		case CMD_DELETE:
			if (!delete_item(cli))
				err = 1;
			break;
		case CMD_SEND_EVENTS:
			cli->wants_events = 1;
			logit ("Request for events");
			break;
		case CMD_GET_ERROR:
			if (!send_data_str(cli, err_msg))
				err = 1;
			break;
		default:
			logit ("Bad command (0x%x) from the client.", cmd);
			err = 1;
	}

	if (err) {
		logit ("Closing client connection due to error.");
		close (cli->socket);
		del_client (cli);
	}
}

/* Add clients file descriptors to fds. */
static void add_clients_fds (fd_set *read, fd_set *write)
{
	int i;

	for (i = 0; i < CLIENTS_MAX; i++)
		if (clients[i].socket != -1) {
			FD_SET (clients[i].socket, read);

			if (clients[i].events.num)
				FD_SET (clients[i].socket, write);
		}
}

/* return the maximum fd from clients and the argument. */
static int max_fd (int max)
{
	int i;

	for (i = 0; i < CLIENTS_MAX; i++)
		if (clients[i].socket > max)
			max = clients[i].socket;
	return max;
}

/* Handle clients which fd are ready to read. */
static void handle_clients (fd_set *fds)
{
	int i;

	for (i = 0; i < CLIENTS_MAX; i++)
		if (clients[i].socket != -1 && FD_ISSET(clients[i].socket, fds))
			handle_command (&clients[i]);
}

/* Close all client connections sending EV_EXIT. */
static void close_clients ()
{
	int i;

	for (i = 0; i < CLIENTS_MAX; i++)
		if (clients[i].socket != -1) {
			send_int (clients[i].socket, EV_EXIT);
			close (clients[i].socket);
			del_client (&clients[i]);
		}
}

/* Handle incomming connections */
void server_loop (int list_sock)
{
	struct sockaddr_un client_name;
	socklen_t name_len = sizeof (client_name);
	int end = 0;

	logit ("MOC server started, pid: %d", getpid());

	do {
		int res;
		fd_set fds_write, fds_read;
		
		FD_ZERO (&fds_read);
		FD_ZERO (&fds_write);
		FD_SET (list_sock, &fds_read);
		add_clients_fds (&fds_read, &fds_write);
		
		if (!server_quit)
			res = select (max_fd(list_sock)+1, &fds_read,
					&fds_write, NULL, NULL);
		else
			res = 0;

		if (res == -1 && errno != EINTR && !server_quit) {
			logit ("select() failed: %s", strerror(errno));
			fatal ("select() failed");
		}
		else if (!server_quit && res >= 0) {
			if (FD_ISSET(list_sock, &fds_read)) {
				int client_sock;
				
				debug ("accept()ing connection...");
				client_sock = accept (list_sock,
					(struct sockaddr *)&client_name,
					&name_len);
				
				if (client_sock == -1)
					fatal ("accept() failed: %s",
							strerror(errno));
				logit ("Incoming connection");
				if (!add_client(client_sock))
					busy (client_sock);
			}

			send_events (&fds_write);
			handle_clients (&fds_read);
		}

		if (server_quit)
			logit ("Exiting...");

	} while (!end && !server_quit);
	
	close_clients ();
	clients_cleanup ();
	close (list_sock);
	server_shutdown ();
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
	int old_bitrate = sound_info.bitrate;

	sound_info.bitrate = bitrate;
	if (old_bitrate <= 0 || bitrate <= 0 || event_throttle())
		add_event_all (EV_BITRATE);
}

void set_info_channels (const int channels)
{
	sound_info.channels = channels;
	add_event_all (EV_CHANNELS);
}

void set_info_rate (const int rate)
{
	sound_info.rate = rate;
	add_event_all (EV_RATE);
}

/* Notify the client about change of the player state. */
void state_change ()
{
	add_event_all (EV_STATE);
}

void ctime_change ()
{
	add_event_all (EV_CTIME);
}
