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

#define DEBUG

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

/* Socket connection to the server. */
static int srv_sock = -1;

/* If the user presses quit, or we receive a termination signal. */
static volatile int want_quit = 0;

#ifdef SIGWINCH
/* If we get SIGWINCH. */
static volatile int want_resize = 0;
#endif

/* Are we running on xterm? */
static int has_xterm = 0;

/* xterm title */
struct
{
	char title[256];
	int state;
} xterm_title;

static char cwd[PATH_MAX] = "";

enum colour_index
{
	CLR_BACKGROUND,
	CLR_FRAME,
	CLR_WIN_TITLE,
	CLR_MENU_ITEM_DIR,
	CLR_MENU_ITEM_DIR_SELECTED,
	CLR_MENU_ITEM_PLAYLIST,
	CLR_MENU_ITEM_PLAYLIST_SELECTED,
	CLR_MENU_ITEM_FILE,
	CLR_MENU_ITEM_FILE_SELECTED,
	CLR_MENU_ITEM_FILE_MARKED,
	CLR_MENU_ITEM_FILE_MARKED_SELECTED,
	CLR_MENU_ITEM_INFO,
	CLR_STATUS,
	CLR_TITLE,
	CLR_STATE,
	CLR_TIME_CURRENT,
	CLR_TIME_LEFT,
	CLR_TIME_TOTAL,
	CLR_TIME_LEFT_FRAMES,
	CLR_SOUND_PARAMS,
	CLR_LEGEND,
	CLR_INFO_DISABLED,
	CLR_INFO_ENABLED,
	CLR_BAR_EMPTY,
	CLR_BAR_FILL,
	CLR_ENTRY,
	CLR_ENTRY_TITLE,
	CLR_ERROR,
	CLR_MESSAGE,
	CLR_LAST, /* Fake element to get number of collors */
	CLR_WRONG
};
static int colours[CLR_LAST];

static WINDOW *main_win = NULL;
static WINDOW *info_win = NULL;

static char mainwin_title[512];
static int msg_timeout = 0;
static int msg_is_error = 0;
static char message[512] = "Welcome to "PACKAGE_STRING"! "
	"Press h for the list of commands.";
static char interface_status[STATUS_LINE_LEN + 1] = "              ";

static struct plist *curr_plist = NULL; /* Current directory */
static struct plist *playlist = NULL; /* The playlist */
static struct plist *visible_plist = NULL; /* The playlist the user sees */
static struct menu *curr_plist_menu = NULL;
static struct menu *playlist_menu = NULL;
static struct menu *curr_menu = NULL;

