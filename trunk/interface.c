/*
 * MOC - music on console
 * Copyright (C) 2004,2005 Damian Pietras <daper@daper.net>
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

#include <stdarg.h>
#include <locale.h>
#include <unistd.h>
#include <assert.h>
#include <string.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <signal.h>
#include <ctype.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#ifdef HAVE_SYS_SELECT_H
# include <sys/select.h>
#endif

#define DEBUG

#include "log.h"
#include "interface_elements.h"
#include "interface.h"
#include "common.h"
#include "playlist.h"
#include "playlist_file.h"
#include "protocol.h"
#include "keys.h"
#include "options.h"
#include "files.h"
#include "decoder.h"

#define INTERFACE_LOG	"mocp_client_log"

/* Socket of the server connection. */
static int srv_sock = -1;

static struct plist *playlist = NULL; /* our playlist */
static struct plist *dir_plist = NULL; /* content of the current directory */

/* Queue for events comming from the server. */
static struct event_queue events;

/* Current working directory (the directory we show). */
static char cwd[PATH_MAX] = "";

/* If the user presses quit, or we receive a termination signal. */
static volatile enum want_quit {
	NO_QUIT,	/* don't want to quit */
	QUIT_CLIENT,	/* only quit the client */
	QUIT_SERVER	/* quit the client and the srever */
} want_quit = NO_QUIT;

/* If user presses CTRL-C, set this to 1. This should interrupt long operations
 * that blocks the interface. */
static volatile int wants_interrupt = 0;

#ifdef SIGWINCH
/* If we get SIGWINCH. */
static volatile int want_resize = 0;
#endif

/* Are we waiting for the playlist we have loaded and sent to the clients? */
static int waiting_for_plist_load = 0;
					  
/* Information about the currently played file. */
static struct file_info {
	char *file;
	struct file_tags *tags;
	char *title;
	int bitrate;
	int rate;
	int curr_time;
	int total_time;
	int channels;
	int state; /* STATE_* */
} curr_file;

/* Silent seeking - where we are in seconds. -1 - no seeking. */
static int silent_seek_pos = -1;
static time_t silent_seek_key_last = (time_t)0; /* when the silent seek key was
						   last used */

/* When the menu was last moved (arrow keys, page up, etc.) */
static time_t last_menu_move_time = (time_t)0;

static void sig_quit (int sig ATTR_UNUSED)
{
	want_quit = QUIT_CLIENT;
}

static void sig_interrupt (int sig)
{
	logit ("Got signal %d: interrupt the operation", sig);
	wants_interrupt = 1;
}

#ifdef SIGWINCH
static void sig_winch (int sig ATTR_UNUSED)
{
	want_resize = 1;
}
#endif

int user_wants_interrupt ()
{
	return wants_interrupt;
}

static void clear_interrupt ()
{
	wants_interrupt = 0;
}

static void send_int_to_srv (const int num)
{
	if (!send_int(srv_sock, num))
		fatal ("Can't send() int to the server.");
}

static void send_str_to_srv (const char *str)
{
	if (!send_str(srv_sock, str))
		fatal ("Can't send() string to the server.");
}

static void send_item_to_srv (const struct plist_item *item)
{
	if (!send_item(srv_sock, item))
		fatal ("Can't send() item to the server.");
}

static int get_int_from_srv ()
{
	int num;
	
	if (!get_int(srv_sock, &num))
		fatal ("Can't receive value from the server.");

	return num;
}

/* Returned memory is malloc()ed. */
static char *get_str_from_srv ()
{
	char *str = get_str (srv_sock);
	
	if (!str)
		fatal ("Can't receive string from the server.");

	return str;
}

static struct file_tags *recv_tags_from_srv ()
{
	struct file_tags *tags = recv_tags (srv_sock);

	if (!tags)
		fatal ("Can't receive tags from the server.");

	return tags;
}

/* Noblocking version of get_int_from_srv(): return 0 if there are no data. */
static int get_int_from_srv_noblock (int *num)
{
	enum noblock_io_status st;
	
	if ((st = get_int_noblock(srv_sock, num)) == NB_IO_ERR)
		fatal ("Can't receive value from the server.");

	return st == NB_IO_OK ? 1 : 0;
}

static struct plist_item *recv_item_from_srv ()
{
	struct plist_item *item;

	if (!(item = recv_item(srv_sock)))
		fatal ("Can't receive item from the server.");

	return item;
}

static struct tag_ev_response *recv_tags_data_from_srv ()
{
	struct tag_ev_response *r;
	
	r = (struct tag_ev_response *)xmalloc (sizeof(struct tag_ev_response));

	r->file = get_str_from_srv ();
	if (!(r->tags = recv_tags(srv_sock)))
		fatal ("Can't receive tags event's data from the server.");

	return r;
}

static struct move_ev_data *recv_move_ev_data_from_srv ()
{
	struct move_ev_data *d;
	
	if (!(d = recv_move_ev_data(srv_sock)))
		fatal ("Can't receive move data from the server");

	return d;
}

/* Receive data for the given type of event and return them. Return NULL if
 * there is no data for the event. */
static void *get_event_data (const int type)
{
	switch (type) {
		case EV_PLIST_ADD:
			return recv_item_from_srv ();
		case EV_PLIST_DEL:
		case EV_STATUS_MSG:
			return get_str_from_srv ();
		case EV_FILE_TAGS:
			return recv_tags_data_from_srv ();
		case EV_PLIST_MOVE:
			return recv_move_ev_data_from_srv ();
	}

	return NULL;
}

/* Wait for EV_DATA handling other events. */
static void wait_for_data ()
{
	int event;
	
	do {
		event = get_int_from_srv ();
		
		if (event != EV_DATA)
			event_push (&events, event, get_event_data(event));
	 } while (event != EV_DATA);
}

/* Get an integer value from the server that will arrive after EV_DATA. */
static int get_data_int ()
{
	wait_for_data ();
	return get_int_from_srv ();
}

/* Get a string value from the server that will arrive after EV_DATA. */
static char *get_data_str ()
{
	wait_for_data ();
	return get_str_from_srv ();
}

static struct file_tags *get_data_tags ()
{
	wait_for_data ();
	return recv_tags_from_srv ();
}

static int send_tags_request (const char *file, const int tags_sel)
{
	assert (file != NULL);
	assert (tags_sel != 0);

	if (file_type(file) == F_SOUND) {
		send_int_to_srv (CMD_GET_FILE_TAGS);
		send_str_to_srv (file);
		send_int_to_srv (tags_sel);
		debug ("Asking for tags for %s", file);

		return 1;
	}
	else {
		debug ("Not sending tags request for URL (%s)", file);
		return 0;
	}
}

/* Send all items from this playlist to other clients */
static void send_items_to_clients (const struct plist *plist)
{
	int i;
	
	for (i = 0; i < plist->num; i++)
		if (!plist_deleted(plist, i)) {
			send_int_to_srv (CMD_CLI_PLIST_ADD);
			send_item_to_srv (&plist->items[i]);
		}
}

static void init_playlists ()
{
	dir_plist = (struct plist *)xmalloc (sizeof(struct plist));
	plist_init (dir_plist);
	playlist = (struct plist *)xmalloc (sizeof(struct plist));
	plist_init (playlist);

	/* set serial numbers for the playlist */
	send_int_to_srv (CMD_GET_SERIAL);
	plist_set_serial (playlist, get_data_int());
}

static void file_info_reset (struct file_info *f)
{
	f->file = NULL;
	f->tags = NULL;
	f->title = NULL;
	f->bitrate = -1;
	f->rate = -1;
	f->curr_time = -1;
	f->total_time = -1;
	f->channels = 1;
	f->state = STATE_STOP;
}

static void file_info_cleanup (struct file_info *f)
{
	if (f->tags)
		tags_free (f->tags);
	if (f->file)
		free (f->file);
	if (f->title)
		free (f->title);

	f->file = NULL;
	f->tags = NULL;
	f->title = NULL;
}

/* Get an integer option from the server (like shuffle) and set it. */
static void sync_int_option (const char *name)
{
	int value;
	
	send_int_to_srv (CMD_GET_OPTION);
	send_str_to_srv (name);
	value = get_data_int ();
	option_set_int (name, value);
	iface_set_option_state (name, value);
}

/* Get the server options and set our options like them. */
static void get_server_options ()
{
	sync_int_option ("Shuffle");
	sync_int_option ("Repeat");
	sync_int_option ("AutoNext");
}

static int get_server_plist_serial ()
{
	send_int_to_srv (CMD_PLIST_GET_SERIAL);
	return get_data_int ();
}

static int get_mixer_value ()
{
	send_int_to_srv (CMD_GET_MIXER);
	return get_data_int ();
}

static int get_state ()
{
	send_int_to_srv (CMD_GET_STATE);
	return get_data_int ();
}

static int get_channels ()
{
	send_int_to_srv (CMD_GET_CHANNELS);
	return get_data_int ();
}

static int get_rate ()
{
	send_int_to_srv (CMD_GET_RATE);
	return get_data_int ();
}

static int get_bitrate ()
{
	send_int_to_srv (CMD_GET_BITRATE);
	return get_data_int ();
}

static int get_curr_time ()
{
	send_int_to_srv (CMD_GET_CTIME);
	return get_data_int ();
}

static char *get_curr_file ()
{
	send_int_to_srv (CMD_GET_SNAME);
	return get_data_str ();
}

static void update_mixer_value ()
{
	iface_set_mixer_value (get_mixer_value());
}

static void update_mixer_name ()
{
	char *name;
	
	send_int_to_srv (CMD_GET_MIXER_CHANNEL_NAME);
	name = get_data_str ();

	debug ("Mixer name: %s", name);

	iface_set_mixer_name (name);
	free (name);
	update_mixer_value ();
}

/* Make new cwd path from CWD and this path */
static void set_cwd (char *path)
{
	if (path[0] == '/')
		strcpy (cwd, "/"); /* for absolute path */
	else if (!cwd[0]) {
		if (!getcwd(cwd, sizeof(cwd)))
			fatal ("Can't get CWD: %s", strerror(errno));
	}

	resolve_path (cwd, sizeof(cwd), path);
}

