/*
 * MOC - music on console
 * Copyright (C) 2003,2004,2005 Damian Pietras <daper@daper.net>
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

#define DEBUG

#include "log.h"
#include "protocol.h"
#include "main.h"
#include "audio.h"
#include "oss.h"
#include "options.h"
#include "server.h"
#include "playlist.h"

#define SERVER_LOG	"mocp_server_log"
#define PID_FILE	"pid"

#define CLIENTS_MAX	10

struct client
{
	int socket; 		/* -1 if inactive */
	int wants_events;	/* requested events? */
	struct event_queue events;
	pthread_mutex_t events_mutex;
	int requests_plist;	/* is the client waiting for the playlist? */
	int can_send_plist;	/* can this client send a playlist? */
	int lock;		/* is this client locking us? */
	int serial;		/* used for generating unique serial numbers */
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
		pthread_mutex_init (&clients[i].events_mutex, NULL);
	}
}

static void clients_cleanup ()
{
	int i;

	for (i = 0; i < CLIENTS_MAX; i++) {
		clients[i].socket = -1;
		if (pthread_mutex_destroy(&clients[i].events_mutex))
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
			event_queue_init (&clients[i].events);
			clients[i].socket = sock;
			clients[i].requests_plist = 0;
			clients[i].can_send_plist = 0;
			clients[i].lock = 0;
			return 1;
		}

	return 0;
}

/* Return index of a client that has a lock acquired. Return -1 if there is no
 * lock. */
static int locking_client ()
{
	int i;

	for (i = 0; i < CLIENTS_MAX; i++)
		if (clients[i].socket != -1 && clients[i].lock)
			return i;
	return -1;
}

/* Acquire a lock for this client. Return 0 on error. */
static int client_lock (struct client *cli)
{
	if (cli->lock) {
		logit ("Client wants deadlock.");
		return 0;
	}
	
	assert (locking_client() == -1);
	
	cli->lock = 1;
	logit ("Lock acquired for client with fd %d", cli->socket);
	return 1;
}

/* Return != 0 if this client holds a lock. */
static int is_locking (const struct client *cli)
{
	return cli->lock;
}

/* Release the lock hold by the client. Return 0 on error. */
static int client_unlock (struct client *cli)
{
	if (!cli->lock) {
		logit ("Client wants to unlock when there is no lock.");
		return 0;
	}

	cli->lock = 0;
	logit ("Lock released by client with fd %d", cli->socket);
	return 1;
}

