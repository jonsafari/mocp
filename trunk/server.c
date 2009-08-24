/*
 * MOC - music on console
 * Copyright (C) 2003 - 2005 Damian Pietras <daper@daper.net>
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
#include <sys/stat.h>
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <strings.h>
#include <signal.h>
#include <errno.h>
#include <stdarg.h>
#include <pthread.h>
#include <assert.h>

#define DEBUG

#include "log.h"
#include "protocol.h"
#include "common.h"
#include "audio.h"
#include "oss.h"
#include "options.h"
#include "server.h"
#include "playlist.h"
#include "tags_cache.h"
#include "files.h"
#include "softmixer.h"
#include "equalizer.h"

#define SERVER_LOG	"mocp_server_log"
#define PID_FILE	"pid"

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

/* Pipe used to wake up the server from select() from another thread. */
static int wake_up_pipe[2];

/* Set to 1 when a signal arrived causing the program to exit. */
static volatile int server_quit = 0;

static char err_msg[265] = "";

/* Information about currently played file */
static struct {
	int avg_bitrate;
	int bitrate;
	int rate;
	int channels;
} sound_info = {
	-1,
	-1,
	-1,
	-1
};

static struct tags_cache tags_cache;

extern char **environ;

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
			LOCK (clients[i].events_mutex);
			event_queue_free (&clients[i].events);
			event_queue_init (&clients[i].events);
			UNLOCK (clients[i].events_mutex);
			clients[i].socket = sock;
			clients[i].requests_plist = 0;
			clients[i].can_send_plist = 0;
			clients[i].lock = 0;
			tags_cache_clear_queue (&tags_cache, i);
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

/* Return the client index from tht clients table. */
static int client_index (const struct client *cli)
{
	int i;

	for (i = 0; i < CLIENTS_MAX; i++)
		if (clients[i].socket == cli->socket)
			return i;
	return -1;
}

static void del_client (struct client *cli)
{
	cli->socket = -1;
	LOCK (cli->events_mutex);
	event_queue_free (&cli->events);
	tags_cache_clear_queue (&tags_cache, client_index(cli));
	UNLOCK (cli->events_mutex);
}

/* CHeck if the process with ginen PID exists. Return != 0 if so. */
static int valid_pid (const int pid)
{
	return kill(pid, 0) == 0 ? 1 : 0;
}

static void wake_up_server ()
{
	int w = 1;
	
	debug ("Waking up the server");

	if (write(wake_up_pipe[1], &w, sizeof(w)) < 0)
		logit ("Can't wake up the server: (write() failed) %s",
				strerror(errno));
}

/* Thread-safe signal() version */
static void thread_signal (const int signum, void (*func)(int))
{
	struct sigaction act;

	act.sa_handler = func;
	act.sa_flags = 0;
	sigemptyset (&act.sa_mask);
	
	if (sigaction(signum, &act, 0) == -1)
		fatal ("sigaction() failed: %s", strerror(errno));
}

static void redirect_output (int out_fd)
{
	int fd;

	fd = open ("/dev/null", O_WRONLY);
	if (fd == -1)
		fatal ("Can't open /dev/null: %s", strerror(errno));

	if (dup2(fd, out_fd) == -1)
		fatal ("dup2() failed: %s", strerror(errno));
}