/* Try to find the directory we can start and set cwd to it. */
static void set_start_dir ()
{
	if (!getcwd(cwd, sizeof(cwd))) {
		if (errno == ERANGE)
			fatal ("CWD is larger than PATH_MAX!");
		else if (!getenv("HOME"))
			fatal ("$HOME is not set.");
		strncpy (cwd, getenv("HOME"), sizeof(cwd));
		if (cwd[sizeof(cwd)-1])
			fatal ("$HOME is larger than PATH_MAX!");
	}
}

/* Set cwd to last directory written to a file, return 1 on success. */
static int read_last_dir ()
{
	FILE *dir_file;
	int res = 1;
	int read;

	if (!(dir_file = fopen(create_file_name("last_directory"), "r")))
		return 0;

	if ((read = fread(cwd, sizeof(char), sizeof(cwd)-1, dir_file)) == 0)
		res = 0;
	else
		cwd[read] = 0;

	fclose (dir_file);
	return res;
}

/* Check if dir2 is in dir1 */
static int is_subdir (const char *dir1, const char *dir2)
{
	return !strncmp(dir1, dir2, strlen(dir1)) ? 1 : 0;
}

static int qsort_strcmp_func (const void *a, const void *b)
{
	return strcoll (*(char **)a, *(char **)b);
}

static int qsort_dirs_func (const void *a, const void *b)
{
	char *sa = *(char **)a;
	char *sb = *(char **)b;
	
	/* '../' is always first */
	if (!strcmp(sa, "../"))
		return -1;
	if (!strcmp(sb, "../"))
		return 1;
	
	return strcoll (sa, sb);
}

static int get_tags_setting ()
{
	int needed_tags = 0;

	if (options_get_int("ReadTags"))
		needed_tags |= TAGS_COMMENTS;
	if (!strcasecmp(options_get_str("ShowTime"), "yes"))
		needed_tags |= TAGS_TIME;

	return needed_tags;
}

/* Send requests for the given tags for every file on the playlist that
 * hasn't this tags. Return the number of requests. */
static int ask_for_tags (const struct plist *plist, const int tags_sel)
{
	int i;
	int req = 0;

	assert (plist != NULL);
	assert (tags_sel != 0);
	
	for (i = 0; i < plist->num; i++)
		if (!plist_deleted(plist, i) && (!plist->items[i].tags
					|| ~plist->items[i].tags->filled
					& tags_sel)) {
			char *file = plist_get_file (plist, i);
			
			req += send_tags_request (file, tags_sel);
			free (file);
		}

	return req;
}

static void interface_message (const char *format, ...)
{
	va_list va;
	char message[128];

	va_start (va, format);
	vsnprintf (message, sizeof(message), format, va);
	message[sizeof(message)-1] = 0;
	va_end (va);

	iface_message (message);
}

/* Update tags (and titles) for the given item on the playlist with new tags. */
static void update_item_tags (struct plist *plist, const int num,
		const struct file_tags *tags)
{
	struct file_tags *old_tags = plist_get_tags (plist, num);
	
	plist_set_tags (plist, num, tags);

	/* Get the time from the old tags if it's not presend in the new tags.
	 * FIXME: There is risk, that the file was modified and the time
	 * from the old tags is not valid. */
	if (!(tags->filled & TAGS_TIME) && old_tags && old_tags->time != -1)
		plist_set_item_time (plist, num, old_tags->time);

	if (options_get_int("ReadTags")) {
		if (plist->items[num].title_tags) {
			free (plist->items[num].title_tags);
			plist->items[num].title_tags = NULL;
		}
		make_tags_title (plist, num);
		if (plist->items[num].title_tags)
			plist->items[num].title = plist->items[num].title_tags;
		else {
			if (!plist->items[num].title_file)
				make_file_title (plist, num, options_get_int(
							"HideFileExtension"));
			plist->items[num].title = plist->items[num].title_file;
		}
	}

	if (old_tags)
		tags_free (old_tags);
}

/* Handle EV_FILE_TAGS. */
static void ev_file_tags (const struct tag_ev_response *data)
{
	int n;
	int found;

	assert (data != NULL);
	assert (data->file != NULL);
	assert (data->tags != NULL);

	debug ("Received tags for %s", data->file);

	if ((n = plist_find_fname(dir_plist, data->file)) != -1) {
		update_item_tags (dir_plist, n, data->tags);
		iface_update_item (dir_plist, n);
		found = 1;
	}
	else
		found = 0;
	
	if ((n = plist_find_fname(playlist, data->file)) != -1) {
		update_item_tags (playlist, n, data->tags);
		if (!found) /* don't do it twice */
			iface_update_item (playlist, n);
	}

	if (curr_file.file && !strcmp(data->file, curr_file.file)) {

		debug ("Tags apply to the currently played file.");
		
		if (data->tags->time != -1) {
			curr_file.total_time = data->tags->time;
			iface_set_total_time (curr_file.total_time);
		}
		else
			debug ("Not time information");

		if (data->tags->title) {
			if (curr_file.title)
				free (curr_file.title);
			curr_file.title = build_title (data->tags);
			iface_set_played_file_title (curr_file.title);
		}
	}
}

/* Update the current time. */
static void update_ctime ()
{
	curr_file.curr_time = get_curr_time ();
	if (silent_seek_pos == -1)
		iface_set_curr_time (curr_file.curr_time);
}

/* Use new tags for current file title (for Internet streams). */
static void update_curr_tags ()
{
	if (curr_file.file && is_url(curr_file.file)) {
		if (curr_file.tags)
			tags_free (curr_file.tags);
		send_int_to_srv (CMD_GET_TAGS);
		curr_file.tags = get_data_tags ();

		if (curr_file.tags->title) {
			curr_file.title = build_title (curr_file.tags);
			iface_set_played_file_title (curr_file.title);
		}
	}
}

/* Make sure that the currently played file is visible if it is in one of our
 * menus. */
static void follow_curr_file ()
{
	if (curr_file.file && file_type(curr_file.file) == F_SOUND
			&& last_menu_move_time <= time(NULL) - 2) {
		int server_plist_serial = get_server_plist_serial();
		
		if (server_plist_serial == plist_get_serial(playlist))
			iface_make_visible (IFACE_MENU_PLIST, curr_file.file);
		else if (server_plist_serial == plist_get_serial(dir_plist))
			iface_make_visible (IFACE_MENU_DIR, curr_file.file);
	}
}

static void update_curr_file ()
{
	char *file;

	file = get_curr_file ();

	if (file[0]) {
		if (!curr_file.file || strcmp(file, curr_file.file)) {
			file_info_cleanup (&curr_file);
			iface_set_played_file (file);
			send_tags_request (file, TAGS_COMMENTS | TAGS_TIME);
			curr_file.file = file;

			/* make a title that will be used until we get tags */
			if (file_type(file) == F_URL || !strchr(file, '/')) {
				curr_file.title = xstrdup (file);
				update_curr_tags ();
			}
			else
				curr_file.title =
					xstrdup (strrchr(file, '/') + 1);

			iface_set_played_file (file);
			iface_set_played_file_title (curr_file.title);
			
			/* Silent seeking makes no sense if the playing file has
			 * changed */
			silent_seek_pos = -1;
			iface_set_curr_time (curr_file.curr_time);

			if (options_get_int("FollowPlayedFile"))
				follow_curr_file ();
		}
		else
			free (file);
	}
	else {
		file_info_cleanup (&curr_file);
		file_info_reset (&curr_file);
		iface_set_played_file (NULL);
		free (file);
	}
}

static void update_rate ()
{
	curr_file.rate = get_rate ();
	iface_set_rate (curr_file.rate);
}

static void update_channels ()
{
	curr_file.channels = get_channels () == 2 ? 2 : 1;
	iface_set_channels (curr_file.channels);
}

static void update_bitrate ()
{
	curr_file.bitrate = get_bitrate ();
	iface_set_bitrate (curr_file.bitrate);
}

/* Get and show the server state. */
static void update_state ()
{
	int old_state = curr_file.state;
	
	/* play | stop | pause */
	curr_file.state = get_state ();
	iface_set_state (curr_file.state);

	/* Silent seeking makes no sense if the state has changed. */
	if (old_state != curr_file.state)
		silent_seek_pos = -1;

	update_curr_file ();
	
	update_channels ();
	update_bitrate ();
	update_rate ();
	update_ctime ();
}

/* Handle EV_PLIST_ADD. */
static void event_plist_add (const struct plist_item *item)
{
	if (plist_find_fname(playlist, item->file) == -1) {
		int item_num = plist_add_from_item (playlist, item);
		int needed_tags = 0;

		if (options_get_int("ReadTags")
				&& (!item->tags || !item->tags->title))
			needed_tags |= TAGS_COMMENTS;
		if (!strcasecmp(options_get_str("ShowTime"), "yes")
				&& (!item->tags || item->tags->time == -1))
			needed_tags |= TAGS_TIME;

		if (needed_tags)
			send_tags_request (item->file, needed_tags);
		
		if (options_get_int("ReadTags")) {
			make_tags_title (playlist, item_num);

			if (playlist->items[item_num].title_tags)
				playlist->items[item_num].title =
					playlist->items[item_num].title_tags;
			else
				playlist->items[item_num].title =
					playlist->items[item_num].title_file;
		}
		else {
			make_file_title (playlist, item_num,
					options_get_int("HideFileExtension"));
			playlist->items[item_num].title =
				playlist->items[item_num].title_file;
		}

		iface_add_to_plist (playlist, item_num);
		
		if (waiting_for_plist_load) {
			if (iface_in_dir_menu())
				iface_switch_to_plist ();
			waiting_for_plist_load = 0;
		}
	}
}

/* Get error message from the server and show it. */
static void update_error ()
{
	char *err;
	
	send_int_to_srv (CMD_GET_ERROR);
	err = get_data_str ();
	error (err);
	free (err);
}

/* Send the playlist to the server to be forwarded to another client. */
static void forward_playlist ()
{
	int i;

	debug ("Forwarding the playlist...");
	
	send_int_to_srv (CMD_SEND_PLIST);
	send_int_to_srv (plist_get_serial(playlist));

	for (i = 0; i < playlist->num; i++)
		if (!plist_deleted(playlist, i))
			send_item_to_srv (&playlist->items[i]);

	send_item_to_srv (NULL);
}

