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

#include <sys/types.h>
#include <sys/socket.h>
#ifdef HAVE_SYS_SELECT_H
# include <sys/select.h>
#endif
#include <sys/un.h>
#include <sys/stat.h>
#include <unistd.h>
#include <ncurses.h>
#include <string.h>
#include <signal.h>
#include <time.h>
#include <errno.h>
#include <assert.h>
#include <ctype.h>
#include "protocol.h"
#include "main.h"
#include "playlist.h"
#include "log.h"
#include "menu.h"
#include "files.h"
#include "options.h"
#include "file_types.h"
#include "interface.h"
#include "playlist_file.h"

#define STATUS_LINE_LEN	25
#define INTERFACE_LOG	"mocp_client_log"
#define MAX_SEARCH_STRING	(COLS - 6)

/* Socket connection to the server. */
static int srv_sock = -1;

/* If the user presses quit, or we receive a termination signal. */
static volatile int want_quit = 0;

#ifdef SIGWINCH
/* If we get SIGWINCH. */
static volatile int want_resize = 0;
#endif

/* Some xterms (gnome, kde terms) needs wclear to correctly refresh the
 * screen. */
static int buggy_xterm = 0;

/* Are we running on xterm? */
static int has_xterm = 0;

/* xterm title */
struct {
	char title[256];
	int state;
} xterm_title;

static char cwd[PATH_MAX] = "";

enum {
	CLR_ITEM = 1,
	CLR_SELECTED,
	CLR_ERROR,
	CLR_MARKED,
	CLR_MARKED_SELECTED,
	CLR_BAR,
	CLR_DISABLED,
	CLR_MESSAGE,
	CLR_NUMBERS
};

static WINDOW *main_win = NULL;
static WINDOW *info_win = NULL;

static char mainwin_title[512];
static int msg_timeout = 0;
static int msg_is_error = 0;
static char message[512] = "Welcome to "PACKAGE_STRING"! "
	"Please send bug reports to "PACKAGE_BUGREPORT;
static char interface_status[STATUS_LINE_LEN + 1] = "              ";

static struct plist *curr_plist = NULL; /* Current directory */
static struct plist *playlist = NULL; /* The playlist */
static struct plist *visible_plist = NULL; /* The playlist the user sees */
static struct menu *menu = NULL;
static struct menu *saved_menu = NULL; /* Menu associated with curr_plist */

static enum {
	WIN_MENU,
	WIN_HELP
} main_win_mode = WIN_MENU;

static struct file_info {
	char title[256];
	char bitrate[4];
	char rate[3];
	char time[6];
	char curr_time[6];
	char time_left[6];
	int time_num;
	int channels;
	char state[3];
} file_info;

/* When we are waiting for data from the server an event can occur. The eventa
 * we don't want to forget are EV_STATE and EV_ERROR, so set this variables to
 * 1 if one of them occur. */
static int must_update_state = 0;
static int must_update_error = 0;

/* for CTRL-g - search an item */
static int search_file_mode = 0;
static char search_string[256];

/* ^c version of c */
#ifndef CTRL
# define CTRL(c) ((c) & 0x1F)
#endif

static void interface_fatal (const char *msg)
{
	endwin ();
	fatal (msg);
}

static void sig_quit (int sig)
{
	want_quit = 1;
}

#ifdef SIGWINCH
static void sig_winch (int sig)
{
	want_resize = 1;
}
#endif

/* Connect to the server, return fd os the socket or -1 on error */
int server_connect ()
{
	struct sockaddr_un sock_name;
	int sock;
	
	/* Create a socket */
	if ((sock = socket (PF_LOCAL, SOCK_STREAM, 0)) == -1)
		 return -1;
	
	sock_name.sun_family = AF_LOCAL;
	strcpy (sock_name.sun_path, socket_name());

	if (connect(sock, (struct sockaddr *)&sock_name,
				SUN_LEN(&sock_name)) == -1) {
		close (sock);
		return -1;
	}

	return sock;
}

/* Draw border around the main window */
static void main_border ()
{
	wattrset (main_win, COLOR_PAIR(CLR_ITEM) | A_NORMAL);
	wborder (main_win, ACS_VLINE, ACS_VLINE, ACS_HLINE, ' ',
			ACS_ULCORNER, ACS_URCORNER, ACS_VLINE, ACS_VLINE);

	/* The title */
	wmove (main_win, 0, COLS / 2 - strlen(mainwin_title) / 2 - 1);
	waddch (main_win, ACS_RTEE);
	waddstr (main_win, mainwin_title);
	waddch (main_win, ACS_LTEE);
}

static void set_title (const char *title)
{
	int len = strlen (title);

	if (len > COLS - 4) {
		snprintf (mainwin_title, sizeof(mainwin_title),
				"...%s", title + len - COLS + 7);
	}
	else
		strcpy (mainwin_title, title);
		
	main_border ();
	wrefresh (main_win);
}

/* Draw border around the info window and add some static text */
static void info_border ()
{
	wborder (info_win, ACS_VLINE, ACS_VLINE, ACS_HLINE, ACS_HLINE,
			ACS_LTEE, ACS_RTEE, ACS_LLCORNER, ACS_LRCORNER);
	wmove (info_win, 2, 25);
	waddstr (info_win, "KHz    Kbps");
}

static void draw_interface_status ()
{
	int i;
	
	mvwaddstr (info_win, 0, 6, interface_status);
	for (i = strlen(interface_status); i < STATUS_LINE_LEN; i++)
		waddch (info_win, ' ');
}

static void send_int_to_srv (const int num)
{
	if (!send_int(srv_sock, num))
		interface_fatal ("Can't send() int to the server.");
}

static void send_str_to_srv (const char *str)
{
	if (!send_str(srv_sock, str))
		interface_fatal ("Can't send() string to the server.");
}

static int get_int_from_srv ()
{
	int num;
	
	if (!get_int(srv_sock, &num))
		interface_fatal ("Can't receive value from the server.");

	return num;
}