/* Initialize the server - return fd of the listening socket or -1 on error */
int server_init (int debug, int foreground)
{
	struct sockaddr_un sock_name;
	int server_sock;
	int pid;

	pid = check_pid_file ();
	if (pid && valid_pid(pid)) {
		fprintf (stderr, "\nIt seems that the server is already running"
				" with pid %d.\n", pid);
		fprintf (stderr, "If it is not true, remove the pid file (%s)"
				" and try again.\n",
				create_file_name(PID_FILE));
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

	if (pipe(wake_up_pipe) < 0)
		fatal ("pipe() failed: %s", strerror(errno));

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

	audio_initialize ();
	tags_cache_init (&tags_cache, options_get_int("TagsCacheSize"));
	tags_cache_load (&tags_cache, create_file_name("cache"));
	clients_init ();

	server_tid = pthread_self ();
	thread_signal (SIGTERM, sig_exit);
	thread_signal (SIGINT, foreground ? sig_exit : SIG_IGN);
	thread_signal (SIGHUP, SIG_IGN);
	thread_signal (SIGQUIT, sig_exit);
	thread_signal (SIGPIPE, SIG_IGN);

	write_pid_file ();

	if (!foreground) {
		setsid ();
		redirect_output (STDOUT_FILENO);
		redirect_output (STDERR_FILENO);
	}

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

void on_song_change()
{
	struct file_tags *curr_tags = NULL;
	static char *last_file = NULL;

	char *curr_file = NULL;
	char *command = NULL;
	char *bin = NULL;
	char **args = NULL;
	char *save = NULL;
	char *track = NULL;
	char duration_sec[10] = "";
	char duration_nice[12] = "";

	int argc = 2;
	int i = 0;

	curr_file = audio_get_sname();
	command = xstrdup(options_get_str("OnSongChange"));

	if((command)&&(curr_file) && 
			((!last_file) || (strcmp(last_file,curr_file)))) {
		while(command[i] != 0) {
			if((command[i] == '/')&&(argc == 2))
				bin = &command[i] + 1;
			else if(command[i] == ' ')
				argc++;
			i++;
		}

		curr_tags = tags_cache_get_immediate (&tags_cache, curr_file,
				TAGS_COMMENTS | TAGS_TIME);
		args = xmalloc(sizeof(char *) * argc);
		
		args[0] = bin;

		strtok_r(command, " ", &save);
		
		for(i=1;i<(argc - 1);i++) {
			args[i] = strtok_r(NULL, " ", &save);
			if((args[i][0] == '%')&&(curr_tags)) {
				switch(args[i][1]) {
					case 'a' :
						args[i] = curr_tags->artist ? 
							curr_tags->artist : 
							"";
						break;
					case 'r' :
						args[i] = curr_tags->album ?
							curr_tags->album :
							"";
						break;
					case 't' :
						args[i] = curr_tags->title ?
							curr_tags->title :
							"";
						break;
					case 'n' :
						if (curr_tags->track >= 0) {
							track = xmalloc(4);
							snprintf(track, 4, "%d",
								curr_tags->track);
							args[i] = track;
						}
						else
							args[i] = "";
						break;
					case 'f' :
						args[i] = curr_file;
						break;
					case 'D':
						if (curr_tags->time >= 0)
							sprintf (duration_sec,
									"%d",
									curr_tags->time);
						args[i] = duration_sec;
						break;
					case 'd':
						if (curr_tags->time >= 0)
							sec_to_min (duration_nice,
									curr_tags->time);
						args[i] = duration_nice;
						break;
				}
			}
		}

		args[argc - 1] = NULL;

		if(!fork()) {
			execve(command, args, environ);
			exit(-1);
		}

		free(command);
		if (track)
			free(track);
		free(args);
		tags_free (curr_tags);
	}

	free(last_file);
	last_file = curr_file;
}

/* Handle running external command on Stop event. */
static void on_stop ()
{
	char *command;

	command = xstrdup (options_get_str("OnStop"));

	if (command) {
		char *args[2];

		args[0] = xstrdup (command);
		args[1] = NULL;

		switch (fork()) {
			case 0:
				execve (command, args, environ);
				exit (0);
			case -1:
				logit ("Error when running OnStop command '%s': %s",
						command, strerror(errno));
				break;
		}

		free (command);
		free (args[0]);
	}
}

static void add_event_all (const int event, const void *data)
{
	int i;
	int added = 0;

	if (event == EV_STATE) {
		switch (audio_get_state()) {
			case STATE_PLAY:
				on_song_change ();
				break;
			case STATE_STOP:
				on_stop ();
				break;
		}
	}

	for (i = 0; i < CLIENTS_MAX; i++)
		if (clients[i].socket != -1 && clients[i].wants_events) {
			void *data_copy = NULL;

			if (data) {
				if (event == EV_PLIST_ADD
						|| event == EV_QUEUE_ADD) {
					data_copy = plist_new_item ();
					plist_item_copy (data_copy, data);
				}
				else if (event == EV_PLIST_DEL
						|| event == EV_QUEUE_DEL
						|| event == EV_STATUS_MSG) {
					data_copy = xstrdup (data);
				}
				else if (event == EV_PLIST_MOVE
						|| event == EV_QUEUE_MOVE)
					data_copy = move_ev_data_dup (
							(struct move_ev_data *)
							data);
				else
					logit ("Unhandled data!");
			}

			
			add_event (&clients[i], event, data_copy);
			added++;
		}

	if (added)
		wake_up_server ();
	else
		debug ("No events have been added because there are no "
				"clients.");
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
		if (clients[i].socket != -1
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
	tags_cache_save (&tags_cache, create_file_name("tags_cache"));
	tags_cache_destroy (&tags_cache);
	unlink (socket_name());
	unlink (create_file_name(PID_FILE));
	close (wake_up_pipe[0]);
	close (wake_up_pipe[1]);
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

/* Handle CMD_QUEUE_ADD, return 1 if ok or 0 on error. */
static int req_queue_add (const struct client *cli)
{
	char *file;
	struct plist_item *item;

	file = get_str (cli->socket);
	if (!file)
		return 0;

	logit ("Adding '%s' to the queue", file);

	audio_queue_add (file);

	/* Wrap the filename in struct plist_item.
	 * We don't need tags, because the player gets them
	 * when playing the file. This may change if there is
	 * support for viewing/reordering the queue and here
	 * is the place to read the tags and fill them into
	 * the item. */

	item = plist_new_item ();
	item->file = xstrdup (file);
	item->type = file_type (file);
	item->mtime = get_mtime (file);

	add_event_all (EV_QUEUE_ADD, item);

	plist_free_item_fields (item);
	free (item);
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
/* Handle CMD_JUMP_TO, return 1 if ok or 0 on error */
static int req_jump_to (struct client *cli)
{
	int sec;

	if (!get_int(cli->socket, &sec))
		return 0;
	logit ("Jumping to %ds", sec);
	audio_jump_to (sec);

	return 1;
}

/* Report an error logging it and sending a message to the client. */
void server_error (const char *msg)
{
	strncpy (err_msg, msg, sizeof(err_msg) - 1);
	err_msg[sizeof(err_msg) - 1] = 0;
	logit ("ERROR: %s", err_msg);
	add_event_all (EV_SRV_ERROR, NULL);
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

static int req_queue_del (const struct client *cli)
{
	char *file;

	if (!(file = get_str(cli->socket)))
		return 0;

	debug ("Deleting '%s' from queue", file);

	audio_queue_delete (file);
	add_event_all (EV_QUEUE_DEL, file);
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

/* Client requested to send queue so we get it from audio.c and
 * send it to the client */
static int req_send_queue (struct client *cli)
{
	int i;
	struct plist *queue;

	logit ("Client with fd %d wants queue ... sending it", cli->socket);

	if (!send_int(cli->socket, EV_DATA)) {
		logit ("Error while sending response, disconnecting"
				" the client.");
		close (cli->socket);
		del_client (cli);
		return 0;
	}

	queue = audio_queue_get_contents ();

	for (i = 0; i < queue->num; i++)
		if (!plist_deleted(queue, i)) {
			if(!send_item(cli->socket, &queue->items[i])){
				logit ("Error sending queue, disconnecting"
						" the client.");
				close (cli->socket);
				del_client (cli);
				return 0;
			}
		}

	plist_free (queue);

	if (!send_item (cli->socket, NULL)) {
		logit ("Error while sending end of playlist mark, disconnecting"
				" the client.");
		close (cli->socket);
		del_client (cli);
		return 0;
	}

	logit ("Queue sent");
	return 1;
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
	else if (cmd == CMD_CLI_PLIST_MOVE) {
		struct move_ev_data m;

		if (!(m.from = get_str(cli->socket))
				|| !(m.to = get_str(cli->socket))) {
			logit ("Error while reveiving file");
			return 0;
		}

		add_event_all (EV_PLIST_MOVE, &m);

		free (m.from);
		free (m.to);
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
		seed = (seed + 1) & 0xFF;
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

/* Send tags to the client. Return 0 on error. */
static int req_get_tags (struct client *cli)
{
	struct file_tags *tags;
	int res = 1;

	debug ("Sending tags to client with fd %d...", cli->socket);
	
	if (!send_int(cli->socket, EV_DATA)) {
		logit ("Error when sending EV_DATA");
		return 0;
	}
	
	tags = audio_get_curr_tags ();
	if (!send_tags(cli->socket, tags)) {
		logit ("Error when sending tags");
		res = 0;
	}

	if (tags)
		tags_free (tags);
	
	return res;
}

/* Handle CMD_GET_MIXER_CHANNEL_NAME. Return 0 on error. */
int req_get_mixer_channel_name (struct client *cli)
{
	int status = 1;
	char *name = audio_get_mixer_channel_name ();
	
	if (!send_data_str(cli, name ? name : ""))
		status = 0;
	free (name);

	return status;
}

/* Handle CMD_TOGGLE_MIXER_CHANNEL. Return 0 on error. */
void req_toggle_mixer_channel ()
{
	audio_toggle_mixer_channel ();
	add_event_all (EV_MIXER_CHANGE, NULL);
}

/* Handle CMD_TOGGLE_SOFTMIXER. Return 0 on error. */
void req_toggle_softmixer ()
{
        softmixer_set_active(!softmixer_is_active());
	add_event_all (EV_MIXER_CHANGE, NULL);
}

void update_eq_name()
{
        char buffer[27];
        
        char *n = equalizer_current_eqname();

        int l = strlen(n);

        /* status message can only take strings up to 25 chars
         * (without terminating zero)
         * The message header has 11 chars (EQ set to...)
         */
        if(l>14)
        {
          n[14] = 0;
          n[13] = '.';
          n[12] = '.';
          n[11] = '.';
        }
        
        sprintf(buffer, "EQ set to: %s", n);
        
        logit("%s", buffer);
        
        free(n);

        status_msg(buffer);       
}

void req_toggle_equalizer ()
{
        equalizer_set_active(!equalizer_is_active());

        update_eq_name();
}

void req_equalizer_refresh()
{
        equalizer_refresh();

        status_msg("Equalizer refreshed");
        
        logit("Equalizer refreshed");
}

void req_equalizer_prev()
{
        equalizer_prev();
        
        update_eq_name();
}

void req_equalizer_next()
{
        equalizer_next();
        
        update_eq_name();
}

void req_toggle_make_mono()
{
        char buffer[128];

        softmixer_set_mono(!softmixer_is_mono());

        sprintf(buffer, "Mono-Mixing set to: %s", softmixer_is_mono()?"on":"off");

        status_msg(buffer);
}

/* Handle CMD_GET_FILE_TAGS. Return 0 on error. */
static int get_file_tags (const int cli_id)
{
	char *file;
	int tags_sel;
	
	if (!(file = get_str(clients[cli_id].socket)))
		return 0;
	if (!get_int(clients[cli_id].socket, &tags_sel)) {
		free (file);
		return 0;
	}

	tags_cache_add_request (&tags_cache, file, tags_sel, cli_id);
	free (file);

	return 1;
}

static int abort_tags_requests (const int cli_id)
{
	char *file;

	if (!(file = get_str(clients[cli_id].socket)))
		return 0;

	tags_cache_clear_up_to (&tags_cache, file, cli_id);
	free (file);
	
	return 1;
}

/* Handle CMD_LIST_MOVE. Return 0 on error. */
static int req_list_move (struct client *cli)
{
	char *from;
	char *to;

	if (!(from = get_str(cli->socket)))
		return 0;
	if (!(to = get_str(cli->socket))) {
		free (from);
		return 0;
	}
	
	audio_plist_move (from, to);
	
	free (from);
	free (to);

	return 1;
}

/* Handle CMD_QUEUE_MOVE. Return 0 on error. */
static int req_queue_move (const struct client *cli)
{
	/*char *from;
	char *to;
	*/
	struct move_ev_data m;

	if (!(m.from = get_str(cli->socket)))
		return 0;
	if (!(m.to = get_str(cli->socket))) {
		free (m.from);
		return 0;
	}

	audio_queue_move (m.from, m.to);

	logit ("Swapping %s with %s in the queue", m.from, m.to);

	/* Broadcast the event to clients */
	add_event_all (EV_QUEUE_MOVE, &m);

	free (m.from);
	free (m.to);

	return 1;
}

/* Reveive a command from the client and execute it. */
static void handle_command (const int client_id)
{
	int cmd;
	int err = 0;
	struct client *cli = &clients[client_id];

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
		case CMD_JUMP_TO:
			if (!req_jump_to(cli))
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
		case CMD_GET_AVG_BITRATE:
			if (!send_data_int(cli, sound_info.avg_bitrate))
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
		case CMD_CLI_PLIST_MOVE:
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
		case CMD_GET_TAGS:
			if (!req_get_tags(cli))
				err = 1;
			break;
		case CMD_TOGGLE_MIXER_CHANNEL:
			req_toggle_mixer_channel ();
			break;
		case CMD_TOGGLE_SOFTMIXER:
			req_toggle_softmixer ();
			break;
		case CMD_GET_MIXER_CHANNEL_NAME:
			if (!req_get_mixer_channel_name(cli))
				err = 1;
			break;
		case CMD_GET_FILE_TAGS:
			if (!get_file_tags(client_id))
				err = 1;
			break;
		case CMD_ABORT_TAGS_REQUESTS:
			if (!abort_tags_requests(client_id))
				err = 1;
			break;
		case CMD_LIST_MOVE:
			if (!req_list_move(cli))
				err = 1;
			break;
                case CMD_TOGGLE_EQUALIZER:
                        req_toggle_equalizer();
                        break;
                case CMD_EQUALIZER_REFRESH:
                        req_equalizer_refresh();
                        break;
                case CMD_EQUALIZER_PREV:
                        req_equalizer_prev();
                        break;
                case CMD_EQUALIZER_NEXT:
                        req_equalizer_next();
                        break;
                case CMD_TOGGLE_MAKE_MONO:
                        req_toggle_make_mono();
                        break;
		case CMD_QUEUE_ADD:
			if (!req_queue_add(cli))
				err = 1;
			break;
		case CMD_QUEUE_DEL:
			if (!req_queue_del(cli))
				err = 1;
			break;
		case CMD_QUEUE_CLEAR:
			logit ("Clearing the queue");
			audio_queue_clear ();
			add_event_all (EV_QUEUE_CLEAR, NULL);
			break;
		case CMD_QUEUE_MOVE:
			if (!req_queue_move(cli))
				err = 1;
			break;
		case CMD_GET_QUEUE:
			if (!req_send_queue(cli))
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
			if (locking_client() == -1 || is_locking(&clients[i]))
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

	if (wake_up_pipe[0] > max)
		max = wake_up_pipe[0];

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
				handle_command (i);
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
		FD_SET (wake_up_pipe[0], &fds_read);
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

			if (FD_ISSET(wake_up_pipe[0], &fds_read)) {
				int w;
				
				logit ("Got 'wake up'");

				if (read(wake_up_pipe[0], &w, sizeof(w)) < 0)
					fatal ("Can't read wake up signal: %s",
							strerror(errno));
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

void set_info_bitrate (const int bitrate)
{
	sound_info.bitrate = bitrate;
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

void set_info_avg_bitrate (const int avg_bitrate)
{
	sound_info.avg_bitrate = avg_bitrate;
	add_event_all (EV_AVG_BITRATE, NULL);
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

void tags_change ()
{
	add_event_all (EV_TAGS, NULL);
}

void status_msg (const char *msg)
{
	add_event_all (EV_STATUS_MSG, msg);
}

void tags_response (const int client_id, const char *file,
		const struct file_tags *tags)
{
	assert (file != NULL);
	assert (tags != NULL);
	assert (client_id >= 0 && client_id < CLIENTS_MAX);
	
	if (clients[client_id].socket != -1) {
		struct tag_ev_response *data
			= (struct tag_ev_response *)xmalloc (
					sizeof(struct tag_ev_response));

		data->file = xstrdup (file);
		data->tags = tags_dup (tags);
		
		add_event (&clients[client_id], EV_FILE_TAGS, data);
		wake_up_server ();
	}
}

/* Announce to clients that first file from the queue is being played
 * and therefore needs to be removed from it */
/* XXX: this function is called from player thread and add_event_all
 *      imho doesn't properly lock all shared variables -- possible
 *      race condition??? */
void server_queue_pop (const char *filename)
{
	debug ("Queue pop -- broadcasting EV_QUEUE_DEL");
	add_event_all (EV_QUEUE_DEL, filename);
}