static int recv_server_plist (struct plist *plist)
{
	int end_of_list = 0;
	struct plist_item *item;

	logit ("Asking server for the playlist from other client.");
	send_int_to_srv (CMD_GET_PLIST);
	logit ("Waiting for response");
	wait_for_data ();

	if (!get_int_from_srv()) {
		debug ("There is no playlist");
		return 0; /* there are no other clients with a playlist */
	}

	logit ("There is a playlist, getting...");
	wait_for_data ();

	logit ("Transfer...");

	plist_set_serial (plist, get_int_from_srv());

	do {
		item = recv_item_from_srv ();
		if (item->file[0])
			plist_add_from_item (plist, item);
		else
			end_of_list = 1;
		plist_free_item_fields (item);
		free (item);
	} while (!end_of_list);

	return 1;
}

/* Clear the playlist locally */
static void clear_playlist ()
{
	if (iface_in_plist_menu())
		iface_switch_to_dir ();
	plist_clear (playlist);
	iface_clear_plist ();
	
	if (!waiting_for_plist_load)
		interface_message ("The playlist was cleared.");
	iface_set_status ("");
}

/* Handle EV_PLIST_DEL. */
static void event_plist_del (char *file)
{
	int item = plist_find_fname (playlist, file);

	if (item != -1) {
		char *file;
		int have_all_times;
		int playlist_total_time;

		file = plist_get_file (playlist, item);
		plist_delete (playlist, item);

		iface_del_plist_item (file);
		playlist_total_time = plist_total_time (playlist,
				&have_all_times);
		iface_plist_set_total_time (playlist_total_time,
				have_all_times);
		free (file);

		if (plist_count(playlist) == 0)
			clear_playlist ();
	}
	else
		logit ("Server requested deleting an item not present on the"
				" playlist.");
}

/* Swap 2 file on the playlist. */
static void swap_playlist_items (const char *file1, const char *file2)
{
	assert (file1 != NULL);
	assert (file2 != NULL);

	plist_swap_files (playlist, file1, file2);
	iface_swap_plist_items (file1, file2);
}

/* Handle EV_PLIST_MOVE. */
static void event_plist_move (const struct move_ev_data *d)
{
	assert (d != NULL);
	assert (d->from != NULL);
	assert (d->to != NULL);

	swap_playlist_items (d->from, d->to);
}

/* Handle server event. */
static void server_event (const int event, void *data)
{
	logit ("EVENT: 0x%02x", event);

	switch (event) {
		case EV_BUSY:
			interface_fatal ("The server is busy, another client "
					"is connected.");
			break;
		case EV_CTIME:
			update_ctime ();
			break;
		case EV_STATE:
			update_state ();
			break;
		case EV_EXIT:
			interface_fatal ("The server exited.");
			break;
		case EV_BITRATE:
			update_bitrate ();
			break;
		case EV_RATE:
			update_rate ();
			break;
		case EV_CHANNELS:
			update_channels ();
			break;
		case EV_SRV_ERROR:
			update_error ();
			break;
		case EV_OPTIONS:
			get_server_options ();
			break;
		case EV_SEND_PLIST:
			forward_playlist ();
			break;
		case EV_PLIST_ADD:
			if (options_get_int("SyncPlaylist"))
				event_plist_add ((struct plist_item *)data);
			plist_free_item_fields (data);
			break;
		case EV_PLIST_CLEAR:
			if (options_get_int("SyncPlaylist"))
				clear_playlist ();
			break;
		case EV_PLIST_DEL:
			if (options_get_int("SyncPlaylist"))
				event_plist_del ((char *)data);
			break;
		case EV_PLIST_MOVE:
			if (options_get_int("SyncPlaylist"))
				event_plist_move ((struct move_ev_data *)data);
			break;
		case EV_TAGS:
			update_curr_tags ();
			break;
		case EV_STATUS_MSG:
			iface_set_status ((char *)data);
			break;
		case EV_MIXER_CHANGE:
			update_mixer_name ();
			break;
		case EV_FILE_TAGS:
			ev_file_tags ((struct tag_ev_response *)data);
			break;
		default:
			interface_fatal ("Unknown event: 0x%02x", event);
	}

	free_event_data (event, data);
}

/* Send requests for the given tags for every file on the playlist and wait
 * for all responses. If no_iface has non-zero value, it will not access the
 * interface. */
static void fill_tags (struct plist *plist, const int tags_sel,
		const int no_iface)
{
	int files;
	
	assert (plist != NULL);
	assert (tags_sel != 0);

	iface_set_status ("Reading tags...");
	files = ask_for_tags (plist, tags_sel);

	/* Process events until we have all tags. */
	while (files && !user_wants_interrupt()) {
		int type = get_int_from_srv ();
		void *data = get_event_data (type);
		
		if (type == EV_FILE_TAGS) {
			struct tag_ev_response *ev
				= (struct tag_ev_response *)data;
			int n;

			if ((n = plist_find_fname(plist, ev->file)) != -1) {
				if ((ev->tags->filled & tags_sel))
					files--;
				update_item_tags (plist, n, ev->tags);
			}
		}
		else if (no_iface)
			abort (); /* can't handle other events without the
				     interface */
	
		if (!no_iface)
			server_event (type, data);
		else
			free_event_data (type, data);
	}
	
	iface_set_status ("");
}

/* Load the directory content into dir_plist and switch the menu to it.
 * If dir is NULL, go to the cwd. if reload is not zero, we are reloading
 * the current directory, so use iface_update_dir_content().
 * Return 1 on success, 0 on error. */
static int go_to_dir (const char *dir, const int reload)
{
	struct plist *old_dir_plist;
	char last_dir[PATH_MAX];
	const char *new_dir = dir ? dir : cwd;
	int going_up = 0;
	struct file_list *dirs, *playlists;

	iface_set_status ("reading directory...");

	if (dir && is_subdir(dir, cwd)) {
		strcpy (last_dir, strrchr(cwd, '/') + 1);
		strcat (last_dir, "/");
		going_up = 1;
	}

	old_dir_plist = dir_plist;
	dir_plist = (struct plist *)xmalloc (sizeof(struct plist));
	plist_init (dir_plist);
	dirs = file_list_new ();
	playlists = file_list_new ();

	if (!read_directory(new_dir, dirs, playlists, dir_plist)) {
		iface_set_status ("");
		plist_free (dir_plist);
		file_list_free (dirs);
		file_list_free (playlists);
		free (dir_plist);
		dir_plist = old_dir_plist;
		return 0;
	}

	/* TODO: use CMD_ABORT_TAGS_REQUESTS (what if we requested tags for the
	 playlist?) */

	plist_free (old_dir_plist);
	free (old_dir_plist);

	if (dir) /* if dir is NULL, we went to cwd */
		strcpy (cwd, dir);

	switch_titles_file (dir_plist);

	plist_sort_fname (dir_plist);
	qsort (dirs->items, dirs->num, sizeof(char *), qsort_dirs_func);
	qsort (playlists->items, playlists->num, sizeof(char *),
			qsort_strcmp_func);

	if (get_tags_setting())
		ask_for_tags (dir_plist, get_tags_setting());
	
	if (reload)
		iface_update_dir_content (IFACE_MENU_DIR, dir_plist, dirs,
				playlists);
	else
		iface_set_dir_content (IFACE_MENU_DIR, dir_plist, dirs,
				playlists);
	file_list_free (dirs);
	file_list_free (playlists);
	if (going_up)
		iface_set_curr_item_title (last_dir);
	
	iface_set_title (IFACE_MENU_DIR, cwd);

	return 1;
}

/* Make sure that the server's playlist has different serial from ours. */
static void change_srv_plist_serial ()
{
	int serial;
	
	do {	
		send_int_to_srv (CMD_GET_SERIAL);
		serial = get_data_int ();
	 } while (serial == plist_get_serial(playlist)
			|| serial == plist_get_serial(dir_plist));

	send_int_to_srv (CMD_PLIST_SET_SERIAL);
	send_int_to_srv (serial);
}

static void enter_first_dir ();

/* Switch between the directory view and the playlist. */
static void toggle_menu ()
{
	int num;
	
	if (iface_in_plist_menu()) {
		if (!cwd[0])
			
			/* we were at the playlist from the startup */
			enter_first_dir ();
		else
			iface_switch_to_dir ();
	}
	else if ((num = plist_count(playlist)))
		iface_switch_to_plist ();
	else
		error ("The playlist is empty.");
}

/* Load the playlist file and switch the menu to it. Return 1 on success. */
static int go_to_playlist (const char *file)
{
	if (plist_count(playlist)) {
		error ("Please clear the playlist, because "
				"I'm not sure you want to do this.");
		return 0;
	}

	plist_clear (playlist);

	iface_set_status ("Loading playlist...");
	if (plist_load(playlist, file, cwd)) {
	
		if (options_get_int("SyncPlaylist")) {
			send_int_to_srv (CMD_LOCK);
			change_srv_plist_serial ();
			send_int_to_srv (CMD_CLI_PLIST_CLEAR);
			iface_set_status ("Notifying clients...");
			send_items_to_clients (playlist);
			iface_set_status ("");
			waiting_for_plist_load = 1;
			send_int_to_srv (CMD_UNLOCK);

			/* We'll use the playlist received from the
			 * server to be synchronized with other clients
			 */
			plist_clear (playlist);
		}
		else
			toggle_menu ();

		interface_message ("Playlist loaded.");
	}
	else {
		interface_message ("The playlist is empty");
		iface_set_status ("");
		return 0;
	}

	return 1;
}

/* Enter to the initial directory or toggle to the initial playlist (only
 * if the function has not been called yet). */
static void enter_first_dir ()
{
	static int first_run = 1;
	
	if (options_get_int("StartInMusicDir")) {
		char *music_dir;

		if ((music_dir = options_get_str("MusicDir"))) {
			set_cwd (music_dir);
			if (first_run && file_type(music_dir) == F_PLAYLIST
					&& plist_count(playlist) == 0
					&& go_to_playlist(music_dir)) {
				cwd[0] = 0;
				first_run = 0;
			}
			else if (file_type(cwd) == F_DIR
					&& go_to_dir(NULL, 0)) {
				first_run = 0;
				return;
			}
		}
		else
			error ("MusicDir is not set");
	}
	
	if (!(read_last_dir() && go_to_dir(NULL, 0))) {
		set_start_dir ();
		if (!go_to_dir(NULL, 0))
			interface_fatal ("Can't enter any directory.");
	}

	first_run = 0;
}