static enum
{
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

/* When we are waiting for data from the server, events can occur. We can't 
 * handle them while waiting, so we push them on the queue. */
#define EVENTS_MAX	10
static struct
{
	int queue[EVENTS_MAX];
	int num;
} events = { {}, 0 };

enum entry_type
{
	ENTRY_DISABLED,
	ENTRY_SEARCH,
	ENTRY_PLIST_SAVE
};

/* User entry. */
static struct
{
	char text[512];
	enum entry_type type;
	char title[32];
	int width;
} entry = {
	"",
	ENTRY_DISABLED,
	"",
	0
};

/* ^c version of c */
#ifndef CTRL
# define CTRL(c) ((c) & 0x1F)
#endif

#ifndef KEY_ESCAPE
# define KEY_ESCAPE	27
#endif

static char *help_text[] = {
"       " PACKAGE_STRING,
"",
"  UP, DOWN     Move up and down in the menu",
"  PAGE UP/DOWN Move one page up/down",
"  HOME, END    Move to the first, last item",
"  ENTER        Start playing files (from this file) or go to directory",
"  s            Stop playing",
"  n            Next song",
"  p, SPACE     Pause/unpause",
"  LEFT, RIGHT  Seek backward, forward",
"  h            Show this help screen",
"  f            Switch between short and full names",
"  m            Go to the music directory (requires an entry in the config)",
"  a/A          Add file to the playlist / Add directory recursively",
"  d/C          Delete item from the playlist / Clear the playlist",
"  l            Switch between playlist and file list",
"  S/R/X        Switch shuffle / repeat / autonext",
"  '.' , ','    Increase, decrease volume by 5%",
"  '>' , '<'    Increase, decrease volume by 1%",
"  M            Hide error/informative message",
"  H            Switch ShowHiddenFiles option",
"  ^r           Refresh the screen",
"  r            Reread directory content",
"  q            Detach MOC from the server",
"  V            Save the playlist in the current directory",
"  g            Search the menu. The following commands can be used in this",
"               entry:",
"      CTRL-g   Find next",
"      CTRL-x   Exit the entry",
"  CTRL-t       Toggle ShowTime option",
"  CTRL-f       Toggle ShowFormat option",
"  Q            Quit"
};
static int help_screen_top = 0;
#define HELP_LINES	((int)(sizeof(help_text)/sizeof(help_text[0])))

#ifdef HAVE_ATTRIBUTE__
static void interface_fatal (const char *format, ...)
	__attribute__ ((format (printf, 1, 2)));
#else
static void interface_fatal (const char *format, ...);
#endif

static void interface_fatal (const char *format, ...)
{
	char err_msg[512];
	va_list va;
	
	va_start (va, format);
	vsnprintf (err_msg, sizeof(err_msg), format, va);
	err_msg[sizeof(err_msg) - 1] = 0;
	va_end (va);

	logit ("FATAL ERROR: %s", err_msg);
	endwin ();
	fatal ("%s", err_msg);
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
	/* Border */
	wattrset (main_win, colours[CLR_FRAME]);
	wborder (main_win, ACS_VLINE, ACS_VLINE, ACS_HLINE, ' ',
			ACS_ULCORNER, ACS_URCORNER, ACS_VLINE, ACS_VLINE);

	/* The title */
	wmove (main_win, 0, COLS / 2 - strlen(mainwin_title) / 2 - 1);
	
	wattrset (main_win, colours[CLR_FRAME]);
	waddch (main_win, ACS_RTEE);
	
	wattrset (main_win, colours[CLR_WIN_TITLE]);
	waddstr (main_win, mainwin_title);
	
	wattrset (main_win, colours[CLR_FRAME]);
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
	wattrset (info_win, colours[CLR_FRAME]);
	wborder (info_win, ACS_VLINE, ACS_VLINE, ACS_HLINE, ACS_HLINE,
			ACS_LTEE, ACS_RTEE, ACS_LLCORNER, ACS_LRCORNER);
	
	wmove (info_win, 2, 25);
	wattrset (info_win, colours[CLR_LEGEND]);
	waddstr (info_win, "KHz    Kbps");
}

static void draw_interface_status ()
{
	int i;
	
	wattrset (info_win, colours[CLR_STATUS]);
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

/* Push an event on the queue if it's not already there. */
static void event_push (const int event)
{
	int i;

	for (i = 0; i < events.num; i++)
		if (events.queue[i] == event) {
			debug ("Not adding event 0x%02x, it's already in the "
					"queue.", event);
			return;
		}

	assert (events.num < EVENTS_MAX - 1);
	events.queue[events.num++] = event;

	debug ("Added event 0x%02x to the queue", event);
}

/* Get an integer value from the server that will arrive after EV_DATA. */
static int get_data_int ()
{
	int event;

	while (1) {
		event = get_int_from_srv ();
		if (event == EV_DATA)
			return get_int_from_srv ();
		else
			event_push (event);
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

	wattrset (info_win, colours[CLR_FRAME]);
	mvwaddch (info_win, 0, COLS - 23, ACS_RTEE);
	mvwaddch (info_win, 0, COLS - 2, ACS_LTEE);

	wattrset (info_win, colours[CLR_BAR_FILL]);
	mvwaddnstr (info_win, 0, COLS - 22, bar, vol / 5);

	wattrset (info_win, colours[CLR_BAR_EMPTY]);
	mvwaddstr (info_win, 0, COLS - 22 + (vol / 5), bar + vol / 5);
}

static void entry_draw ()
{
	wmove (info_win, 0, 1);
	wattrset (info_win, colours[CLR_ENTRY_TITLE]);
	wprintw (info_win, "%s: ", entry.title);
	
	wattrset (info_win, colours[CLR_ENTRY]);
	wprintw (info_win, "%-*s", entry.width - strlen(entry.title) - 2,
			entry.text);
}

static void make_entry (const enum entry_type type, const char *title)
{
	entry.type = type;
	entry.text[0] = 0;
	strncpy (entry.title, title, sizeof(entry.title));
	entry.width = COLS - strlen(title) - 4;
	entry_draw ();
	wrefresh (info_win);
}

static void entry_disable ()
{
	entry.type = ENTRY_DISABLED;
}

/* Update the info win */
static void update_info_win ()
{
	werase (info_win);
	info_border ();

	/* Show message it it didn't expire yet */
	if (time(NULL) <= msg_timeout) {
		wattrset (info_win, msg_is_error ? colours[CLR_ERROR]
				: colours[CLR_MESSAGE]);
		mvwaddnstr (info_win, 1, 1, message, COLS - 2);
	}
	else {

		/* The title */
		wattrset (info_win, colours[CLR_TITLE]);
		mvwaddnstr (info_win, 1, 4, file_info.title,
				COLS - 5);

		/* State of playing */
		wattrset (info_win, colours[CLR_STATE]);
		mvwaddstr (info_win, 1, 1, file_info.state);
	}

	/* Current time */
	wattrset (info_win, colours[CLR_TIME_CURRENT]);
	wmove (info_win, 2, 1);
	waddstr (info_win, file_info.curr_time);

	/* Time left */
	if (*file_info.time_left) {
		wattrset (info_win, colours[CLR_TIME_LEFT]);
		waddch (info_win, ' ');
		waddstr (info_win, file_info.time_left);
	}

	/* Total_time */
	if (file_info.time[0] != 0) {
		wmove (info_win, 2, 13);
		
		wattrset (info_win, colours[CLR_TIME_LEFT_FRAMES]);
		waddch (info_win, '[');
		
		wattrset (info_win, colours[CLR_TIME_TOTAL]);
		wprintw (info_win, "%s", file_info.time);
		
		wattrset (info_win, colours[CLR_TIME_LEFT_FRAMES]);
		waddch (info_win, ']');
	}

	wattrset (info_win, colours[CLR_SOUND_PARAMS]);

	/* Rate */
	wmove (info_win, 2, 25 - strlen(file_info.rate));
	waddstr (info_win, file_info.rate);
	
	/* Bitrate */
	wmove (info_win, 2, 32 - strlen(file_info.bitrate));
	waddstr (info_win, file_info.bitrate);

	/* Channels */
	wmove (info_win, 2, 38);
	if (file_info.channels == 2)
		wattrset (info_win, colours[CLR_INFO_ENABLED]);
	else
		wattrset (info_win, colours[CLR_INFO_DISABLED]);
	waddstr (info_win, "[STEREO]");
	
	/* Shuffle & repeat */
	wmove (info_win, 2, COLS - sizeof("[SHUFFLE] [REPEAT] [NEXT]"));
	if (options_get_int("Shuffle"))
		wattrset (info_win, colours[CLR_INFO_ENABLED]);
	else
		wattrset (info_win, colours[CLR_INFO_DISABLED]);
	waddstr (info_win, "[SHUFFLE] ");

	if (options_get_int("Repeat"))
		wattrset (info_win, colours[CLR_INFO_ENABLED]);
	else
		wattrset (info_win, colours[CLR_INFO_DISABLED]);
	waddstr (info_win, "[REPEAT] ");

	if (options_get_int("AutoNext"))
		wattrset (info_win, colours[CLR_INFO_ENABLED]);
	else
		wattrset (info_win, colours[CLR_INFO_DISABLED]);
	waddstr (info_win, "[NEXT]");
	
	if (entry.type != ENTRY_DISABLED)
		entry_draw ();
	else {
		/* Status line */
		wattrset (info_win, colours[CLR_FRAME]);
		mvwaddch (info_win, 0, 5, ACS_RTEE);
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

static void set_iface_status_ref (const char *msg)
{
	set_interface_status (msg);
	wrefresh (info_win);
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

/* Convert time in second to min:sec text format. */
static void sec_to_min (char *buff, const int seconds)
{
	int min, sec;

	min = seconds / 60;
	sec = seconds % 60;

	snprintf (buff, 6, "%02d:%02d", min, sec);
}

/* Make menu using the playlist and directory table. */
static struct menu *make_menu (struct plist *plist, struct file_list *dirs,
		struct file_list *playlists)
{
	int i;
	int menu_pos;
	struct menu_item **menu_items;
	struct menu *menu;
	int plist_items;
	int nitems;
	int read_time = !strcasecmp(options_get_str("ShowTime"), "yes");
	
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
	menu_items[0] = menu_newitem ("../", -1, F_DIR, "..");
	menu_item_set_attr_normal (menu_items[0], colours[CLR_MENU_ITEM_DIR]);
	menu_item_set_attr_sel (menu_items[0],
			colours[CLR_MENU_ITEM_DIR_SELECTED]);
	menu_pos = 1;
	
	if (dirs)
		for (i = 0; i < dirs->num; i++) {
			char title[PATH_MAX];

			strcpy (title, strrchr(dirs->items[i], '/') + 1);
			strcat (title, "/");
			
			menu_items[menu_pos] =
				menu_newitem (title, -1, F_DIR, dirs->items[i]);
			menu_item_set_attr_normal (menu_items[menu_pos],
					colours[CLR_MENU_ITEM_DIR]);
			menu_item_set_attr_sel (menu_items[menu_pos],
					colours[CLR_MENU_ITEM_DIR_SELECTED]);
			menu_pos++;
		}

	if (playlists)
		for (i = 0; i < playlists->num; i++){
			menu_items[menu_pos] = menu_newitem (
					strrchr(playlists->items[i], '/') + 1,
					-1, F_PLAYLIST,	playlists->items[i]);
			menu_item_set_attr_normal (menu_items[menu_pos],
					colours[CLR_MENU_ITEM_PLAYLIST]);
			menu_item_set_attr_sel (menu_items[menu_pos],
					colours[
					CLR_MENU_ITEM_PLAYLIST_SELECTED]);
			menu_pos++;
		}
	
	/* playlist items */
	for (i = 0; i < plist->num; i++) {
		if (!plist_deleted(plist, i)) {
			menu_items[menu_pos] = menu_newitem (
					plist->items[i].title, i, F_SOUND,
					plist->items[i].file);
			
			menu_item_set_attr_normal (menu_items[menu_pos],
					colours[CLR_MENU_ITEM_FILE]);
			menu_item_set_attr_sel (menu_items[menu_pos],
					colours[CLR_MENU_ITEM_FILE_SELECTED]);
			menu_item_set_attr_marked (menu_items[menu_pos],
					colours[CLR_MENU_ITEM_FILE_MARKED]);
			menu_item_set_attr_sel_marked (menu_items[menu_pos],
					colours[
					CLR_MENU_ITEM_FILE_MARKED_SELECTED]);
			
			menu_item_set_format (menu_items[menu_pos],
					format_name(plist->items[i].file));

			if (read_time)
				plist->items[i].tags = read_file_tags (
						plist->items[i].file,
						plist->items[i].tags,
						TAGS_TIME);
			
			if (plist->items[i].tags
					&& plist->items[i].tags->time != -1) {
				char time_str[6];
				
				sec_to_min (time_str,
						plist->items[i].tags->time);
				menu_item_set_time (menu_items[menu_pos]
						,time_str);
			}
				
			menu_pos++;
		}
	}
	
	menu = menu_new (main_win, menu_items, nitems);
	menu_set_show_format (menu, options_get_int("ShowFormat"));
	menu_set_show_time (menu,
			strcasecmp(options_get_str("ShowTime"), "no"));
	menu_set_info_attr (menu, colours[CLR_MENU_ITEM_INFO]);
	return menu;
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
		else
			event_push (event);
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
		int i;
		
		if (curr_plist_menu
				&& (i = plist_find_fname(curr_plist, file))
				!= -1)
			menu_mark_plist_item (curr_plist_menu, i);

		if (playlist_menu
				&&(i = plist_find_fname(playlist, file))
				!= -1)
			menu_mark_plist_item (playlist_menu, i);
		
		if (main_win_mode == WIN_MENU)
			menu_draw (curr_menu);
	}
	else {
		if (curr_plist_menu)
			menu_unmark_item (curr_plist_menu);
		if (playlist_menu)
			menu_unmark_item (playlist_menu);
	}
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

/* Find the item on curr_plist and playlist, return 1 if found, 0 otherwise.
 * Pointer to the plist structure and the index are filled.
 * If the file exists on both lists, choose the list where the file has
 * needed_tags. */
static int find_item_plists (const char *file, int *index, struct plist **plist,
		const int needed_tags)
{
	int curr_plist_idx, playlist_idx;

	if ((curr_plist_idx = plist_find_fname(curr_plist, file)) != -1
			&& curr_plist->items[curr_plist_idx].tags->filled
			& needed_tags) {
		*index = curr_plist_idx;
		*plist = curr_plist;
		return 1;
	}
	
	if ((playlist_idx = plist_find_fname(playlist, file)) != -1) {
		*index = playlist_idx;
		*plist = playlist;
		return 1;
	}
	else if (curr_plist_idx != -1) {
		*index = curr_plist_idx;
		*plist = curr_plist;
		return 1;
	}

	return 0;
}

/* Find the title for a file. Check if it's on the playlist, if not, try to
 * make the title. Returned memory is malloc()ed. */
static char *find_title (char *file)
{
	/* remember last file to avoid probably exepensive read_file_tags() */
	static char *cache_file = NULL;
	static char *cache_title = NULL;
	
	int index;
	struct file_tags *tags;
	char *title = NULL;
	struct plist *plist;

	if (cache_file && !strcmp(cache_file, file)) {
		debug ("Using cache");
		return xstrdup (cache_title);
	}
	else
		debug ("Getting file title for %s", file);
	
	if (find_item_plists(file, &index, &plist, TAGS_COMMENTS)) {
		debug ("Found title on the playlist");
		plist->items[index].tags = read_file_tags (file,
				plist->items[index].tags, TAGS_COMMENTS);
		if (plist->items[index].tags->title)
			title = build_title (plist->items[index].tags);
	}
	else if ((tags = read_file_tags(file, NULL, TAGS_COMMENTS))) {
		if (tags->title)
			title = build_title (tags);
		tags_free (tags);
	}
	
	if (!title)
		title = strrchr(file, '/') ? xstrdup (strrchr(file, '/') + 1)
			: xstrdup (file);

	if (cache_file) {
		free (cache_file);
		free (cache_title);
	}

	cache_file = xstrdup (file);
	cache_title = title;

	return xstrdup (title);
}

static void set_time (const int time)
{
	file_info.time_num = time;
	if (time != -1)
		sec_to_min (file_info.time, time);
	else
		strcpy (file_info.time, "00:00");
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
	else {
		debug ("Cleared bitrate");
		file_info.bitrate[0] = 0;
	}

	update_info_win ();
	wrefresh (info_win);
}

static void update_ctime ()
{
	int ctime;
	int left;
	
	send_int_to_srv (CMD_GET_CTIME);
	ctime = get_data_int ();

	sec_to_min (file_info.curr_time, ctime);

	if (file_info.time_num != -1) {
		left = file_info.time_num - ctime;
		sec_to_min (file_info.time_left, left > 0 ? left : 0);
	}
	else
		file_info.time_left[0] = 0;

	update_info_win ();
	wrefresh (info_win);
}

/* Update time in the menus and items for playlist and curr_plist for the given
 * file. */
void update_times (const char *file, const int time)
{
	char time_str[6];
	int i;

	sec_to_min (time_str, time);

	if ((i = plist_find_fname(curr_plist, file)) != -1) {
		curr_plist->items[i].tags = read_file_tags (
				curr_plist->items[i].file,
				curr_plist->items[i].tags, 0);
		curr_plist->items[i].tags->time = time;
		if (curr_plist_menu)
			menu_item_set_time_plist (curr_plist_menu, i, time_str);
	}
	
	if ((i = plist_find_fname(playlist, file)) != -1) {
		playlist->items[i].tags = read_file_tags (
				playlist->items[i].file,
				playlist->items[i].tags, 0);
		playlist->items[i].tags->time = time;
		if (playlist_menu)
			menu_item_set_time_plist (playlist_menu, i, time_str);
	}
}

/* Return the file time, or -1 on error. */
static int get_file_time (char *file)
{
	/* To remember last file time - counting time can be expensive. */
	static char *cache_file = NULL;
	static int cache_time = -1;
	
	int index;
	struct file_tags *tags;
	int ftime = -1;
	struct plist *plist;

	if (cache_file && !strcmp(cache_file, file)) {
		debug ("Using cache");
		return cache_time;
	}
	else
		debug ("Getting file time for %s", file);
	
	if (find_item_plists(file, &index, &plist, TAGS_TIME)) {
		debug ("Found item on the playlist");
		plist->items[index].tags = read_file_tags (file,
				plist->items[index].tags, TAGS_TIME);
		ftime = plist->items[index].tags->time;
		
		update_times (file, plist->items[index].tags->time);
	}
	else if ((tags = read_file_tags(file, NULL, TAGS_TIME))) {
		ftime = tags->time;
		tags_free (tags);
	}

	if (cache_file)
		free (cache_file);
	cache_file = xstrdup (file);
	cache_time = ftime;
	
	return ftime;
}

/* Update the name of the currently played file. */
static void update_curr_file ()
{
	char *file;

	send_int_to_srv (CMD_GET_SNAME);
	file = get_data_str ();

	if (playlist_menu)
		menu_unmark_item (playlist_menu);
	if (curr_plist_menu)
		menu_unmark_item (curr_plist_menu);

	if (file[0]) {
		char *title = find_title (file);

		strncpy (file_info.title, title,
				sizeof(file_info.title) - 1);
		file_info.title[sizeof(file_info.title)-1] = 0;
		set_time (get_file_time(file));
		xterm_set_title (file_info.title);
		mark_file (file);
		free (title);
	}
	else {
		file_info.title[0] = 0;
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

	update_curr_file ();
	
	if (main_win_mode == WIN_MENU) {
		menu_draw (curr_menu);
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

	set_iface_status_ref ("reading directory...");

	if (dir && is_subdir(dir, cwd)) {
		strcpy (last_dir, strrchr(cwd, '/') + 1);
		strcat (last_dir, "/");
		going_up = 1;
	}

	old_curr_plist = curr_plist;
	curr_plist = (struct plist *)xmalloc (sizeof(struct plist));
	plist_init (curr_plist);
	dirs = file_list_new ();
	playlists = file_list_new ();

	if (!read_directory(new_dir, dirs, playlists, curr_plist)) {
		set_iface_status_ref (NULL);
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

	if (curr_plist_menu)
		menu_free (curr_plist_menu);
	
	if (dir)
		strcpy (cwd, dir);

	if (options_get_int("ReadTags")) {
		set_iface_status_ref ("Reading tags...");
		read_tags (curr_plist);
		make_titles_tags (curr_plist);
	}
	else
		make_titles_file (curr_plist);
	
	plist_sort_fname (curr_plist);
	qsort (dirs->items, dirs->num, sizeof(char *), qsort_dirs_func);
	qsort (playlists->items, playlists->num, sizeof(char *),
			qsort_strcmp_func);
	
	curr_plist_menu = make_menu (curr_plist, dirs, playlists);
	curr_menu = curr_plist_menu;
	file_list_free (dirs);
	file_list_free (playlists);
	if (going_up)
		menu_setcurritem_title (curr_menu, last_dir);
	
	set_title (cwd);

	update_state ();
	sprintf (msg, "%d files and directories", curr_plist_menu->nitems - 1);
	set_iface_status_ref (msg);

	return 1;
}

/* Make new cwd path from CWD and this path */
static void make_path (char *path)
{
	if (path[0] == '/')
		strcpy (cwd, "/"); /* for absolute path */
	else if (!cwd[0]) {
		if (!getcwd(cwd, sizeof(cwd)))
			interface_fatal ("Can't get CWD: %s", strerror(errno));
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
		char this_cwd[PATH_MAX];
		
		if (!getcwd(this_cwd, sizeof(cwd)))
			interface_fatal ("Can't get CWD: %s.", strerror(errno));

		for (i = 0; i < num; i++) {
			char path[2*PATH_MAX];
			int dir = isdir(args[i]);

			if (args[i][0] == '/')
				strcpy (path, "/");
			else
				strcpy (path, this_cwd);
			resolve_path (path, sizeof(path), args[i]);

			if (dir == 1)
				read_directory_recurr (path, playlist);
			else if (dir == 0 && is_sound_file(path))
				plist_add (playlist, path);
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

			playlist_menu = make_menu (playlist, NULL, 0);
			curr_menu = playlist_menu;
			set_title ("Playlist");
			update_curr_file ();
			sprintf (msg, "%d files on the list",
					plist_count(playlist));
			set_interface_status (msg);
		}
		else
			enter_first_dir ();
	}
}

/* Load the playlist from .moc directory. */
static void load_playlist ()
{
	char *plist_file = create_file_name ("playlist.m3u");

	set_iface_status_ref ("Loading playlist...");
	if (file_type(plist_file) == F_PLAYLIST)
		plist_load (playlist, plist_file, cwd);
	set_iface_status_ref (NULL);
}

/* Initialize a colour item of given index (CLR_*) with colours and
 * attributes. */
static void make_colour (const enum colour_index index, const short foreground,
		const short background,	const int attr)
{
	static int pair = 1;

	assert (pair < COLOR_PAIRS);
	assert (index < CLR_LAST);

	init_pair (pair, foreground, background);
	colours[index] = COLOR_PAIR (pair) | attr;

	pair++;
}

static void set_default_colours ()
{
	make_colour (CLR_BACKGROUND, COLOR_WHITE, COLOR_BLUE, A_NORMAL);
	make_colour (CLR_FRAME, COLOR_WHITE, COLOR_BLUE, A_NORMAL);
	make_colour (CLR_WIN_TITLE, COLOR_WHITE, COLOR_BLUE, A_NORMAL);
	make_colour (CLR_MENU_ITEM_DIR, COLOR_WHITE, COLOR_BLUE, A_BOLD);
	make_colour (CLR_MENU_ITEM_DIR_SELECTED, COLOR_WHITE, COLOR_BLACK,
			A_BOLD);
	make_colour (CLR_MENU_ITEM_PLAYLIST, COLOR_WHITE, COLOR_BLUE, A_BOLD);
	make_colour (CLR_MENU_ITEM_PLAYLIST_SELECTED, COLOR_WHITE, COLOR_BLACK,
			A_BOLD);
	make_colour (CLR_MENU_ITEM_FILE, COLOR_WHITE, COLOR_BLUE, A_NORMAL);
	make_colour (CLR_MENU_ITEM_FILE_SELECTED, COLOR_WHITE,
			COLOR_BLACK, A_NORMAL);
	make_colour (CLR_MENU_ITEM_FILE_MARKED, COLOR_GREEN, COLOR_BLUE,
			A_BOLD);
	make_colour (CLR_MENU_ITEM_FILE_MARKED_SELECTED, COLOR_GREEN,
			COLOR_BLACK, A_BOLD);
	make_colour (CLR_MENU_ITEM_INFO, COLOR_BLUE, COLOR_BLUE, A_BOLD);
	make_colour (CLR_STATUS, COLOR_WHITE, COLOR_BLUE, A_NORMAL);
	make_colour (CLR_TITLE, COLOR_WHITE, COLOR_BLUE, A_BOLD);
	make_colour (CLR_STATE, COLOR_WHITE, COLOR_BLUE, A_BOLD);
	make_colour (CLR_TIME_CURRENT, COLOR_WHITE, COLOR_BLUE, A_BOLD);
	make_colour (CLR_TIME_LEFT, COLOR_WHITE, COLOR_BLUE, A_BOLD);
	make_colour (CLR_TIME_LEFT_FRAMES, COLOR_WHITE, COLOR_BLUE, A_NORMAL);
	make_colour (CLR_TIME_TOTAL, COLOR_WHITE, COLOR_BLUE, A_BOLD);
	make_colour (CLR_SOUND_PARAMS, COLOR_WHITE, COLOR_BLUE, A_BOLD);
	make_colour (CLR_LEGEND, COLOR_WHITE, COLOR_BLUE, A_NORMAL);
	make_colour (CLR_INFO_DISABLED, COLOR_BLUE, COLOR_BLUE, A_BOLD);
	make_colour (CLR_INFO_ENABLED, COLOR_WHITE, COLOR_BLUE, A_BOLD);
	make_colour (CLR_BAR_EMPTY, COLOR_WHITE, COLOR_BLUE, A_NORMAL);
	make_colour (CLR_BAR_FILL, COLOR_BLACK, COLOR_CYAN, A_NORMAL);
	make_colour (CLR_ENTRY, COLOR_WHITE, COLOR_BLUE, A_NORMAL);
	make_colour (CLR_ENTRY_TITLE, COLOR_BLACK, COLOR_CYAN, A_BOLD);
	make_colour (CLR_ERROR, COLOR_RED, COLOR_BLUE, A_BOLD);
	make_colour (CLR_MESSAGE, COLOR_GREEN, COLOR_BLUE, A_BOLD);
}

static void theme_parse_error (const int line, const char *msg)
{
	interface_fatal ("Parse error in theme file line %d: %s", line, msg);
}

/* Find the index of a colour element by name. Return CLR_WRONG if not found. */
static enum colour_index find_colour_element_name (const char *name)
{
	int i;
	static struct
	{
		char *name;
		enum colour_index idx;
	} colour_tab[] = {
		{ "background",		CLR_BACKGROUND },
		{ "frame",		CLR_FRAME },
		{ "window_title",	CLR_WIN_TITLE },
		{ "directory",		CLR_MENU_ITEM_DIR },
		{ "selected_directory", CLR_MENU_ITEM_DIR_SELECTED },
		{ "playlist",		CLR_MENU_ITEM_PLAYLIST },
		{ "selected_playlist",	CLR_MENU_ITEM_PLAYLIST_SELECTED },
		{ "file",		CLR_MENU_ITEM_FILE },
		{ "selected_file",	CLR_MENU_ITEM_FILE_SELECTED },
		{ "marked_file",	CLR_MENU_ITEM_FILE_MARKED },
		{ "marked_selected_file", CLR_MENU_ITEM_FILE_MARKED_SELECTED },
		{ "info",		CLR_MENU_ITEM_INFO },
		{ "status",		CLR_STATUS },
		{ "title",		CLR_TITLE },
		{ "state",		CLR_STATE },
		{ "current_time",	CLR_TIME_CURRENT },
		{ "time_left",		CLR_TIME_LEFT },
		{ "total_time",		CLR_TIME_TOTAL },
		{ "time_left_frames",	CLR_TIME_LEFT_FRAMES },
		{ "sound_parameters",	CLR_SOUND_PARAMS },
		{ "legend",		CLR_LEGEND },
		{ "disabled",		CLR_INFO_DISABLED },
		{ "enabled",		CLR_INFO_ENABLED },
		{ "empty_bar",		CLR_BAR_EMPTY },
		{ "filled_bar",		CLR_BAR_FILL },
		{ "entry",		CLR_ENTRY },
		{ "entry_title",	CLR_ENTRY_TITLE },
		{ "error",		CLR_ERROR },
		{ "message",		CLR_MESSAGE }
	};
	assert (name != NULL);

	for (i = 0; i < (int)(sizeof(colour_tab)/sizeof(colour_tab[0])); i++)
		if (!strcasecmp(colour_tab[i].name, name))
			return colour_tab[i].idx;

	return CLR_WRONG;
}

/* Find the curses colour by name. Return -1 if the colour is unknown. */
static short find_colour_name (const char *name)
{
	int i;
	static struct
	{
		char *name;
		short colour;
	} colour_tab[] = {
		{ "black",	COLOR_BLACK },
		{ "red",	COLOR_RED },
		{ "green",	COLOR_GREEN },
		{ "yellow",	COLOR_YELLOW },
		{ "blue",	COLOR_BLUE },
		{ "magenta",	COLOR_MAGENTA },
		{ "cyan",	COLOR_CYAN },
		{ "white",	COLOR_WHITE },

	};
	
	for (i = 0; i < (int)(sizeof(colour_tab)/sizeof(colour_tab[0])); i++)
		if (!strcasecmp(colour_tab[i].name, name))
			return colour_tab[i].colour;
	
	return -1;
}

static void load_colour_theme (const char *fname)
{
	FILE *file;
	char path[PATH_MAX];
	char *line;
	int line_num = 0;

	if (fname[0] != '/') {
		if (snprintf(path, sizeof(path), "themes/%s", fname)
				>= (int)sizeof(path))
			interface_fatal ("Theme path too long!");
	}

	if (!(file = fopen(fname[0] == '/' ? fname : create_file_name(path)
					, "r")))
		interface_fatal ("Can't open theme file: %s", strerror(errno));

	/* The lines should be in format:
	 * ELEMENT = FOREGROUND BACKGROUND [ATTRIBUTE[,ATTRIBUTE,..]]
	 * Blank lines and beginning with # are ignored, see example_theme. */

	while ((line = read_line(file))) {
		char *name;
		char *tmp;
		char *foreground, *background, *attributes;
		int curses_attr = 0;
		enum colour_index element;
		short clr_fore, clr_back;
		
		line_num++;
		if (line[0] == '#' || !(name = strtok(line, " \t"))) {

			/* empty line or a comment */
			free (line);
			continue;
		}

		if (!(tmp = strtok(NULL, " \t")) || strcmp(tmp, "="))
			theme_parse_error (line_num, "expected '='");
		if (!(foreground = strtok(NULL, " \t")))
			theme_parse_error (line_num, "foreground colour not "
					"specified");
		if (!(background = strtok(NULL, " \t")))
			theme_parse_error (line_num, "background colour not "
					"specified");
		if ((attributes = strtok(NULL, " \t"))) {
			char *attr;

			if ((tmp = strtok(NULL, " \t")))
				theme_parse_error (line_num,
						"unexpected chars at the end "
						"of line");
			
			attr = strtok (attributes, ",");

			do {
				if (!strcasecmp(attr, "normal"))
					curses_attr |= A_NORMAL;
				else if (!strcasecmp(attr, "standout"))
					curses_attr |= A_STANDOUT;
				else if (!strcasecmp(attr, "underline"))
					curses_attr |= A_UNDERLINE;
				else if (!strcasecmp(attr, "reverse"))
					curses_attr |= A_REVERSE;
				else if (!strcasecmp(attr, "blink"))
					curses_attr |= A_BLINK;
				else if (!strcasecmp(attr, "dim"))
					curses_attr |= A_DIM;
				else if (!strcasecmp(attr, "bold"))
					curses_attr |= A_BOLD;
				else if (!strcasecmp(attr, "protected"))
					curses_attr |= A_PROTECT;
				else
					theme_parse_error (line_num,
							"unknown attribute");
			} while ((attr = strtok(NULL, ",")));
		}

		if ((element = find_colour_element_name(name)) == CLR_WRONG)
			theme_parse_error (line_num, "unknown element");
		if ((clr_fore = find_colour_name(foreground)) == -1)
			theme_parse_error (line_num,
					"bad foreground colour name");
		if ((clr_back = find_colour_name(background)) == -1)
			theme_parse_error (line_num,
					"bad background colour name");

		make_colour (element, clr_fore, clr_back, curses_attr);
		
		free (line);
	}

	fclose (file);
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
	
	detect_term ();
	start_color ();
	set_default_colours ();

	if (options_get_str("Theme"))
		load_colour_theme (options_get_str("Theme"));

	/* windows */
	main_win = newwin (LINES - 4, COLS, 0, 0);
	keypad (main_win, TRUE);
	info_win = newwin (4, COLS, LINES - 4, 0);
	wbkgd (main_win, colours[CLR_BACKGROUND]);
	wbkgd (info_win, colours[CLR_BACKGROUND]);

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
	load_playlist ();
	
	menu_draw (curr_menu);
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
			curr_menu = curr_plist_menu;
			set_title (cwd);
			update_curr_file ();
			set_interface_status (NULL);
		}
	}
	else if (playlist && playlist->num) {
		char msg[50];

		visible_plist = playlist;
		if (!playlist_menu)
			playlist_menu = make_menu (playlist, NULL, NULL);
		curr_menu = playlist_menu;
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
	struct menu_item *menu_item = menu_curritem (curr_menu);

	if (menu_item->type == F_SOUND)
		play_it (menu_item->plist_pos);
	else if (menu_item->type == F_DIR && visible_plist == curr_plist) {
		char dir[PATH_MAX + 1];
		
		if (!strcmp(menu_item->file, "..")) {
			char *slash;

			strcpy (dir, cwd);				
			slash = strrchr (dir, '/');
			assert (slash != NULL);
			if (slash == dir)
				*(slash + 1) = 0;
			else
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
		set_iface_status_ref ("Loading playlist...");
		if (plist_load(playlist, menu_item->file, cwd))
			interface_message ("Playlist loaded.");
		set_iface_status_ref (NULL);
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
	struct menu_item *menu_item = menu_curritem (curr_menu);

	if (visible_plist == playlist) {
		interface_error ("Can't add to the playlist a file from the "
				"playlist.");
		return;
	}

	if (menu_item->type != F_SOUND) {
		interface_error ("To add a directory, use the 'A' command.");
		return;
	}

	if (plist_find_fname(playlist, menu_item->file) == -1) {
		plist_add_from_item (playlist,
				&curr_plist->items[menu_item->plist_pos]);
		if (playlist_menu) {
			menu_free (playlist_menu);
			playlist_menu = NULL;
		}
	}
	else
		interface_error ("The file is already on the playlist.");
}

/* Clear the playlist */
static void clear_playlist ()
{
	if (visible_plist == playlist)
		toggle_plist();
	plist_clear (playlist);
	if (playlist_menu) {
		menu_free (playlist_menu);
		playlist_menu = NULL;
	}
	interface_message ("The playlist was cleared.");
}

/* Recursively add the conted to a directory to the playlist. */
static void add_dir_plist ()
{
	struct menu_item *menu_item = menu_curritem (curr_menu);
	char msg[50];

	if (visible_plist == playlist) {
		interface_error ("Can't add to the playlist a file from the "
				"playlist.");
		return;
	}

	if (menu_item->type != F_DIR) {
		interface_error ("To add a file, use the 'a' command.");
		return;
	}

	if (curr_menu->selected == 0) {
		interface_error ("Can't add '..'.");
		return;
	}

	set_iface_status_ref ("reading directories...");
	read_directory_recurr (menu_item->file, playlist);
	if (options_get_int("ReadTags")) {
		read_tags (playlist);
		make_titles_tags (playlist);
	}
	else
		make_titles_file (playlist);
	
	if (playlist_menu) {
		menu_free (playlist_menu);
		playlist_menu = NULL;
	}

	sprintf (msg, "%d files on the list", plist_count(playlist));
	set_iface_status_ref (msg);
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
	int i;
	int max_lines = help_screen_top + LINES - 6;
	
	werase (main_win);
	wbkgd (main_win, colours[CLR_BACKGROUND]);

	wmove (main_win, 0, 0);
	if (help_screen_top != 0) {
		wattrset (main_win, colours[CLR_MESSAGE]);
		mvwaddstr (main_win, 0, COLS/2 - (sizeof("...MORE...")-1)/2,
				"...MORE...");
	}
	wmove (main_win, 1, 0);
	for (i = help_screen_top; i < max_lines && i < HELP_LINES; i++) {
		wattrset (main_win, colours[CLR_LEGEND]);
		waddstr (main_win, help_text[i]);
		waddch (main_win, '\n');
	}
	if (i != HELP_LINES) {
		wattrset (main_win, colours[CLR_MESSAGE]);
		mvwaddstr (main_win, LINES-5,
				COLS/2 - (sizeof("...MORE...")-1)/2,
				"...MORE...");
	}

	wrefresh (main_win);
}

static void help_screen ()
{
	help_screen_top = 0;
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
		set_iface_status_ref ("ReadTags: no");
	}
	else {
		option_set_int("ReadTags", 1);
		set_iface_status_ref ("Reading tags...");
		read_tags (playlist);
		make_titles_tags (playlist);
		read_tags (curr_plist);
		make_titles_tags (curr_plist);
		set_iface_status_ref ("ReadTags: yes");
	}

	if (playlist_menu)
		update_menu_titles (playlist_menu, playlist);
	if (curr_plist_menu)
		update_menu_titles (curr_plist_menu, curr_plist);
}

/* Reread the directory. */
static void reread_dir ()
{
	int selected_item = curr_menu->selected;
	int top_item = curr_menu->top;

	go_to_dir (NULL);
	menu_set_top_item (curr_menu, top_item);
	menu_setcurritem (curr_menu, selected_item);
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

	selected_item = curr_menu->selected;
	top_item = curr_menu->top;
	
	menu_item = menu_curritem (curr_menu);
	send_int_to_srv (CMD_DELETE);
	send_str_to_srv (playlist->items[menu_item->plist_pos].file);

	plist_delete (playlist, menu_item->plist_pos);
	if (plist_count(playlist) > 0) {
		char msg[50];
		
		menu_free (playlist_menu);
		playlist_menu = make_menu (playlist, NULL, 0);
		menu_set_top_item (playlist_menu, top_item);
		menu_setcurritem (playlist_menu, selected_item);
		curr_menu = playlist_menu;
		update_curr_file ();
		sprintf (msg, "%d files on the list", plist_count(playlist));
		set_iface_status_ref (msg);
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
	menu_draw (curr_menu);
	wrefresh (main_win);
}

static int entry_search_key (const int ch)
{
	if (isgraph(ch) || ch == ' ') {
		int item;
		int len = strlen (entry.text);
	
		if (len == entry.width)
			return 1;
		entry.text[len++] = ch;
		entry.text[len] = 0;

		item = menu_find_pattern_next (curr_menu, entry.text,
				menu_get_selected(curr_menu));

		if (item != -1) {
			menu_setcurritem (curr_menu, item);
			update_menu ();
			entry_draw ();
		}
		else
			entry.text[len-1] = 0;
		return 1;
	}
	if (ch == CTRL('g')) {
		int item;

		/* Find next matching */
		item = menu_find_pattern_next (curr_menu, entry.text,
				menu_next_turn(curr_menu));

		menu_setcurritem (curr_menu, item);
		update_menu ();
		return 1;
	}
	else if (ch == '\n') {

		/* go to the file */
		go_file ();
		entry_disable ();
		update_info_win ();
		update_menu ();

		return 1;
	}

	return 0;
}

/* Handle keys specific to ENTRY_PLIST_SAVE. Return 1 if a key was handled. */
static int entry_plist_save_key (const int ch)
{
	if (ch == '\n') {
		if (strchr(entry.text, '/'))
			interface_error ("Only file name is accepted, not a "
					"path");
		else {
			char *ext = ext_pos (entry.text);
			char path[PATH_MAX];

			entry_disable ();
			if (!ext || strcmp(ext, "m3u"))
				strncat (entry.text, ".m3u", sizeof(entry.text)
						- strlen(entry.text) - 1);

			path[sizeof(path)-1] = 0;
			if (snprintf(path, sizeof(path), "%s/%s", cwd,
						entry.text)
					>= (int)sizeof(path)) {
				interface_error ("Path too long!");
				return 1;
			}

			set_iface_status_ref ("Saving the playlist...");
			if (plist_save(playlist, path, cwd))
				interface_message ("Playlist saved.");
			set_iface_status_ref (NULL);
		}
		update_info_win ();
		return 1;
	}
	return 0;
}

/* Handle common entry key. Return 1 if a key was handled. */
static int entry_common_key (const int ch)
{
	if (isgraph(ch) || ch == ' ') {
		int len = strlen (entry.text);
	
		if (len == entry.width)
			return 1;
		
		entry.text[len++] = ch;
		entry.text[len] = 0;
		entry_draw ();
		return 1;
	}
	if (ch == CTRL('x') || ch == KEY_ESCAPE) {
		entry_disable ();
		update_info_win ();
		return 1;
	}
	if (ch == KEY_BACKSPACE) {

		/* delete last character */
		if (entry.text[0] != 0) {
			int len = strlen (entry.text);

			entry.text[len-1] = 0;
			entry_draw ();
		}
		return 1;
	}

	return 0;
}

static void entry_key (const int ch)
{
	int handled = 0;
	
	if (entry.type == ENTRY_SEARCH)
		handled = entry_search_key(ch);
	if (entry.type == ENTRY_PLIST_SAVE)
		handled = entry_plist_save_key(ch);
	if (!handled)
		entry_common_key (ch);
	wrefresh (info_win);
}

/* Fill time in tags and menu items for all items in plist. */
static void fill_times (struct plist *plist, struct menu *menu)
{
	int i;

	for (i = 0; i < plist->num; i++)
		if (!plist_deleted(plist, i)) {		
			plist->items[i].tags = read_file_tags(
					plist->items[i].file,
					plist->items[i].tags,
					TAGS_TIME);
			if (menu && plist->items[i].tags->time != -1) {
				char time_str[6];

				sec_to_min (time_str,
						plist->items[i].tags->time);
				menu_item_set_time_plist (menu, i, time_str);
			}
		}
}

static void toggle_show_time ()
{
	if (!strcasecmp(options_get_str("ShowTime"), "yes")) {
		option_set_str("ShowTime", "IfAvailable");
		set_iface_status_ref ("ShowTime: IfAvailable");
	}
	else if (!strcasecmp(options_get_str("ShowTime"), "no")) {
		option_set_str("ShowTime", "yes");
		if (playlist_menu)
			menu_set_show_time (playlist_menu, 1);
		if (curr_plist_menu)
			menu_set_show_time (curr_plist_menu, 1);
		set_iface_status_ref ("Getting times...");
		fill_times (playlist, playlist_menu);
		fill_times (curr_plist, curr_plist_menu);
		set_iface_status_ref ("ShowTime: yes");
		
	}
	else { /* IfAvailable */
		option_set_str("ShowTime", "no");
		if (playlist_menu)
			menu_set_show_time (playlist_menu, 0);
		if (curr_plist_menu)
			menu_set_show_time (curr_plist_menu, 0);
		set_iface_status_ref ("ShowTime: no");
	}	
}

static void toggle_show_format ()
{
	int show_format = !options_get_int("ShowFormat");
	
	option_set_int ("ShowFormat", show_format);
	if (show_format)
		set_iface_status_ref ("ShowFormat: yes");
	else
		set_iface_status_ref ("ShowFormat: no");

	if (curr_plist_menu)
		menu_set_show_format (curr_plist_menu, show_format);
	if (playlist_menu)
		menu_set_show_format (playlist_menu, show_format);
}

/* Handle key */
static void menu_key (const int ch)
{
	int do_update_menu = 0;

	if (main_win_mode == WIN_HELP) {

		if (ch == KEY_DOWN || ch == KEY_NPAGE || ch == '\n') {
			if (help_screen_top + LINES - 5 <= HELP_LINES) {
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
	}
	else if (entry.type != ENTRY_DISABLED)
		entry_key (ch);
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
				menu_driver (curr_menu, REQ_DOWN);
				do_update_menu = 1;
				break;
			case KEY_UP:
				menu_driver (curr_menu, REQ_UP);
				do_update_menu = 1;
				break;
			case KEY_NPAGE:
				menu_driver (curr_menu, REQ_PGDOWN);
				do_update_menu = 1;
				break;
			case KEY_PPAGE:
				menu_driver (curr_menu, REQ_PGUP);
				do_update_menu = 1;
				break;
			case KEY_HOME:
				menu_driver (curr_menu, REQ_TOP);
				do_update_menu = 1;
				break;
			case KEY_END:
				menu_driver (curr_menu, REQ_BOTTOM);
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
			case 'g':
				make_entry (ENTRY_SEARCH, "SEARCH");
				break;
			case 'V':
				if (plist_count(playlist))
					make_entry (ENTRY_PLIST_SAVE,
							"SAVE PLAYLIST");
				else
					interface_error ("The playlist is "
							"empty.");
				break;
			case CTRL('t'):
				toggle_show_time ();
				do_update_menu = 1;
				break;
			case CTRL('f'):
				toggle_show_format ();
				do_update_menu = 1;
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
	entry.width = COLS - strlen(entry.title) - 4;

	if (curr_plist_menu)
		menu_update_size (curr_plist_menu, main_win);
	if (playlist_menu)
		menu_update_size (playlist_menu, main_win);

	if (main_win_mode == WIN_MENU) {
		main_border ();
		
		menu_draw (curr_menu);
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

/* Handle events from the queue. */
static void dequeue_events ()
{
	/* While handling events, new events could be added to the queue,
	 * we recognize such situation be checking if number of events has
	 * changed. */

	debug ("Dequeuing events...");

	while (events.num) {
		int i;
		int num_before = events.num; /* number of events can change
						during events handling */
		
		debug ("%d events pending", events.num);

		for (i = 0; i < num_before; i++) {
			int event = events.queue[i];

			/* "Mark" it as handled. When such event occur before
			 * we handle all events, we don't lose it in
			 * push_event(). */
			events.queue[i] = -1;
			server_event (event);
		}

		memmove (events.queue, events.queue + num_before,
				sizeof(int) * (events.num - num_before));
		events.num -= num_before;
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
			interface_fatal ("select() failed: %s", strerror(errno));

#ifdef SIGWINCH
		if (want_resize)
			do_resize ();
#endif

		if (ret > 0) {
			if (FD_ISSET(STDIN_FILENO, &fds))
				menu_key (wgetch(main_win));
			if (FD_ISSET(srv_sock, &fds))
				server_event (get_int_from_srv());
			dequeue_events ();
		}
	}
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

/* Save the playlist in .moc directory or remove the old playist if the
 * playlist is empty. */
static void save_playlist ()
{
	char *plist_file = create_file_name("playlist.m3u");

	if (plist_count(playlist)) {
		set_iface_status_ref ("Saving the playlist...");
		plist_save (playlist, plist_file, NULL);
		set_iface_status_ref (NULL);
	}
	else
		unlink (plist_file);
}

void interface_end ()
{
	save_curr_dir ();
	save_playlist ();
	close (srv_sock);
	endwin ();
	xterm_clear_title ();
	plist_free (curr_plist);
	plist_free (playlist);
	if (playlist_menu)
		menu_free (playlist_menu);
	if (curr_plist_menu)
		menu_free (curr_plist_menu);
	free (curr_plist);
	free (playlist);
}