/* Returned memory is malloc()ed. */
static char *get_str_from_srv ()
{
	char *str = get_str (srv_sock);
	
	if (!str)
		interface_fatal ("Can't receive string from the server.");

	return str;
}

/* Get an integer value from the server that will arrive after EV_DATA. */
static int get_data_int ()
{
	int event;

	while (1) {
		event = get_int_from_srv ();
		if (event == EV_DATA)
			return get_int_from_srv ();
		else if (event == EV_STATE)
			 must_update_state = 1;
		else if (event == EV_ERROR)
			must_update_error = 1;
	}		
}

static int get_mixer ()
{
	send_int_to_srv (CMD_GET_MIXER);
	return get_data_int ();
}

/* Draw the mixer bar */
static void draw_mixer () {
	char bar[21];
	int vol;

	vol = get_mixer ();

	if (vol == -1)
		return;

	if (vol == 100)
		sprintf (bar, " Vol %d%%           ", vol);
	else
		sprintf (bar, " Vol  %02d%%           ", vol);
	mvwaddch (info_win, 0, COLS - 23, ACS_RTEE);
	mvwaddch (info_win, 0, COLS - 2, ACS_LTEE);

	wattron (info_win, COLOR_PAIR(CLR_BAR));
	mvwaddnstr (info_win, 0, COLS - 22, bar, vol / 5);
	wattron (info_win, COLOR_PAIR(CLR_NUMBERS));
	mvwaddnstr (info_win, 0, COLS - 22 + (vol / 5),
			bar + vol / 5, 20 - (vol / 5));
}

static void draw_search_entry ()
{
	wmove (info_win, 0, 1);
	wattrset (info_win, COLOR_PAIR(CLR_BAR));
	wprintw (info_win, "GO: %-*s", MAX_SEARCH_STRING, search_string);
}

/* Update the info win */
static void update_info_win ()
{
	werase (info_win);
	wattrset (info_win, COLOR_PAIR(CLR_NUMBERS));
	info_border ();
	wattron (info_win, A_BOLD);

	/* Show message it it didn't expire yet */
	if (time(NULL) <= msg_timeout) {
		wattron (info_win, msg_is_error ? COLOR_PAIR(CLR_ERROR)
				: COLOR_PAIR(CLR_MESSAGE));
		mvwaddnstr (info_win, 1, 1, message, COLS - 2);
	}
	else {

		/* The title */
		mvwaddstr (info_win, 1, 4, file_info.title);

		/* State of playing */
		mvwaddstr (info_win, 1, 1, file_info.state);
	}

	wattron (info_win, COLOR_PAIR(CLR_NUMBERS));

	/* Current time */
	wmove (info_win, 2, 1);
	waddstr (info_win, file_info.curr_time);

	/* Time left */
	if (*file_info.time_left) {
		waddch (info_win, ' ');
		waddstr (info_win, file_info.time_left);
	}

	/* Total_time */
	if (file_info.time[0] != 0) {
		wmove (info_win, 2, 13);
		wprintw (info_win, "[%s]", file_info.time);
	}

	/* Rate */
	wmove (info_win, 2, 25 - strlen(file_info.rate));
	waddstr (info_win, file_info.rate);
	
	/* Bitrate */
	wmove (info_win, 2, 32 - strlen(file_info.bitrate));
	waddstr (info_win, file_info.bitrate);

	/* Channels */
	wmove (info_win, 2, 38);
	if (file_info.channels == 2)
		wattron (info_win, COLOR_PAIR(CLR_ITEM) | A_BOLD);
	else
		wattron (info_win, COLOR_PAIR(CLR_DISABLED) | A_BOLD);
	waddstr (info_win, "[STEREO]");
	
	/* Shuffle & repeat */
	wmove (info_win, 2, COLS - sizeof("[SHUFFLE] [REPEAT] [NEXT]"));
	if (options_get_int("Shuffle"))
		wattron (info_win, COLOR_PAIR(CLR_ITEM) | A_BOLD);
	else
		wattron (info_win, COLOR_PAIR(CLR_DISABLED) | A_BOLD);
	waddstr (info_win, "[SHUFFLE] ");

	if (options_get_int("Repeat"))
		wattron (info_win, COLOR_PAIR(CLR_ITEM) | A_BOLD);
	else
		wattron (info_win, COLOR_PAIR(CLR_DISABLED) | A_BOLD);
	waddstr (info_win, "[REPEAT] ");

	if (options_get_int("AutoNext"))
		wattron (info_win, COLOR_PAIR(CLR_ITEM) | A_BOLD);
	else
		wattron (info_win, COLOR_PAIR(CLR_DISABLED) | A_BOLD);
	waddstr (info_win, "[NEXT]");
	
	wattron (info_win, COLOR_PAIR(CLR_ITEM));

	if (search_file_mode)
		draw_search_entry ();
	else {
		/* Status line */
		wattroff (info_win, A_BOLD);
		mvwaddch (info_win, 0, 5, ACS_RTEE);
		wattroff (info_win, A_BOLD);
		mvwaddch (info_win, 0, 5 + STATUS_LINE_LEN + 1, ACS_LTEE);
		draw_interface_status ();
				
		draw_mixer ();
	}
}

static void set_interface_status (const char *msg)
{
	assert (!msg || strlen(msg) <= STATUS_LINE_LEN);

	if (msg)
		strncpy (interface_status, msg, sizeof(interface_status));
	else
		interface_status[0] = 0;
	draw_interface_status ();
}

void interface_error (const char *format, ...)
{
	va_list va;

	va_start (va, format);
	vsnprintf (message, sizeof(message), format, va);
	message[sizeof(message)-1] = 0;
	msg_timeout = time(NULL) + 3;
	msg_is_error = 1;
	
	/* The interface could have not been initialized yet. */
	if (main_win) {
		update_info_win ();
		wrefresh (info_win);
	}
	else
		fprintf (stderr, "%s\n", message);

	logit ("ERROR: %s", message);

	va_end (va);
}