/* Request the playlist from the server (given by another client). Make the
 * titles. Return 0 if such a list doesn't exists. */
static int get_server_playlist (struct plist *plist)
{
	iface_set_status ("Getting the playlist...");
	debug ("Getting the playlist...");
	if (recv_server_plist(plist)) {
		ask_for_tags (plist, get_tags_setting());
		switch_titles_tags (plist);
		iface_set_status ("");
		return 1;
	}

	iface_set_status ("");
	
	return 0;
}

/* Get the playlist from another client and use it as our playlist.
 * Return 0 if there is no client with a playlist. */
static int use_server_playlist ()
{
	if (get_server_playlist(playlist)) {
		iface_set_dir_content (IFACE_MENU_PLIST, playlist, NULL, NULL);
		return 1;
	}

	return 0;
}

/* Process file names passwd as arguments. */
static void process_args (char **args, const int num)
{
	if (num == 1 && !is_url(args[0]) && isdir(args[0]) == 1) {
		set_cwd (args[0]);
		if (!go_to_dir(NULL, 0))
			enter_first_dir ();
		return;
	}
	
	if (num == 1 && is_plist_file(args[0])) {
		char path[PATH_MAX+1]; /* the directory where the playlist is */
		char *slash;

		if (args[0][0] == '/')
			strcpy (path, "/");
		else if (!getcwd(path, sizeof(path)))
			interface_fatal ("Can't get CWD: %s", strerror(errno));

		resolve_path (path, sizeof(path), args[0]);
		slash = strrchr (path, '/');
		assert (slash != NULL);
		*slash = 0;
		
		iface_set_status ("Loading playlist...");
		plist_load (playlist, args[0], path);
		iface_set_status ("");
	}
	else {
		int i;
		char this_cwd[PATH_MAX];
		
		if (!getcwd(this_cwd, sizeof(cwd)))
			interface_fatal ("Can't get CWD: %s.", strerror(errno));

		for (i = 0; i < num; i++) {
			char path[2*PATH_MAX];
			int dir = !is_url(args[i]) && isdir(args[i]);

			if (is_url(args[i])) {
				strncpy(path, args[i], sizeof(path));
				path[sizeof(path) - 1] = 0;
			}
			else {
				if (args[i][0] == '/')
					strcpy (path, "/");
				else
					strcpy (path, this_cwd);
				resolve_path (path, sizeof(path), args[i]);
			}

			if (dir == 1)
				read_directory_recurr (path, playlist);
			else if (!dir && (is_sound_file(path)
						|| is_url(path)))
				plist_add (playlist, path);
			else if (is_plist_file(path))
				plist_load (playlist, path, NULL);
		}
	}

	if (plist_count(playlist) && !options_get_int("SyncPlaylist")) {
		switch_titles_file (playlist);
		ask_for_tags (playlist, get_tags_setting());
		iface_set_dir_content (IFACE_MENU_PLIST, playlist, NULL, NULL);
		iface_switch_to_plist ();
	}
	else
		enter_first_dir ();
}

/* Load the playlist from .moc directory. */
static void load_playlist ()
{
	char *plist_file = create_file_name ("playlist.m3u");

	if (file_type(plist_file) == F_PLAYLIST)
		go_to_playlist (plist_file);
}

void init_interface (const int sock, const int logging, char **args,
		const int arg_num)
{
	srv_sock = sock;

	if (logging) {
		FILE *logfp;

		if (!(logfp = fopen(INTERFACE_LOG, "a")))
			fatal ("Can't open log file for the interface");
		log_init_stream (logfp);
	}

	logit ("Starting MOC interface...");

	/* set locale acording to the environment variables */
	if (!setlocale(LC_CTYPE, ""))
		logit ("Could not net locate!");

	file_info_reset (&curr_file);
	init_playlists ();
	event_queue_init (&events);
	keys_init ();
	windows_init ();
	get_server_options ();
	update_mixer_name ();

	signal (SIGQUIT, sig_quit);
	/*signal (SIGTERM, sig_quit);*/
	signal (SIGINT, sig_interrupt);
	
#ifdef SIGWINCH
	signal (SIGWINCH, sig_winch);
#endif
	

	if (arg_num) {
		process_args (args, arg_num);
	
		if (plist_count(playlist) == 0) {
			if (!options_get_int("SyncPlaylist")
					|| !use_server_playlist())
				load_playlist ();
			send_int_to_srv (CMD_SEND_EVENTS);
		}
		else if (options_get_int("SyncPlaylist")) {
			struct plist tmp_plist;
			
			/* We have made the playlist from command line. */
			
			/* the playlist should be now clear, but this will give
			 * us the serial number of the playlist used by other
			 * clients. */
			plist_init (&tmp_plist);
			get_server_playlist (&tmp_plist);

			send_int_to_srv (CMD_SEND_EVENTS);

			send_int_to_srv (CMD_LOCK);
			send_int_to_srv (CMD_CLI_PLIST_CLEAR);

			plist_set_serial (playlist,
					plist_get_serial(&tmp_plist));
			plist_free (&tmp_plist);

			change_srv_plist_serial ();
		
			iface_set_status ("Notifying clients...");
			send_items_to_clients (playlist);
			iface_set_status ("");
			plist_clear (playlist);
			waiting_for_plist_load = 1;
			send_int_to_srv (CMD_UNLOCK);

			/* Now enter_first_dir() should not go to the music
			 * directory. */
			option_set_int ("StartInMusicDir", 0);

		}

	}
	else {
		send_int_to_srv (CMD_SEND_EVENTS);
		if (!options_get_int("SyncPlaylist")
				|| !use_server_playlist())
			load_playlist ();
		enter_first_dir ();
	}
	
	if (options_get_int("SyncPlaylist"))
		send_int_to_srv (CMD_CAN_SEND_PLIST);
	
	update_state ();
}

#ifdef SIGWINCH
/* Handle resizeing xterm */
static void do_resize ()
{
	iface_resize ();
	logit ("resize");
	want_resize = 0;
}
#endif

static void go_dir_up ()
{
	char *dir;
	char *slash;

	dir = xstrdup (cwd);
	slash = strrchr (dir, '/');
	assert (slash != NULL);
	if (slash == dir)
		*(slash + 1) = 0;
	else
		*slash = 0;

	go_to_dir (dir, 0);
	free (dir);
}

/* Get (generate) a playlist serial from the server and make sure it's not
 * the same as our playlists' serial. */
static int get_safe_serial ()
{
	int serial;

	do {
		send_int_to_srv (CMD_GET_SERIAL);
		serial = get_data_int ();
	} while (serial == plist_get_serial(playlist)); /* check only the
							   playlist, because
							   dir_plist has serial
							   -1 */

	return serial;
}

/* Send the playlist to the server. If clear != 0, clear the server's playlist
 * before sending. */
static void send_playlist (struct plist *plist, const int clear)
{
	int i;
	
	if (clear)
		send_int_to_srv (CMD_LIST_CLEAR);
	
	for (i = 0; i < plist->num; i++) {
		if (!plist_deleted(plist, i)) {
			send_int_to_srv (CMD_LIST_ADD);
			send_str_to_srv (plist->items[i].file);
		}
	}
}

/* Send the playlist to the server if necessary and request playing this
 * item. */
static void play_it (const char *file)
{
	struct plist *curr_plist;
	
	assert (file != NULL);

	if (iface_in_dir_menu())
		curr_plist = dir_plist;
	else
		curr_plist = playlist;
	
	send_int_to_srv (CMD_LOCK);

	if (plist_get_serial(curr_plist) == -1 || get_server_plist_serial()
			!= plist_get_serial(curr_plist)) {
		int serial;

		logit ("The server has different playlist");

		serial = get_safe_serial();
		plist_set_serial (curr_plist, serial);
		send_int_to_srv (CMD_PLIST_SET_SERIAL);
		send_int_to_srv (serial);
	
		send_playlist (curr_plist, 1);
	}
	else
		logit ("The server already has my playlist");
	
	send_int_to_srv (CMD_PLAY);
	send_str_to_srv (file);

	send_int_to_srv (CMD_UNLOCK);
}

/* Action when the user selected a file. */
static void go_file ()
{
	enum file_type type = iface_curritem_get_type ();
	char *file = iface_get_curr_file ();

	if (!file)
		return;

	if (type == F_SOUND || type == F_URL)
		play_it (file);
	else if (type == F_DIR && iface_in_dir_menu()) {
		if (!strcmp(file, ".."))
			go_dir_up ();
		else 
			go_to_dir (file, 0);
	}
	else if (type == F_PLAYLIST)
		go_to_playlist (file);

	free (file);
}

/* pause/unpause */
static void switch_pause ()
{
	switch (curr_file.state) {
		case STATE_PLAY:
			send_int_to_srv (CMD_PAUSE);
			break;
		case STATE_PAUSE:
			send_int_to_srv (CMD_UNPAUSE);
			break;
		default:
			logit ("User pressed pause when not playing.");
	}
}

static void set_mixer (int val)
{
	if (val < 0)
		val = 0;
	else if (val > 100)
		val = 100;

	send_int_to_srv (CMD_SET_MIXER);
	send_int_to_srv (val);
}

static void adjust_mixer (const int diff)
{
	set_mixer (get_mixer_value() + diff);
}

/* Add the currently selected file to the playlist. */
static void add_file_plist ()
{
	char *file;

	if (iface_in_plist_menu()) {
		error ("Can't add to the playlist a file from the "
				"playlist.");
		return;
	}

	if (iface_curritem_get_type() == F_DIR) {
		error ("This is a directory.");
		return;
	}
	
	file = iface_get_curr_file ();

	if (!file)
		return;
	
	if (iface_curritem_get_type() != F_SOUND) {
		error ("You can only add a file using this command.");
		free (file);
		return;
	}


	if (plist_find_fname(playlist, file) == -1) {
		struct plist_item *item = &dir_plist->items[
			plist_find_fname(dir_plist, file)];

		send_int_to_srv (CMD_LOCK);

		if (options_get_int("SyncPlaylist")) {
			send_int_to_srv (CMD_CLI_PLIST_ADD);
			send_item_to_srv (item);
		}
		else {
			int added;
			
			added = plist_add_from_item (playlist, item);
			iface_add_to_plist (playlist, added);
		}
				
		/* Add to the server's playlist if the server has our
		 * playlist */
		if (get_server_plist_serial() == plist_get_serial(playlist)) {
			send_int_to_srv (CMD_LIST_ADD);
			send_str_to_srv (file);
		}
		send_int_to_srv (CMD_UNLOCK);
	}
	else
		error ("The file is already on the playlist.");

	iface_menu_key (KEY_CMD_MENU_DOWN);

	free (file);
}