static void del_client (struct client *cli)
{
	cli->socket = -1;
	struct event *e;

	/* Free the event queue - we can't just use event_queue_free(), because
	 * it can't free() the event's data. */

	while ((e = event_get_first(&cli->events))) {
		if (e->type == EV_PLIST_ADD) {
			plist_free_item_fields (e->data);
			free (e->data);
		}
		else if (e->type == EV_PLIST_DEL)
			free (e->data);
		event_pop (&cli->events);
	}
	
	/* To be sure :) */
	event_queue_free (&cli->events);
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

/* Add event to the client's queue */
static void add_event (struct client *cli, const int event, void *data)
{
	LOCK (cli->events_mutex);
	event_push (&cli->events, event, data);
	UNLOCK (cli->events_mutex);
}

static void add_event_all (const int event, void *data)
{
	int i;
	int added = 0;

	for (i = 0; i < CLIENTS_MAX; i++)
		if (clients[i].socket != -1 && clients[i].wants_events) {
			void *data_copy = NULL;

			if (data) {
				if (event == EV_PLIST_ADD) {
					data_copy = plist_new_item ();
					plist_item_copy (data_copy, data);
				}
				else if (event == EV_PLIST_DEL) {
					data_copy = xstrdup (data);
				}
				else
					logit ("Unhandled data!");
			}
			
			add_event (&clients[i], event, data_copy);
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
	enum noblock_io_status st = NB_IO_OK;

	LOCK (cli->events_mutex);
	while (!event_queue_empty(&cli->events)
			&& (st = event_send_noblock(cli->socket, &cli->events))
			== NB_IO_OK)
		;
	UNLOCK (cli->events_mutex);

	return st != NB_IO_ERR ? 1 : 0;
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
void server_error (const char *msg)
{
	strncpy (err_msg, msg, sizeof(err_msg) - 1);
	err_msg[sizeof(err_msg) - 1] = 0;
	logit ("ERROR: %s", err_msg);
	add_event_all (EV_ERROR, NULL);
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

	add_event_all (EV_OPTIONS, NULL);
	
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

	debug ("Request for deleting %s", file);

	audio_plist_delete (file);
	free (file);
	return 1;
}

/* Handle CMD_GET_FTIME. */
static int req_get_ftime (struct client *cli)
{
	char *file;

	if (!(file = get_str(cli->socket)))
		return 0;
	if (!send_data_int(cli, audio_get_ftime(file)))
		return 0;
	free (file);
	return 1;
}

/* Return the index of the first client able to send the playlist or -1 if
 * there isn't any. */
static int find_sending_plist ()
{
	int i;
	
	for (i = 0; i < CLIENTS_MAX; i++)
		if (clients[i].socket != -1 && clients[i].can_send_plist)
			return i;
	return -1;
}

/* Handle CMD_GET_PLIST. Return 0 on error. */
static int get_client_plist (struct client *cli)
{
	int first;

	debug ("Client with fd %d requests the playlist.", cli->socket);

	/* Find the first connected client, and ask it to send the playlist.
	 * Here, send 1 if there is a client with the playlist, or 0 if there
	 * isn't. */

	cli->requests_plist = 1;
	
	first = find_sending_plist ();
	if (first == -1) {
		debug ("No clients with the playlist.");
		cli->requests_plist = 0;
		if (!send_data_int(cli, 0))
			return 0;
		return 1;
	}
	
	if (!send_data_int(cli, 1))
		return 0;

	if (!send_int(clients[first].socket, EV_SEND_PLIST))
		return 0;

	return 1;
}

/* Find the client requesting for the playlist. */
static int find_cli_requesting_plist ()
{
	int i;
	
	for (i = 0; i < CLIENTS_MAX; i++)
		if (clients[i].requests_plist)
			return i;
	return -1;
}

/* Handle CMD_SEND_PLIST. Some client requested to get the playlist, so we asked
 * another client to send it (EV_SEND_PLIST). */
static int req_send_plist (struct client *cli)
{
	int requesting = find_cli_requesting_plist ();
	int send_fd;
	struct plist_item *item;
	int serial;

	debug ("Client with fd %d wants to send its playlists", cli->socket);

	if (requesting == -1) {
		logit ("No clients are requesting the playlist.");
		send_fd = -1;
	}
	else {
		send_fd = clients[requesting].socket;
		if (!send_int(send_fd, EV_DATA)) {
			logit ("Error while sending response, disconnecting"
					" the client");
			close (send_fd);
			del_client (&clients[requesting]);
			send_fd = -1;
		}
	}

	if (!get_int(cli->socket, &serial)) {
		logit ("Error while getting serial");
		return 0;
	}

	if (send_fd != -1 && !send_int(send_fd, serial)) {
		error ("Error while sending serial, disconnecting the client");
		close (send_fd);
		del_client (&clients[requesting]);
		send_fd = -1;
	}

	/* Even if no clients are requesting the playlist, we must read it,
	 * because there is no way to say that we don't need it. */
	while ((item = recv_item(cli->socket)) && item->file[0]) {
		if (send_fd != -1 && !send_item(send_fd, item)) {
			logit ("Error while sending item, disconnecting the"
					" client");
			close (send_fd);
			del_client (&clients[requesting]);
			send_fd = -1;
		}
		plist_free_item_fields (item);
		free (item);
	}

	if (item) {
		plist_free_item_fields (item);
		free (item);
		logit ("Playlist sent");
	}
	else
		logit ("Error while receiving item");

	if (send_fd != -1 && !send_item (send_fd, NULL)) {
		logit ("Error while sending end of playlist mark, disconnecting"
				" the client.");
		close (send_fd);
		del_client (&clients[requesting]);
		return 0;
	}

	if (requesting != -1)
		clients[requesting].requests_plist = 0;
	
	return item ? 1 : 0;
}

/* Handle command that synchinize playlists between interfaces (except
 * forwarding the whole list). Return 0 on error. */
static int plist_sync_cmd (struct client *cli, const int cmd)
{
	if (cmd == CMD_CLI_PLIST_ADD) {
		struct plist_item *item;

		debug ("Sending EV_PLIST_ADD");

		if (!(item = recv_item(cli->socket))) {
			logit ("Error while reveiving item");
			return 0;
		}
		
		add_event_all (EV_PLIST_ADD, item);
		plist_free_item_fields (item);
		free (item);
	}
	else if (cmd == CMD_CLI_PLIST_DEL) {
		char *file;

		debug ("Sending EV_PLIST_DEL");

		if (!(file = get_str(cli->socket))) {
			logit ("Error while reveiving file");
			return 0;
		}

		add_event_all (EV_PLIST_DEL, file);
		free (file);
	}
	else { /* it can be only CMD_CLI_PLIST_CLEAR */
		debug ("Sending EV_PLIST_CLEAR");
		add_event_all (EV_PLIST_CLEAR, NULL);
	}

	return 1;
}

/* Handle CMD_PLIST_GET_SERIAL. Return 0 on error. */
static int req_plist_get_serial (struct client *cli)
{
	if (!send_data_int(cli, audio_plist_get_serial()))
		return 0;
	return 1;
}

/* Handle CMD_PLIST_SET_SERIAL. Return 0 on error. */
static int req_plist_set_serial (struct client *cli)
{
	int serial;

	if (!get_int(cli->socket, &serial))
		return 0;
	
	if (serial < 0) {
		logit ("Clients wants to set bad serial number");
		return 0;
	}

	debug ("Setting the playlist serial number to %d", serial);
	audio_plist_set_serial (serial);

	return 1;
}

/* Return the client index from tht clients table. */
static int client_index (const struct client *cli)
{
	int i;

	for (i = 0; i < CLIENTS_MAX; i++)
		if (clients[i].socket == cli->socket)
			return i;
	return -1;
}

/* Generate a unique playlist serial number. */
static int gen_serial (const struct client *cli)
{
	static int seed = 0;
	int serial;
	
	/* Each client must always get a different serial number, so we use
	 * also the client index to generate it. It must not be also used by
	 * our playlist to not confise clients.
	 * There can be 256 different serial number per client, but it's
	 * enough, since clients use only two playlists. */

	do {
		serial = (seed << 8) | client_index(cli);
		seed = (seed + 1) & 0x07; /* TODO:  it should be 0xFF */
	} while (serial == audio_plist_get_serial());

	debug ("Generated serial %d for client with fd %d", serial,
			cli->socket);

	return serial;
}

/* Send the unique number to the client that no other client has. Return 0 on
 * error. */
static int send_serial (struct client *cli)
{
	if (!send_data_int(cli, gen_serial(cli))) {
		logit ("Error when sending serial");
		return 0;
	}
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
		case CMD_PREV:
			audio_prev ();
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
		case CMD_GET_FTIME:
			if (!req_get_ftime(cli))
				err = 1;
			break;
		case CMD_GET_PLIST:
			if (!get_client_plist(cli))
				err = 1;
			break;
		case CMD_SEND_PLIST:
			if (!req_send_plist(cli))
				err = 1;
			break;
		case CMD_CAN_SEND_PLIST:
			cli->can_send_plist = 1;
			break;
		case CMD_CLI_PLIST_ADD:
		case CMD_CLI_PLIST_DEL:
		case CMD_CLI_PLIST_CLEAR:
			if (!plist_sync_cmd(cli, cmd))
				err = 1;
			break;
		case CMD_LOCK:
			if (!client_lock(cli))
				err = 1;
			break;
		case CMD_UNLOCK:
			if (!client_unlock(cli))
				err = 1;
			break;
		case CMD_GET_SERIAL:
			if (!send_serial(cli))
				err = 1;
			break;
		case CMD_PLIST_GET_SERIAL:
			if (!req_plist_get_serial(cli))
				err = 1;
			break;
		case CMD_PLIST_SET_SERIAL:
			if (!req_plist_set_serial(cli))
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

			LOCK (clients[i].events_mutex);
			if (!event_queue_empty(&clients[i].events))
				FD_SET (clients[i].socket, write);
			UNLOCK (clients[i].events_mutex);
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
		if (clients[i].socket != -1
				&& FD_ISSET(clients[i].socket, fds)) {
			if (locking_client() == -1
					|| is_locking(&clients[i]))
				handle_command (&clients[i]);
			else
				debug ("Not getting a command from client with"
						" fd %d because of lock",
						clients[i].socket);
		}
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
		add_event_all (EV_BITRATE, NULL);
}

void set_info_channels (const int channels)
{
	sound_info.channels = channels;
	add_event_all (EV_CHANNELS, NULL);
}

void set_info_rate (const int rate)
{
	sound_info.rate = rate;
	add_event_all (EV_RATE, NULL);
}

/* Notify the client about change of the player state. */
void state_change ()
{
	add_event_all (EV_STATE, NULL);
}

void ctime_change ()
{
	add_event_all (EV_CTIME, NULL);
}
