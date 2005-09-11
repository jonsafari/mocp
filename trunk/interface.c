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
#include <sys/types.h>
#include <sys/socket.h>
#ifdef HAVE_SYS_SELECT_H
# include <sys/select.h>
#endif

#define DEBUG

#include "log.h"
#include "interface_elements.h"
#include "interface.h"
#include "main.h"
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
static volatile int want_quit = 0;

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

static void sig_quit (int sig ATTR_UNUSED)
{
	want_quit = 1;
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

static void send_tags_request (const char *file, const int tags_sel)
{
	assert (file != NULL);
	assert (tags_sel != 0);

	send_int_to_srv (CMD_GET_FILE_TAGS);
	send_str_to_srv (file);
	send_int_to_srv (tags_sel);

	debug ("Asking for tags for %s", file);
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

static int get_mixer_value ()
{
	send_int_to_srv (CMD_GET_MIXER);
	return get_data_int ();
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
	return strcmp (*(char **)a, *(char **)b);
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
	
	return strcmp (sa, sb);
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

/* Send requests for the given tags for every file on the playlist. */
static void ask_for_tags (const struct plist *plist, const int tags_sel)
{
	int i;

	assert (plist != NULL);
	assert (tags_sel != 0);
	
	for (i = 0; i < plist->num; i++)
		if (!plist_deleted(plist, i)) {
			char *file = plist_get_file (plist, i);
			
			send_tags_request (file, tags_sel);
			free (file);
		}
}

static void interface_message (const char *format, ...)
{
	va_list va;
	char message[128];

	va_start (va, format);
	vsnprintf (message, sizeof(message), format, va);
	message[sizeof(message)-1] = 0;
	va_end (va);

	//TODO: iface_message (message);
}

/* Update tags (and titles) for the given item on the playlist with new tags. */
static void update_item_tags (struct plist *plist, const int num,
		const struct file_tags *tags)
{
	struct file_tags *old_tags = plist->items[num].tags;
	
	plist->items[num].tags = tags_dup (tags);

	/* Get the time from the old tags if it's not presend in the new tags.
	 * FIXME: There is risk, that the file was modified and the time
	 * from the old tags is not valid. */
	if (!(tags->filled & TAGS_TIME)) {
		plist->items[num].tags->filled |= TAGS_TIME;
		plist->items[num].tags->time = old_tags->time;
	}	

	plist_count_total_time (plist);

	if (options_get_int("ReadTags")) {
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

/* Update the current time. TODO: If silent_seek_pos >= 0, use this time instead
 * of the real time. */
static void update_ctime ()
{
	send_int_to_srv (CMD_GET_CTIME);
	curr_file.curr_time = get_data_int ();
	
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

static void update_curr_file ()
{
	char *file;

	send_int_to_srv (CMD_GET_SNAME);
	file = get_data_str ();

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
	send_int_to_srv (CMD_GET_RATE);
	curr_file.rate = get_data_int ();
	iface_set_rate (curr_file.rate);
}

static void update_channels ()
{
	send_int_to_srv (CMD_GET_CHANNELS);
	curr_file.channels = get_data_int () == 2 ? 2 : 1;
	iface_set_channels (curr_file.channels);
}

static void update_bitrate ()
{
	send_int_to_srv (CMD_GET_BITRATE);
	curr_file.bitrate = get_data_int ();
	iface_set_bitrate (curr_file.bitrate);
}

/* Get and show the server state. */
static void update_state ()
{
	/* play | stop | pause */
	send_int_to_srv (CMD_GET_STATE);
	curr_file.state = get_data_int ();
	iface_set_state (curr_file.state);

	/* Silent seeking makes no sense if the state has changed. */
/*	if (new_state != file_info.state_code) {
		file_info.state_code = new_state;
		silent_seek_pos = -1;
	}*/

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
	
	send_int_to_srv (CMD_GET_PLIST);
	if (get_int_from_srv() != EV_DATA)
		fatal ("Server didn't send data while requesting for the"
				" playlist.");

	if (!get_int_from_srv()) {
		debug ("There is no playlist");
		return 0; /* there are no other clients with a playlist */
	}

	debug ("There is a playlist, getting...");

	if (get_int_from_srv() != EV_DATA)
		fatal ("Server didnt send EV_DATA when it was supposed to send"
				" the playlist.");

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
		int need_recount_time;
		char *file;

		if (get_item_time(playlist, item) != -1)
			need_recount_time = 1;
		else
			need_recount_time = 0;

		file = plist_get_file (playlist, item);
		plist_delete (playlist, item);

		if (need_recount_time)
			plist_count_total_time (playlist);

		iface_del_plist_item (file);
		free (file);

		if (plist_count(playlist) == 0) {
			plist_clear (playlist);

			if (iface_in_plist_menu())
				iface_switch_to_dir ();
		}
	}
	else
		logit ("Server requested deleting an item not present on the"
				" playlist.");
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
			free (data);
			break;
		case EV_PLIST_CLEAR:
			if (options_get_int("SyncPlaylist"))
				clear_playlist ();
			break;
		case EV_PLIST_DEL:
			if (options_get_int("SyncPlaylist"))
				event_plist_del ((char *)data);
			free (data);
			break;
		case EV_TAGS:
			update_curr_tags ();
			break;
		case EV_STATUS_MSG:
			iface_set_status ((char *)data);
			free (data);
			break;
		case EV_MIXER_CHANGE:
			update_mixer_name ();
			break;
		case EV_FILE_TAGS:
			ev_file_tags ((struct tag_ev_response *)data);
			free_tag_ev_data ((struct tag_ev_response *)data);
			break;
		default:
			interface_fatal ("Unknown event: 0x%02x", event);
	}
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
	
	iface_set_dir_title (cwd);
	iface_set_status ("");

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
static void toggle_plist ()
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
			toggle_plist ();

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
static void process_args (char **args, const int num, const int recursively)
{
	if (num == 1 && !recursively && !is_url(args[0])
			&& isdir(args[0]) == 1) {
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
				read_directory_recurr (path, playlist, 1);
			else if (!dir && (is_sound_file(path)
						|| is_url(path)))
				plist_add (playlist, path);
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

	iface_set_status ("Loading playlist...");
	if (file_type(plist_file) == F_PLAYLIST)
		plist_load (playlist, plist_file, cwd);
	iface_set_status ("");
}

void init_interface (const int sock, const int logging, char **args,
		const int arg_num, const int recursively)
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
	windows_init ();
	keys_init ();
	get_server_options ();
	update_mixer_name ();

	signal (SIGQUIT, sig_quit);
	/*signal (SIGTERM, sig_quit);*/
	signal (SIGINT, sig_interrupt);
	
#ifdef SIGWINCH
	signal (SIGWINCH, sig_winch);
#endif
	

	if (arg_num) {
		process_args (args, arg_num, recursively);
	
		if (plist_count(playlist) == 0) {
			if (!options_get_int("SyncPlaylist")
					|| !use_server_playlist())
				load_playlist ();
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
		if (!options_get_int("SyncPlaylist")
				|| !use_server_playlist())
			load_playlist ();
		send_int_to_srv (CMD_SEND_EVENTS);
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
	int serial; /* serial number of the playlist */
	struct plist *curr_plist;
	
	assert (file != NULL);

	if (iface_in_dir_menu())
		curr_plist = dir_plist;
	else
		curr_plist = playlist;
	
	send_int_to_srv (CMD_LOCK);

	send_int_to_srv (CMD_PLIST_GET_SERIAL);
	serial = get_data_int ();
	
	if (plist_get_serial(curr_plist) == -1
			|| serial != plist_get_serial(curr_plist)) {

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


	if (plist_find_fname(playlist, file)) {
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
		send_int_to_srv (CMD_PLIST_GET_SERIAL);
		if (get_data_int() == plist_get_serial(playlist)) {
			send_int_to_srv (CMD_LIST_ADD);
			send_str_to_srv (file);
		}
		send_int_to_srv (CMD_UNLOCK);
	}
	else
		error ("The file is already on the playlist.");

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
	read_directory_recurr (file, &plist, 0);

	plist_sort_fname (&plist);
	
	if (get_tags_setting())
		ask_for_tags (&plist, get_tags_setting());

	send_int_to_srv (CMD_LOCK);

	plist_remove_common_items (&plist, playlist);

	/* Add the new files to the server's playlist if the server has our
	 * playlist */
	send_int_to_srv (CMD_PLIST_GET_SERIAL);
	if (get_data_int() == plist_get_serial(playlist))
		send_playlist (&plist, 0);

	if (options_get_int("SyncPlaylist")) {
		iface_set_status ("Notifying clients...");
		send_items_to_clients (&plist);
		iface_set_status ("");
	}
	else {
		int i;
		
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

/* Handle key */
static void menu_key (const int ch)
{
	if (iface_in_help()) {
#if 0
		int help_lines;

		get_keys_help (&help_lines);

		if (ch == KEY_DOWN || ch == KEY_NPAGE || ch == '\n') {
			if (help_screen_top + LINES - 5 <= help_lines) {
				help_screen_top++;
				print_help_screen ();
			}
		}
		else if (ch == KEY_UP || ch == KEY_PPAGE) {
			if (help_screen_top > 0) {
				help_screen_top--;
				print_help_screen ();
			}
		}
		else if (ch != KEY_RESIZE) {
			
			/* Switch to menu */
			werase (main_win);
			main_border ();
			menu_draw (curr_menu);
			update_info_win ();
			wrefresh (main_win);
			wrefresh (info_win);
		
			main_win_mode = WIN_MENU;
		}
#endif
	}
#if 0
	else if (entry.type != ENTRY_DISABLED)
		entry_key (ch);
#endif
	else if (!iface_key_is_resize(ch)) {
		enum key_cmd cmd = get_key_cmd (CON_MENU, ch);
		
		switch (cmd) {
			case KEY_CMD_QUIT_CLIENT:
				want_quit = 1;
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
				break;
			case KEY_CMD_QUIT:
				send_int_to_srv (CMD_QUIT);
				want_quit = 1;
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
#if 0
			case KEY_CMD_TOGGLE_READ_TAGS:
				switch_read_tags ();
				do_update_menu = 1;
				break;
#endif
			case KEY_CMD_TOGGLE_SHUFFLE:
				toggle_option ("Shuffle");
				break;
			case KEY_CMD_TOGGLE_REPEAT:
				toggle_option ("Repeat");
				break;
			case KEY_CMD_TOGGLE_AUTO_NEXT:
				toggle_option ("AutoNext");
				break;
			case KEY_CMD_TOGGLE_PLAYLIST:
				toggle_plist ();
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
#if 0
			case KEY_CMD_SEEK_BACKWARD:
				seek (-options_get_int("SeekTime"));
				break;
			case KEY_CMD_SEEK_FORWARD:
				seek (options_get_int("SeekTime"));
				break;
			case KEY_CMD_HELP:
				help_screen ();
				break;
			case KEY_CMD_HIDE_MESSAGE:
				interface_message (NULL);
				update_info_win ();
				wrefresh (info_win);
				break;
#endif
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
					reread_dir (1);
				break;
#if 0
			case KEY_CMD_GO_MUSIC_DIR:
				go_to_music_dir ();
				do_update_menu = 1;
				break;
			case KEY_CMD_PLIST_DEL:
				delete_item ();
				do_update_menu = 1;
				break;
			case KEY_CMD_MENU_SEARCH:
				make_entry (ENTRY_SEARCH, "SEARCH");
				break;
			case KEY_CMD_PLIST_SAVE:
				if (plist_count(playlist))
					make_entry (ENTRY_PLIST_SAVE,
							"SAVE PLAYLIST");
				else
					error ("The playlist is "
							"empty.");
				break;
#endif
			case KEY_CMD_TOGGLE_SHOW_TIME:
				toggle_show_time ();
				break;
			case KEY_CMD_TOGGLE_SHOW_FORMAT:
				toggle_show_format ();
				break;
#if 0
			case KEY_CMD_GO_TO_PLAYING_FILE:
				go_to_file_dir ();
				do_update_menu = 1;
				break;
			case KEY_CMD_GO_DIR:
				make_entry (ENTRY_GO_DIR, "GO");
				break;
			case KEY_CMD_GO_URL:
				make_entry (ENTRY_GO_URL, "URL");
				break;
#endif
			case KEY_CMD_GO_DIR_UP:
				go_dir_up ();
				break;
			case KEY_CMD_WRONG:
				error ("Bad command");
				break;
#if 0
			case KEY_CMD_SEEK_FORWARD_5:
				seek_silent (5);
				break;
			case KEY_CMD_SEEK_BACKWARD_5:
				seek_silent (-5);
				break;
#endif
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

void interface_loop ()
{
	while (!want_quit) {
		fd_set fds;
		int ret;
		struct timeval timeout = { 1, 0 };
		
		FD_ZERO (&fds);
		FD_SET (srv_sock, &fds);
		FD_SET (STDIN_FILENO, &fds);

		dequeue_events ();
		ret = select (srv_sock + 1, &fds, NULL, NULL, &timeout);

		iface_tick ();
		
		if (ret == 0) {
			//do_silent_seek ();
		}
		else if (ret == -1 && !want_quit && errno != EINTR)
			interface_fatal ("select() failed: %s",
					strerror(errno));

#ifdef SIGWINCH
		if (want_resize)
			do_resize ();
#endif

		if (ret > 0) {
			if (FD_ISSET(STDIN_FILENO, &fds)) {
				int ch = iface_get_char ();

				clear_interrupt ();
				
				menu_key (ch);
				if (!want_quit)
					dequeue_events ();
			}

			if (!want_quit) {
				if (FD_ISSET(srv_sock, &fds))
					get_and_handle_event ();
				//do_silent_seek ();
				dequeue_events ();
			}
		}
		else if (user_wants_interrupt())
			/*handle_interrupt ()*/;

		if (!want_quit)
			update_mixer_value ();
	}
}

void interface_end ()
{
	send_int_to_srv (CMD_DISCONNECT);
	close (srv_sock);
	srv_sock = -1;
	
	windows_end ();
	keys_cleanup ();

	/* TODO: save last dir, save playlist */
	
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
}

void interface_cmdline_append (int server_sock, char **args,
		const int arg_num)
{
}

void interface_cmdline_play_first (int server_sock)
{
}

void interface_cmdline_file_info (const int server_sock)
{
}

void interface_cmdline_playit (int server_sock, char **args, const int arg_num)
{
}