/* Recursively add the content of a directory to the playlist. */
static void add_dir_plist ()
{
	struct plist plist;
	char *file;

	if (iface_in_plist_menu()) {
		error ("Can't add to the playlist a file from the "
				"playlist.");
		return;
	}

	file = iface_get_curr_file ();

	if (!file)
		return;
	
	if (iface_curritem_get_type() != F_DIR) {
		error ("This is not a directory.");
		free (file);
		return;
	}


	if (!strcmp(file, "../")) {
		error ("Can't add '..'.");
		free (file);
		return;
	}

	iface_set_status ("reading directories...");
	plist_init (&plist);
	read_directory_recurr (file, &plist);

	plist_sort_fname (&plist);
	
	send_int_to_srv (CMD_LOCK);

	plist_remove_common_items (&plist, playlist);

	/* Add the new files to the server's playlist if the server has our
	 * playlist */
	if (get_server_plist_serial() == plist_get_serial(playlist))
		send_playlist (&plist, 0);

	if (options_get_int("SyncPlaylist")) {
		iface_set_status ("Notifying clients...");
		send_items_to_clients (&plist);
		iface_set_status ("");
	}
	else {
		int i;
		
		switch_titles_file (&plist);
		if (get_tags_setting())
			ask_for_tags (&plist, get_tags_setting());

		for (i = 0; i < plist.num; i++)
			if (!plist_deleted(&plist, i))
				iface_add_to_plist (&plist, i);
		plist_cat (playlist, &plist);
	}

	send_int_to_srv (CMD_UNLOCK);

	plist_free (&plist);
	free (file);
}

static void toggle_option (const char *name)
{
	send_int_to_srv (CMD_SET_OPTION);
	send_str_to_srv (name);
	send_int_to_srv (!options_get_int(name));
	sync_int_option (name);
}

static void toggle_show_time ()
{
	if (!strcasecmp(options_get_str("ShowTime"), "yes")) {
		option_set_str("ShowTime", "IfAvailable");
		iface_set_status ("ShowTime: IfAvailable");
	}
	else if (!strcasecmp(options_get_str("ShowTime"), "no")) {
		option_set_str("ShowTime", "yes");
		iface_update_show_time ();
		ask_for_tags (dir_plist, TAGS_TIME);
		ask_for_tags (playlist, TAGS_TIME);
		iface_set_status ("ShowTime: yes");
		
	}
	else { /* IfAvailable */
		option_set_str("ShowTime", "no");
		iface_update_show_time ();
		iface_set_status ("ShowTime: no");
	}	
}

static void toggle_show_format ()
{
	int show_format = !options_get_int("ShowFormat");
	
	option_set_int ("ShowFormat", show_format);
	if (show_format)
		iface_set_status ("ShowFormat: yes");
	else
		iface_set_status ("ShowFormat: no");

	iface_update_show_format ();
}

/* Reread the directory. */
static void reread_dir ()
{
	go_to_dir (NULL, 1);
}

/* Clear the playlist on user request. */
static void cmd_clear_playlist ()
{
	if (options_get_int("SyncPlaylist")) {
		send_int_to_srv (CMD_LOCK);
		send_int_to_srv (CMD_CLI_PLIST_CLEAR);
		change_srv_plist_serial ();
		send_int_to_srv (CMD_UNLOCK);
	}
	else
		clear_playlist ();
}

static void go_to_music_dir ()
{
	if (options_get_str("MusicDir")) {
		if (file_type(options_get_str("MusicDir")) == F_DIR)
			go_to_dir (options_get_str("MusicDir"), 0);
		else if (file_type(options_get_str("MusicDir")) == F_PLAYLIST)
			go_to_playlist (options_get_str("MusicDir"));
		else
			error ("MusicDir is neither a directory nor a "
					"playlist.");
	}
	else
		error ("MusicDir not defined");
}

/* Make a directory from the string resolving ~, './' and '..'.
 * Return the directory, the memory is malloc()ed.
 * Return NULL on error. */
static char *make_dir (const char *str)
{
	char *dir;
	int add_slash = 0;

	dir = (char *)xmalloc (sizeof(char) * (PATH_MAX + 1));

	dir[PATH_MAX] = 0;

	/* If the string ends with a slash and is not just '/', add this
	 * slash. */
	if (strlen(str) > 1 && str[strlen(str)-1] == '/')
		add_slash = 1;
	
	if (str[0] == '~') {
		strncpy (dir, getenv("HOME"), PATH_MAX);
		
		if (dir[PATH_MAX]) {
			logit ("Path too long!");
			return NULL;
		}

		if (!strcmp(str, "~"))
			add_slash = 1;
		
		str++;
	}
	else if (str[0] != '/')
		strcpy (dir, cwd);
	else
		strcpy (dir, "/");

	resolve_path (dir, PATH_MAX, str);

	if (add_slash && strlen(dir) < PATH_MAX)
		strcat (dir, "/");

	return dir;
}

static void entry_key_go_dir (const struct iface_key *k)
{
	if (k->type == IFACE_KEY_CHAR && k->key.ucs == '\t') {
		char *dir;
		char *complete_dir;
		char buf[PATH_MAX+1];
		char *entry_text;

		entry_text = iface_entry_get_text ();		
		if (!(dir = make_dir(entry_text))) {
			free (entry_text);
			return;
		}
		free (entry_text);
		
		complete_dir = find_match_dir (dir);
		
		strncpy (buf, complete_dir ? complete_dir : dir, sizeof(buf));
		if (complete_dir)
			free (complete_dir);

		iface_entry_set_text (buf);
		free (dir);
	}
	else if (k->type == IFACE_KEY_CHAR && k->key.ucs == '\n') {
		char *entry_text = iface_entry_get_text ();
		
		if (entry_text[0]) {
			char *dir = make_dir (entry_text);

			iface_entry_history_add ();
			
			if (dir) {
				/* strip trailing slash */
				if (dir[strlen(dir)-1] == '/'
						&& strcmp(dir, "/"))
					dir[strlen(dir)-1] = 0;
				go_to_dir (dir, 0);
				free (dir);
			}
		}
		
		iface_entry_disable ();
		free (entry_text);
	}
	else
		iface_entry_handle_key (k);
}

/* Request playing from the specified URL. */
static void play_from_url (const char *url)
{
	send_int_to_srv (CMD_LOCK);

	change_srv_plist_serial ();
	send_int_to_srv (CMD_LIST_CLEAR);
	send_int_to_srv (CMD_LIST_ADD);
	send_str_to_srv (url);
	
	send_int_to_srv (CMD_PLAY);
	send_str_to_srv ("");

	send_int_to_srv (CMD_UNLOCK);
}

/* Return malloc()ed string that is a copy of str without leading and trailing
 * white spaces. */
static char *strip_white_spaces (const char *str)
{
	char *clean;
	int n;

	assert (str != NULL);

	n = strlen (str);

	/* Strip trailing */
	while (isblank(str[n-1]))
		n--;

	/* Strip leading */
	while (*str && isblank(*str)) {
		str++;
		n--;
	}

	if (n > 0) {
		clean = (char *)xmalloc ((n + 1) * sizeof(char));
		strncpy (clean, str, n);
		clean[n] = 0;
	}
	else
		clean = strdup ("");

	return clean;
}

static void entry_key_go_url (const struct iface_key *k)
{
	if (k->type == IFACE_KEY_CHAR && k->key.ucs == '\n') {
		char *entry_text = iface_entry_get_text ();
		
		if (entry_text[0]) {
			char *clean_url = strip_white_spaces (entry_text);
			
			iface_entry_history_add ();

			if (is_url(clean_url))
				play_from_url (clean_url);
			else
				error ("Not a valid URL.");

			free (clean_url);
		}

		free (entry_text);
		iface_entry_disable ();
	}
	else
		iface_entry_handle_key (k);
}

static void add_url_to_plist (const char *url)
{
	assert (url != NULL);
	
	if (plist_find_fname(playlist, url) == -1) {
		send_int_to_srv (CMD_LOCK);

		if (options_get_int("SyncPlaylist")) {
			struct plist_item *item = plist_new_item ();

			item->file = xstrdup (url);
			item->title_file = xstrdup (url);
			item->title = item->title_file;
			
			send_int_to_srv (CMD_CLI_PLIST_ADD);
			send_item_to_srv (item);

			plist_free_item_fields (item);
			free (item);
		}
		else {
			int added;
			
			added = plist_add (playlist, url);
			make_file_title (playlist, added, 0);
			iface_add_to_plist (playlist, added);
		}
				
		/* Add to the server's playlist if the server has our
		 * playlist */
		if (get_server_plist_serial() == plist_get_serial(playlist)) {
			send_int_to_srv (CMD_LIST_ADD);
			send_str_to_srv (url);
		}
		send_int_to_srv (CMD_UNLOCK);
	}
	else
		error ("URL already on the playlist");
}

static void entry_key_add_url (const struct iface_key *k)
{
	if (k->type == IFACE_KEY_CHAR && k->key.ucs == '\n') {
		char *entry_text = iface_entry_get_text ();
		
		if (entry_text[0]) {
			char *clean_url = strip_white_spaces (entry_text);
			
			iface_entry_history_add ();

			if (is_url(clean_url))
				add_url_to_plist (clean_url);
			else
				error ("Not a valid URL.");

			free (clean_url);
		}

		free (entry_text);
		iface_entry_disable ();
	}
	else
		iface_entry_handle_key (k);
}