static void interface_message (const char *format, ...)
{
	va_list va;

	va_start (va, format);
	if (format) {
		vsnprintf (message, sizeof(message), format, va);
		message[sizeof(message)-1] = 0;
		msg_timeout = time(NULL) + 3;
	}
	else
		msg_timeout = 0;
	msg_is_error = 0;
	update_info_win ();
	wrefresh (info_win);

	va_end (va);
}

/* Reset the title, bitrate etc */
static void reset_file_info ()
{
	file_info.title[0] = 0;
	strcpy (file_info.curr_time, "00:00");
	strcpy (file_info.time_left, "00:00");
	strcpy (file_info.time, "00:00");
	file_info.bitrate[0] = 0;
	file_info.rate[0] = 0;
	file_info.time_num = 0;
	file_info.channels = 1;
	strcpy (file_info.state, "[]");
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

/* Try to find the directory we can start. */
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

static int qsort_dirs_func (const void *a, const void *b)
{
	char *sa = *(char **)a;
	char *sb = *(char **)b;
	
	if (!strcmp(sa, "../"))
		return -1;
	if (!strcmp(sb, "../"))
		return 1;
	return strcmp (sa, sb);
}	

static int qsort_strcmp_func (const void *a, const void *b)
{
	return strcmp (*(char **)a, *(char **)b);
}

/* Make menu using the playlist and directory table. */
static struct menu *make_menu (struct plist *plist, struct file_list *dirs,
		struct file_list *playlists)
{
	int i;
	int menu_pos;
	struct menu_item **menu_items;
	int plist_items;
	int nitems;
	
	plist_items = plist_count (plist);

	/* +1 for '..' */
	nitems = plist_items + 1;
	if (dirs)
		nitems +=  dirs->num;
	if (playlists)
		nitems += playlists->num;

	menu_items = (struct menu_item **)xmalloc (sizeof(struct menu_item *)
			* nitems);
	
	/* add '..' */
	menu_items[0] = menu_newitem ("..", -1, F_DIR, "..");
	menu_items[0]->attr_normal = COLOR_PAIR(CLR_ITEM) | A_BOLD;
	menu_items[0]->attr_sel = COLOR_PAIR(CLR_SELECTED) | A_BOLD;
	menu_pos = 1;
	
	if (dirs)
		for (i = 0; i < dirs->num; i++) {
			menu_items[menu_pos] =
				menu_newitem (strrchr(dirs->items[i], '/') + 1,
						-1, F_DIR, dirs->items[i]);
			menu_items[menu_pos]->attr_normal =
				COLOR_PAIR(CLR_ITEM) | A_BOLD;
			menu_items[menu_pos]->attr_sel =
				COLOR_PAIR(CLR_SELECTED) | A_BOLD;
			menu_pos++;
		}

	if (playlists)
		for (i = 0; i < playlists->num; i++){
			menu_items[menu_pos] = menu_newitem (
					strrchr(playlists->items[i], '/') + 1,
					-1, F_PLAYLIST,	playlists->items[i]);
			menu_items[menu_pos]->attr_normal =
				COLOR_PAIR(CLR_ITEM) | A_BOLD;
			menu_items[menu_pos]->attr_sel =
				COLOR_PAIR(CLR_SELECTED) | A_BOLD;
			menu_pos++;
		}
	
	/* playlist items */
	for (i = 0; i < plist->num; i++) {
		if (!plist_deleted(plist, i)) {
			menu_items[menu_pos] = menu_newitem (
					plist->items[i].title, i, F_SOUND,
					plist->items[i].file);
			menu_items[menu_pos]->attr_normal = COLOR_PAIR(CLR_ITEM);
			menu_items[menu_pos]->attr_sel = COLOR_PAIR(CLR_SELECTED);
			menu_pos++;
		}
	}
	
	return menu_new (main_win, menu_items, nitems,
			COLOR_PAIR(CLR_ITEM), COLOR_PAIR(CLR_SELECTED),
			COLOR_PAIR(CLR_MARKED) | A_BOLD,
			COLOR_PAIR(CLR_MARKED_SELECTED) | A_BOLD);
}

/* Check if dir2 is in dir1 */
static int is_subdir (char *dir1, char *dir2)
{
	char *slash = strrchr (dir2, '/');

	assert (slash != NULL);

	return !strncmp(dir1, dir2, strlen(dir1)) ? 1 : 0;
}

/* Get a string value from the server that will arrive after EV_DATA. */
static char *get_data_str ()
{
	int event;

	while (1) {
		event = get_int_from_srv ();
		if (event == EV_DATA)
			return get_str_from_srv ();
		else if (event == EV_STATE)
			must_update_state = 1;
		else if (event == EV_ERROR)
			must_update_error = 1;
	}		
}

static void send_playlist (struct plist *plist)
{
	int i;
	
	send_int_to_srv (CMD_LIST_CLEAR);
	
	for (i = 0; i < plist->num; i++) {
		if (!plist_deleted(plist, i)) {
			send_int_to_srv (CMD_LIST_ADD);
			send_str_to_srv (plist->items[i].file);
		}
	}
}

/* Mark this file in the menu. Umark if NULL*/
static void mark_file (const char *file)
{
	if (file) {
		int i = plist_find_fname (visible_plist, file);
		
		if (i != -1) {
			menu_mark_plist_item (menu, i);
			if (main_win_mode == WIN_MENU)
				menu_draw (menu);
		}
	}
	else
		menu_unmark_item (menu);
}

/* Update the xterm title. */
static void xterm_update_title ()
{
	write (1, "\033]2;", sizeof("\033]2;")-1);
	write (1, "MOC ", sizeof("MOC ")-1);
	
	switch (xterm_title.state) {
		case STATE_PLAY:
			write (1, "[play]", sizeof("[play]")-1);
			break;
		case STATE_STOP:
			write (1, "[stop]", sizeof("[stop]")-1);
			break;
		case STATE_PAUSE:
			write (1, "[pause]", sizeof("[pause]")-1);
			break;
	}
	
	if (xterm_title.title[0]) {
		write (1, " - ", sizeof(" - ")-1);
		write (1, xterm_title.title, strlen(xterm_title.title));
	}

	write (1, "\007", 1);
}

/* Set the xterm title */
static void xterm_set_title (const char *title)
{
	if (has_xterm) {
		strncpy (xterm_title.title, title, sizeof(xterm_title.title));
		xterm_title.title[sizeof(xterm_title.title)-1] = 0;
		xterm_update_title ();
	}
}

static void xterm_clear_title ()
{
	if (has_xterm)
		write (1, "\033]2;\007", sizeof("\033]2;\007")-1);
}

static void xterm_set_state (int state)
{
	if (has_xterm) {
		xterm_title.state = state;
		xterm_update_title ();
	}
}

/* Set the state. */
static void set_state (const int state)
{
	switch (state) {
		case STATE_PLAY:
			strcpy (file_info.state, " >");
			break;
		case STATE_STOP:
			strcpy (file_info.state, "[]");
			break;
		case STATE_PAUSE:
			strcpy (file_info.state, "||");
			break;
	}
	xterm_set_state (state);
}

/* Find the title for a file. Check if it's on the playlist, if not, try to
 * make the title. Returned memory is malloc()ed. */
static char *find_title (char *file)
{
	int index;
	struct file_tags *tags;
	
	if ((index = plist_find_fname(visible_plist, file)) != -1) {
		if (!visible_plist->items[index].tags)
			visible_plist->items[index].tags = read_file_tags (file);
		if (visible_plist->items[index].tags)
			return build_title (visible_plist->items[index].tags);
	}
	else if ((tags = read_file_tags(file))) {
		char *title = NULL;
		
		if (tags->title)
			title = build_title (tags);
		tags_free (tags);

		if (title)
			return title;
	}
	
	return strrchr(file, '/') ? xstrdup (strrchr(file, '/') + 1)
		: xstrdup (file);
}

/* Convert time in second to min:sec text format. */
static void sec_to_min (char *buff, const int seconds)
{
	int min, sec;

	min = seconds / 60;
	sec = seconds % 60;

	snprintf (buff, 6, "%02d:%02d", min, sec);
}

static void set_time (const int time)
{
	file_info.time_num = time;
	sec_to_min (file_info.time, time);
}

static void update_channels ()
{
	send_int_to_srv (CMD_GET_CHANNELS);
	file_info.channels = get_data_int ();
	update_info_win ();
	wrefresh (info_win);
}

static void update_rate ()
{
	int rate;

	send_int_to_srv (CMD_GET_RATE);
	if ((rate = get_data_int()) > 0)
		sprintf (file_info.rate, "%d", rate);
	else
		file_info.rate[0] = 0;

	update_info_win ();
	wrefresh (info_win);
}

static void update_bitrate ()
{
	int bitrate;

	send_int_to_srv (CMD_GET_BITRATE);
	if ((bitrate = get_data_int()) > 0) {
		snprintf (file_info.bitrate, sizeof(file_info.bitrate),
				"%d", bitrate);
		file_info.bitrate[sizeof(file_info.bitrate)-1] = 0;
	}
	else
		file_info.bitrate[0] = 0;

	update_info_win ();
	wrefresh (info_win);
}

static void update_ctime ()
{
	int ctime;
	int left;
	
	send_int_to_srv (CMD_GET_CTIME);
	ctime = get_data_int ();

	left = file_info.time_num - ctime;
	sec_to_min (file_info.curr_time, ctime);
	sec_to_min (file_info.time_left, left > 0 ? left : 0);

	update_info_win ();
	wrefresh (info_win);
}

/* Update the name of the currently played file. */
static void update_curr_file ()
{
	char *file;

	send_int_to_srv (CMD_GET_SNAME);
	file = get_data_str ();
	if (file[0]) {
		char *title = find_title (file);

		strncpy (file_info.title, title,
				sizeof(file_info.title) - 1);
		file_info.title[sizeof(file_info.title)-1] = 0;
		xterm_set_title (file_info.title);
		mark_file (file);
		free (title);
	}
	else {
		file_info.title[0] = 0;
		menu_unmark_item (menu);
		xterm_set_title ("");
	}

	free (file);
}

/* Get and show the server state. */
static void update_state ()
{
	/* play | stop | pause */
	send_int_to_srv (CMD_GET_STATE);
	set_state (get_data_int());

	/* time of the song */
	send_int_to_srv (CMD_GET_STIME);
	set_time (get_data_int());
	
	update_curr_file ();
	
	if (main_win_mode == WIN_MENU) {
		menu_draw (menu);
		wrefresh (main_win);
	}

	update_channels ();
	update_bitrate ();
	update_rate ();
	update_ctime (); /* will also update the window */
}

/* Go to the directory dir, it it is NULL, go to the cwd. Return 1 on success,
 * 0 on error. */
static int go_to_dir (char *dir)
{
	struct plist *old_curr_plist;
	char last_dir[PATH_MAX];
	char *new_dir = dir ? dir : cwd;
	int going_up = 0;
	char msg[50];
	struct file_list *dirs, *playlists;

	set_interface_status ("reading directory...");
	wrefresh (info_win);

	if (chdir(new_dir)) {
		set_interface_status (NULL);
		wrefresh (info_win);
		interface_error ("Can't chdir() to %s, %s", new_dir,
				strerror(errno));
		return 0;
	}

	if (dir && is_subdir(dir, cwd)) {
		strcpy (last_dir, strrchr(cwd, '/') + 1);
		going_up = 1;
	}

	old_curr_plist = curr_plist;
	curr_plist = (struct plist *)xmalloc (sizeof(struct plist));
	plist_init (curr_plist);
	dirs = file_list_new ();
	playlists = file_list_new ();

	if (!read_directory(new_dir, dirs, playlists, curr_plist)) {
		if (chdir(cwd))
			interface_fatal ("Can't go to the previous directory.");
		set_interface_status (NULL);
		wrefresh (info_win);
		plist_free (curr_plist);
		file_list_free (dirs);
		file_list_free (playlists);
		free (curr_plist);
		curr_plist = old_curr_plist;
		return 0;
	}

	plist_free (old_curr_plist);
	free (old_curr_plist);
	visible_plist = curr_plist;

	if (menu)
		menu_free (menu);
	
	if (dir)
		strcpy (cwd, dir);

	if (options_get_int("ReadTags")) {
		read_tags (curr_plist);
		make_titles_tags (curr_plist);
	}
	else
		make_titles_file (curr_plist);
	
	plist_sort_fname (curr_plist);
	qsort (dirs->items, dirs->num, sizeof(char *), qsort_dirs_func);
	qsort (playlists->items, playlists->num, sizeof(char *),
			qsort_strcmp_func);
	
	menu = make_menu (curr_plist, dirs, playlists);
	file_list_free (dirs);
	file_list_free (playlists);
	if (going_up)
		menu_setcurritem_title (menu, last_dir);
	
	set_title (cwd);

	update_state ();
	sprintf (msg, "%d files and directories", menu->nitems - 1);
	set_interface_status (msg);
	wrefresh (info_win);

	return 1;
}

/* Make new cwd path from CWD and this path */
static void make_path (char *path)
{
	if (path[0] == '/')
		strcpy (cwd, "/"); /* for absolute path */
	else if (!cwd[0]) {
		if (!getcwd(cwd, sizeof(cwd)))
			interface_fatal ("Can't get CWD");
	}

	resolve_path (cwd, sizeof(cwd), path);
}

/* Enter to the initial directory. */
static void enter_first_dir ()
{
	if (options_get_int("StartInMusicDir")) {
		char *music_dir;
		
		if ((music_dir = options_get_str("MusicDir"))) {
			make_path (music_dir);
			if (go_to_dir(NULL))
				return;
		}
		else
			interface_error ("MusicDir is not set");
	}
	if (read_last_dir() && go_to_dir(NULL))
		return;
	set_start_dir ();
	if (!go_to_dir(NULL))
		interface_fatal ("Can't enter any directory.");
}

/* Set the has_xterm variable. */
static void detect_term ()
{
	char *term;

	if (((term = getenv("TERM")) && !strcmp(term, "xterm")))
		has_xterm = 1;
}

/* Get an integer option from the server and set it. */
static void sync_int_option (const char *name)
{
	send_int_to_srv (CMD_GET_OPTION);
	send_str_to_srv (name);
	option_set_int (name, get_data_int());
}

/* Get the server options that we show. */
static void get_server_options ()
{
	sync_int_option ("Shuffle");
	sync_int_option ("Repeat");
	sync_int_option ("AutoNext");
	sync_int_option ("ShowStreamErrors");
}

/* End the program if the terminal is too small. */
static void check_term_size ()
{
	if (COLS < 72 || LINES < 7)
		interface_fatal ("The terminal is too small after resizeing.");
}

/* Process file names passwd as arguments. */
static void process_args (char **args, const int num)
{
	if (num == 1 && isdir(args[0]) == 1) {
		make_path (args[0]);
		if (!go_to_dir(NULL))
			enter_first_dir ();
	}
	else {
		int i;

		for (i = 0; i < num; i++) {
			int dir = isdir(args[i]);

			if (dir == 1)
				read_directory_recurr (args[i], playlist);
			else if (dir == 0 && is_sound_file(args[i]))
				plist_add (playlist, args[i]);
		}

		if (playlist->num) {
			char msg[50];

			visible_plist = playlist;
			
			if (options_get_int("ReadTags")) {
				read_tags (playlist);
				make_titles_tags (playlist);
			}
			else
				make_titles_file (playlist);

			menu = make_menu (playlist, NULL, 0);
			set_title ("Playlist");
			update_curr_file ();
			sprintf (msg, "%d files on the list",
					plist_count(playlist));
			set_interface_status (msg);

			send_playlist (playlist);
			send_int_to_srv (CMD_PLAY);
			send_str_to_srv (playlist->items[0].file);
		}
		else
			enter_first_dir ();
	}
}

/* Initialize the interface. args are command line file names. arg_num is the
 * number of arguments. */
void init_interface (const int sock, const int debug, char **args,
		const int arg_num)
{
	srv_sock = sock;
	if (debug) {
		FILE *logfp;

		if (!(logfp = fopen(INTERFACE_LOG, "a")))
			fatal ("Can't open log file for the interface");
		log_init_stream (logfp);
	}
	curr_plist = (struct plist *)xmalloc (sizeof(struct plist));
	plist_init (curr_plist);
	playlist = (struct plist *)xmalloc (sizeof(struct plist));
	plist_init (playlist);
	send_int_to_srv (CMD_SEND_EVENTS);

	initscr ();
	cbreak ();
	noecho ();

	check_term_size ();

	signal (SIGQUIT, sig_quit);
	//signal (SIGTERM, sig_quit);
	signal (SIGINT, sig_quit);
	
#ifdef SIGWINCH
	signal (SIGWINCH, sig_winch);
#endif
	
	/* Try to detect if we are running in buggy X terminal */
	if (getenv("DISPLAY") && strcmp(getenv("TERM"), "xterm")) 
		buggy_xterm = 1;

	detect_term ();

	start_color ();
	init_pair (CLR_ITEM, COLOR_WHITE, COLOR_BLUE);
	init_pair (CLR_SELECTED, COLOR_WHITE, COLOR_BLACK);
	init_pair (CLR_ERROR, COLOR_RED, COLOR_BLUE);
	init_pair (CLR_MARKED, COLOR_GREEN, COLOR_BLUE);
	init_pair (CLR_MARKED_SELECTED, COLOR_GREEN, COLOR_BLACK);
	init_pair (CLR_BAR, COLOR_BLACK, COLOR_CYAN);
	init_pair (CLR_DISABLED, COLOR_BLUE, COLOR_BLUE);
	init_pair (CLR_MESSAGE, COLOR_GREEN, COLOR_BLUE);
	init_pair (CLR_NUMBERS, COLOR_WHITE, COLOR_BLUE);

	/* windows */
	main_win = newwin (LINES - 4, COLS, 0, 0);
	keypad (main_win, TRUE);
	info_win = newwin (4, COLS, LINES - 4, 0);
	wbkgd (main_win, COLOR_PAIR(CLR_ITEM));
	wbkgd (info_win, COLOR_PAIR(CLR_ITEM));

	msg_timeout = time(NULL) + 3;
	reset_file_info ();
	set_interface_status (NULL);
	xterm_set_state (STATE_STOP);
	xterm_set_title ("");
	
	main_border ();
	get_server_options ();
	
	if (arg_num)
		process_args (args, arg_num);
	else
		enter_first_dir ();
	
	menu_draw (menu);
	wrefresh (main_win);
	update_state ();
}

/* Get error message from the server and show it. */
static void update_error ()
{
	char *err;
	
	send_int_to_srv (CMD_GET_ERROR);
	err = get_data_str ();
	interface_error (err);
	free (err);
}

/* Handle server event. */
static void server_event (const int event)
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
		case EV_ERROR:
			update_error ();
			break;
		default:
			interface_message ("Unknown event: 0x%02x", event);
			logit ("Unknown event 0x%02x", event);
	}

	if (must_update_state) {
		update_state ();
		must_update_state = 0;
	}
	if (must_update_error) {
		update_error ();
		must_update_error = 0;
	}
}

