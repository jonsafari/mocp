/*
 * MOC - music on console
 * Copyright (C) 2004 - 2006 Damian Pietras <daper@daper.net>
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
#include <assert.h>
#include <string.h>
#include <strings.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <signal.h>
#include <ctype.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <dirent.h>
#include <sys/select.h>

#define DEBUG

#include "common.h"
#include "log.h"
#include "interface_elements.h"
#include "interface.h"
#include "lists.h"
#include "playlist.h"
#include "playlist_file.h"
#include "protocol.h"
#include "keys.h"
#include "options.h"
#include "files.h"
#include "decoder.h"
#include "themes.h"
#include "softmixer.h"
#include "utf8.h"

#define INTERFACE_LOG	"mocp_client_log"
#define PLAYLIST_FILE	"playlist.m3u"

#define QUEUE_CLEAR_THRESH 128

/* Socket of the server connection. */
static int srv_sock = -1;

static struct plist *playlist = NULL; /* our playlist */
static struct plist *queue = NULL; /* our queue */
static struct plist *dir_plist = NULL; /* contents of the current directory */

/* Queue for events coming from the server. */
static struct event_queue events;

/* Current working directory (the directory we show). */
static char cwd[PATH_MAX] = "";

/* If the user presses quit, or we receive a termination signal. */
static volatile enum want_quit want_quit = NO_QUIT;

/* If user presses CTRL-C, set this to 1.  This should interrupt long
 * operations which block the interface. */
static volatile int wants_interrupt = 0;

#ifdef SIGWINCH
/* If we get SIGWINCH. */
static volatile int want_resize = 0;
#endif

/* Are we waiting for the playlist we have loaded and sent to the clients? */
static int waiting_for_plist_load = 0;

/* Information about the currently played file. */
static struct file_info curr_file;

/* Silent seeking - where we are in seconds. -1 - no seeking. */
static int silent_seek_pos = -1;
static time_t silent_seek_key_last = (time_t)0; /* when the silent seek key was
						   last used */

/* When the menu was last moved (arrow keys, page up, etc.) */
static time_t last_menu_move_time = (time_t)0;

static void sig_quit (int sig LOGIT_ONLY)
{
	log_signal (sig);
	want_quit = QUIT_CLIENT;
}

static void sig_interrupt (int sig LOGIT_ONLY)
{
	log_signal (sig);
	wants_interrupt = 1;
}

#ifdef SIGWINCH
static void sig_winch (int sig LOGIT_ONLY)
{
	log_signal (sig);
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
		fatal ("Can't send() int to the server!");
}

static void send_bool_to_srv (const bool t)
{
	if (!send_int(srv_sock, t ? 1 : 0))
		fatal ("Can't send() bool to the server!");
}

static void send_str_to_srv (const char *str)
{
	if (!send_str(srv_sock, str))
		fatal ("Can't send() string to the server!");
}

static void send_item_to_srv (const struct plist_item *item)
{
	if (!send_item(srv_sock, item))
		fatal ("Can't send() item to the server!");
}

static int get_int_from_srv ()
{
	int num;

	if (!get_int(srv_sock, &num))
		fatal ("Can't receive value from the server!");

	return num;
}

static bool get_bool_from_srv ()
{
	int num;

	if (!get_int(srv_sock, &num))
		fatal ("Can't receive value from the server!");

	return num == 1 ? true : false;
}

/* Returned memory is malloc()ed. */
static char *get_str_from_srv ()
{
	char *str = get_str (srv_sock);

	if (!str)
		fatal ("Can't receive string from the server!");

	return str;
}

static struct file_tags *recv_tags_from_srv ()
{
	struct file_tags *tags = recv_tags (srv_sock);

	if (!tags)
		fatal ("Can't receive tags from the server!");

	return tags;
}

/* Noblocking version of get_int_from_srv(): return 0 if there are no data. */
static int get_int_from_srv_noblock (int *num)
{
	enum noblock_io_status st;

	if ((st = get_int_noblock(srv_sock, num)) == NB_IO_ERR)
		fatal ("Can't receive value from the server!");

	return st == NB_IO_OK ? 1 : 0;
}

static struct plist_item *recv_item_from_srv ()
{
	struct plist_item *item;

	if (!(item = recv_item(srv_sock)))
		fatal ("Can't receive item from the server!");

	return item;
}

static struct tag_ev_response *recv_tags_data_from_srv ()
{
	struct tag_ev_response *r;

	r = (struct tag_ev_response *)xmalloc (sizeof(struct tag_ev_response));

	r->file = get_str_from_srv ();
	if (!(r->tags = recv_tags(srv_sock)))
		fatal ("Can't receive tags event's data from the server!");

	return r;
}

static struct move_ev_data *recv_move_ev_data_from_srv ()
{
	struct move_ev_data *d;

	if (!(d = recv_move_ev_data(srv_sock)))
		fatal ("Can't receive move data from the server!");

	return d;
}

/* Receive data for the given type of event and return them. Return NULL if
 * there is no data for the event. */