static void entry_key_search (const struct iface_key *k)
{
	if (k->type == IFACE_KEY_CHAR && k->key.ucs == '\n') {
		char *file = iface_get_curr_file ();
		char *text = iface_entry_get_text ();
		
		iface_entry_disable ();
		
		if (text[0]) {
			if (is_url(file))
				play_from_url (file);
			else if (file_type(file) == F_DIR)
				go_to_dir (file, 0);
			else if (file_type(file) == F_PLAYLIST)
				go_to_playlist (file);
			else
				play_it (file);
		}

		free (text);
		free (file);
	}
	else
		iface_entry_handle_key (k);
}

static void save_playlist (const char *file, const char *cwd)
{
	iface_set_status ("Saving the playlist...");
	fill_tags (playlist, TAGS_COMMENTS | TAGS_TIME, 0);
	if (!user_wants_interrupt()) {
		if (plist_save(playlist, file, cwd))
			interface_message ("Playlist saved");
	}
	else
		iface_set_status ("Aborted");
	iface_set_status ("");
}

static void entry_key_plist_save (const struct iface_key *k)
{
	if (k->type == IFACE_KEY_CHAR && k->key.ucs == '\n') {
		char *text = iface_entry_get_text ();

		iface_entry_disable ();
		
		if (text[0]) {
			char *ext = ext_pos (text);
			char *file;

			/* add extension if necessary */
			if (!ext || strcmp(ext, "m3u")) {
				char *tmp = (char *)xmalloc((strlen(text) + 5) *
						sizeof(char));

				sprintf (tmp, "%s.m3u", text);
				free (text);
				text = tmp;
			}

			file = make_dir (text);

			if (file_exists(file)) {
				iface_make_entry (ENTRY_PLIST_OVERWRITE);
				iface_entry_set_file (file);
			}
			else {
				save_playlist (file, strchr(text, '/')
						? NULL : cwd);

				if (iface_in_dir_menu())
					reread_dir ();
			}

			free (file);
		}
			
		free (text);
	}
	else
		iface_entry_handle_key (k);
}

static void entry_key_plist_overwrite (const struct iface_key *k)
{
	if (k->type == IFACE_KEY_CHAR && toupper(k->key.ucs) == 'Y') {
		char *file = iface_entry_get_file ();

		assert (file != NULL);

		iface_entry_disable ();
		
		save_playlist (file, NULL); /* FIXME: not always NULL! */
		if (iface_in_dir_menu())
			reread_dir ();
		
		free (file);
	}
	else if (k->type == IFACE_KEY_CHAR && toupper(k->key.ucs) == 'N') {
		iface_entry_disable ();
		iface_message ("Not overwriting.");
	}
}

/* Handle keys while in an entry. */
static void entry_key (const struct iface_key *k)
{
	switch (iface_get_entry_type()) {
		case ENTRY_GO_DIR:
			entry_key_go_dir (k);
			break;
		case ENTRY_GO_URL:
			entry_key_go_url (k);
			break;
		case ENTRY_ADD_URL:
			entry_key_add_url (k);
			break;
		case ENTRY_SEARCH:
			entry_key_search (k);
			break;
		case ENTRY_PLIST_SAVE:
			entry_key_plist_save (k);
			break;
		case ENTRY_PLIST_OVERWRITE:
			entry_key_plist_overwrite (k);
			break;
		default:
			abort (); /* BUG */
	}
}

/* Update items in the menu for all items on the playlist. */
static void update_iface_menu (const struct plist *plist)
{
	int i;
	
	assert (plist != NULL);
	
	for (i = 0; i < plist->num; i++)
		if (!plist_deleted(plist, i))
			iface_update_item (plist, i);
}

/* Switch ReadTags options and update the menu. */
static void switch_read_tags ()
{
	if (options_get_int("ReadTags")) {
		option_set_int("ReadTags", 0);
		switch_titles_file (dir_plist);
		switch_titles_file (playlist);
		iface_set_status ("ReadTags: no");
	}
	else {
		option_set_int("ReadTags", 1);
		ask_for_tags (dir_plist, TAGS_COMMENTS);
		ask_for_tags (playlist, TAGS_COMMENTS);
		switch_titles_tags (dir_plist);
		switch_titles_tags (playlist);
		iface_set_status ("ReadTags: yes");
	}

	update_iface_menu (dir_plist);
	update_iface_menu (playlist);
}

static void seek (const int sec)
{
	send_int_to_srv (CMD_SEEK);
	send_int_to_srv (sec);
}

static void delete_item ()
{
	char *file;

	if (!iface_in_plist_menu()) {
		error ("You can only delete an item from the "
				"playlist.");
		return;
	}

	assert (plist_count(playlist) > 0);
	
	file = iface_get_curr_file ();
	
	send_int_to_srv (CMD_LOCK);
	
	if (options_get_int("SyncPlaylist")) {
		send_int_to_srv (CMD_CLI_PLIST_DEL);
		send_str_to_srv (file);
	}
	else {
		int n = plist_find_fname (playlist, file);

		assert (n != -1);

		plist_delete (playlist, n);
		iface_del_plist_item (file);

		if (plist_count(playlist) == 0)
			clear_playlist ();
	}

	/* Delete this item from the server's playlist if it has our
	 * playlist */
	if (get_server_plist_serial() == plist_get_serial(playlist)) {
		send_int_to_srv (CMD_DELETE);
		send_str_to_srv (file);
	}
	
	send_int_to_srv (CMD_UNLOCK);

	free (file);
}

/* Select the file that is currently played. */
static void go_to_playing_file ()
{
	if (curr_file.file && file_type(curr_file.file) == F_SOUND) {
		if (plist_find_fname(playlist, curr_file.file) != -1)
			iface_switch_to_plist ();
		else if (plist_find_fname(dir_plist,  curr_file.file) != -1)
			iface_switch_to_dir ();
		else {
			char *slash;
			char *file = xstrdup (curr_file.file);

			slash = strrchr (file, '/');
			assert (slash != NULL);
			*slash = 0;

			if (file[0])
				go_to_dir (file, 0);
			else
				go_to_dir ("/", 0);

			iface_switch_to_dir ();
			free (file);
		}
			
		iface_select_file (curr_file.file);
	}
}

/* Return the time like the standard time() function, but rounded i.e. if we
 * have 11.8 seconds, return 12 seconds. */
static time_t rounded_time ()
{
	struct timeval exact_time;
	time_t curr_time;

	if (gettimeofday(&exact_time, NULL) == -1)
		interface_fatal ("gettimeofday() failed: %s", strerror(errno));
	
	curr_time = exact_time.tv_sec;
	if (exact_time.tv_usec > 500000)
		curr_time++;

	return curr_time;
}

/* Handle silent seek key. */
static void seek_silent (const int sec)
{
	if (curr_file.state == STATE_PLAY && curr_file.file
			&& !is_url(curr_file.file)) {
		if (silent_seek_pos == -1) {
			silent_seek_pos = curr_file.curr_time + sec;
		}
		else
			silent_seek_pos += sec;

		if (silent_seek_pos < 0)
			silent_seek_pos = 0;
		else if (silent_seek_pos > curr_file.total_time)
			silent_seek_pos = curr_file.total_time;

		silent_seek_key_last = rounded_time ();
		iface_set_curr_time (silent_seek_pos);
	}
}

/* Move the current playlist item (direction: 1 - up, -1 - down). */
static void move_item (const int direction)
{
	char *file;
	int second;
	char *second_file;
	
	if (!iface_in_plist_menu()) {
		error ("You can move only playlist items.");
		return;
	}

	if (!(file = iface_get_curr_file()))
		return;

	second = plist_find_fname (playlist, file);
	assert (second != -1);

	if (direction == -1)
		second = plist_next (playlist, second);
	else if (direction == 1)
		second = plist_prev (playlist, second);
	else
		abort (); /* BUG */

	if (second == -1) {
		free (file);
		return;
	}
		
	second_file = plist_get_file (playlist, second);
	
	send_int_to_srv (CMD_LOCK);
	
	if (options_get_int("SyncPlaylist")) {
		send_int_to_srv (CMD_CLI_PLIST_MOVE);
		send_str_to_srv (file);
		send_str_to_srv (second_file);
	}
	else
		swap_playlist_items (file, second_file);

	/* update the server's playlist */
	if (get_server_plist_serial() == plist_get_serial(playlist)) {
		send_int_to_srv (CMD_LIST_MOVE);
		send_str_to_srv (file);
		send_str_to_srv (second_file);
	}

	send_int_to_srv (CMD_UNLOCK);
	
	free (second_file);
	free (file);
}

/* Handle releasing silent seek key. */
static void do_silent_seek ()
{
	time_t curr_time = time(NULL);
	
	if (silent_seek_pos != -1 && silent_seek_key_last < curr_time) {
		seek (silent_seek_pos - curr_file.curr_time - 1);
		silent_seek_pos = -1;
		iface_set_curr_time (curr_file.curr_time);
	}
}