/* Send the playlist and request playing this item. */
static void play_it (const int plist_pos)
{
	char *file = plist_get_file (visible_plist, plist_pos);

	assert (file != NULL);
	
	send_playlist (visible_plist);
	send_int_to_srv (CMD_PLAY);
	send_str_to_srv (file);
	free (file);
}

/* Switch between the current playlist and the playlist
 * (curr_plist/playlist). */
static void toggle_plist ()
{
	if (visible_plist == playlist) {
		if (!cwd[0])
			
			/* we were at the playlist from the startup */
			enter_first_dir ();
		else {
			visible_plist = curr_plist;
			menu_free (menu);
			menu = saved_menu;
			set_title (cwd);
			update_curr_file ();
			set_interface_status (NULL);
		}
	}
	else if (playlist && playlist->num) {
		char msg[50];

		visible_plist = playlist;
		saved_menu = menu;
		menu = make_menu (playlist, NULL, NULL);
		set_title ("Playlist");
		update_curr_file ();
		sprintf (msg, "%d files on the list", plist_count(playlist));
		set_interface_status (msg);
	}
	else
		interface_error ("The playlist is empty.");

	wrefresh (info_win);
}

/* Action when the user selected a file. */
static void go_file ()
{
	struct menu_item *menu_item = menu_curritem (menu);

	if (menu_item->type == F_SOUND)
		play_it (menu_item->plist_pos);
	else if (menu_item->type == F_DIR && visible_plist == curr_plist) {
		char dir[PATH_MAX + 1];
		
		if (!strcmp(menu_item->file, "..")) {
			char *slash;

			strcpy (dir, cwd);				
			slash = strrchr (dir, '/');
			assert (slash != NULL);
			*slash = 0;
		}
		else
			strcpy (dir, menu_item->file);

		go_to_dir (dir);
	}
	else if (menu_item->type == F_DIR && visible_plist == playlist)
		
		/* the only item on the playlist of type F_DIR is '..' */
		toggle_plist ();
	else if (menu_item->type == F_PLAYLIST) {
		if (plist_count(playlist)) {
			interface_error ("Please clear the playlist, because "
					"I'm not sure you want to do this.");
			return;
		}

		plist_clear (playlist);
		if (plist_load_m3u(playlist, menu_item->file))
			interface_message ("Playlist loaded.");
		toggle_plist ();
	}
}