static void *get_event_data (const int type)
{
	switch (type) {
		case EV_PLIST_ADD:
		case EV_QUEUE_ADD:
			return recv_item_from_srv ();
		case EV_PLIST_DEL:
		case EV_QUEUE_DEL:
		case EV_STATUS_MSG:
		case EV_SRV_ERROR:
			return get_str_from_srv ();
		case EV_FILE_TAGS:
			return recv_tags_data_from_srv ();
		case EV_PLIST_MOVE:
		case EV_QUEUE_MOVE:
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
		if (event == EV_EXIT)
			interface_fatal ("The server exited!");
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

/* Get a boolean value from the server that will arrive after EV_DATA. */
static bool get_data_bool ()
{
	wait_for_data ();
	return get_bool_from_srv ();
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

/* Send all items from this playlist to other clients. */
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
	queue = (struct plist *)xmalloc (sizeof(struct plist));
	plist_init (queue);

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

/* Initialise the block marker information. */
static void file_info_block_init (struct file_info *f)
{
	f->block_file = NULL;
}

/* Reset the block start and end markers. */
static void file_info_block_reset (struct file_info *f)
{
	if (f->block_file)
		free (f->block_file);
	f->block_file = NULL;
}

/* Enter the current time into a block start or end marker. */
static void file_info_block_mark (int *marker)
{
	if (curr_file.state == STATE_STOP) {
		error ("Cannot make block marks while stopped.");
	} else if (file_type (curr_file.file) == F_URL) {
		error ("Cannot make block marks in URLs.");
	} else if (file_type (curr_file.file) != F_SOUND) {
		error ("Cannot make block marks in non-audio files.");
	} else if (!curr_file.block_file) {
		error ("Cannot make block marks in files of unknown duration.");
	} else {
		*marker = curr_file.curr_time;
		iface_set_block (curr_file.block_start, curr_file.block_end);
	}
}

/* Get a boolean option from the server (like Shuffle) and set it. */
static void sync_bool_option (const char *name)
{
	bool value;

	send_int_to_srv (CMD_GET_OPTION);
	send_str_to_srv (name);
	value = get_data_bool ();
	options_set_bool (name, value);
	iface_set_option_state (name, value);
}

/* Get the server options and set our options like them. */
static void get_server_options ()
{
	sync_bool_option ("Shuffle");
	sync_bool_option ("Repeat");
	sync_bool_option ("AutoNext");
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

static int get_avg_bitrate ()
{
	send_int_to_srv (CMD_GET_AVG_BITRATE);
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
	int val;

	val = get_mixer_value ();
	iface_set_mixer_value (MAX(val, 0));
}

static void update_mixer_name ()
{
	char *name;

	send_int_to_srv (CMD_GET_MIXER_CHANNEL_NAME);
	name = get_data_str();
	debug ("Mixer name: %s", name);

	iface_set_mixer_name (name);

	free (name);

	update_mixer_value ();
}

/* Make new cwd path from CWD and this path. */
static void set_cwd (const char *path)
{
	if (path[0] == '/')
		strcpy (cwd, "/"); /* for absolute path */
	else if (!cwd[0]) {
		if (!getcwd(cwd, sizeof(cwd)))
			fatal ("Can't get CWD: %s", xstrerror (errno));
	}

	resolve_path (cwd, sizeof(cwd), path);
}

/* Try to find the directory we can start and set cwd to it. */
static void set_start_dir ()
{
	if (!getcwd(cwd, sizeof (cwd))) {
		if (errno == ERANGE)
			fatal ("CWD is larger than PATH_MAX!");
		strncpy (cwd, get_home (), sizeof (cwd));
		if (cwd[sizeof (cwd) - 1])
			fatal ("Home directory path is longer than PATH_MAX!");
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

/* Check if dir2 is in dir1. */
static int is_subdir (const char *dir1, const char *dir2)
{
	return !strncmp(dir1, dir2, strlen(dir1)) ? 1 : 0;
}

static int sort_strcmp_func (const void *a, const void *b)
{
	return strcoll (*(char **)a, *(char **)b);
}

static int sort_dirs_func (const void *a, const void *b)
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

	if (options_get_bool("ReadTags"))
		needed_tags |= TAGS_COMMENTS;
	if (!strcasecmp(options_get_symb("ShowTime"), "yes"))
		needed_tags |= TAGS_TIME;

	return needed_tags;
}

/* For each file in the playlist, send a request for all the given tags if
 * the file is missing any of those tags.  Return the number of requests. */
static int ask_for_tags (const struct plist *plist, const int tags_sel)
{
	int i;
	int req = 0;

	assert (plist != NULL);

	if (tags_sel != 0) {
		for (i = 0; i < plist->num; i++) {
			if (!plist_deleted(plist, i) &&
			    (!plist->items[i].tags ||
			     ~plist->items[i].tags->filled & tags_sel)) {
				char *file;

				file = plist_get_file (plist, i);
				req += send_tags_request (file, tags_sel);
				free (file);
			}
		}
	}

	return req;
}

static void interface_message (const char *format, ...)
{
	va_list va;
	char *msg;

	va_start (va, format);
	msg = format_msg_va (format, va);
	va_end (va);

	iface_message (msg);

	free (msg);
}

/* Update tags (and titles) for the given item on the playlist with new tags. */
static void update_item_tags (struct plist *plist, const int num,
		const struct file_tags *tags)
{
	struct file_tags *old_tags = plist_get_tags (plist, num);

	plist_set_tags (plist, num, tags);

	/* Get the time from the old tags if it's not present in the new tags.
	 * FIXME: There is risk, that the file was modified and the time
	 * from the old tags is not valid. */
	if (!(tags->filled & TAGS_TIME) && old_tags && old_tags->time != -1)
		plist_set_item_time (plist, num, old_tags->time);

	if (plist->items[num].title_tags) {
		free (plist->items[num].title_tags);
		plist->items[num].title_tags = NULL;
	}

	make_tags_title (plist, num);

	if (options_get_bool ("ReadTags") && !plist->items[num].title_tags) {
		if (!plist->items[num].title_file)
			make_file_title (plist, num,
					options_get_bool ("HideFileExtension"));
	}

	if (old_tags)
		tags_free (old_tags);
}

/* Truncate string at screen-upsetting whitespace. */
static void sanitise_string (char *str)
{
	if (!str)
		return;

	while (*str) {
		if (*str != ' ' && isspace (*str)) {
			*str = 0x00;
			break;
		}
		str++;
	}
}

/* Handle EV_FILE_TAGS. */
static void ev_file_tags (const struct tag_ev_response *data)
{
	int n;

	assert (data != NULL);
	assert (data->file != NULL);
	assert (data->tags != NULL);

	debug ("Received tags for %s", data->file);

	sanitise_string (data->tags->title);
	sanitise_string (data->tags->artist);
	sanitise_string (data->tags->album);

	if ((n = plist_find_fname(dir_plist, data->file)) != -1) {
		update_item_tags (dir_plist, n, data->tags);
		iface_update_item (IFACE_MENU_DIR, dir_plist, n);
	}

	if ((n = plist_find_fname(playlist, data->file)) != -1) {
		update_item_tags (playlist, n, data->tags);
		iface_update_item (IFACE_MENU_PLIST, playlist, n);
	}

	if (curr_file.file && !strcmp(data->file, curr_file.file)) {

		debug ("Tags apply to the currently played file.");

		if (data->tags->time != -1) {
			curr_file.total_time = data->tags->time;
			iface_set_total_time (curr_file.total_time);
			if (file_type (curr_file.file) == F_SOUND) {
				if (!curr_file.block_file) {
					curr_file.block_file = xstrdup(curr_file.file);
					curr_file.block_start = 0;
					curr_file.block_end = curr_file.total_time;
				}
				iface_set_block (curr_file.block_start, curr_file.block_end);
			}
		}
		else
			debug ("No time information");

		if (data->tags->title) {
			if (curr_file.title)
				free (curr_file.title);
			curr_file.title = build_title (data->tags);
			iface_set_played_file_title (curr_file.title);
		}

		if (curr_file.tags)
			tags_free (curr_file.tags);
		curr_file.tags = tags_dup (data->tags);
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
			if (curr_file.title)
				free (curr_file.title);
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
		else
			logit ("Not my playlist.");
	}
}

static void update_curr_file ()
{
	char *file;

	file = get_curr_file ();

	if (!file[0] || curr_file.state == STATE_STOP) {

		/* Nothing is played/paused. */

		file_info_cleanup (&curr_file);
		file_info_reset (&curr_file);
		iface_set_played_file (NULL);
		iface_load_lyrics (NULL);
		free (file);
	}
	else if (file[0] &&
			(!curr_file.file || strcmp(file, curr_file.file))) {

		/* played file has changed */

		file_info_cleanup (&curr_file);
		if (curr_file.block_file && strcmp(file, curr_file.block_file))
			file_info_block_reset (&curr_file);

		/* The total time could not get reset. */
		iface_set_total_time (-1);

		iface_set_played_file (file);
		send_tags_request (file, TAGS_COMMENTS | TAGS_TIME);
		curr_file.file = file;

		/* make a title that will be used until we get tags */
		if (file_type(file) == F_URL || !strchr(file, '/')) {
			curr_file.title = xstrdup (file);
			update_curr_tags ();
		}
		else
		{
			if (options_get_bool ("FileNamesIconv"))
			{
				curr_file.title = files_iconv_str (
					strrchr(file, '/') + 1);
			}
			else
			{
				curr_file.title = xstrdup (
					strrchr(file, '/') + 1);
			}
		}

		iface_set_played_file (file);
		iface_set_played_file_title (curr_file.title);
		/* Try to load the lyrics of the new file. */
		iface_load_lyrics (file);
		/* Silent seeking makes no sense if the playing file has changed. */
		silent_seek_pos = -1;
		iface_set_curr_time (curr_file.curr_time);

		if (options_get_bool("FollowPlayedFile"))
			follow_curr_file ();
	}
	else
		free (file);
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
		int i;

		if (options_get_bool("ReadTags")
				&& (!item->tags || !item->tags->title))
			needed_tags |= TAGS_COMMENTS;
		if (!strcasecmp(options_get_symb("ShowTime"), "yes")
				&& (!item->tags || item->tags->time == -1))
			needed_tags |= TAGS_TIME;

		if (needed_tags)
			send_tags_request (item->file, needed_tags);

		if (options_get_bool ("ReadTags"))
			make_tags_title (playlist, item_num);
		else
			make_file_title (playlist, item_num,
					options_get_bool ("HideFileExtension"));

		/* Just calling iface_update_queue_positions (queue, playlist,
		 * NULL, NULL) is too slow in cases when we receive a large
		 * number of items from server (e.g., loading playlist w/
		 * SyncPlaylist on).  Since we know the filename in question,
		 * we try to find it in queue and eventually update the value.
		 */
		if ((i = plist_find_fname(queue, item->file)) != -1) {
			playlist->items[item_num].queue_pos
				= plist_get_position (queue, i);
		}

		iface_add_to_plist (playlist, item_num);

		if (waiting_for_plist_load) {
			if (iface_in_dir_menu())
				iface_switch_to_plist ();
			waiting_for_plist_load = 0;
		}
	}
}

/* Handle EV_QUEUE_ADD. */
static void event_queue_add (const struct plist_item *item)
{
	if (plist_find_fname(queue, item->file) == -1) {
		plist_add_from_item (queue, item);
		iface_set_files_in_queue (plist_count(queue));
		iface_update_queue_position_last (queue, playlist, dir_plist);
		logit ("Adding %s to queue", item->file);
	}
	else
		logit ("Adding file already present in queue");
}

/* Get error message from the server and show it. */
static void update_error (char *err)
{
	error ("%s", err);
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

static void recv_server_queue (struct plist *queue)
{
	int end_of_list = 0;
	struct plist_item *item;

	logit ("Asking server for the queue.");
	send_int_to_srv (CMD_GET_QUEUE);
	logit ("Waiting for response");
	wait_for_data (); /* There must always be (possibly empty) queue. */

	do {
		item = recv_item_from_srv ();
		if (item->file[0])
			plist_add_from_item (queue, item);
		else
			end_of_list = 1;
		plist_free_item_fields (item);
		free (item);
	} while (!end_of_list);
}

/* Clear the playlist locally. */
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

static void clear_queue ()
{
	iface_clear_queue_positions (queue, playlist, dir_plist);

	plist_clear (queue);
	iface_set_files_in_queue (0);

	interface_message ("The queue was cleared.");
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

/* Handle EV_QUEUE_DEL. */
static void event_queue_del (char *file)
{
	int item = plist_find_fname (queue, file);

	if (item != -1) {
		plist_delete (queue, item);

		/* Free the deleted items occasionally.
		 * QUEUE_CLEAR_THRESH is chosen to be two times
		 * the initial size of the playlist. */
		if (plist_count(queue) == 0
				&& queue->num >= QUEUE_CLEAR_THRESH)
			plist_clear (queue);

		iface_set_files_in_queue (plist_count(queue));
		iface_update_queue_positions (queue, playlist, dir_plist, file);
		logit ("Deleting %s from queue", file);
	}
	else
		logit ("Deleting an item not present in the queue");

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

/* Handle EV_QUEUE_MOVE. */
static void event_queue_move (const struct move_ev_data *d)
{
	assert (d != NULL);
	assert (d->from != NULL);
	assert (d->to != NULL);

	plist_swap_files (queue, d->from, d->to);
}

/* Handle server event. */
static void server_event (const int event, void *data)
{
	logit ("EVENT: 0x%02x", event);

	switch (event) {
		case EV_BUSY:
			interface_fatal ("The server is busy; "
			                 "too many other clients are connected!");
			break;
		case EV_CTIME:
			update_ctime ();
			break;
		case EV_STATE:
			update_state ();
			break;
		case EV_EXIT:
			interface_fatal ("The server exited!");
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
			update_error ((char *)data);
			break;
		case EV_OPTIONS:
			get_server_options ();
			break;
		case EV_SEND_PLIST:
			forward_playlist ();
			break;
		case EV_PLIST_ADD:
			if (options_get_bool("SyncPlaylist"))
				event_plist_add ((struct plist_item *)data);
			break;
		case EV_PLIST_CLEAR:
			if (options_get_bool("SyncPlaylist"))
				clear_playlist ();
			break;
		case EV_PLIST_DEL:
			if (options_get_bool("SyncPlaylist"))
				event_plist_del ((char *)data);
			break;
		case EV_PLIST_MOVE:
			if (options_get_bool("SyncPlaylist"))
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
		case EV_AVG_BITRATE:
			curr_file.avg_bitrate = get_avg_bitrate ();
			break;
		case EV_QUEUE_ADD:
			event_queue_add ((struct plist_item *)data);
			break;
		case EV_QUEUE_DEL:
			event_queue_del ((char *)data);
			break;
		case EV_QUEUE_CLEAR:
			clear_queue ();
			break;
		case EV_QUEUE_MOVE:
			event_queue_move ((struct move_ev_data *)data);
			break;
		case EV_AUDIO_START:
			break;
		case EV_AUDIO_STOP:
			break;
		default:
			interface_fatal ("Unknown event: 0x%02x!", event);
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
		int type;
		void *data;

		/* Event queue is not initialized if there is no interface. */
		if (!no_iface && !event_queue_empty (&events)) {
			struct event e = *event_get_first (&events);

			type = e.type;
			data = e.data;

			event_pop (&events);

		}
		else {
			type = get_int_from_srv ();
			data = get_event_data (type);
		}

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
			abort (); /* can't handle other events without the interface */

		if (!no_iface)
			server_event (type, data);
		else
			free_event_data (type, data);
	}

	iface_set_status ("");
}

/* Load the directory content into dir_plist and switch the menu to it.
 * If dir is NULL, go to the cwd.  If reload is not zero, we are reloading
 * the current directory, so use iface_update_dir_content().
 * Return 1 on success, 0 on error. */
static int go_to_dir (const char *dir, const int reload)
{
	struct plist *old_dir_plist;
	char last_dir[PATH_MAX];
	const char *new_dir = dir ? dir : cwd;
	int going_up = 0;
	lists_t_strs *dirs, *playlists;

	iface_set_status ("Reading directory...");

	if (dir && is_subdir(dir, cwd)) {
		strcpy (last_dir, strrchr(cwd, '/') + 1);
		strcat (last_dir, "/");
		going_up = 1;
	}

	old_dir_plist = dir_plist;
	dir_plist = (struct plist *)xmalloc (sizeof(struct plist));
	plist_init (dir_plist);
	dirs = lists_strs_new (FILES_LIST_INIT_SIZE);
	playlists = lists_strs_new (FILES_LIST_INIT_SIZE);

	if (!read_directory(new_dir, dirs, playlists, dir_plist)) {
		iface_set_status ("");
		plist_free (dir_plist);
		lists_strs_free (dirs);
		lists_strs_free (playlists);
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
	lists_strs_sort (dirs, sort_dirs_func);
	lists_strs_sort (playlists, sort_strcmp_func);

	ask_for_tags (dir_plist, get_tags_setting());

	if (reload)
		iface_update_dir_content (IFACE_MENU_DIR, dir_plist, dirs, playlists);
	else
		iface_set_dir_content (IFACE_MENU_DIR, dir_plist, dirs, playlists);
	lists_strs_free (dirs);
	lists_strs_free (playlists);
	if (going_up)
		iface_set_curr_item_title (last_dir);

	iface_set_title (IFACE_MENU_DIR, cwd);
	iface_update_queue_positions (queue, NULL, dir_plist, NULL);

	if (iface_in_plist_menu())
		iface_switch_to_dir ();

	return 1;
}

/* Make sure that the server's playlist has different serial from ours. */
static void change_srv_plist_serial ()
{
	int serial;

	do {
		send_int_to_srv (CMD_GET_SERIAL);
		serial = get_data_int ();
	 } while (serial == plist_get_serial(playlist) ||
	          serial == plist_get_serial(dir_plist));

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
static int go_to_playlist (const char *file, const int load_serial,
                           bool default_playlist)
{
	if (plist_count(playlist)) {
		error ("Please clear the playlist, because "
				"I'm not sure you want to do this.");
		return 0;
	}

	plist_clear (playlist);

	iface_set_status ("Loading playlist...");
	if (plist_load(playlist, file, cwd, load_serial)) {

		if (options_get_bool("SyncPlaylist")) {
			send_int_to_srv (CMD_LOCK);
			if (!load_serial)
				change_srv_plist_serial ();
			send_int_to_srv (CMD_CLI_PLIST_CLEAR);
			iface_set_status ("Notifying clients...");
			send_items_to_clients (playlist);
			iface_set_status ("");
			waiting_for_plist_load = 1;
			send_int_to_srv (CMD_UNLOCK);

			/* We'll use the playlist received from the
			 * server to be synchronized with other clients.
			 */
			plist_clear (playlist);
		}
		else {
			if (!default_playlist)
				toggle_menu ();
			iface_set_dir_content (IFACE_MENU_PLIST, playlist, NULL, NULL);
			iface_update_queue_positions (queue, playlist, NULL, NULL);
		}

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

	if (options_get_bool("StartInMusicDir")) {
		char *music_dir;

		if ((music_dir = options_get_str("MusicDir"))) {
			set_cwd (music_dir);
			if (first_run && file_type(music_dir) == F_PLAYLIST
					&& plist_count(playlist) == 0
					&& go_to_playlist(music_dir, 0, false)) {
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
			interface_fatal ("Can't enter any directory!");
	}

	first_run = 0;
}

/* Request the playlist from the server (given by another client).  Make
 * the titles.  Return 0 if such a list doesn't exist. */
static int get_server_playlist (struct plist *plist)
{
	iface_set_status ("Getting the playlist...");
	debug ("Getting the playlist...");
	if (recv_server_plist(plist)) {
		ask_for_tags (plist, get_tags_setting());
		if (options_get_bool ("ReadTags"))
			switch_titles_tags (plist);
		else
			switch_titles_file (plist);
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
		iface_update_queue_positions (queue, playlist, NULL, NULL);
		return 1;
	}

	return 0;
}

static void use_server_queue ()
{
	iface_set_status ("Getting the queue...");
	debug ("Getting the queue...");

	recv_server_queue(queue);
	iface_set_files_in_queue (plist_count(queue));
	iface_update_queue_positions (queue, playlist, dir_plist, NULL);
	iface_set_status ("");
}

/* Process a single directory argument. */
static void process_dir_arg (const char *dir)
{
	set_cwd (dir);
	if (!go_to_dir (NULL, 0))
		enter_first_dir ();
}

/* Process a single playlist argument. */
static void process_plist_arg (const char *file)
{
	char path[PATH_MAX + 1];   /* the playlist's directory */
	char *slash;

	if (file[0] == '/')
		strcpy (path, "/");
	else if (!getcwd (path, sizeof (path)))
		interface_fatal ("Can't get CWD: %s", xstrerror (errno));

	resolve_path (path, sizeof (path), file);
	slash = strrchr (path, '/');
	assert (slash != NULL);
	*slash = 0;

	iface_set_status ("Loading playlist...");
	plist_load (playlist, file, path, 0);
	iface_set_status ("");
}

/* Process a list of arguments. */
static void process_multiple_args (lists_t_strs *args)
{
	int size, ix;
	const char *arg;
	char this_cwd[PATH_MAX];

	if (!getcwd (this_cwd, sizeof (cwd)))
		interface_fatal ("Can't get CWD: %s", xstrerror (errno));

	size = lists_strs_size (args);

	for (ix = 0; ix < size; ix += 1) {
		int dir;
		char path[2 * PATH_MAX];

		arg = lists_strs_at (args, ix);
		dir = is_dir (arg);

		if (is_url (arg)) {
			strncpy (path, arg, sizeof (path));
			path[sizeof(path) - 1] = 0;
		}
		else {
			if (arg[0] == '/')
				strcpy (path, "/");
			else
				strcpy (path, this_cwd);
			resolve_path (path, sizeof (path), arg);
		}

		if (dir == 1)
			read_directory_recurr (path, playlist);
		else if (!dir && (is_sound_file (path) || is_url (path))) {
			if (plist_find_fname (playlist, path) == -1)
				plist_add (playlist, path);
		}
		else if (is_plist_file (path)) {
			char *plist_dir, *slash;

			/* Here we've chosen to resolve the playlist's relative paths
			 * with respect to the directory of the playlist (or of the
			 * symlink being used to reference it).  If some other base is
			 * desired, then we probably need to introduce a new option. */

			plist_dir = xstrdup (path);
			slash = strrchr (plist_dir, '/');
			assert (slash != NULL);
			*slash = 0;

			plist_load (playlist, path, plist_dir, 0);

			free (plist_dir);
		}
	}
}

/* Process file names passed as arguments. */
static void process_args (lists_t_strs *args)
{
	int size;
	const char *arg;

	size = lists_strs_size (args);
	arg = lists_strs_at (args, 0);

	if (size == 1 && is_dir (arg) == 1) {
		process_dir_arg (arg);
		return;
	}

	if (size == 1 && is_plist_file (arg))
		process_plist_arg (arg);
	else
		process_multiple_args (args);

	if (plist_count (playlist) && !options_get_bool ("SyncPlaylist")) {
		switch_titles_file (playlist);
		ask_for_tags (playlist, get_tags_setting ());
		iface_set_dir_content (IFACE_MENU_PLIST, playlist, NULL, NULL);
		iface_update_queue_positions (queue, playlist, NULL, NULL);
		iface_switch_to_plist ();
	}
	else
		enter_first_dir ();
}

/* Load the playlist from .moc directory. */
static void load_playlist ()
{
	char *plist_file = create_file_name (PLAYLIST_FILE);

	if (file_type(plist_file) == F_PLAYLIST) {
		go_to_playlist (plist_file, 1, true);

		/* We don't want to switch to the playlist after loading. */
		waiting_for_plist_load = 0;
	}
}

#ifdef SIGWINCH
/* Handle resizing xterm. */
static void do_resize ()
{
	iface_resize ();
	logit ("resize");
	want_resize = 0;
}
#endif

/* Strip the last directory from the path. Returned memory is mallod()ed. */
static char *dir_up (const char *path)
{
	char *slash;
	char *dir;

	assert (path != NULL);

	dir = xstrdup (path);
	slash = strrchr (dir, '/');
	assert (slash != NULL);
	if (slash == dir)
		*(slash + 1) = 0;
	else
		*slash = 0;

	return dir;
}

static void go_dir_up ()
{
	char *dir;

	dir = dir_up (cwd);
	go_to_dir (dir, 0);
	free (dir);
}

/* Return a generated playlist serial from the server and make sure
 * it's not the same as our playlist's serial. */
static int get_safe_serial ()
{
	int serial;

	do {
		send_int_to_srv (CMD_GET_SERIAL);
		serial = get_data_int ();
	} while (playlist &&
			serial == plist_get_serial(playlist)); /* check only the
							   playlist, because dir_plist has serial
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
		go_to_playlist (file, 0, false);

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
	val = CLAMP(0, val, 100);
	send_int_to_srv (CMD_SET_MIXER);
	send_int_to_srv (val);
}

static void adjust_mixer (const int diff)
{
	set_mixer (get_mixer_value() + diff);
}

/* Recursively add the content of a directory to the playlist. */
static void add_dir_plist ()
{
	struct plist plist;
	char *file;
	enum file_type type;

	if (iface_in_plist_menu()) {
		error ("Can't add to the playlist a file from the playlist.");
		return;
	}

	file = iface_get_curr_file ();

	if (!file)
		return;

	type = iface_curritem_get_type ();
	if (type != F_DIR && type != F_PLAYLIST) {
		error ("This is not a directory or a playlist.");
		free (file);
		return;
	}

	if (!strcmp(file, "..")) {
		error ("Can't add '..'.");
		free (file);
		return;
	}

	iface_set_status ("Reading directories...");
	plist_init (&plist);

	if (type == F_DIR) {
		read_directory_recurr (file, &plist);
		plist_sort_fname (&plist);
	}
	else
		plist_load (&plist, file, cwd, 0);

	send_int_to_srv (CMD_LOCK);

	plist_remove_common_items (&plist, playlist);

	/* Add the new files to the server's playlist if the server has our
	 * playlist. */
	if (get_server_plist_serial() == plist_get_serial(playlist))
		send_playlist (&plist, 0);

	if (options_get_bool("SyncPlaylist")) {
		iface_set_status ("Notifying clients...");
		send_items_to_clients (&plist);
		iface_set_status ("");
	}
	else {
		int i;

		switch_titles_file (&plist);
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

/* To avoid lots of locks and unlocks, this assumes a lock is sent before
 * the first call and an unlock after the last.
 *
 * It's also assumed to be in the menu.
 */
static void remove_file_from_playlist (const char *file)
{
	assert (file != NULL);
	assert (plist_count(playlist) > 0);

	if (options_get_bool("SyncPlaylist")) {
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
	 * playlist. */
	if (get_server_plist_serial() == plist_get_serial(playlist)) {
		send_int_to_srv (CMD_DELETE);
		send_str_to_srv (file);
	}
}

/* Remove all dead entries (point to non-existent or unreadable). */
static void remove_dead_entries_plist ()
{
	const char *file = NULL;
	int i;

	if (! iface_in_plist_menu()) {
		error ("Can't prune when not in the playlist.");
		return;
	}

	send_int_to_srv (CMD_LOCK);
	for (i = 0, file = plist_get_next_dead_entry(playlist, &i);
	     file != NULL;
	     file = plist_get_next_dead_entry(playlist, &i)) {
		remove_file_from_playlist (file);
	}
	send_int_to_srv (CMD_UNLOCK);
}

/* Add the currently selected file to the playlist. */
static void add_file_plist ()
{
	char *file;

	if (iface_in_plist_menu()) {
		error ("Can't add to the playlist a file from the playlist.");
		return;
	}

	if (iface_curritem_get_type() == F_DIR) {
		add_dir_plist();
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

		if (options_get_bool("SyncPlaylist")) {
			send_int_to_srv (CMD_CLI_PLIST_ADD);
			send_item_to_srv (item);
		}
		else {
			int added;

			added = plist_add_from_item (playlist, item);
			iface_add_to_plist (playlist, added);
		}

		/* Add to the server's playlist if the server has our
		 * playlist. */
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

static void queue_toggle_file ()
{
	char *file;

	file = iface_get_curr_file ();

	if (!file)
		return;

	if (iface_curritem_get_type() != F_SOUND
			&& iface_curritem_get_type() != F_URL) {
		error ("You can only add a file or URL using this command.");
		free (file);
		return;
	}

	/* Check if the file is already in the queue; if it isn't, add it,
	 * otherwise, remove it. */

	if (plist_find_fname(queue, file) == -1) {
		/* Add item to the server's queue. */
		send_int_to_srv (CMD_QUEUE_ADD);
		send_str_to_srv (file);

		logit ("Added to queue: %s", file);
	}
	else {
		/* Delete this item from the server's queue. */
		send_int_to_srv (CMD_QUEUE_DEL);
		send_str_to_srv (file);

		logit ("Removed from queue: %s", file);
	}

	iface_menu_key (KEY_CMD_MENU_DOWN);

	free (file);
}

static void toggle_option (const char *name)
{
	send_int_to_srv (CMD_SET_OPTION);
	send_str_to_srv (name);
	send_bool_to_srv (!options_get_bool(name));
	sync_bool_option (name);
}

static void toggle_show_time ()
{
	if (!strcasecmp (options_get_symb ("ShowTime"), "yes")) {
		options_set_symb ("ShowTime", "IfAvailable");
		iface_set_status ("ShowTime: IfAvailable");
	}
	else if (!strcasecmp (options_get_symb ("ShowTime"), "no")) {
		options_set_symb ("ShowTime", "yes");
		iface_update_show_time ();
		ask_for_tags (dir_plist, TAGS_TIME);
		ask_for_tags (playlist, TAGS_TIME);
		iface_set_status ("ShowTime: yes");

	}
	else { /* IfAvailable */
		options_set_symb ("ShowTime", "no");
		iface_update_show_time ();
		iface_set_status ("ShowTime: no");
	}
}

static void toggle_show_format ()
{
	bool show_format = !options_get_bool("ShowFormat");

	options_set_bool ("ShowFormat", show_format);
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
	if (options_get_bool("SyncPlaylist")) {
		send_int_to_srv (CMD_LOCK);
		send_int_to_srv (CMD_CLI_PLIST_CLEAR);
		change_srv_plist_serial ();
		send_int_to_srv (CMD_UNLOCK);
	}
	else
		clear_playlist ();
}

static void cmd_clear_queue ()
{
	send_int_to_srv (CMD_QUEUE_CLEAR);
}

static void go_to_music_dir ()
{
	const char *musicdir_optn;
	char music_dir[PATH_MAX] = "/";

	musicdir_optn = options_get_str ("MusicDir");

	if (!musicdir_optn) {
		error ("MusicDir not defined");
		return;
	}

	resolve_path (music_dir, sizeof(music_dir), musicdir_optn);

	switch (file_type (music_dir)) {
	case F_DIR:
		go_to_dir (music_dir, 0);
		break;
	case F_PLAYLIST:
		go_to_playlist (music_dir, 0, false);
		break;
	default:
		error ("MusicDir is neither a directory nor a playlist!");
	}
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
		strncpy (dir, get_home (), PATH_MAX);

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

	/* Strip trailing. */
	while (n > 0 && isblank(str[n-1]))
		n--;

	/* Strip leading whitespace. */
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
		clean = xstrdup ("");

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

		if (options_get_bool("SyncPlaylist")) {
			struct plist_item *item = plist_new_item ();

			item->file = xstrdup (url);
			item->title_file = xstrdup (url);

			send_int_to_srv (CMD_CLI_PLIST_ADD);
			send_item_to_srv (item);

			plist_free_item_fields (item);
			free (item);
		}
		else {
			int added;

			added = plist_add (playlist, url);
			make_file_title (playlist, added, false);
			iface_add_to_plist (playlist, added);
		}

		/* Add to the server's playlist if the server has our
		 * playlist. */
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

			if (!strcmp(file, "..")) {
				free (file);
				file = dir_up (cwd);
			}

			if (is_url(file))
				play_from_url (file);
			else if (file_type(file) == F_DIR)
				go_to_dir (file, 0);
			else if (file_type(file) == F_PLAYLIST)
				go_to_playlist (file, 0, false);
			else
				play_it (file);
		}

		free (text);
		free (file);
	}
	else
		iface_entry_handle_key (k);
}

static void save_playlist (const char *file, const char *cwd,
		const int save_serial)
{
	iface_set_status ("Saving the playlist...");
	fill_tags (playlist, TAGS_COMMENTS | TAGS_TIME, 0);
	if (!user_wants_interrupt()) {
		if (plist_save(playlist, file, cwd, save_serial))
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
						? NULL : cwd, 0);

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

		save_playlist (file, NULL, 0); /* FIXME: not always NULL! */
		if (iface_in_dir_menu())
			reread_dir ();

		free (file);
	}
	else if (k->type == IFACE_KEY_CHAR && toupper(k->key.ucs) == 'N') {
		iface_entry_disable ();
		iface_message ("Not overwriting.");
	}
}

static void entry_key_user_query (const struct iface_key *k)
{
	if (k->type == IFACE_KEY_CHAR && k->key.ucs == '\n') {
		char *entry_text = iface_entry_get_text ();
		iface_entry_disable ();
		iface_user_reply (entry_text);
		free (entry_text);
	}
	else
		iface_entry_handle_key (k);
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
		case ENTRY_USER_QUERY:
			entry_key_user_query (k);
			break;
		default:
			abort (); /* BUG */
	}
}

/* Update items in the menu for all items on the playlist. */
static void update_iface_menu (const enum iface_menu menu,
		const struct plist *plist)
{
	int i;

	assert (plist != NULL);

	for (i = 0; i < plist->num; i++)
		if (!plist_deleted(plist, i))
			iface_update_item (menu, plist, i);
}

/* Switch ReadTags options and update the menu. */
static void switch_read_tags ()
{
	if (options_get_bool ("ReadTags")) {
		options_set_bool ("ReadTags", false);
		switch_titles_file (dir_plist);
		switch_titles_file (playlist);
		iface_set_status ("ReadTags: no");
	}
	else {
		options_set_bool ("ReadTags", true);
		ask_for_tags (dir_plist, TAGS_COMMENTS);
		ask_for_tags (playlist, TAGS_COMMENTS);
		switch_titles_tags (dir_plist);
		switch_titles_tags (playlist);
		iface_set_status ("ReadTags: yes");
	}

	update_iface_menu (IFACE_MENU_DIR, dir_plist);
	update_iface_menu (IFACE_MENU_PLIST, playlist);
}

static void seek (const int sec)
{
	send_int_to_srv (CMD_SEEK);
	send_int_to_srv (sec);
}

static void jump_to (const int sec)
{
	send_int_to_srv (CMD_JUMP_TO);
	send_int_to_srv (sec);
}

static void delete_item ()
{
	char *file;

	if (!iface_in_plist_menu()) {
		error ("You can only delete an item from the playlist.");
		return;
	}

	assert (plist_count(playlist) > 0);

	file = iface_get_curr_file ();

	send_int_to_srv (CMD_LOCK);
	remove_file_from_playlist (file);
	send_int_to_srv (CMD_UNLOCK);

	free (file);
}

/* Select the file that is currently played. */
static void go_to_playing_file ()
{
	if (curr_file.file && file_type(curr_file.file) == F_SOUND) {
		if (plist_find_fname(playlist, curr_file.file) != -1)
			iface_switch_to_plist ();
		else if (plist_find_fname(dir_plist, curr_file.file) != -1)
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
	struct timespec exact_time;
	time_t curr_time;

	if (clock_gettime (CLOCK_REALTIME, &exact_time) == -1)
		interface_fatal ("clock_gettime() failed: %s", xstrerror (errno));

	curr_time = exact_time.tv_sec;
	if (exact_time.tv_nsec > 500000000L)
		curr_time += 1;

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

		silent_seek_pos = CLAMP(0, silent_seek_pos, curr_file.total_time);

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

	if (options_get_bool("SyncPlaylist")) {
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

/* Handle the 'next' command. */
static void cmd_next ()
{
	if (curr_file.state != STATE_STOP)
		send_int_to_srv (CMD_NEXT);
	else if (plist_count(playlist)) {
		if (plist_get_serial(playlist) != -1
				|| get_server_plist_serial()
				!= plist_get_serial(playlist)) {
			int serial;

			send_int_to_srv (CMD_LOCK);

			send_playlist (playlist, 1);
			serial = get_safe_serial();
			plist_set_serial (playlist, serial);
			send_int_to_srv (CMD_PLIST_SET_SERIAL);
			send_int_to_srv (plist_get_serial(playlist));

			send_int_to_srv (CMD_UNLOCK);
		}

		send_int_to_srv (CMD_PLAY);
		send_str_to_srv ("");
	}
}

/* Add themes found in the directory to the list of theme files. */
static void add_themes_to_list (lists_t_strs *themes, const char *themes_dir)
{
	DIR *dir;
	struct dirent *entry;

	assert (themes);
	assert (themes_dir);

	if (!(dir = opendir (themes_dir))) {
		char *err = xstrerror (errno);
		logit ("Can't open themes directory %s: %s", themes_dir, err);
		free (err);
		return;
	}

	while ((entry = readdir(dir))) {
		int rc;
		char file[PATH_MAX];

		if (entry->d_name[0] == '.')
			continue;

		/* Filter out backup files (*~). */
		if (entry->d_name[strlen(entry->d_name)-1] == '~')
			continue;

		rc = snprintf(file, sizeof(file), "%s/%s", themes_dir, entry->d_name);
		if (rc >= ssizeof(file))
			continue;

		lists_strs_append (themes, file);
	}

	closedir (dir);
}

/* Compare two pathnames based on filename. */
static int themes_cmp (const void *a, const void *b)
{
	int result;
	char *sa = *(char **)a;
	char *sb = *(char **)b;

	result = strcoll (strrchr (sa, '/') + 1, strrchr (sb, '/') + 1);
	if (result == 0)
		result = strcoll (sa, sb);

	return result;
}

/* Add themes found in the directories to the theme selection menu.
 * Return the number of items added. */
static int add_themes_to_menu (const char *user_themes,
                               const char *system_themes)
{
	int ix;
	lists_t_strs *themes;

	assert (user_themes);
	assert (system_themes);

	themes = lists_strs_new (16);
	add_themes_to_list (themes, user_themes);
	add_themes_to_list (themes, system_themes);
	lists_strs_sort (themes, themes_cmp);

	for (ix = 0; ix < lists_strs_size (themes); ix += 1) {
		char *file;

		file = lists_strs_at (themes, ix);
		iface_add_file (file, strrchr (file, '/') + 1, F_THEME);
	}

	lists_strs_free (themes);

	return ix;
}

static void make_theme_menu ()
{
	iface_switch_to_theme_menu ();

	if (add_themes_to_menu (create_file_name ("themes"),
	                        SYSTEM_THEMES_DIR) == 0) {
		if (!cwd[0])
			enter_first_dir (); /* we were at the playlist from the startup */
		else
			iface_switch_to_dir ();

		error ("No themes found.");
	}

	iface_update_theme_selection (get_current_theme ());
	iface_refresh ();
}

/* Use theme from the currently selected file. */
static void use_theme ()
{
	char *file = iface_get_curr_file ();

	assert (iface_curritem_get_type() == F_THEME);

	if (!file)
		return;

	themes_switch_theme (file);
	iface_update_attrs ();
	iface_refresh ();

	free (file);
}

/* Handle keys while in the theme menu. */
static void theme_menu_key (const struct iface_key *k)
{
	if (!iface_key_is_resize(k)) {
		enum key_cmd cmd = get_key_cmd (CON_MENU, k);

		switch (cmd) {
			case KEY_CMD_GO:
				use_theme ();
				break;
			case KEY_CMD_MENU_DOWN:
			case KEY_CMD_MENU_UP:
			case KEY_CMD_MENU_NPAGE:
			case KEY_CMD_MENU_PPAGE:
			case KEY_CMD_MENU_FIRST:
			case KEY_CMD_MENU_LAST:
				iface_menu_key (cmd);
				break;
			default:
				iface_switch_to_dir ();
				logit ("Bad key");
		}
	}
}

/* Make sure that we have tags and a title for this file which is in a menu. */
static void make_sure_tags_exist (const char *file)
{
	struct plist *plist;
	int item_num;

	if (file_type(file) != F_SOUND)
		return;

	if ((item_num = plist_find_fname(dir_plist, file)) != -1)
		plist = dir_plist;
	else if ((item_num = plist_find_fname(playlist, file)) != -1)
		plist = playlist;
	else
		return;

	if (!plist->items[item_num].tags
			|| plist->items[item_num].tags->filled
				!= (TAGS_COMMENTS | TAGS_TIME)) {
		int got_it = 0;

		send_tags_request (file, TAGS_COMMENTS | TAGS_TIME);

		while (!got_it) {
			int type = get_int_from_srv ();
			void *data = get_event_data (type);

			if (type == EV_FILE_TAGS) {
				struct tag_ev_response *ev
					= (struct tag_ev_response *)data;

				if (!strcmp(ev->file, file))
					got_it = 1;
			}

			server_event (type, data);
		}
	}
}

/* Request tags from the server for a file in the playlist or the directory
 * menu, wait until they arrive and return them (malloc()ed). */
static struct file_tags *get_tags (const char *file)
{
	struct plist *plist;
	int item_num;

	make_sure_tags_exist (file);

	if ((item_num = plist_find_fname(dir_plist, file)) != -1)
		plist = dir_plist;
	else if ((item_num = plist_find_fname(playlist, file)) != -1)
		plist = playlist;
	else
		return tags_new ();

	if (file_type(file) == F_SOUND)
		return tags_dup (plist->items[item_num].tags);

	return tags_new ();
}

/* Get the title of a file (malloc()ed) that is present in a menu. */
static char *get_title (const char *file)
{
	struct plist *plist;
	int item_num;

	make_sure_tags_exist (file);

	if ((item_num = plist_find_fname(dir_plist, file)) != -1)
		plist = dir_plist;
	else if ((item_num = plist_find_fname(playlist, file)) != -1)
		plist = playlist;
	else
		return NULL;

	return xstrdup (plist->items[item_num].title_tags
			? plist->items[item_num].title_tags
			: plist->items[item_num].title_file);
}

/* Substitute arguments for custom command that begin with '%'.
 * The new value is returned. */
static char *custom_cmd_substitute (const char *arg)
{
	char *result = NULL;
	char *file = NULL;
	struct file_tags *tags = NULL;

	if (strlen (arg) == 2 && arg[0] == '%') {
		switch (arg[1]) {
		case 'i':
			file = iface_get_curr_file ();
			result = get_title (file);
			break;
		case 't':
			file = iface_get_curr_file ();
			tags = get_tags (file);
			result = xstrdup (tags->title);
			break;
		case 'a':
			file = iface_get_curr_file ();
			tags = get_tags (file);
			result = xstrdup (tags->album);
			break;
		case 'r':
			file = iface_get_curr_file ();
			tags = get_tags (file);
			result = xstrdup (tags->artist);
			break;
		case 'n':
			file = iface_get_curr_file ();
			tags = get_tags (file);
			result = (char *) xmalloc (sizeof (char) * 10);
			snprintf (result, 10, "%d", tags->track);
			break;
		case 'm':
			file = iface_get_curr_file ();
			tags = get_tags (file);
			result = (char *) xmalloc (sizeof (char) * 10);
			snprintf (result, 10, "%d", tags->time);
			break;
		case 'f':
			result = iface_get_curr_file ();
			break;
		case 'I':
			result = xstrdup (curr_file.title);
			break;
		case 'T':
			if (curr_file.tags && curr_file.tags->title)
				result = xstrdup (curr_file.tags->title);
			break;
		case 'A':
			if (curr_file.tags && curr_file.tags->album)
				result = xstrdup (curr_file.tags->album);
			break;
		case 'R':
			if (curr_file.tags && curr_file.tags->artist)
				result = xstrdup (curr_file.tags->artist);
			break;
		case 'N':
			if (curr_file.tags && curr_file.tags->track != -1) {
				result = (char *) xmalloc (sizeof (char) * 10);
				snprintf (result, 10, "%d", curr_file.tags->track);
			}
			break;
		case 'M':
			if (curr_file.tags && curr_file.tags->time != -1) {
				result = (char *) xmalloc (sizeof (char) * 10);
				snprintf (result, 10, "%d", curr_file.tags->time);
			}
			break;
		case 'F':
			if (curr_file.file)
				result = xstrdup (curr_file.file);
			break;
		case 'S':
			if (curr_file.file && curr_file.block_file) {
				result = (char *) xmalloc (sizeof (char) * 10);
				snprintf (result, 10, "%d", curr_file.block_start);
			}
			break;
		case 'E':
			if (curr_file.file && curr_file.block_file) {
				result = (char *) xmalloc (sizeof (char) * 10);
				snprintf (result, 10, "%d", curr_file.block_end);
			}
			break;
		default:
			result = xstrdup (arg);
		}
	}
	else
		result = xstrdup (arg);

	/* Replace nonexisting data with an empty string. */
	if (!result)
		result = xstrdup ("");

	free (file);
	if (tags)
		tags_free (tags);

	return result;
}

static void run_external_cmd (char **args, const int arg_num ASSERT_ONLY)
{
	pid_t child;

	assert (args != NULL);
	assert (arg_num >= 1);
	assert (args[0] != NULL);
	assert (args[arg_num] == NULL);

	iface_temporary_exit ();

	child = fork();
	if (child == -1)
		error_errno ("fork() failed", errno);
	else {
		int status;

		if (child == 0) { /* I'm a child. */
			char *err;

			putchar ('\n');
			execvp (args[0], args);

			/* We have an error. */
			err = xstrerror (errno);
			fprintf (stderr, "\nError executing %s: %s\n", args[0], err);
			free (err);
			xsleep (2, 1);
			exit (EXIT_FAILURE);
		}

		/* parent */
		waitpid (child, &status, 0);
		if (WIFEXITED(status) && WEXITSTATUS(status) != 0) {
			fprintf (stderr, "\nCommand exited with error (status %d).\n",
			                 WEXITSTATUS(status));
			xsleep (2, 1);
		}
		iface_restore ();
	}
}

/* Exec (execvp()) a custom command (ExecCommand[1-10] options). */
static void exec_custom_command (const char *option)
{
	char *cmd, *arg;
	char **args;
	int ix, arg_num;
	lists_t_strs *cmd_list, *arg_list;

	assert (option != NULL);

	cmd = options_get_str (option);
	if (!cmd || !cmd[0]) {
		error ("%s is not set", option);
		return;
	}

	/* Split into arguments. */
	cmd_list = lists_strs_new (4);
	arg_num = lists_strs_tokenise (cmd_list, cmd);
	if (arg_num == 0) {
		error ("Malformed %s option", option);
		lists_strs_free (cmd_list);
		return;
	}

	arg_list = lists_strs_new (lists_strs_size (cmd_list));
	for (ix = 0; ix < arg_num; ix += 1) {
		arg = custom_cmd_substitute (lists_strs_at (cmd_list, ix));
		lists_strs_push (arg_list, arg);
	}
	lists_strs_free (cmd_list);

	cmd = lists_strs_fmt (arg_list, " %s");
	logit ("Running command:%s", cmd);
	free (cmd);

	args = lists_strs_save (arg_list);
	lists_strs_free (arg_list);
	run_external_cmd (args, arg_num);
	free (args);

	if (iface_in_dir_menu())
		reread_dir ();
}

static void go_to_fast_dir (const int num)
{
	char option_name[20];

	assert (RANGE(1, num, 10));

	sprintf (option_name, "FastDir%d", num);

	if (options_get_str(option_name)) {
		char dir[PATH_MAX] = "/";

		resolve_path (dir, sizeof(dir), options_get_str(option_name));
		go_to_dir (dir, 0);
	}
	else
		error ("%s is not defined", option_name);
}

static void toggle_playlist_full_paths (void)
{
	bool new_val = !options_get_bool ("PlaylistFullPaths");

	options_set_bool ("PlaylistFullPaths", new_val);

	if (new_val)
		iface_set_status ("PlaylistFullPaths: on");
	else
		iface_set_status ("PlaylistFullPaths: off");

	update_iface_menu (IFACE_MENU_PLIST, playlist);
}

/* Handle key. */
static void menu_key (const struct iface_key *k)
{
	if (iface_in_help ())
		iface_handle_help_key (k);
	else if (iface_in_lyrics ())
		iface_handle_lyrics_key (k);
	else if (iface_in_entry ())
		entry_key (k);
	else if (iface_in_theme_menu ())
		theme_menu_key (k);
	else if (!iface_key_is_resize (k)) {
		enum key_cmd cmd = get_key_cmd (CON_MENU, k);

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
				cmd_next ();
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
			case KEY_CMD_TOGGLE_PLAYLIST_FULL_PATHS:
				toggle_playlist_full_paths ();
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
			case KEY_CMD_PLIST_REMOVE_DEAD_ENTRIES:
				remove_dead_entries_plist ();
				break;
			case KEY_CMD_MIXER_DEC_1:
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
				seek (-options_get_int ("SeekTime"));
				break;
			case KEY_CMD_SEEK_FORWARD:
				seek (options_get_int ("SeekTime"));
				break;
			case KEY_CMD_HELP:
				iface_switch_to_help ();
				break;
			case KEY_CMD_LYRICS:
				iface_switch_to_lyrics ();
				break;
			case KEY_CMD_HIDE_MESSAGE:
				iface_disable_message ();
				break;
			case KEY_CMD_REFRESH:
				iface_refresh ();
				break;
			case KEY_CMD_RELOAD:
				if (iface_in_dir_menu ())
					reread_dir ();
				break;
			case KEY_CMD_TOGGLE_SHOW_HIDDEN_FILES:
				options_set_bool ("ShowHiddenFiles",
				                  !options_get_bool ("ShowHiddenFiles"));
				if (iface_in_dir_menu ())
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
				if (plist_count (playlist))
					iface_make_entry (ENTRY_PLIST_SAVE);
				else
					error ("The playlist is empty.");
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
				seek_silent (options_get_int ("SilentSeekTime"));
				break;
			case KEY_CMD_SEEK_BACKWARD_5:
				seek_silent (-options_get_int ("SilentSeekTime"));
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
			case KEY_CMD_MARK_START:
				file_info_block_mark (&curr_file.block_start);
				break;
			case KEY_CMD_MARK_END:
				file_info_block_mark (&curr_file.block_end);
				break;
			case KEY_CMD_FAST_DIR_1:
				go_to_fast_dir (1);
				break;
			case KEY_CMD_FAST_DIR_2:
				go_to_fast_dir (2);
				break;
			case KEY_CMD_FAST_DIR_3:
				go_to_fast_dir (3);
				break;
			case KEY_CMD_FAST_DIR_4:
				go_to_fast_dir (4);
				break;
			case KEY_CMD_FAST_DIR_5:
				go_to_fast_dir (5);
				break;
			case KEY_CMD_FAST_DIR_6:
				go_to_fast_dir (6);
				break;
			case KEY_CMD_FAST_DIR_7:
				go_to_fast_dir (7);
				break;
			case KEY_CMD_FAST_DIR_8:
				go_to_fast_dir (8);
				break;
			case KEY_CMD_FAST_DIR_9:
				go_to_fast_dir (9);
				break;
			case KEY_CMD_FAST_DIR_10:
				go_to_fast_dir (10);
				break;
			case KEY_CMD_TOGGLE_MIXER:
				debug ("Toggle mixer.");
				send_int_to_srv (CMD_TOGGLE_MIXER_CHANNEL);
				break;
			case KEY_CMD_TOGGLE_SOFTMIXER:
				debug ("Toggle softmixer.");
				send_int_to_srv (CMD_TOGGLE_SOFTMIXER);
				break;
			case KEY_CMD_TOGGLE_EQUALIZER:
				debug ("Toggle equalizer.");
				send_int_to_srv (CMD_TOGGLE_EQUALIZER);
				break;
			case KEY_CMD_EQUALIZER_REFRESH:
				debug ("Equalizer Refresh.");
				send_int_to_srv (CMD_EQUALIZER_REFRESH);
				break;
			case KEY_CMD_EQUALIZER_PREV:
				debug ("Equalizer Prev.");
				send_int_to_srv (CMD_EQUALIZER_PREV);
				break;
			case KEY_CMD_EQUALIZER_NEXT:
				debug ("Equalizer Next.");
				send_int_to_srv (CMD_EQUALIZER_NEXT);
				break;
			case KEY_CMD_TOGGLE_MAKE_MONO:
				debug ("Toggle Mono-Mixing.");
				send_int_to_srv (CMD_TOGGLE_MAKE_MONO);
				break;
			case KEY_CMD_TOGGLE_LAYOUT:
				iface_toggle_layout ();
				break;
			case KEY_CMD_TOGGLE_PERCENT:
				iface_toggle_percent ();
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
			case KEY_CMD_THEME_MENU:
				make_theme_menu ();
				break;
			case KEY_CMD_EXEC1:
				exec_custom_command ("ExecCommand1");
				break;
			case KEY_CMD_EXEC2:
				exec_custom_command ("ExecCommand2");
				break;
			case KEY_CMD_EXEC3:
				exec_custom_command ("ExecCommand3");
				break;
			case KEY_CMD_EXEC4:
				exec_custom_command ("ExecCommand4");
				break;
			case KEY_CMD_EXEC5:
				exec_custom_command ("ExecCommand5");
				break;
			case KEY_CMD_EXEC6:
				exec_custom_command ("ExecCommand6");
				break;
			case KEY_CMD_EXEC7:
				exec_custom_command ("ExecCommand7");
				break;
			case KEY_CMD_EXEC8:
				exec_custom_command ("ExecCommand8");
				break;
			case KEY_CMD_EXEC9:
				exec_custom_command ("ExecCommand9");
				break;
			case KEY_CMD_EXEC10:
				exec_custom_command ("ExecCommand10");
				break;
			case KEY_CMD_QUEUE_TOGGLE_FILE:
				queue_toggle_file ();
				break;
			case KEY_CMD_QUEUE_CLEAR:
				cmd_clear_queue ();
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

/* Action after CTRL-C was pressed. */
static void handle_interrupt ()
{
	if (iface_in_entry())
		iface_entry_disable ();
}

void init_interface (const int sock, const int logging, lists_t_strs *args)
{
	FILE *logfp;

	logit ("Starting MOC Interface");

	logfp = NULL;
	if (logging) {
		logfp = fopen (INTERFACE_LOG, "a");
		if (!logfp)
			fatal ("Can't open client log file: %s", xstrerror (errno));
	}
	log_init_stream (logfp, INTERFACE_LOG);

	/* Set locale according to the environment variables. */
	if (!setlocale(LC_CTYPE, ""))
		logit ("Could not set locale!");

	srv_sock = sock;

	file_info_reset (&curr_file);
	file_info_block_init (&curr_file);
	init_playlists ();
	event_queue_init (&events);
	keys_init ();
	windows_init ();
	get_server_options ();
	update_mixer_name ();

	xsignal (SIGQUIT, sig_quit);
	xsignal (SIGTERM, sig_quit);
	xsignal (SIGHUP, sig_quit);
	xsignal (SIGINT, sig_interrupt);

#ifdef SIGWINCH
	xsignal (SIGWINCH, sig_winch);
#endif

	if (!lists_strs_empty (args)) {
		process_args (args);

		if (plist_count(playlist) == 0) {
			if (!options_get_bool("SyncPlaylist") || !use_server_playlist())
				load_playlist ();
			send_int_to_srv (CMD_SEND_PLIST_EVENTS);
		}
		else if (options_get_bool("SyncPlaylist")) {
			struct plist tmp_plist;

			/* We have made the playlist from command line. */

			/* The playlist should be now clear, but this will give
			 * us the serial number of the playlist used by other
			 * clients. */
			plist_init (&tmp_plist);
			get_server_playlist (&tmp_plist);

			send_int_to_srv (CMD_SEND_PLIST_EVENTS);

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
			options_set_bool ("StartInMusicDir", false);
		}
	}
	else {
		send_int_to_srv (CMD_SEND_PLIST_EVENTS);
		if (!options_get_bool("SyncPlaylist") || !use_server_playlist())
			load_playlist ();
		enter_first_dir ();
	}

	/* Ask the server for queue. */
	use_server_queue ();

	if (options_get_bool("SyncPlaylist"))
		send_int_to_srv (CMD_CAN_SEND_PLIST);

	update_state ();

	if (options_get_bool("CanStartInPlaylist")
			&& curr_file.file
			&& plist_find_fname(playlist, curr_file.file) != -1)
		iface_switch_to_plist ();
}

void interface_loop ()
{
	log_circular_start ();

	while (want_quit == NO_QUIT) {
		fd_set fds;
		int ret;
		struct timespec timeout = { 1, 0 };

		FD_ZERO (&fds);
		FD_SET (srv_sock, &fds);
		FD_SET (STDIN_FILENO, &fds);

		dequeue_events ();
		ret = pselect (srv_sock + 1, &fds, NULL, NULL, &timeout, NULL);
		if (ret == -1 && !want_quit && errno != EINTR)
			interface_fatal ("pselect() failed: %s", xstrerror (errno));

		iface_tick ();

		if (ret == 0)
			do_silent_seek ();

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

	log_circular_log ();
	log_circular_stop ();
}

/* Save the current directory path to a file. */
static void save_curr_dir ()
{
	FILE *dir_file;

	if (!(dir_file = fopen(create_file_name("last_directory"), "w"))) {
		error_errno ("Can't save current directory", errno);
		return;
	}

	fprintf (dir_file, "%s", cwd);
	fclose (dir_file);
}

/* Save the playlist in .moc directory or remove the old playist if the
 * playlist is empty. */
static void save_playlist_in_moc ()
{
	char *plist_file = create_file_name (PLAYLIST_FILE);

	if (plist_count(playlist) && options_get_bool("SavePlaylist"))
		save_playlist (plist_file, NULL, 1);
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
	srv_sock = -1;

	windows_end ();
	keys_cleanup ();

	plist_free (dir_plist);
	plist_free (playlist);
	plist_free (queue);
	free (dir_plist);
	free (playlist);
	free (queue);

	event_queue_free (&events);

	logit ("Interface exited");

	log_close ();
}

void interface_fatal (const char *format, ...)
{
	char *msg;
	va_list va;

	va_start (va, format);
	msg = format_msg_va (format, va);
	va_end (va);

	windows_end ();
	fatal ("%s", msg);
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

	if (options_get_bool("SyncPlaylist"))
		send_int_to_srv (CMD_CLI_PLIST_CLEAR);

	if (recv_server_plist(&plist) && plist_get_serial(&plist)
			== get_server_plist_serial()) {
		send_int_to_srv (CMD_LOCK);
		send_int_to_srv (CMD_GET_SERIAL);
		serial = get_data_int ();
		send_int_to_srv (CMD_PLIST_SET_SERIAL);
		send_int_to_srv (serial);
		send_int_to_srv (CMD_LIST_CLEAR);
		send_int_to_srv (CMD_UNLOCK);
	}

	unlink (create_file_name (PLAYLIST_FILE));

	plist_free (&plist);
}

static void add_recursively (struct plist *plist, lists_t_strs *args)
{
	int ix;

	for (ix = 0; ix < lists_strs_size (args); ix += 1) {
		int dir;
		char path[PATH_MAX + 1];
		const char *arg;

		arg = lists_strs_at (args, ix);

		if (!is_url (arg) && arg[0] != '/') {
			if (arg[0] == '/')
				strcpy (path, "/");
			else {
				strncpy (path, cwd, sizeof (path));
				path[sizeof (path) - 1] = 0;
			}
			resolve_path (path, sizeof (path), arg);
		}
		else {
			strncpy (path, arg, sizeof (path));
			path[sizeof (path) - 1] = 0;

			if (!is_url (arg))
				resolve_path (path, sizeof (path), "");
		}

		dir = is_dir (path);

		if (dir == 1)
			read_directory_recurr (path, plist);
		else if (is_plist_file (arg))
			plist_load (plist, arg, cwd, 0);
		else if ((is_url (path) || is_sound_file (path))
				&& plist_find_fname (plist, path) == -1) {
			int added = plist_add (plist, path);

			if (is_url (path))
				make_file_title (plist, added, false);
		}
	}
}

void interface_cmdline_append (int server_sock, lists_t_strs *args)
{
	srv_sock = server_sock; /* the interface is not initialized, so set it
				   here */

	if (options_get_bool("SyncPlaylist")) {
		struct plist clients_plist;
		struct plist new;

		plist_init (&clients_plist);
		plist_init (&new);

		if (!getcwd(cwd, sizeof(cwd)))
			fatal ("Can't get CWD: %s", xstrerror (errno));

		if (recv_server_plist(&clients_plist)) {
			add_recursively (&new, args);
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

			plist_init (&saved_plist);

			/* this checks if the file exists */
			if (file_type (create_file_name (PLAYLIST_FILE))
						== F_PLAYLIST)
					plist_load (&saved_plist,
						create_file_name (PLAYLIST_FILE),
						cwd, 1);
			add_recursively (&new, args);
			plist_sort_fname (&new);

			send_int_to_srv (CMD_LOCK);
			plist_remove_common_items (&new, &saved_plist);
			if (plist_get_serial(&saved_plist))
				plist_set_serial(&saved_plist,
						get_safe_serial());
			plist_set_serial(&new, plist_get_serial(&saved_plist));
			send_playlist (&new, 0);
			send_int_to_srv (CMD_UNLOCK);

			plist_cat (&saved_plist, &new);
			if (options_get_bool("SavePlaylist")) {
				fill_tags (&saved_plist, TAGS_COMMENTS
						| TAGS_TIME, 1);
				plist_save (&saved_plist,
						create_file_name (PLAYLIST_FILE),
						NULL, 1);
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
		fatal ("Can't get CWD: %s", xstrerror (errno));
	plist_init (&plist);

	send_int_to_srv (CMD_GET_SERIAL);
	plist_set_serial (&plist, get_data_int());

	/* the second condition will checks if the file exists */
	if (!recv_server_plist(&plist)
			&& file_type (create_file_name (PLAYLIST_FILE))
			== F_PLAYLIST)
		plist_load (&plist, create_file_name (PLAYLIST_FILE), cwd, 1);

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
 * (malloc()ed). This function assumes that the interface is not initialized. */
static struct file_tags *get_tags_no_iface (const char *file,
		const int tags_sel)
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
			logit ("Server sent an event which I didn't expect!");
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
	file_info_block_init (&curr_file);

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
				curr_file.tags = get_tags_no_iface (
						curr_file.file,
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
		curr_file.avg_bitrate = get_avg_bitrate ();

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
				sec_to_min (time_left_str, MAX(left, 0));
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

		printf ("Bitrate: %dkbps\n", MAX(curr_file.bitrate, 0));
		printf ("AvgBitrate: %dkbps\n", MAX(curr_file.avg_bitrate, 0));
		printf ("Rate: %dkHz\n", curr_file.rate);

		file_info_cleanup (&curr_file);
		free (title);
	}

	plist_free (dir_plist);
	plist_free (playlist);
	plist_free (queue);
}

void interface_cmdline_enqueue (int server_sock, lists_t_strs *args)
{
	int ix;

	/* the interface is not initialized, so set it here */
	srv_sock = server_sock;

	if (!getcwd (cwd, sizeof (cwd)))
		fatal ("Can't get CWD: %s", xstrerror (errno));

	for (ix = 0; ix < lists_strs_size (args); ix += 1) {
		const char *arg;

		arg = lists_strs_at (args, ix);
		if (is_sound_file (arg) || is_url (arg)) {
			char *path = absolute_path (arg, cwd);
			send_int_to_srv (CMD_QUEUE_ADD);
			send_str_to_srv (path);
			free (path);
		}
	}
}

void interface_cmdline_playit (int server_sock, lists_t_strs *args)
{
	int ix, serial;
	struct plist plist;

	srv_sock = server_sock; /* the interface is not initialized, so set it
				   here */

	if (!getcwd(cwd, sizeof(cwd)))
		fatal ("Can't get CWD: %s", xstrerror (errno));

	plist_init (&plist);

	for (ix = 0; ix < lists_strs_size (args); ix += 1) {
		const char *arg;

		arg = lists_strs_at (args, ix);
		if (is_url(arg) || is_sound_file(arg)) {
			char *path = absolute_path (arg, cwd);
			plist_add (&plist, path);
			free (path);
		}
	}

	if (plist_count (&plist) == 0)
		fatal ("No files added - no sound files on command line!");

	send_int_to_srv (CMD_LOCK);

	send_playlist (&plist, 1);

	send_int_to_srv (CMD_GET_SERIAL);
	serial = get_data_int ();
	send_int_to_srv (CMD_PLIST_SET_SERIAL);
	send_int_to_srv (serial);

	send_int_to_srv (CMD_UNLOCK);

	send_int_to_srv (CMD_PLAY);
	send_str_to_srv ("");

	plist_free (&plist);
}

void interface_cmdline_seek_by (int server_sock, const int seek_by)
{
	srv_sock = server_sock; /* the interface is not initialized, so set it
				   here */
	seek (seek_by);
}

void interface_cmdline_jump_to (int server_sock, const int pos)
{
	srv_sock = server_sock; /* the interface is not initialized, so set it here */
	jump_to (pos);
}

void interface_cmdline_jump_to_percent (int server_sock, const int percent)
{
	srv_sock = server_sock; /* the interface is not initialized, so set it here */
	curr_file.file = get_curr_file ();
	int new_pos;

	if (percent >= 100) {
		fprintf (stderr, "Can't jump beyond the end of file.\n");
		return;
	}

	if (!curr_file.file[0]) {
		fprintf (stderr, "Nothing is played.\n");
		return;
	}

	if (file_type(curr_file.file) == F_URL) {
		fprintf (stderr, "Can't seek in network stream.\n");
		return;
	}

	curr_file.tags = get_tags_no_iface (curr_file.file,TAGS_TIME);
	new_pos = (percent*curr_file.tags->time)/100;
	printf("Jumping to: %ds. Total time is: %ds\n", new_pos, curr_file.tags->time);
	jump_to (new_pos);
}

void interface_cmdline_adj_volume (int server_sock, const char *arg)
{
	srv_sock = server_sock;

	if (arg[0] == '+')
		adjust_mixer (atoi (arg + 1));
	else if (arg[0] == '-')
		adjust_mixer (atoi (arg)); /* atoi can handle '-' */
	else if (arg[0] != 0)
		set_mixer (atoi (arg));
}

void interface_cmdline_set (int server_sock, char *arg, const int val)
{
	srv_sock = server_sock;
	char *last = NULL;
	char *tok;

	tok = strtok_r (arg, ",", &last);

	while (tok) {

		if (!strcasecmp (tok, "Shuffle") || !strcasecmp (tok, "s"))
			tok = "Shuffle";
		else if (!strcasecmp (tok, "AutoNext") || !strcasecmp (tok, "n"))
			tok = "AutoNext";
		else if (!strcasecmp (tok, "Repeat") || !strcasecmp (tok, "r"))
			tok = "Repeat";
		else {
			fprintf (stderr, "Unknown option '%s'\n", tok);
			break;
		}

		if (val == 2) {
			send_int_to_srv (CMD_GET_OPTION);
			send_str_to_srv (tok);
			options_set_bool (tok, get_data_bool());
		}

		send_int_to_srv (CMD_SET_OPTION);
		send_str_to_srv (tok);

		if (val == 2)
			send_bool_to_srv (!options_get_bool(tok));
		else
			send_bool_to_srv (val);

		tok = strtok_r (NULL, ",", &last);
	}
}

/* Print formatted info
State       %state
File        %file
Title       %title
Artist      %artist
SongTitle   %song
Album       %album
TotalTime   %tt
TimeLeft    %tl
TotalSec    %ts
CurrentTime %ct
CurrentSec  %cs
Bitrate     %b
Rate        %r
*/
void interface_cmdline_formatted_info (const int server_sock,
		const char *format_str)
{
	typedef struct {
		char *state;
		char *file;
		char *title;
		char *artist;
		char *song;
		char *album;
		char *totaltime;
		char *timeleft;
		char *totalsec;
		char *currenttime;
		char *currentsec;
		char *bitrate;
		char *rate;
	} info_t;

	char curr_time_str[6];
	char time_left_str[6];
	char time_str[6];
	char time_sec_str[5];
	char curr_time_sec_str[5];
	char file_bitrate_str[4];
	char file_rate_str[3];

	char *fmt, *str;
	info_t str_info;

	srv_sock = server_sock;	/* the interface is not initialized, so set it
				   here */
	init_playlists ();
	file_info_reset (&curr_file);
	file_info_block_init (&curr_file);

	curr_file.state = get_state ();

	/* extra paranoid about struct data */
	memset(&str_info, 0, sizeof(str_info));
	curr_time_str[0] = time_left_str[0] = time_str[0] =
	  time_sec_str[0] = curr_time_sec_str[0] =
	  file_bitrate_str[0] = file_rate_str[0] = '\0';

	str_info.currenttime = curr_time_str;
	str_info.timeleft = time_left_str;
	str_info.totaltime = time_str;
	str_info.totalsec = time_sec_str;
	str_info.currentsec = curr_time_sec_str;
	str_info.bitrate = file_bitrate_str;
	str_info.rate = file_rate_str;

	if (curr_file.state == STATE_STOP)
		str_info.state = "STOP";
	else {
		int left;

		if (curr_file.state == STATE_PLAY)
			str_info.state = "PLAY";
		else if (curr_file.state == STATE_PAUSE)
			str_info.state = "PAUSE";

		curr_file.file = get_curr_file ();

		if (curr_file.file[0]) {

			/* get tags */
			if (file_type(curr_file.file) == F_URL) {
				send_int_to_srv (CMD_GET_TAGS);
				curr_file.tags = get_data_tags ();
			}
			else
				curr_file.tags = get_tags_no_iface (
						curr_file.file,
						TAGS_COMMENTS | TAGS_TIME);

			/* get the title */
			if (curr_file.tags->title)
				str_info.title = build_title (curr_file.tags);
			else
				str_info.title = xstrdup ("");
		}
		else
			str_info.title = xstrdup ("");

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
				sec_to_min (time_left_str, MAX(left, 0));
			}
		}
		else {
			strcpy (curr_time_str, "00:00");
			time_left_str[0] = 0;
		}

		str_info.file = curr_file.file;

		str_info.artist =
					curr_file.tags->artist ? curr_file.tags->artist : NULL;
		str_info.song = curr_file.tags->title ? curr_file.tags->title : NULL;
		str_info.album = curr_file.tags->album ? curr_file.tags->album : NULL;

		if (curr_file.tags->time != -1)
			snprintf(time_sec_str, 5, "%d", curr_file.tags->time);

		snprintf(curr_time_sec_str, 5, "%d", curr_file.curr_time);
		snprintf(file_bitrate_str, 4, "%d", MAX(curr_file.bitrate, 0));
		snprintf(file_rate_str, 3, "%d", curr_file.rate);
	}

	/* string with formatting tags */
	fmt = xstrdup(format_str);

	fmt = str_repl(fmt, "%state", str_info.state);
	fmt = str_repl(fmt, "%file",
			str_info.file ? str_info.file : "");
	fmt = str_repl(fmt, "%title",
			str_info.title ? str_info.title : "");
	fmt = str_repl(fmt, "%artist",
			str_info.artist ? str_info.artist : "");
	fmt = str_repl(fmt, "%song",
			str_info.song ? str_info.song : "");
	fmt = str_repl(fmt, "%album",
			str_info.album ? str_info.album : "");
	fmt = str_repl(fmt, "%tt",
			str_info.totaltime ? str_info.totaltime : "");
	fmt = str_repl(fmt, "%tl",
			str_info.timeleft ? str_info.timeleft : "");
	fmt = str_repl(fmt, "%ts",
			str_info.totalsec ? str_info.totalsec : "");
	fmt = str_repl(fmt, "%ct",
			str_info.currenttime ? str_info.currenttime : "");
	fmt = str_repl(fmt, "%cs",
			str_info.currentsec ? str_info.currentsec : "");
	fmt = str_repl(fmt, "%b",
			str_info.bitrate ? str_info.bitrate : "");
	fmt = str_repl(fmt, "%r",
			str_info.rate ? str_info.rate : "");
	fmt = str_repl(fmt, "\\n", "\n");

	str = build_title_with_format (curr_file.tags, fmt);

	printf("%s\n", str);
	free(str);
	free(fmt);

	if (str_info.title)
		free(str_info.title);

	if (curr_file.state != STATE_STOP)
		file_info_cleanup (&curr_file);

	plist_free (dir_plist);
	plist_free (playlist);
	plist_free (queue);
}