/* Handle key */
static void menu_key (const struct iface_key *k)
{
	if (iface_in_help())
		iface_handle_help_key (k);
	else if (iface_in_entry())
		entry_key (k);
	else if (!iface_key_is_resize(k)) {
		enum key_cmd cmd = get_key_cmd (CON_MENU,
				k->type == IFACE_KEY_CHAR ? k->key.ucs :
				k->key.func);
		
		switch (cmd) {
			case KEY_CMD_QUIT_CLIENT:
				want_quit = QUIT_CLIENT;
				break;
			case KEY_CMD_GO:
				go_file ();
				break;
			case KEY_CMD_MENU_DOWN:
			case KEY_CMD_MENU_UP:
			case KEY_CMD_MENU_NPAGE:
			case KEY_CMD_MENU_PPAGE:
			case KEY_CMD_MENU_FIRST:
			case KEY_CMD_MENU_LAST:
				iface_menu_key (cmd);
				last_menu_move_time = time (NULL);
				break;
			case KEY_CMD_QUIT:
				want_quit = QUIT_SERVER;
				break;
			case KEY_CMD_STOP:
				send_int_to_srv (CMD_STOP);
				break;
			case KEY_CMD_NEXT:
				send_int_to_srv (CMD_NEXT);
				break;
			case KEY_CMD_PREVIOUS:
				send_int_to_srv (CMD_PREV);
				break;
			case KEY_CMD_PAUSE:
				switch_pause ();
				break;
			case KEY_CMD_TOGGLE_READ_TAGS:
				switch_read_tags ();
				break;
			case KEY_CMD_TOGGLE_SHUFFLE:
				toggle_option ("Shuffle");
				break;
			case KEY_CMD_TOGGLE_REPEAT:
				toggle_option ("Repeat");
				break;
			case KEY_CMD_TOGGLE_AUTO_NEXT:
				toggle_option ("AutoNext");
				break;
			case KEY_CMD_TOGGLE_MENU:
				toggle_menu ();
				break;
			case KEY_CMD_PLIST_ADD_FILE:
				add_file_plist ();
				break;
			case KEY_CMD_PLIST_CLEAR:
				cmd_clear_playlist ();
				break;
			case KEY_CMD_PLIST_ADD_DIR:
				add_dir_plist ();
				break;
			case KEY_CMD_MIXED_DEC_1:
				adjust_mixer (-1);
				break;
			case KEY_CMD_MIXER_DEC_5:
				adjust_mixer (-5);
				break;
			case KEY_CMD_MIXER_INC_5:
				adjust_mixer (+5);
				break;
			case KEY_CMD_MIXER_INC_1:
				adjust_mixer (+1);
				break;
			case KEY_CMD_SEEK_BACKWARD:
				seek (-options_get_int("SeekTime"));
				break;
			case KEY_CMD_SEEK_FORWARD:
				seek (options_get_int("SeekTime"));
				break;
			case KEY_CMD_HELP:
				iface_switch_to_help ();
				break;
			case KEY_CMD_HIDE_MESSAGE:
				iface_disable_message ();
				break;
			case KEY_CMD_REFRESH:
				iface_refresh ();
				break;
			case KEY_CMD_RELOAD:
				if (iface_in_dir_menu())
					reread_dir ();
				break;
			case KEY_CMD_TOGGLE_SHOW_HIDDEN_FILES:
				option_set_int ("ShowHiddenFiles",
						!options_get_int(
							"ShowHiddenFiles"));
				if (iface_in_dir_menu())
					reread_dir ();
				break;
			case KEY_CMD_GO_MUSIC_DIR:
				go_to_music_dir ();
				break;
			case KEY_CMD_PLIST_DEL:
				delete_item ();
				break;
			case KEY_CMD_MENU_SEARCH:
				iface_make_entry (ENTRY_SEARCH);
				break;
			case KEY_CMD_PLIST_SAVE:
				if (plist_count(playlist))
					iface_make_entry (ENTRY_PLIST_SAVE);
				else
					error ("The playlist is "
							"empty.");
				break;
			case KEY_CMD_TOGGLE_SHOW_TIME:
				toggle_show_time ();
				break;
			case KEY_CMD_TOGGLE_SHOW_FORMAT:
				toggle_show_format ();
				break;
			case KEY_CMD_GO_TO_PLAYING_FILE:
				go_to_playing_file ();
				break;
			case KEY_CMD_GO_DIR:
				iface_make_entry (ENTRY_GO_DIR);
				break;
			case KEY_CMD_GO_URL:
				iface_make_entry (ENTRY_GO_URL);
				break;
			case KEY_CMD_GO_DIR_UP:
				go_dir_up ();
				break;
			case KEY_CMD_WRONG:
				error ("Bad command");
				break;
			case KEY_CMD_SEEK_FORWARD_5:
				seek_silent (options_get_int("SilentSeekTime"));
				break;
			case KEY_CMD_SEEK_BACKWARD_5:
				seek_silent (-options_get_int(
							"SilentSeekTime"));
				break;
			case KEY_CMD_VOLUME_10:
				set_mixer (10);
				break;
			case KEY_CMD_VOLUME_20:
				set_mixer (20);
				break;
			case KEY_CMD_VOLUME_30:
				set_mixer (30);
				break;
			case KEY_CMD_VOLUME_40:
				set_mixer (40);
				break;
			case KEY_CMD_VOLUME_50:
				set_mixer (50);
				break;
			case KEY_CMD_VOLUME_60:
				set_mixer (60);
				break;
			case KEY_CMD_VOLUME_70:
				set_mixer (70);
				break;
			case KEY_CMD_VOLUME_80:
				set_mixer (80);
				break;
			case KEY_CMD_VOLUME_90:
				set_mixer (90);
				break;
			case KEY_CMD_FAST_DIR_1:
				if (options_get_str("FastDir1"))
					go_to_dir (options_get_str(
								"FastDir1"), 0);
				else
					error ("FastDir1 not "
							"defined");
				break;
			case KEY_CMD_FAST_DIR_2:
				if (options_get_str("FastDir2"))
					go_to_dir (options_get_str(
								"FastDir2"), 0);
				else
					error ("FastDir2 not "
							"defined");
				break;
			case KEY_CMD_FAST_DIR_3:
				if (options_get_str("FastDir3"))
					go_to_dir (options_get_str(
								"FastDir3"), 0);
				else
					error ("FastDir3 not "
							"defined");
				break;
			case KEY_CMD_FAST_DIR_4:
				if (options_get_str("FastDir4"))
					go_to_dir (options_get_str(
								"FastDir4"), 0);
				else
					error ("FastDir4 not "
							"defined");
				break;
			case KEY_CMD_FAST_DIR_5:
				if (options_get_str("FastDir5"))
					go_to_dir (options_get_str(
								"FastDir5"), 0);
				else
					error ("FastDir5 not "
							"defined");
				break;
			case KEY_CMD_FAST_DIR_6:
				if (options_get_str("FastDir6"))
					go_to_dir (options_get_str(
								"FastDir6"), 0);
				else
					error ("FastDir6 not "
							"defined");
				break;
			case KEY_CMD_FAST_DIR_7:
				if (options_get_str("FastDir7"))
					go_to_dir (options_get_str(
								"FastDir7"), 0);
				else
					error ("FastDir7 not "
							"defined");
				break;
			case KEY_CMD_FAST_DIR_8:
				if (options_get_str("FastDir8"))
					go_to_dir (options_get_str(
								"FastDir8"), 0);
				else
					error ("FastDir8 not "
							"defined");
				break;
			case KEY_CMD_FAST_DIR_9:
				if (options_get_str("FastDir9"))
					go_to_dir (options_get_str(
								"FastDir9"), 0);
				else
					error ("FastDir9 not "
							"defined");
				break;
			case KEY_CMD_FAST_DIR_10:
				if (options_get_str("FastDir10"))
					go_to_dir (options_get_str(
								"FastDir10"), 0);
				else
					error ("FastDir10 not "
							"defined");
				break;
			case KEY_CMD_TOGGLE_MIXER:
				debug ("Toggle mixer.");
				send_int_to_srv (CMD_TOGGLE_MIXER_CHANNEL);
				break;
			case KEY_CMD_TOGGLE_LAYOUT:
				iface_toggle_layout ();
				break;
			case KEY_CMD_PLIST_MOVE_UP:
				move_item (1);
				break;
			case KEY_CMD_PLIST_MOVE_DOWN:
				move_item (-1);
				break;
			case KEY_CMD_ADD_STREAM:
				iface_make_entry (ENTRY_ADD_URL);
				break;
			default:
				abort ();
		}
	}
}

/* Get event from the server and handle it. */
static void get_and_handle_event ()
{
	int type;

	if (!get_int_from_srv_noblock(&type)) {
		debug ("Getting event would block.");
		return;
	}

	server_event (type, get_event_data(type));
}

/* Handle events from the queue. */
static void dequeue_events ()
{
	struct event *e;
	
	debug ("Dequeuing events...");

	while ((e = event_get_first(&events))) {
		server_event (e->type, e->data);
		event_pop (&events);
	}

	debug ("done");
}

/* Actrion after CTRL-C was pressed. */
static void handle_interrupt ()
{
	if (iface_in_entry())
		iface_entry_disable ();
}

void interface_loop ()
{
	while (want_quit == NO_QUIT) {
		fd_set fds;
		int ret;
		struct timeval timeout = { 1, 0 };
		
		FD_ZERO (&fds);
		FD_SET (srv_sock, &fds);
		FD_SET (STDIN_FILENO, &fds);

		dequeue_events ();
		ret = select (srv_sock + 1, &fds, NULL, NULL, &timeout);

		iface_tick ();
		
		if (ret == 0)
			do_silent_seek ();
		else if (ret == -1 && !want_quit && errno != EINTR)
			interface_fatal ("select() failed: %s",
					strerror(errno));

#ifdef SIGWINCH
		if (want_resize)
			do_resize ();
#endif

		if (ret > 0) {
			if (FD_ISSET(STDIN_FILENO, &fds)) {
				struct iface_key k;
				
				iface_get_key (&k);

				clear_interrupt ();
				menu_key (&k);
			}

			if (!want_quit) {
				if (FD_ISSET(srv_sock, &fds))
					get_and_handle_event ();
				do_silent_seek ();
			}
		}
		else if (user_wants_interrupt())
			handle_interrupt ();

		if (!want_quit)
			update_mixer_value ();
	}
}

/* Save the current directory path to a file. */
static void save_curr_dir ()
{
	FILE *dir_file;

	if (!(dir_file = fopen(create_file_name("last_directory"), "w"))) {
		error ("Can't save current directory: %s",
				strerror(errno));
		return;
	}

	fprintf (dir_file, "%s", cwd);
	fclose (dir_file);
}

/* Save the playlist in .moc directory or remove the old playist if the
 * playlist is empty. */
static void save_playlist_in_moc ()
{
	char *plist_file = create_file_name("playlist.m3u");

	if (plist_count(playlist) && options_get_int("SavePlaylist"))
		save_playlist (plist_file, NULL);
	else
		unlink (plist_file);
}

void interface_end ()
{
	save_curr_dir ();
	save_playlist_in_moc ();
	if (want_quit == QUIT_SERVER)
		send_int_to_srv (CMD_QUIT);
	else
		send_int_to_srv (CMD_DISCONNECT);
	close (srv_sock);
	srv_sock = -1;
	
	windows_end ();
	keys_cleanup ();

	plist_free (dir_plist);
	plist_free (playlist);
	free (dir_plist);
	free (playlist);

	event_queue_free (&events);
	
	logit ("Interface exited");
}