/* pause/unpause */
static void switch_pause ()
{
	send_int_to_srv (CMD_GET_STATE);
	switch (get_data_int()) {
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

static void toggle_option (const char *name)
{
	send_int_to_srv (CMD_SET_OPTION);
	send_str_to_srv (name);
	send_int_to_srv (!options_get_int(name));
	sync_int_option (name);
	update_info_win ();
	wrefresh (info_win);
}

/* Add the current selected file to the playlist. */
static void add_file_plist ()
{
	struct menu_item *menu_item = menu_curritem (menu);

	if (visible_plist == playlist) {
		interface_error ("Can't add to the playlist a file from the "
				"playlist.");
		return;
	}

	if (menu_item->plist_pos == -1) {
		interface_error ("To add a directory, use the 'A' command.");
		return;
	}

	if (plist_find_fname(playlist,
				curr_plist->items[menu_item->plist_pos].file)
			== -1)
		plist_add_from_item (playlist,
				&curr_plist->items[menu_item->plist_pos]);
	else
		interface_error ("The file is already on the playlist.");
}

/* Clear the playlist */
static void clear_playlist ()
{
	if (visible_plist == playlist)
		toggle_plist();
	plist_clear (playlist);
	interface_message ("The playlist was cleared.");
}

/* Recursively add the conted to a directory to the playlist. */
static void add_dir_plist ()
{
	struct menu_item *menu_item = menu_curritem (menu);
	char dir[PATH_MAX + 1];
	char msg[50];

	if (visible_plist == playlist) {
		interface_error ("Can't add to the playlist a file from the "
				"playlist.");
		return;
	}

	if (menu_item->plist_pos != -1) {
		interface_error ("To add a file, use the 'a' command.");
		return;
	}

	strcpy (dir, cwd);
	resolve_path (dir, sizeof(dir), menu_item->title);

	set_interface_status ("reading directories...");
	wrefresh (info_win);
	read_directory_recurr (dir, playlist);
	if (options_get_int("ReadTags")) {
		read_tags (playlist);
		make_titles_tags (playlist);
	}
	else
		make_titles_file (playlist);
	
	plist_sort_fname (playlist);

	sprintf (msg, "%d files on the list", plist_count(playlist));
	set_interface_status (msg);
	wrefresh (info_win);
}

static void set_mixer (int val)
{
	if (val < 0)
		val = 0;
	else if (val > 100)
		val = 100;

	send_int_to_srv (CMD_SET_MIXER);
	send_int_to_srv (val);

	draw_mixer ();
	wrefresh (info_win);
}

static void seek (const int sec)
{
	send_int_to_srv (CMD_SEEK);
	send_int_to_srv (sec);
}

static void print_help_screen ()
{
	wclear (main_win);
	wbkgd (main_win, COLOR_PAIR(CLR_ITEM));

	mvwprintw (main_win, 0, 0,
"  UP, DOWN     Move up and down in the menu\n"
"  PAGE UP/DOWN Move one page up/down\n"
"  HOME, END    Move to the first, last item\n"
"  ENTER        Start playing files (from this file) or go to directory\n"
"  s            Stop playing\n"
"  n            Next song\n"
"  p, SPACE     Pause/unpause\n"
"  LEFT, RIGHT  Seek backward, forward\n"
"  h            Show this help screen\n"
"  f            Switch between short and full names\n"
"  m            Go to the music directory (requires an entry in the config)\n"
"  a/A          Add file to the playlist / Add directory recursively\n"
"  d/C          Delete item from the playlist / Clear the playlist\n"
"  l            Switch between playlist and file list\n"
"  S/R/X        Switch shuffle / repeat / autonext\n"
"  '.' , ','    Increase, decrease volume by 5%\n"
"  '>' , '<'    Increase, decrease volume by 1%\n"
"  M            Hide error/informative message\n"
"  H            Switch ShowHiddenFiles option\n"
"  ^r           Refresh the screen\n"
"  r            Reread directory content\n"
"  q            Detach MOC from the server\n"
"  Q            Quit\n");

	wrefresh (main_win);
}

static void help_screen ()
{
	main_win_mode = WIN_HELP;
	print_help_screen ();
}

/* Make new menu titles using titles from the items. */
static void update_menu_titles (struct menu *menu, struct plist *plist)
{
	int i;

	assert (menu != NULL);

	for (i = 0; i < menu->nitems; i++)
		if (menu->items[i]->plist_pos != -1) {
			free (menu->items[i]->title);
			menu->items[i]->title = xstrdup (
					plist->items[menu->items[i]->plist_pos].title);
		}
}

/* Switch ReadTags options and update the menu. */
static void switch_read_tags ()
{
	if (options_get_int("ReadTags")) {
		option_set_int("ReadTags", 0);
		make_titles_file (curr_plist);
		make_titles_file (playlist);
	}
	else {
		option_set_int("ReadTags", 1);
		read_tags (playlist);
		make_titles_tags (playlist);
		read_tags (curr_plist);
		make_titles_tags (curr_plist);
	}

	if (visible_plist == curr_plist)
		update_menu_titles (menu, curr_plist);
	else
	{
		update_menu_titles (menu, playlist);
		update_menu_titles (saved_menu, curr_plist);
	}
	update_curr_file ();
}

/* Reread the directory. */
static void reread_dir ()
{
	int selected_item = menu->selected;
	int top_item = menu->top;

	go_to_dir (NULL);
	menu_set_top_item (menu, top_item);
	menu_setcurritem (menu, selected_item);
}

static void delete_item ()
{
	struct menu_item *menu_item;
	int selected_item;
	int top_item;

	if (visible_plist != playlist) {
		interface_error ("You can only delete an item from the "
				"playlist.");
		return;
	}

	assert (playlist->num > 0);

	selected_item = menu->selected;
	top_item = menu->top;
	
	menu_item = menu_curritem (menu);
	send_int_to_srv (CMD_DELETE);
	send_str_to_srv (playlist->items[menu_item->plist_pos].file);

	plist_delete (playlist, menu_item->plist_pos);
	if (plist_count(playlist) > 0) {
		char msg[50];
		
		menu_free (menu);
		menu = make_menu (playlist, NULL, 0);
		menu_set_top_item (menu, top_item);
		menu_setcurritem (menu, selected_item);
		update_curr_file ();
		sprintf (msg, "%d files on the list", plist_count(playlist));
		set_interface_status (msg);
		wrefresh (info_win);
	}
	else {
		toggle_plist ();
		plist_clear (playlist);
	}
}

static void update_menu ()
{
	werase (main_win);
	main_border ();
	menu_draw (menu);
	wrefresh (main_win);
}

/* Turn on search file mode. */
void search_file_mode_on ()
{
	search_file_mode = 1;
	search_string[0] = 0;
	draw_search_entry ();
	wrefresh (info_win);
}

void search_file_key (const int ch)
{
	if (isgraph(ch) || ch == ' ') {
		int item;
		int len = strlen (search_string);
	
		if (len == MAX_SEARCH_STRING)
			return;
		search_string[len++] = ch;
		search_string[len] = 0;

		item = menu_find_pattern_next (menu, search_string,
				menu_get_selected(menu));

		if (item != -1) {
			menu_setcurritem (menu, item);
			update_menu ();
			draw_search_entry ();
		}
		else
			search_string[len-1] = 0;
	}
	else if (ch == KEY_BACKSPACE) {

		/* delete last character */
		if (search_string[0] != 0) {
			int len = strlen (search_string);

			search_string[len-1] = 0;
			draw_search_entry ();
		}
	}
	else if (ch == CTRL('x')) {

		/* exit file search */
		search_file_mode = 0;
		update_info_win ();
	}
	else if (ch == CTRL('g')) {
		int item;

		/* Find next matching */
		item = menu_find_pattern_next (menu, search_string,
				menu_next_turn(menu));

		menu_setcurritem (menu, item);
		update_menu ();
		draw_search_entry ();
	}
	else if (ch == '\n') {

		/* go to the file */
		go_file ();
		search_file_mode = 0;
		update_info_win ();
		update_menu ();
	}
	wrefresh (info_win);
}

/* Handle key */
static void menu_key (const int ch)
{
	int do_update_menu = 0;

	if (main_win_mode == WIN_HELP) {

		/* Switch to menu */
		werase (main_win);
		main_border ();
		menu_update_size (menu, main_win);
		menu_draw (menu);
		update_info_win ();
		wrefresh (main_win);
		wrefresh (info_win);
		
		main_win_mode = WIN_MENU;
	}
	else if (search_file_mode) {
		search_file_key (ch);
		return;
	}
	else {
		switch (ch) {
			case 'q':
				want_quit = 1;
				send_int (srv_sock, CMD_DISCONNECT);
				break;
			case '\n':
				go_file ();
				do_update_menu = 1;
				break;
			case KEY_DOWN:
				menu_driver (menu, REQ_DOWN);
				do_update_menu = 1;
				break;
			case KEY_UP:
				menu_driver (menu, REQ_UP);
				do_update_menu = 1;
				break;
			case KEY_NPAGE:
				menu_driver (menu, REQ_PGDOWN);
				do_update_menu = 1;
				break;
			case KEY_PPAGE:
				menu_driver (menu, REQ_PGUP);
				do_update_menu = 1;
				break;
			case KEY_HOME:
				menu_driver (menu, REQ_TOP);
				do_update_menu = 1;
				break;
			case KEY_END:
				menu_driver (menu, REQ_BOTTOM);
				do_update_menu = 1;
				break;
			case 'Q':
				send_int_to_srv (CMD_QUIT);
				want_quit = 1;
				break;
			case 's':
				send_int_to_srv (CMD_STOP);
				break;
			case 'n':
				send_int_to_srv (CMD_NEXT);
				break;
			case 'p':
			case ' ':
				switch_pause ();
				break;
			case 'f':
				switch_read_tags ();
				do_update_menu = 1;
				break;
			case 'S':
				toggle_option ("Shuffle");
				break;
			case 'R':
				toggle_option ("Repeat");
				break;
			case 'X':
				toggle_option ("AutoNext");
				break;
			case 'l':
				toggle_plist ();
				do_update_menu = 1;
				break;
			case 'a':
				add_file_plist ();
				break;
			case 'C':
				clear_playlist ();
				do_update_menu = 1;
				break;
			case 'A':
				add_dir_plist ();
				break;
			case '<':
				set_mixer (get_mixer() - 1);
				break;
			case ',':
				set_mixer (get_mixer() - 5);
				break;
			case '.':
				set_mixer (get_mixer() + 5);
				break;
			case '>':
				set_mixer (get_mixer() + 1);
				break;
			case KEY_LEFT:
				seek (-1);
				break;
			case KEY_RIGHT:
				seek (1);
				break;
			case 'h':
				help_screen ();
				break;
			case 'M':
				interface_message (NULL);
				update_info_win ();
				wrefresh (info_win);
				break;
			case CTRL('r'):
				wclear (info_win);
				update_info_win ();
				wrefresh (info_win);
				wclear (main_win);
				do_update_menu = 1;
				break;
			case 'r':
				if (visible_plist == curr_plist) {
					reread_dir ();
					do_update_menu = 1;
				}
				break;
			case 'H':
				option_set_int ("ShowHiddenFiles",
						!options_get_int(
							"ShowHiddenFiles"));
				if (visible_plist == curr_plist) {
					reread_dir ();
					do_update_menu = 1;
				}
				break;
			case 'm':
				if (options_get_str("MusicDir")) {
					go_to_dir (options_get_str(
								"MusicDir"));
					do_update_menu = 1;
				}
				else
					interface_error ("MusicDir not "
							"defined");
				break;
			case 'd':
				delete_item ();
				do_update_menu = 1;
				break;
			case CTRL('g'):
				search_file_mode_on ();
				break;
			case KEY_RESIZE:
				break;
			default:
				interface_error ("Bad key");
		}

		if (do_update_menu)
			update_menu ();
	}
}

#ifdef SIGWINCH
/* Initialize the screen again after resizeing xterm */
static void do_resize ()
{
	check_term_size ();
	endwin ();
	refresh ();
	keypad (main_win, TRUE);
	wresize (main_win, LINES - 4, COLS);
	wresize (info_win, 4, COLS);
	mvwin (info_win, LINES - 4, 0);
	werase (main_win);

	if (main_win_mode == WIN_MENU) {
		main_border ();
		menu_update_size (menu, main_win);
		menu_draw (menu);
		update_info_win ();	
		wrefresh (main_win);
	}
	else
		print_help_screen ();

	wrefresh (info_win);
	logit ("resize");
	want_resize = 0;
}
#endif

void interface_loop ()
{
	while (!want_quit) {
		fd_set fds;
		int ret;
		struct timeval timeout = { 1, 0 };
		
		FD_ZERO (&fds);
		FD_SET (srv_sock, &fds);
		FD_SET (STDIN_FILENO, &fds);

		ret = select (srv_sock + 1, &fds, NULL, NULL, &timeout);
		
		if (ret == 0) {
			if (msg_timeout && msg_timeout < time(NULL)
					&& !msg_is_error) {
				update_info_win ();
				wrefresh (info_win);
				msg_timeout = 0;
			}
		}
		else if (ret == -1 && !want_quit && errno != EINTR)
			interface_fatal ("select() failed");

#ifdef SIGWINCH
		if (want_resize)
			do_resize ();
#endif

		if (ret > 0) {
			if (FD_ISSET(STDIN_FILENO, &fds))
				menu_key (wgetch(main_win));
			if (FD_ISSET(srv_sock, &fds))
				server_event (get_int_from_srv());
		}
	}

	plist_free (curr_plist);
	plist_free (playlist);
	menu_free (menu);
	free (curr_plist);
	free (playlist);
}

/* Save the current directory path to a file. */
static void save_curr_dir ()
{
	FILE *dir_file;

	if (!(dir_file = fopen(create_file_name("last_directory"), "w"))) {
		interface_error ("Can't save current directory: %s",
				strerror(errno));
		return;
	}

	fprintf (dir_file, "%s", cwd);
	fclose (dir_file);
}

void interface_end ()
{
	save_curr_dir ();
	close (srv_sock);
	endwin ();
	xterm_clear_title ();
}