void interface_fatal (const char *format, ...)
{
	char err_msg[512];
	va_list va;
	
	va_start (va, format);
	vsnprintf (err_msg, sizeof(err_msg), format, va);
	err_msg[sizeof(err_msg) - 1] = 0;
	va_end (va);

	logit ("FATAL ERROR: %s", err_msg);
	windows_end ();
	fatal ("%s", err_msg);
}

void interface_error (const char *msg)
{
	iface_error (msg);
}

void interface_cmdline_clear_plist (int server_sock)
{
	struct plist plist;
	int serial;
	srv_sock = server_sock; /* the interface is not initialized, so set it
				   here */

	plist_init (&plist);
	
	if (options_get_int("SyncPlaylist"))
		send_int_to_srv (CMD_CLI_PLIST_CLEAR);

	if (recv_server_plist(&plist) && plist_get_serial(&plist)
			== get_server_plist_serial()) {
		send_int_to_srv (CMD_LOCK);
		send_int_to_srv (CMD_GET_SERIAL);
		serial = get_data_int ();
		send_int_to_srv (CMD_PLIST_SET_SERIAL);
		send_int_to_srv (serial);
		send_int_to_srv (CMD_LIST_CLEAR);
	}
	send_int_to_srv (CMD_UNLOCK);
	
	unlink (create_file_name("playlist.m3u"));

	plist_free (&plist);
}

static void add_recursively (struct plist *plist, char **args,
		const int arg_num)
{
	int i;

	for (i = 0; i < arg_num; i++) {
		int dir;
		char path[PATH_MAX+1];

		if (!is_url(args[i]) && args[i][0] != '/') {
			if (args[0][0] == '/')
				strcpy (path, "/");
			else if (!getcwd(path, sizeof(path)))
				interface_fatal ("Can't get CWD: %s",
						strerror(errno));
			resolve_path (path, sizeof(path), args[i]);
		}
		else {
			strncpy (path, args[i], sizeof(path));
			path[sizeof(path)-1] = 0;
			resolve_path (path, sizeof(path), "");
		}
			
		dir = !is_url(path) && isdir(path);

		if (dir == 1)
			read_directory_recurr (path, plist);
		else if ((is_url(path) || is_sound_file(path))
				&& plist_find_fname(plist, path) == -1)
			plist_add (plist, path);
	}
}

void interface_cmdline_append (int server_sock, char **args,
		const int arg_num)
{
	srv_sock = server_sock; /* the interface is not initialized, so set it
				   here */

	if (options_get_int("SyncPlaylist")) {
		struct plist clients_plist;
		struct plist new;

		plist_init (&clients_plist);
		plist_init (&new);
		
		if (recv_server_plist(&clients_plist)) {
			add_recursively (&new, args, arg_num);
			plist_sort_fname (&new);
			
			send_int_to_srv (CMD_LOCK);

			plist_remove_common_items (&new, &clients_plist);
			send_items_to_clients (&new);

			if (get_server_plist_serial()
					== plist_get_serial(&clients_plist))
				send_playlist (&new, 0);
			send_int_to_srv (CMD_UNLOCK);
		}
		else {
			struct plist saved_plist;

			if (!getcwd(cwd, sizeof(cwd)))
				fatal ("Can't get CWD: %s.",
						strerror(errno));
			plist_init (&saved_plist);

			/* this checks if the file exists */
			if (file_type(create_file_name("playlist.m3u"))
						== F_PLAYLIST)
					plist_load (&saved_plist,
						create_file_name(
							"playlist.m3u"),
						cwd);
			add_recursively (&new, args, arg_num);
			plist_sort_fname (&new);

			send_int_to_srv (CMD_LOCK);
			plist_remove_common_items (&new, &saved_plist);
			send_playlist (&saved_plist, 0);
			send_int_to_srv (CMD_UNLOCK);

			plist_cat (&saved_plist, &new);
			if (options_get_int("SavePlaylist")) {
				fill_tags (&saved_plist, TAGS_COMMENTS
						| TAGS_TIME, 1);
				plist_save (&saved_plist,
						create_file_name(
							"playlist.m3u"),
						NULL);
			}

			plist_free (&saved_plist);
		}

		plist_free (&clients_plist);
		plist_free (&new);
	}
}

void interface_cmdline_play_first (int server_sock)
{
	struct plist plist;

	srv_sock = server_sock; /* the interface is not initialized, so set it
				   here */

	if (!getcwd(cwd, sizeof(cwd)))
		fatal ("Can't get CWD: %s.", strerror(errno));
	plist_init (&plist);
	
	send_int_to_srv (CMD_GET_SERIAL);
	plist_set_serial (&plist, get_data_int());

	/* the second condition will checks if the file exists */
	if (!recv_server_plist(&plist)
			&& file_type(create_file_name("playlist.m3u"))
			== F_PLAYLIST)
		plist_load (&plist, create_file_name("playlist.m3u"), cwd);
	
	send_int_to_srv (CMD_LOCK);
	if (get_server_plist_serial() != plist_get_serial(&plist)) {
		send_playlist (&plist, 1);
		send_int_to_srv (CMD_PLIST_SET_SERIAL);
		send_int_to_srv (plist_get_serial(&plist));
	}
	
	send_int_to_srv (CMD_PLAY);
	send_str_to_srv ("");

	plist_free (&plist);
}

/* Request tags from the server, wait until they arrive and return them
 * (malloc()ed). */
static struct file_tags *get_tags (const char *file, const int tags_sel)
{
	struct file_tags *tags = NULL;

	assert (file_type(file) == F_SOUND);
	
	send_tags_request (file, tags_sel);

	while (!tags) {
		int type = get_int_from_srv ();
		void *data = get_event_data (type);
		
		if (type == EV_FILE_TAGS) {
			struct tag_ev_response *ev
				= (struct tag_ev_response *)data;

			if (!strcmp(ev->file, file))
				tags = tags_dup (ev->tags);

			free_tag_ev_data (ev);
		}
		else {
			/* We can't handle other events, since this function
			 * is to be invoked without the interface. */
			logit ("Server send an event that I didn't extect!");
			abort ();
		}
	}

	return tags;
}

void interface_cmdline_file_info (const int server_sock)
{
	srv_sock = server_sock;	/* the interface is not initialized, so set it
				   here */
	init_playlists ();
	file_info_reset (&curr_file);
	
	curr_file.state = get_state ();
	
	if (curr_file.state == STATE_STOP)
		puts ("State: STOP");
	else {
		int left;
		char curr_time_str[6];
		char time_left_str[6];
		char time_str[6];
		char *title;
		
		if (curr_file.state == STATE_PLAY)
			puts ("State: PLAY");
		else if (curr_file.state == STATE_PAUSE)
			puts ("State: PAUSE");

		curr_file.file = get_curr_file ();

		if (curr_file.file[0]) {

			/* get tags */
			if (file_type(curr_file.file) == F_URL) {
				send_int_to_srv (CMD_GET_TAGS);
				curr_file.tags = get_data_tags ();
			}
			else
				curr_file.tags = get_tags (curr_file.file,
						TAGS_COMMENTS | TAGS_TIME);
			
			/* get the title */
			if (curr_file.tags->title)
				title = build_title (curr_file.tags);
			else
				title = xstrdup ("");
		}
		else
			title = xstrdup ("");

		curr_file.channels = get_channels ();
		curr_file.rate = get_rate ();
		curr_file.bitrate = get_bitrate ();
		curr_file.curr_time = get_curr_time ();

		if (curr_file.tags->time != -1)
			sec_to_min (time_str, curr_file.tags->time);
		else
			time_str[0] = 0;

		if (curr_file.curr_time != -1) {
			sec_to_min (curr_time_str, curr_file.curr_time);
		
			if (curr_file.tags->time != -1) {
				sec_to_min (curr_time_str, curr_file.curr_time);
				left = curr_file.tags->time -
					curr_file.curr_time;
				sec_to_min (time_left_str, left > 0 ? left : 0);
			}
		}
		else {
			strcpy (curr_time_str, "00:00");
			time_left_str[0] = 0;
		}

		printf ("File: %s\n", curr_file.file);
		printf ("Title: %s\n", title);

		if (curr_file.tags) {
			printf ("Artist: %s\n",
					curr_file.tags->artist
					? curr_file.tags->artist : "");
			printf ("SongTitle: %s\n",
					curr_file.tags->title
					? curr_file.tags->title : "");
			printf ("Album: %s\n",
					curr_file.tags->album
					? curr_file.tags->album : "");
		}

		if (curr_file.tags->time != -1) {
			printf ("TotalTime: %s\n", time_str);
			printf ("TimeLeft: %s\n", time_left_str);
			printf ("TotalSec: %d\n", curr_file.tags->time);
		}

		printf ("CurrentTime: %s\n", curr_time_str);
		printf ("CurrentSec: %d\n", curr_file.curr_time);

		printf ("Bitrate: %dKbps\n",
				curr_file.bitrate > 0 ? curr_file.bitrate : 0);
		printf ("Rate: %dKHz\n", curr_file.rate);
		
		file_info_cleanup (&curr_file);
		free (title);
	}

	plist_free (dir_plist);
	plist_free (playlist);
}

void interface_cmdline_playit (int server_sock, char **args, const int arg_num)
{
	struct plist plist;
	int i;

	srv_sock = server_sock; /* the interface is not initialized, so set it
				   here */

	plist_init (&plist);

	for (i = 0; i < arg_num; i++)
		if (is_url(args[i]) || is_sound_file(args[i]))
			plist_add (&plist, args[i]);

	if (plist_count(&plist)) {
		int serial;
		
		send_int_to_srv (CMD_LOCK);
		
		send_playlist (&plist, 1);
		
		send_int_to_srv (CMD_GET_SERIAL);
		serial = get_data_int ();
		send_int_to_srv (CMD_PLIST_SET_SERIAL);
		send_int_to_srv (serial);
		
		send_int_to_srv (CMD_UNLOCK);
		
		send_int_to_srv (CMD_PLAY);
		send_str_to_srv ("");
	}
	else
		fatal ("No files added - no sound files on command line.");

	plist_free (&plist);
}

void interface_cmdline_seek_by (int server_sock, const int seek_by)
{
	srv_sock = server_sock; /* the interface is not initialized, so set it
				   here */
	seek (seek_by);
}
