/*
 * MOC - music on console
 * Copyright (C) 2004,2005 Damian Pietras <daper@daper.net>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * Contributors:
 *  - Kamil Tarkowski <kamilt@interia.pl> - "back" command, sec_to_min_plist(),
 *  		fixes.
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
#include <sys/time.h>
#include <unistd.h>

#ifdef HAVE_NCURSES_H
# include <ncurses.h>
#elif HAVE_CURSES_H
# include <curses.h>
#endif

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
#include "decoder.h"
#include "interface.h"
#include "playlist_file.h"
#include "themes.h"
#include "keys.h"

#define STATUS_LINE_LEN	25
#define INTERFACE_LOG	"mocp_client_log"
#define HISTORY_MAX	50

/* Socket connection to the server. */
static int srv_sock = -1;

/* If the user presses quit, or we receive a termination signal. */
static volatile int want_quit = 0;

/* If user presses CTRL-C, set this to 1. This should interrupt long operations
 * that blocks the interface. */
static volatile int wants_interrupt = 0;

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

static WINDOW *main_win = NULL;
static WINDOW *info_win = NULL;

static char mainwin_title[512];
static int msg_timeout = 0;
static int msg_is_error = 0;
static char message[512] = "Welcome to "PACKAGE_STRING"! "
	"Press h for the list of commands.";
static char interface_status[STATUS_LINE_LEN + 1] = "              ";
static int help_screen_top; /* First visible line of the help screen. */

static struct plist *curr_plist = NULL; /* Current directory */
static struct plist *playlist = NULL; /* The playlist */
static struct plist *visible_plist = NULL; /* The playlist the user sees */
static struct menu *curr_plist_menu = NULL;
static struct menu *playlist_menu = NULL;
static struct menu *curr_menu = NULL;

/* Chars used to make lines (for borders etc.). */
static struct
{
	chtype vert;	/* vertical */
	chtype horiz;	/* horizontal */
	chtype ulcorn;	/* upper left corner */
	chtype urcorn;	/* upper right corner */
	chtype llcorn;	/* lower left corner */
	chtype lrcorn;	/* lower right corner */
	chtype rtee;	/* right tee */
	chtype ltee;	/* left tee */
} lines;

static enum
{
	WIN_MENU,
	WIN_HELP
} main_win_mode = WIN_MENU;

/* Information about the currently played file. */
static struct file_info {
	char title[256];
	char bitrate[5];
	char rate[3];
	char time[6];
	char curr_time[6];
	char time_left[6];
	int curr_time_num;
	int time_num;
	int channels;
	char state[3];
	int state_code;
	char *curr_file;
	struct file_tags *tags;
} file_info;

struct event_queue events;

enum entry_type
{
	ENTRY_DISABLED,
	ENTRY_SEARCH,
	ENTRY_PLIST_SAVE,
	ENTRY_GO_DIR,
	ENTRY_GO_URL,
	ENTRY_PLIST_OVERWRITE
};

/* User entry. */
static struct
{
	char text[512];		/* the text the user types */
	enum entry_type type;	/* type of the entry */
	char title[32];		/* displayed title */
	char *file;		/* optional: file associated with the entry */
	int width;		/* width of the entry part for typing */
	int cur_pos;		/* cursor position */
	int display_from;	/* displaying from this char */
} entry = {
	"",
	ENTRY_DISABLED,
	"",
	NULL,
	0,
	0,
	0
};

/* Silent seeking - where we are in seconds. -1 - no seeking. */
static int silent_seek_pos = -1;
static time_t silent_seek_key_last = (time_t)0; /* when the silent seek key was
						   last used */
static int waiting_for_plist_load = 0; /* Are we waiting for the playlist we
					  have loaded and sent to the clients?
					  */

static char mixer_name[15] = ""; /* mixer channel's name */

/* History of values in "Go" and "URL" entries. */
static char *files_history[HISTORY_MAX];
static int files_history_len = 0;
static char *urls_history[HISTORY_MAX];
static int urls_history_len = 0;

static void xterm_clear_title ()
{
	if (has_xterm)
		write (1, "\033]2;\007", sizeof("\033]2;\007")-1);
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
	xterm_clear_title ();
	endwin ();
	fatal ("%s", err_msg);
}

static void sig_quit (int sig ATTR_UNUSED)
{
	want_quit = 1;
}

static void sig_interrupt (int sig)
{
	logit ("Got signal %d: interrupt the operation", sig);
	wants_interrupt = 1;
}

/* Return 1 if user wants interrupt an operation by pressing CTRL-C. */
int user_wants_interrupt ()
{
	if (wants_interrupt)
		return 1;
	return 0;
}

static void clear_interrupt ()
{
	wants_interrupt = 0;
}

#ifdef SIGWINCH
static void sig_winch (int sig ATTR_UNUSED)
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
	wattrset (main_win, get_color(CLR_FRAME));
	wborder (main_win, lines.vert, lines.vert, lines.horiz, ' ',
			lines.ulcorn, lines.urcorn, lines.vert, lines.vert);

	/* The title */
	wmove (main_win, 0, COLS / 2 - strlen(mainwin_title) / 2 - 1);
	
	wattrset (main_win, get_color(CLR_FRAME));
	waddch (main_win, lines.rtee);
	
	wattrset (main_win, get_color(CLR_WIN_TITLE));
	waddstr (main_win, mainwin_title);
	
	wattrset (main_win, get_color(CLR_FRAME));
	waddch (main_win, lines.ltee);
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
	wattrset (info_win, get_color(CLR_FRAME));
	wborder (info_win, lines.vert, lines.vert, lines.horiz, lines.horiz,
			lines.ltee, lines.rtee, lines.llcorn, lines.lrcorn);
	
	wmove (info_win, 2, 25);
	wattrset (info_win, get_color(CLR_LEGEND));
	waddstr (info_win, "KHz     Kbps");
}

static void draw_interface_status ()
{
	int i;
	
	wattrset (info_win, get_color(CLR_STATUS));
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

/* Noblocking version of get_int_from_srv(): return 0 if there are no data. */
static int get_int_from_srv_noblock (int *num)
{
	enum noblock_io_status st;
	
	if ((st = get_int_noblock(srv_sock, num)) == NB_IO_ERR)
		interface_fatal ("Can't receive value from the server.");

	return st == NB_IO_OK ? 1 : 0;
}


static void send_item_to_srv (const struct plist_item *item)
{
	if (!send_item(srv_sock, item))
		interface_fatal ("Can't send() item to the server.");
}

/* Returned memory is malloc()ed. */
static char *get_str_from_srv ()
{
	char *str = get_str (srv_sock);
	
	if (!str)
		interface_fatal ("Can't receive string from the server.");

	return str;
}


static struct plist_item *recv_item_from_srv ()
{
	struct plist_item *item;

	if (!(item = recv_item(srv_sock)))
		interface_fatal ("Can't receive item from the server.");

	return item;
}

static struct file_tags *get_tags_from_srv ()
{
	struct file_tags *tags;

	if (!(tags = recv_tags(srv_sock)))
		interface_fatal ("Can't receive tags from the server.");

	return tags;
}

/* Wait for EV_DATA handling other events. */
static void wait_for_data ()
{
	int event;
	
	do {
		event = get_int_from_srv ();
		
		if (event == EV_PLIST_ADD)
			event_push (&events, event, recv_item_from_srv());
		else if (event == EV_PLIST_DEL || event == EV_STATUS_MSG)
			event_push (&events, event, get_str_from_srv());
		else if (event != EV_DATA)
			event_push (&events, event, NULL);
	 } while (event != EV_DATA);
}

/* Get an integer value from the server that will arrive after EV_DATA. */
static int get_data_int ()
{
	wait_for_data ();
	return get_int_from_srv ();
}

static int get_mixer ()
{
	send_int_to_srv (CMD_GET_MIXER);
	return get_data_int ();
}

/* Draw the mixer bar */
static void draw_mixer ()
{
	char bar[21];
	int vol;

	vol = get_mixer ();

	if (vol == -1)
		return;

	if (vol == 100)
		sprintf (bar, "%14s %d%% ", mixer_name, vol);
	else
		sprintf (bar, "%14s %02d%%  ", mixer_name, vol);

	wattrset (info_win, get_color(CLR_FRAME));
	mvwaddch (info_win, 0, COLS - 38, lines.rtee);
	mvwaddch (info_win, 0, COLS - 17, lines.ltee);

	wattrset (info_win, get_color(CLR_MIXER_BAR_FILL));
	mvwaddnstr (info_win, 0, COLS - 37, bar, vol / 5);

	wattrset (info_win, get_color(CLR_MIXER_BAR_EMPTY));
	mvwaddstr (info_win, 0, COLS - 37 + (vol / 5), bar + vol / 5);
}

/* Convert time in second to min:sec text format. buff must be 6 chars long. */
static void sec_to_min (char *buff, const int seconds)
{
	assert (seconds >= 0);

	if (seconds < 6000) {

		/* the time is less than 99:59 */
		int min, sec;
		
		min = seconds / 60;
		sec = seconds % 60;

		snprintf (buff, 6, "%02d:%02d", min, sec);
	}
	else if (seconds < 10000 * 60) 

		/* the time is less than 9999 minutes */
		snprintf (buff, 6, "%dm", seconds/60);
	else
		strcpy (buff, "!!!!!");
}

/* Convert time in second to min:sec text format(for total time in playlist).
 * buff must be 10 chars long. */
static void sec_to_min_plist (char *buff, const int seconds)
{
	assert (seconds >= 0);
	if (seconds < 999 * 60 * 60 - 1) {
		
		/* the time is less than 999 * 60 minutes */
		int hour, min, sec;
		hour = seconds / 3600;
		min  = (seconds / 60) % 60;
		sec  = seconds % 60;
		
		snprintf (buff, 10, "%03d:%02d:%02d", hour, min, sec);
	}
	else
		strcpy (buff, "!!!!!!!!!");
}

/* Draw the playlist time. */
static void draw_plist_time ()
{
	int all_files; /* Does the time count all files? */
	int time;
	char buf[10];
	
	wattrset (info_win, get_color(CLR_FRAME));
	mvwaddch (info_win, 0, COLS - 13, lines.rtee);
	mvwaddch (info_win, 0, COLS - 2, lines.ltee);

	time = plist_total_time (visible_plist, &all_files);
	
	wattrset (info_win, get_color(CLR_PLIST_TIME));
	sec_to_min_plist (buf, time);
	wmove (info_win, 0, COLS - 12);
	waddch (info_win, all_files ? ' ' : '>');
	wprintw (info_win, "%s", buf);
}

/* Draw the entry. Use this function at the end of screen drawing, because
 * Set the cursor position in the right place. */
static void entry_draw ()
{
	int c;
	
	wmove (info_win, 0, 1);
	wattrset (info_win, get_color(CLR_ENTRY_TITLE));
	wprintw (info_win, "%s:", entry.title);
	
	wattrset (info_win, get_color(CLR_ENTRY));

	/* Truncate the text, it must be shorter than the entry width */
	c = entry.text[entry.width + entry.display_from];
	entry.text[entry.width + entry.display_from] = 0;
	
	if (entry.display_from == 0) {
		
		/* the text fits into the screen */
		wprintw (info_win, " %-*s", entry.width, entry.text);
		wmove (info_win, 0, entry.cur_pos + strlen(entry.title) + 3);
	}
	else {

		/* the entry text is longer than the screen */
		wprintw (info_win, " %-*s", entry.width,
				entry.text + entry.display_from);
		wmove (info_win, 0, entry.cur_pos - entry.display_from
				+ strlen(entry.title) + 3);
	}

	/* Restore the truncated char */
	entry.text[entry.width + entry.display_from] = c;
}

static void make_entry (const enum entry_type type, const char *title)
{
	entry.type = type;
	entry.text[0] = 0;
	entry.file = NULL;
	strncpy (entry.title, title, sizeof(entry.title));
	entry.width = COLS - strlen(title) - 4;
	entry.cur_pos = 0;
	entry.display_from = 0;
	entry_draw ();
	curs_set (1);
	wrefresh (info_win);
}

/* Set the entry text. Move the cursor to the end. */
static void entry_set_text (const char *text)
{
	int len;
	
	strncpy (entry.text, text, sizeof(entry.text));
	entry.text[sizeof(entry.text)-1] = 0;
	len = strlen (entry.text);
	entry.cur_pos = len;
	
	if (entry.cur_pos - entry.display_from > entry.width)
		entry.display_from = len - entry.width;
}

/* Add a char to the entry where the cursor is placed. */
static void entry_add_char (const char c)
{
	unsigned int len = strlen(entry.text);

	if (len < sizeof(entry.text) - 1) {
		memmove (entry.text + entry.cur_pos + 1,
				entry.text + entry.cur_pos,
				len - entry.cur_pos + 1);
		
		entry.text[entry.cur_pos] = c;
		entry.cur_pos++;

		if (entry.cur_pos - entry.display_from > entry.width)
			entry.display_from++;
	}
}

/* Delete the char before the cursor. */
static void entry_back_space ()
{
	if (entry.cur_pos > 0) {
		int len = strlen (entry.text);
		
		memmove (entry.text + entry.cur_pos - 1,
				entry.text + entry.cur_pos,
				len - entry.cur_pos);
		entry.text[--len] = 0;
		entry.cur_pos--;

		if (entry.cur_pos < entry.display_from)
			entry.display_from--;

		/* Can we show more after deleting the char? */
		if (entry.display_from > 0
				&& len - entry.display_from < entry.width)
			entry.display_from--;
	}
}

/* Delete the char under the cursor. */
static void entry_del_char ()
{
	int len = strlen (entry.text);

	if (entry.cur_pos < len) {
		len--;
		memmove (entry.text + entry.cur_pos,
				entry.text + entry.cur_pos + 1,
				len - entry.cur_pos);
		entry.text[len] = 0;
		
		/* Can we show more after deleting the char? */
		if (entry.display_from > 0
				&& len - entry.display_from < entry.width)
			entry.display_from--;
	
	}
}

/* Move the cursor one char left. */
static void entry_curs_left ()
{
	if (entry.cur_pos > 0) {
		entry.cur_pos--;

		if (entry.cur_pos < entry.display_from)
			entry.display_from--;
	}
}

/* Move the cursor one char right. */
static void entry_curs_right ()
{
	int len = strlen (entry.text);
	
	if (entry.cur_pos < len) {
		entry.cur_pos++;

		if (entry.cur_pos > entry.width + entry.display_from)
			entry.display_from++;
	}
}

/* Move the cursor to the end of the entry text. */
static void entry_end ()
{
	int len = strlen(entry.text);
	
	entry.cur_pos = len;
	
	if (len > entry.width)
		entry.display_from = len - entry.width;
	else
		entry.display_from = 0;
}

/* Move the cursor to the beginning of the entry field. */
static void entry_home ()
{
	entry.display_from = 0;
	entry.cur_pos = 0;
}

static void entry_disable ()
{
	entry.type = ENTRY_DISABLED;
	if (entry.file) {
		free (entry.file);
		entry.file = NULL;
	}
	curs_set (0);
}

/* Draw the current time bar, or the silent seek time if silent_seek_pos >= 0.
 */
static void draw_curr_time_bar ()
{
	int i;
	int to_fill;
	int curr_time = silent_seek_pos >= 0
		? silent_seek_pos : file_info.curr_time_num;
	
	wattrset (info_win, get_color(CLR_FRAME));
	mvwaddch (info_win, 3, COLS - 2, lines.ltee);
	mvwaddch (info_win, 3, 1, lines.rtee);

	if (curr_time)

		/* The duration can be smaller than the current time, if the
		 * file was changed while playing. */
		to_fill =  curr_time <= file_info.time_num ?
			((float)curr_time / file_info.time_num)
			* (COLS - 4)
			: COLS - 4;
	else
		to_fill = 0;

	wattrset (info_win, get_color(CLR_TIME_BAR_FILL));
	for (i = 0; i < to_fill; i++)
		waddch (info_win, ' ');

	wattrset (info_win, get_color(CLR_TIME_BAR_EMPTY));
	while (i++ < COLS - 4)
		waddch (info_win, ' ');
}

static void draw_message ()
{
	int i, len;
	wattrset (info_win, msg_is_error ? get_color(CLR_ERROR)
			: get_color(CLR_MESSAGE));
	mvwaddnstr (info_win, 1, 1, message, COLS - 2);

	len = strlen (message);
	for (i = 0; i < COLS - 2 - len; i++)
		waddch (info_win, ' ');
}

/* Update the info win */
static void update_info_win ()
{
	werase (info_win);
	info_border ();

	/* Show message it it didn't expire yet */
	if (time(NULL) <= msg_timeout)
		draw_message ();
	else {

		/* The title */
		wattrset (info_win, get_color(CLR_TITLE));
		mvwaddnstr (info_win, 1, 4, file_info.title,
				COLS - 5);

		/* State of playing */
		wattrset (info_win, get_color(CLR_STATE));
		mvwaddstr (info_win, 1, 1, file_info.state);
	}

	/* Current time */
	wattrset (info_win, get_color(CLR_TIME_CURRENT));
	wmove (info_win, 2, 1);
	waddstr (info_win, file_info.curr_time);
	draw_curr_time_bar ();

	/* Time left */
	if (*file_info.time_left) {
		wattrset (info_win, get_color(CLR_TIME_LEFT));
		wmove (info_win, 2, 6);
		waddch (info_win, ' ');
		waddstr (info_win, file_info.time_left);
	}

	/* Total_time */
	if (file_info.time[0] != 0) {
		wmove (info_win, 2, 13);
		
		wattrset (info_win, get_color(CLR_TIME_TOTAL_FRAMES));
		waddch (info_win, '[');
		
		wattrset (info_win, get_color(CLR_TIME_TOTAL));
		wprintw (info_win, "%s", file_info.time);
		
		wattrset (info_win, get_color(CLR_TIME_TOTAL_FRAMES));
		waddch (info_win, ']');
	}

	wattrset (info_win, get_color(CLR_SOUND_PARAMS));

	/* Rate */
	wmove (info_win, 2, 25 - strlen(file_info.rate));
	waddstr (info_win, file_info.rate);
	
	/* Bitrate */
	wmove (info_win, 2, 33 - strlen(file_info.bitrate));
	waddstr (info_win, file_info.bitrate);

	/* Channels */
	wmove (info_win, 2, 38);
	if (file_info.channels == 2)
		wattrset (info_win, get_color(CLR_INFO_ENABLED));
	else
		wattrset (info_win, get_color(CLR_INFO_DISABLED));
	waddstr (info_win, "[STEREO]");
	
	/* Network stream */
	if (file_info.curr_file && is_url(file_info.curr_file))
		wattrset (info_win, get_color(CLR_INFO_ENABLED));
	else
		wattrset (info_win, get_color(CLR_INFO_DISABLED));
	waddstr (info_win, " [NET]");
	
	/* Shuffle & repeat */
	wmove (info_win, 2, COLS - sizeof("[SHUFFLE] [REPEAT] [NEXT]"));
	if (options_get_int("Shuffle"))
		wattrset (info_win, get_color(CLR_INFO_ENABLED));
	else
		wattrset (info_win, get_color(CLR_INFO_DISABLED));
	waddstr (info_win, "[SHUFFLE] ");

	if (options_get_int("Repeat"))
		wattrset (info_win, get_color(CLR_INFO_ENABLED));
	else
		wattrset (info_win, get_color(CLR_INFO_DISABLED));
	waddstr (info_win, "[REPEAT] ");

	if (options_get_int("AutoNext"))
		wattrset (info_win, get_color(CLR_INFO_ENABLED));
	else
		wattrset (info_win, get_color(CLR_INFO_DISABLED));
	waddstr (info_win, "[NEXT]");
	
	if (entry.type != ENTRY_DISABLED)
		entry_draw ();
	else {
		/* Status line */
		wattrset (info_win, get_color(CLR_FRAME));
		mvwaddch (info_win, 0, 5, lines.rtee);
		mvwaddch (info_win, 0, 5 + STATUS_LINE_LEN + 1, lines.ltee);
		
		draw_interface_status ();		
		draw_mixer ();
		draw_plist_time ();
	}
}

static void set_interface_status (const char *msg)
{
	assert (!msg || strlen(msg) <= STATUS_LINE_LEN);

	if (info_win) {
		if (msg)
			strncpy (interface_status, msg,
					sizeof(interface_status));
		else
			interface_status[0] = 0;
		draw_interface_status ();
	}
}

static void set_iface_status_ref (const char *msg)
{
	if (info_win) {
		set_interface_status (msg);
		wrefresh (info_win);
	}
}

void interface_error (const char *msg)
{
	strncpy (message, msg, sizeof(message) - 1);
	message[sizeof(message)-1] = 0;
	msg_timeout = time(NULL) + 3;
	msg_is_error = 1;
	
	/* The interface could have not been initialized yet. */
	if (main_win) {
		draw_message ();
		wrefresh (info_win);
	}
	else
		fprintf (stderr, "%s\n", message);

	logit ("ERROR: %s", message);
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
	file_info.curr_time_num = -1;
	file_info.channels = 1;
	strcpy (file_info.state, "[]");
	file_info.curr_file = NULL;
	file_info.tags = NULL;
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

/* Get the file time from the server. */
static int get_file_time_server (const char *file)
{
	int t;
	
	send_int_to_srv (CMD_GET_FTIME);
	send_str_to_srv (file);
	t = get_data_int ();

	debug ("Server time for %s: %d", file, t);
	return t;
}

static void read_item_time (struct plist_item *item)
{
	update_file (item);

	if (!item->tags)
		item->tags = tags_new ();

	if (!(item->tags->filled & TAGS_TIME)) {
		if ((item->tags->time = get_file_time_server(item->file)) == -1)
			item->tags = read_file_tags (item->file, item->tags,
					TAGS_TIME);
		else {
			debug ("Got time from the server.");
			item->tags->filled |= TAGS_TIME;
		}
	}
}

static int read_file_time (const char *file)
{
	int time;
	struct file_tags *tags;

	if ((time = get_file_time_server(file)) == -1) {
		tags = read_file_tags (file, NULL, TAGS_TIME);
		time = tags->time;
		tags_free (tags);
	}
	else
		debug ("Got time from the server.");

	return time;
}

/* Fill time in tags and menu items for all items in plist. If menu is NULL,
 * igore it. */
static void fill_times (struct plist *plist, struct menu *menu)
{
	int i;

	set_iface_status_ref ("Getting times...");
	
	for (i = 0; i < plist->num; i++)
		if (!plist_deleted(plist, i)) {
			if (user_wants_interrupt()) {
				error ("Getting times interrupted");
				break;
			}

			read_item_time (&plist->items[i]);

			if (menu && get_item_time(plist, i) != -1) {
				char time_str[6];
				sec_to_min (time_str, get_item_time(plist, i));
				menu_item_set_time_plist (menu, i, time_str);
			}
		}

	plist_count_total_time (plist);

	set_iface_status_ref (NULL);
}

/* Add an item from the playlist to the menu. */
static void add_to_menu (struct menu *menu, struct plist *plist, const int num)
{
	int added;
	struct plist_item *item = &plist->items[num];
	
	added = menu_add (menu, item->title, num, plist_file_type(plist, num),
			item->file);

	if (item->tags && item->tags->time != -1) {
		char time_str[6];
		
		sec_to_min (time_str, item->tags->time);
		menu_item_set_time (menu, added, time_str);
	}

	menu_item_set_attr_normal (menu, added, get_color(CLR_MENU_ITEM_FILE));
	menu_item_set_attr_sel (menu, added,
			get_color(CLR_MENU_ITEM_FILE_SELECTED));
	menu_item_set_attr_marked (menu, added,
			get_color(CLR_MENU_ITEM_FILE_MARKED));
	menu_item_set_attr_sel_marked (menu, added,
			get_color(CLR_MENU_ITEM_FILE_MARKED_SELECTED));
	
	menu_item_set_format (menu, added, file_type_name(item->file));
}

/* Make menu using the playlist and directory table. */
static struct menu *make_menu (struct plist *plist, struct file_list *dirs,
		struct file_list *playlists)
{
	int i;
	struct menu *menu;
	int plist_items;
	int added;
	int read_time = !strcasecmp(options_get_str("ShowTime"), "yes");
	
	plist_items = plist_count (plist);

	menu = menu_new (main_win);

	added = menu_add (menu, "../", -1, F_DIR, "..");
	menu_item_set_attr_normal (menu, added, get_color(CLR_MENU_ITEM_DIR));
	menu_item_set_attr_sel (menu, added,
			get_color(CLR_MENU_ITEM_DIR_SELECTED));
	
	if (dirs)
		for (i = 0; i < dirs->num; i++) {
			char title[PATH_MAX];

			strcpy (title, strrchr(dirs->items[i], '/') + 1);
			strcat (title, "/");
			
			added = menu_add (menu, title, -1, F_DIR,
					dirs->items[i]);
			menu_item_set_attr_normal (menu, added,
					get_color(CLR_MENU_ITEM_DIR));
			menu_item_set_attr_sel (menu, added,
					get_color(CLR_MENU_ITEM_DIR_SELECTED));
		}

	if (playlists)
		for (i = 0; i < playlists->num; i++){
			added = menu_add (menu,
					strrchr(playlists->items[i], '/') + 1,
					-1, F_PLAYLIST,	playlists->items[i]);
			menu_item_set_attr_normal (menu, added,
					get_color(CLR_MENU_ITEM_PLAYLIST));
			menu_item_set_attr_sel (menu, added,
					get_color(
					CLR_MENU_ITEM_PLAYLIST_SELECTED));
		}
	
	/* playlist items */
	for (i = 0; i < plist->num; i++) {
		if (!plist_deleted(plist, i))
			add_to_menu (menu, plist, i);
	}
	
	menu_set_show_format (menu, options_get_int("ShowFormat"));
	menu_set_show_time (menu,
			strcasecmp(options_get_str("ShowTime"), "no"));
	menu_set_info_attr (menu, get_color(CLR_MENU_ITEM_INFO));

	if (read_time)
		fill_times (plist, menu);

	return menu;
}

/* Check if dir2 is in dir1 */
static int is_subdir (char *dir1, char *dir2)
{
	return !strncmp(dir1, dir2, strlen(dir1)) ? 1 : 0;
}

/* Get a string value from the server that will arrive after EV_DATA. */
static char *get_data_str ()
{
	wait_for_data ();
	return get_str_from_srv ();
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

/* Mark this file from the playlist with this serial number in the menu. */
static void mark_file (const char *file)
{
	int i;

	assert (file != NULL);
	
	if (playlist_menu && (i = plist_find_fname(playlist, file)) != -1)
		menu_mark_plist_item (playlist_menu, i);
	if (curr_plist_menu && (i = plist_find_fname(curr_plist, file))
			!= -1)
		menu_mark_plist_item (curr_plist_menu, i);


	if (main_win_mode == WIN_MENU)
		menu_draw (curr_menu);
}

/* Update the xterm title. */
static void xterm_update_title ()
{
	write (1, "\033]0;", sizeof("\033]0;")-1);
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

static void xterm_set_state (int state)
{
	if (has_xterm) {
		xterm_title.state = state;
		xterm_update_title ();
	}
}

static void set_time (const int time)
{
	file_info.time_num = time;
	if (time != -1)
		sec_to_min (file_info.time, time);
	else
		strcpy (file_info.time, "00:00");
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
			set_time (-1);
			break;
		case STATE_PAUSE:
			strcpy (file_info.state, "||");
			break;
	}
	xterm_set_state (state);
}

/* Find the title_tags for a file. Check if it's on the playlist, if not, try
 * to make the title. Returned memory is malloc()ed. */
static char *find_title (char *file)
{
	/* remember last file to avoid probably exepensive read_file_tags() */
	static char *cache_file = NULL;
	static char *cache_title = NULL;
	
	int idx;
	char *title = NULL;

	if (is_url(file)) {
		if (!file_info.tags) {
			send_int_to_srv (CMD_GET_TAGS);
			wait_for_data ();
			file_info.tags = get_tags_from_srv ();
		}
		
		if (file_info.tags && file_info.curr_file
				&& !strcmp(file_info.curr_file, file))
			title = build_title (file_info.tags);

		return title;
	}
	
	if (cache_file && !strcmp(cache_file, file)) {
		debug ("Using cache");
		return xstrdup (cache_title);
	}
	else
		debug ("Getting file title for %s", file);

	if ((idx = plist_find_fname(curr_plist, file)) != -1
			&& (curr_plist->items[idx].title_tags
			   || (curr_plist->items[idx].tags
				   && curr_plist->items[idx].tags->filled
			   & TAGS_COMMENTS))) {
		debug ("Found title on the curr_plist");

		update_file (&curr_plist->items[idx]);

		if (!curr_plist->items[idx].title_tags
				&& curr_plist->items[idx].tags->title)
			curr_plist->items[idx].title_tags
				= build_title (curr_plist->items[idx].tags);

		title = xstrdup (curr_plist->items[idx].title_tags);
	}
	else if ((idx = plist_find_fname(playlist, file)) != -1) {
		debug ("Found title on the playlist");

		update_file (&playlist->items[idx]);
		playlist->items[idx].tags = read_file_tags (file,
				playlist->items[idx].tags, TAGS_COMMENTS);

		if (!playlist->items[idx].title_tags
				&& playlist->items[idx].tags->title)
			playlist->items[idx].title_tags
				= build_title (curr_plist->items[idx].tags);

		title = xstrdup (playlist->items[idx].title_tags);

	}
	else {
		struct file_tags *tags;

		tags = read_file_tags (file, NULL, TAGS_COMMENTS);
		if (tags->title)
			title = build_title (tags);
		tags_free (tags);
	}
	
	if (!title)
		title = !is_url(file) && strrchr(file, '/')
			? xstrdup (strrchr(file, '/') + 1) : xstrdup (file);

	if (cache_file) {
		free (cache_file);
		free (cache_title);
	}

	cache_file = xstrdup (file);
	cache_title = title;

	return xstrdup (title);
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
		if (bitrate < 9999) {
			snprintf (file_info.bitrate, sizeof(file_info.bitrate),
					"%d", bitrate);
			file_info.bitrate[sizeof(file_info.bitrate)-1] = 0;
		}
		else
			strcpy (file_info.bitrate, "!!!!");
			
	}
	else {
		debug ("Cleared bitrate");
		file_info.bitrate[0] = 0;
	}

	update_info_win ();
	wrefresh (info_win);
}

/* Update the current time. If silent_seek_pos >= 0, use this time instead of
 * the real time. */
static void update_ctime ()
{
	int left;
	int curr_time;
	
	if (silent_seek_pos >= 0)
		curr_time = silent_seek_pos;
	else {
		send_int_to_srv (CMD_GET_CTIME);
		curr_time = file_info.curr_time_num = get_data_int ();
	}

	sec_to_min (file_info.curr_time, curr_time);

	if (file_info.time_num != -1) {
		left = file_info.time_num - curr_time;
		sec_to_min (file_info.time_left, left > 0 ? left : 0);
	}
	else
		file_info.time_left[0] = 0;


	update_info_win ();
	wrefresh (info_win);
}

/* Update time in the menus and items for playlist and curr_plist for the given
 * file. */
static void update_times (const char *file, const int time)
{
	char time_str[6];
	int i;

	sec_to_min (time_str, time);

	if ((i = plist_find_fname(curr_plist, file)) != -1) {
		update_item_time (&curr_plist->items[i], time);
		if (curr_plist_menu)
			menu_item_set_time_plist (curr_plist_menu, i, time_str);
		plist_count_total_time (curr_plist);
	}
	
	if ((i = plist_find_fname(playlist, file)) != -1) {
		update_item_time (&playlist->items[i], time);

		if (playlist_menu)
			menu_item_set_time_plist (playlist_menu, i, time_str);
		
		plist_count_total_time (playlist);
	}
}

/* Return the file time, or -1 on error. Don't use the time from the
 * playlists. */
static int get_file_time (char *file)
{
	/* To remember last file time - counting time can be expensive. */
	static char *cache_file = NULL;
	static int cache_time = -1;
	static time_t cache_mtime = (time_t)-1;
	
	int ftime = -1;
	int file_mtime;

	if (is_url(file))
		return -1;
	
	file_mtime = get_mtime(file);

	if (cache_file && cache_time != -1 && !strcmp(cache_file, file)
			&& cache_mtime == file_mtime) {
		debug ("Using cache");
		update_times (file, cache_time);
		return cache_time;
	}
	else
		debug ("Getting file time for %s", file);

	ftime = read_file_time (file);
	if (ftime != -1)
		update_times (file, ftime);

	if (cache_file)
		free (cache_file);
	cache_file = xstrdup (file);
	cache_time = ftime;
	cache_mtime = file_mtime;
	
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

	if (!file_info.curr_file || strcmp(file_info.curr_file, file)) {
		if (file_info.curr_file)
			free (file_info.curr_file);
		if (file_info.tags) {
			tags_free (file_info.tags);
			file_info.tags = NULL;
		}

		if (file[0])
			file_info.curr_file = file;
		else {
			file_info.curr_file = NULL;
			free (file);
		}

		/* Silent seeking makes no sense if the playing file has
		 * changed */
		silent_seek_pos = -1;
	}
	else
		free (file);

	if (file_info.curr_file && file_info.curr_file[0]) {
		char *title = find_title (file_info.curr_file);

		strncpy (file_info.title, title,
				sizeof(file_info.title) - 1);
		file_info.title[sizeof(file_info.title)-1] = 0;
		set_time (get_file_time(file_info.curr_file));
		xterm_set_title (file_info.title);
		mark_file (file_info.curr_file);
		free (title);
	}
	else {
		file_info.title[0] = 0;
		xterm_set_title ("");
	}
}

/* Get and show the server state. */
static void update_state ()
{
	int new_state;
	
	/* play | stop | pause */
	send_int_to_srv (CMD_GET_STATE);
	new_state = get_data_int ();
	set_state (new_state);

	/* Silent seeking makes no sense if the state has changed. */
	if (new_state != file_info.state_code) {
		file_info.state_code = new_state;
		silent_seek_pos = -1;
	}

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

/* Load the directory content into curr_plist and make curr_plist_menu.
 * If dir is NULL, go to the cwd.
 * Return 1 on success, 0 on error. */
static int load_dir (char *dir)
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

	if (curr_plist_menu)
		menu_free (curr_plist_menu);
	
	if (dir)
		strcpy (cwd, dir);

	if (options_get_int("ReadTags")) {
		set_iface_status_ref ("Reading tags...");
		sync_plists_data (curr_plist, playlist);
		switch_titles_tags (curr_plist);
		sync_plists_data (playlist, curr_plist);
	}
	else
		switch_titles_file (curr_plist);
	
	plist_sort_fname (curr_plist);
	qsort (dirs->items, dirs->num, sizeof(char *), qsort_dirs_func);
	qsort (playlists->items, playlists->num, sizeof(char *),
			qsort_strcmp_func);
	
	curr_plist_menu = make_menu (curr_plist, dirs, playlists);
	file_list_free (dirs);
	file_list_free (playlists);
	if (going_up)
		menu_setcurritem_title (curr_plist_menu, last_dir);
	
	sprintf (msg, "%d files/directories", curr_plist_menu->nitems - 1);
	set_iface_status_ref (msg);

	return 1;
}

static int go_to_dir (char *dir)
{
	if (load_dir(dir)) {
		visible_plist = curr_plist;
		curr_menu = curr_plist_menu;
		set_title (cwd);
		update_state ();
		return 1;
	}

	return 0;
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

static void toggle_plist ();

/* Enter to the initial directory. */
static void enter_first_dir ()
{
	static int first_run = 1;
	
	if (options_get_int("StartInMusicDir")) {
		char *music_dir;

		if ((music_dir = options_get_str("MusicDir"))) {
			make_path (music_dir);
			if (file_type(cwd) == F_PLAYLIST
					&& plist_count(playlist) == 0
					&& plist_load(playlist, cwd, NULL)
					&& first_run) {
				toggle_plist ();
				cwd[0] = 0;
				first_run = 0;
				return;
			}
			else if (file_type(cwd) == F_DIR && go_to_dir(NULL)) {
				first_run = 0;
				return;
			}
		}
		else
			error ("MusicDir is not set");
	}
	if (!(read_last_dir() && go_to_dir(NULL))) {
		set_start_dir ();
		if (!go_to_dir(NULL))
			interface_fatal ("Can't enter any directory.");
	}

	first_run = 0;
}

/* Set the has_xterm variable. */
static void detect_term ()
{
	char *term;

	if ((((term = getenv("TERM")) && !strcmp(term, "xterm"))
				|| !strcmp(term, "rxvt")
				|| !strcmp(term, "eterm")
				|| !strcmp(term, "Eterm")))
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
static void process_args (char **args, const int num, const int recursively)
{
	if (num == 1 && !recursively && isdir(args[0]) == 1) {
		make_path (args[0]);
		if (!go_to_dir(NULL))
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
		
		plist_load (playlist, args[0], path);
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
				read_directory_recurr (path, playlist, 1);
			else if (dir == 0 && is_sound_file(path))
				plist_add (playlist, path);
		}
	}

	if (plist_count(playlist)) {
		char msg[50];

		visible_plist = playlist;
		
		if (options_get_int("ReadTags"))
			switch_titles_tags (playlist);
		else
			switch_titles_file (playlist);

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

/* Load the playlist from .moc directory. */
static void load_playlist ()
{
	char *plist_file = create_file_name ("playlist.m3u");

	set_iface_status_ref ("Loading playlist...");
	if (file_type(plist_file) == F_PLAYLIST)
		plist_load (playlist, plist_file, cwd);
	set_iface_status_ref (NULL);
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

/* Request the playlist from the server (given by another client). Make the
 * titles. Return 0 if such a list doesn't exists. */
static int get_server_playlist (struct plist *plist)
{
	set_iface_status_ref ("Getting the playlist...");
	debug ("Getting the playlist...");
	if (recv_server_plist(plist)) {
		
		if (options_get_int("ReadTags")) {
			set_iface_status_ref ("Reading tags...");
			switch_titles_tags (playlist);
		}
		else
			switch_titles_file (plist);

		set_iface_status_ref (NULL);
		return 1;
	}

	set_iface_status_ref (NULL);
	
	return 0;
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

static void init_playlists ()
{
	curr_plist = (struct plist *)xmalloc (sizeof(struct plist));
	plist_init (curr_plist);
	playlist = (struct plist *)xmalloc (sizeof(struct plist));
	plist_init (playlist);

	/* set serial numbers for playlists */
	send_int_to_srv (CMD_GET_SERIAL);
	plist_set_serial (curr_plist, get_data_int());
	send_int_to_srv (CMD_GET_SERIAL);
	plist_set_serial (playlist, get_data_int());
}

/* Send all items from this playlist to other clients */
static void send_all_items (struct plist *plist)
{
	int i;
	
	for (i = 0; i < plist->num; i++)
		if (!plist_deleted(plist, i)) {
			send_int_to_srv (CMD_CLI_PLIST_ADD);
			send_item_to_srv (&plist->items[i]);
		}
}

/* Make sure that the server's playlist has different serial from ours. */
static void change_srv_plist_serial ()
{
	int serial;
	
	do {	
		send_int_to_srv (CMD_GET_SERIAL);
		serial = get_data_int ();
	 } while (serial == plist_get_serial(playlist)
			|| serial == plist_get_serial(curr_plist));

	send_int_to_srv (CMD_PLIST_SET_SERIAL);
	send_int_to_srv (serial);
}

/* Based on ASCIILines option initialize line characters with curses lines or
 * ASCII characters. */
static void init_lines ()
{
	if (options_get_int("ASCIILines")) {
		lines.vert = '|';
		lines.horiz = '-';
		lines.ulcorn = '+';
		lines.urcorn = '+';
		lines.llcorn = '+';
		lines.lrcorn = '+';
		lines.rtee = '|';
		lines.ltee = '|';
	}
	else {
		lines.vert = ACS_VLINE;
		lines.horiz = ACS_HLINE;
		lines.ulcorn = ACS_ULCORNER;
		lines.urcorn = ACS_URCORNER;
		lines.llcorn = ACS_LLCORNER;
		lines.lrcorn = ACS_LRCORNER;
		lines.rtee = ACS_RTEE;
		lines.ltee = ACS_LTEE;
	}
}

void get_mixer_name ()
{
	char *name;
	
	send_int_to_srv (CMD_GET_MIXER_CHANNEL_NAME);
	name = get_data_str ();

	assert (strlen(name) <= 14);

	strcpy (mixer_name, name);
	free (name);
}

/* Initialize the interface. args are command line file names. arg_num is the
 * number of arguments. recursively should be set to non-zero if --recursively
 * was used. */
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

	init_playlists ();
	event_queue_init (&events);
	iconv_init ();
	keys_init ();
	
	initscr ();
	cbreak ();
	noecho ();
	use_default_colors ();

	check_term_size ();

	signal (SIGQUIT, sig_quit);
	/*signal (SIGTERM, sig_quit);*/
	signal (SIGINT, sig_interrupt);
	
#ifdef SIGWINCH
	signal (SIGWINCH, sig_winch);
#endif
	
	detect_term ();
	start_color ();
	theme_init (has_xterm);
	init_lines ();

	/* windows */
	main_win = newwin (LINES - 4, COLS, 0, 0);
	keypad (main_win, TRUE);
	info_win = newwin (4, COLS, LINES - 4, 0);
	wbkgd (main_win, get_color(CLR_BACKGROUND));
	wbkgd (info_win, get_color(CLR_BACKGROUND));
	nodelay (main_win, TRUE);

	msg_timeout = time(NULL) + 3;
	reset_file_info ();
	set_interface_status (NULL);
	xterm_set_state (STATE_STOP);
	xterm_set_title ("");
	
	main_border ();
	get_server_options ();
	get_mixer_name ();
	
	if (arg_num) {
		process_args (args, arg_num, recursively);
	
		if (plist_count(playlist) == 0) {
			if (!options_get_int("SyncPlaylist")
					|| !get_server_playlist(playlist))
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

			send_int_to_srv (CMD_LOCK);
			send_int_to_srv (CMD_CLI_PLIST_CLEAR);

			plist_set_serial (playlist,
					plist_get_serial(&tmp_plist));
			plist_free (&tmp_plist);

			change_srv_plist_serial ();
		
			set_iface_status_ref ("Notifying clients...");
			send_all_items (playlist);
			set_iface_status_ref (NULL);
			send_int_to_srv (CMD_UNLOCK);

			/* Now enter_first_dir() should not go to the music
			 * directory. */
			option_set_int ("StartInMusicDir", 0);
		}
	}
	else {
		if (!options_get_int("SyncPlaylist")
				|| !get_server_playlist(playlist))
			load_playlist ();
		enter_first_dir ();
	}
	
	send_int_to_srv (CMD_SEND_EVENTS);
	if (options_get_int("SyncPlaylist"))
		send_int_to_srv (CMD_CAN_SEND_PLIST);
	menu_draw (curr_menu);
	wrefresh (main_win);
	update_state ();
	curs_set (0);
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

static void update_menu ()
{
	werase (main_win);
	main_border ();
	menu_draw (curr_menu);
	wrefresh (main_win);
}

/* Switch between the current playlist and the playlist
 * (curr_plist/playlist). */
static void toggle_plist ()
{
	int num;
	
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
	else if ((num = plist_count(playlist))) {
		char msg[50];

		visible_plist = playlist;
		if (!playlist_menu)
			playlist_menu = make_menu (playlist, NULL, NULL);
		curr_menu = playlist_menu;
		set_title ("Playlist");
		update_curr_file ();
		sprintf (msg, "%d files on the list", num);
		set_interface_status (msg);
	}
	else
		error ("The playlist is empty.");

	update_info_win ();
	wrefresh (info_win);
}

/* Handle EV_PLIST_ADD. */
static void event_plist_add (struct plist_item *item)
{
	if (plist_find_fname(playlist, item->file) == -1) {
		char msg[50];
		int item_num = plist_add_from_item (playlist, item);
		
		if (options_get_int("ReadTags")) {
			make_tags_title (playlist, item_num);

			if (playlist->items[item_num].title_tags)
				playlist->items[item_num].title =
					playlist->items[item_num].title_tags;
			else {
				make_file_title (playlist, item_num,
						options_get_int(
							"HideFileExtension"));
				playlist->items[item_num].title =
					playlist->items[item_num].title_file;
			}
		}
		else {
			make_file_title (playlist, item_num,
					options_get_int("HideFileExtension"));
			playlist->items[item_num].title =
				playlist->items[item_num].title_file;
		}

		if (playlist_menu)
			add_to_menu (playlist_menu, playlist, item_num);
		
		sprintf (msg, "%d files on the list", plist_count(playlist));
		set_iface_status_ref (msg);

		if (waiting_for_plist_load) {
			if (visible_plist == curr_plist)
				toggle_plist ();
			waiting_for_plist_load = 0;
		}
		else if (visible_plist == playlist)
			update_curr_file (); /* toggle_plist() above already
						does it. */
	}
}

/* Handle EV_PLIST_DEL. */
static void event_plist_del (char *file)
{
	int item = plist_find_fname (playlist, file);

	if (item != -1) {
		int selected_item = 0;
		int top_item = 0;
		int num;
		int need_recount_time = 0;
		
		if (get_item_time(playlist, item) != -1)
			need_recount_time = 1;
		
		plist_delete (playlist, item);

		if (need_recount_time)
			plist_count_total_time (playlist);
		
		if (playlist_menu) {
			selected_item = playlist_menu->selected;
			top_item = playlist_menu->top;
			menu_free (playlist_menu);
		}

		if ((num = plist_count(playlist)) > 0) {
			if (curr_menu == playlist_menu) {
				char msg[50];
				
				playlist_menu = make_menu (playlist, NULL, 0);
				menu_set_top_item (playlist_menu, top_item);
				menu_setcurritem (playlist_menu, selected_item);
				curr_menu = playlist_menu;
				update_curr_file ();
				update_info_win ();
				wrefresh (info_win);
				
				sprintf (msg, "%d files on the list", num);
				set_iface_status_ref (msg);
			}
			else 
				playlist_menu = NULL;
		}
		else {
			if (curr_menu == playlist_menu)
				toggle_plist ();
			plist_clear (playlist);
			playlist_menu = NULL;
			set_iface_status_ref (NULL);
		}
	}
	else
		logit ("Server requested deleting an item not present on the"
				" playlist.");
}

/* Clear the playlist locally */
static void clear_playlist ()
{
	if (visible_plist == playlist)
		toggle_plist();
	plist_clear (playlist);
	if (playlist_menu) {
		menu_free (playlist_menu);
		playlist_menu = NULL;
	}
	
	if (!waiting_for_plist_load)
		interface_message ("The playlist was cleared.");
	set_iface_status_ref (NULL);
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

/* Use these tags for current file title. */
static void update_curr_tags ()
{
	char *title;
	
	if (file_info.tags) {
		tags_free (file_info.tags);
		file_info.tags = NULL;
	}

	if (file_info.curr_file) {
		title = find_title (file_info.curr_file);

		strncpy (file_info.title, title, sizeof(file_info.title) - 1);
		file_info.title[sizeof(file_info.title)-1] = 0;
		xterm_set_title (title);
		free (title);

		update_info_win ();
		wrefresh (info_win);
	}
}

/* Handle EV_MIXER_CHANGE. */
static void ev_mixer_change ()
{
	get_mixer_name ();
	update_info_win ();
	wrefresh (info_win);
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
			update_info_win ();
			wrefresh (info_win);
			break;
		case EV_SEND_PLIST:
			forward_playlist ();
			break;
		case EV_PLIST_ADD:
			if (options_get_int("SyncPlaylist")) {
				event_plist_add ((struct plist_item *)data);
				if (curr_menu == playlist_menu)
					update_menu ();
			}
			plist_free_item_fields (data);
			free (data);
			break;
		case EV_PLIST_CLEAR:
			if (options_get_int("SyncPlaylist")) {
				clear_playlist ();
				update_menu ();
			}
			break;
		case EV_PLIST_DEL:
			if (options_get_int("SyncPlaylist")) {
				event_plist_del ((char *)data);
				update_menu ();
			}
			free (data);
			break;
		case EV_TAGS:
			update_curr_tags (data);
			break;
		case EV_STATUS_MSG:
			set_iface_status_ref (data);
			free (data);
			break;
		case EV_MIXER_CHANGE:
			ev_mixer_change ();
			break;
		default:
			interface_message ("Unknown event: 0x%02x", event);
			logit ("Unknown event 0x%02x", event);
	}
}

/* Get (generate) a playlist serial from the server and make sure it's not
 * the same as our playlist's serial. */
static int get_safe_serial ()
{
	int serial;

	do {
		send_int_to_srv (CMD_GET_SERIAL);
		serial = get_data_int ();
	} while (serial == plist_get_serial(playlist));

	return serial;
}

/* Generate a unique playlist serial number. */
/* Send the playlist to the server if necessary and request playing this
 * item. */
static void play_it (const int plist_pos)
{
	char *file = plist_get_file (visible_plist, plist_pos);
	long serial; /* serial number of the playlist */
	
	assert (file != NULL);
	
	send_int_to_srv (CMD_LOCK);

	send_int_to_srv (CMD_PLIST_GET_SERIAL);
	serial = get_data_int ();
	
	if (plist_get_serial(visible_plist) == -1
			|| serial != plist_get_serial(visible_plist)) {

		logit ("The server has different playlist");

		serial = get_safe_serial();
		plist_set_serial (visible_plist, serial);
		send_int_to_srv (CMD_PLIST_SET_SERIAL);
		send_int_to_srv (serial);
	
		send_playlist (visible_plist, 1);
	}
	else
		logit ("The server already has my playlist");
	
	send_int_to_srv (CMD_PLAY);
	send_str_to_srv (file);
	free (file);

	send_int_to_srv (CMD_UNLOCK);
}

static void go_dir_up ()
{
	char dir[PATH_MAX + 1];
	char *slash;

	strcpy (dir, cwd);				
	slash = strrchr (dir, '/');
	assert (slash != NULL);
	if (slash == dir)
		*(slash + 1) = 0;
	else
		*slash = 0;

	go_to_dir (dir);
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

	set_iface_status_ref ("Loading playlist...");
	if (plist_load(playlist, file, cwd)) {
	
		if (options_get_int("SyncPlaylist")) {
			send_int_to_srv (CMD_LOCK);
			change_srv_plist_serial ();
			send_int_to_srv (CMD_CLI_PLIST_CLEAR);
			set_iface_status_ref ("Notifying clients...");
			send_all_items (playlist);
			set_iface_status_ref (NULL);
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
		set_iface_status_ref (NULL);
		return 0;
	}

	return 1;
}

/* Action when the user selected a file. */
static void go_file ()
{
	int selected = menu_curritem (curr_menu);
	enum file_type type = menu_item_get_type(curr_menu, selected);

	if (type == F_SOUND || type == F_URL)
		play_it (menu_item_get_plist_pos(curr_menu, selected));
	else if (type == F_DIR && visible_plist == curr_plist) {
		if (!strcmp(menu_item_get_file(curr_menu, selected), ".."))
			go_dir_up ();
		else {
			char dir[PATH_MAX + 1];

			strcpy (dir, menu_item_get_file(curr_menu, selected));
			go_to_dir (dir);
		}
	}
	else if (type == F_DIR && visible_plist == playlist)
		
		/* the only item on the playlist of type F_DIR is '..' */
		toggle_plist ();
	else if (type == F_PLAYLIST)
		go_to_playlist(menu_item_get_file(curr_menu, selected));
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
	int selected = menu_curritem (curr_menu);

	if (visible_plist == playlist) {
		error ("Can't add to the playlist a file from the "
				"playlist.");
		return;
	}

	if (menu_item_get_type(curr_menu, selected) == F_DIR) {
		error ("This is a directory.");
		return;
	}
	if (menu_item_get_type(curr_menu, selected) != F_SOUND) {
		error ("You can only add a file using this command.");
		return;
	}

	if (plist_find_fname(playlist,
				menu_item_get_file(curr_menu, selected)) == -1) {
		struct plist_item *item = &curr_plist->items[
			menu_item_get_plist_pos(curr_menu, selected)];

		send_int_to_srv (CMD_LOCK);

		if (options_get_int("SyncPlaylist")) {
			send_int_to_srv (CMD_CLI_PLIST_ADD);
			send_item_to_srv (item);
		}
		else {
			int added = plist_add_from_item (playlist, item);
			
			if (playlist_menu)
				add_to_menu (playlist_menu, playlist, added);
		}
				
		/* Add to the server's playlist if the server has our
		 * playlist */
		send_int_to_srv (CMD_PLIST_GET_SERIAL);
		if (get_data_int() == plist_get_serial(playlist)) {
			send_int_to_srv (CMD_LIST_ADD);
			send_str_to_srv (item->file);
		}
		send_int_to_srv (CMD_UNLOCK);
	}
	else
		error ("The file is already on the playlist.");
}

/* Recursively add the conted to a directory to the playlist. */
static void add_dir_plist ()
{
	int selected = menu_curritem (curr_menu);
	struct plist plist;

	if (visible_plist == playlist) {
		error ("Can't add to the playlist a file from the "
				"playlist.");
		return;
	}

	if (menu_item_get_type(curr_menu, selected) != F_DIR) {
		error ("This is not a directory.");
		return;
	}

	if (selected == 0) {
		error ("Can't add '..'.");
		return;
	}

	set_iface_status_ref ("reading directories...");
	plist_init (&plist);
	read_directory_recurr (menu_item_get_file(curr_menu, selected), &plist,
			0);
	if (options_get_int("ReadTags")) {
		set_iface_status_ref ("Getting tags...");
		switch_titles_tags (&plist);
		sync_plists_data (curr_plist, &plist);
	}
	else
		switch_titles_file (&plist);

	plist_sort_fname (&plist);

	send_int_to_srv (CMD_LOCK);

	/* Add the new files to the server's playlist if the server has our
	 * playlist */
	send_int_to_srv (CMD_PLIST_GET_SERIAL);
	if (get_data_int() == plist_get_serial(playlist))
		send_playlist (&plist, 0);

	if (options_get_int("SyncPlaylist")) {
		set_iface_status_ref ("Notifying clients...");
		send_all_items (&plist);
		set_iface_status_ref (NULL);
	}
	else {
		char msg[50];

		plist_cat (playlist, &plist);
		
		if (playlist_menu) {
			menu_free (playlist_menu);
			playlist_menu = NULL;
		}
		
		sprintf (msg, "%d files on the list", plist_count(playlist));
		set_iface_status_ref (msg);
	}

	send_int_to_srv (CMD_UNLOCK);
	
	wrefresh (info_win);
	plist_free (&plist);
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

static void adjust_mixer (const int diff)
{
	int vol = get_mixer ();

	vol += diff;

	if (vol < 0)
		vol = 0;
	else if (vol > 100)
		vol = 100;

	send_int_to_srv (CMD_SET_MIXER);
	send_int_to_srv (vol);

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
	int help_lines;
	char **help;

	help = get_keys_help (&help_lines);
	
	werase (main_win);
	wbkgd (main_win, get_color(CLR_BACKGROUND));

	wmove (main_win, 0, 0);
	if (help_screen_top != 0) {
		wattrset (main_win, get_color(CLR_MESSAGE));
		mvwaddstr (main_win, 0, COLS/2 - (sizeof("...MORE...")-1)/2,
				"...MORE...");
	}
	wmove (main_win, 1, 0);
	for (i = help_screen_top; i < max_lines && i < help_lines; i++) {
		wattrset (main_win, get_color(CLR_LEGEND));
		waddstr (main_win, help[i]);
		waddch (main_win, '\n');
	}
	if (i != help_lines) {
		wattrset (main_win, get_color(CLR_MESSAGE));
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
	int plist_pos;

	assert (menu != NULL);

	for (i = 0; i < menu->nitems; i++)
		if ((plist_pos = menu_item_get_plist_pos(menu, i)) != -1)
			menu_item_set_title (menu, i,
					plist->items[plist_pos].title);
}

/* Switch ReadTags options and update the menu. */
static void switch_read_tags ()
{
	if (options_get_int("ReadTags")) {
		option_set_int("ReadTags", 0);
		switch_titles_file (curr_plist);
		switch_titles_file (playlist);
		set_iface_status_ref ("ReadTags: no");
	}
	else {
		option_set_int("ReadTags", 1);
		set_iface_status_ref ("Reading tags...");
		switch_titles_tags (curr_plist);
		switch_titles_tags (playlist);
		set_iface_status_ref ("ReadTags: yes");
	}

	if (playlist_menu)
		update_menu_titles (playlist_menu, playlist);
	if (curr_plist_menu)
		update_menu_titles (curr_plist_menu, curr_plist);
}

/* Reread the directory. */
static void reread_dir (const int set_curr_menu)
{
	int selected_item = curr_plist_menu->selected;
	int top_item = curr_plist_menu->top;

	if (load_dir(NULL)) {
		if (set_curr_menu) {
			visible_plist = curr_plist;
			curr_menu = curr_plist_menu;
		}

		menu_set_top_item (curr_plist_menu, top_item);
		menu_setcurritem (curr_plist_menu, selected_item);

		update_state ();
	}
}

static void delete_item ()
{
	int selected;
	int top_item;
	int num;

	if (visible_plist != playlist) {
		error ("You can only delete an item from the "
				"playlist.");
		return;
	}

	assert (playlist->num > 0);

	selected = menu_curritem (curr_menu);
	top_item = curr_menu->top;
	
	if (menu_item_get_plist_pos(curr_menu, selected) == -1) {
		interface_error ("You can't delete '..'");
		return;
	}

	send_int_to_srv (CMD_LOCK);
	
	if (options_get_int("SyncPlaylist")) {
		send_int_to_srv (CMD_CLI_PLIST_DEL);
		send_str_to_srv (menu_item_get_file(curr_menu, selected));
	}
	else {
		plist_delete (playlist, menu_item_get_plist_pos(curr_menu,
					selected));
		if ((num = plist_count(playlist)) > 0) {
			char msg[50];
			
			menu_free (playlist_menu);
			playlist_menu = make_menu (playlist, NULL, 0);
			menu_set_top_item (playlist_menu, top_item);
			menu_setcurritem (playlist_menu, selected);
			curr_menu = playlist_menu;
			update_curr_file ();
			sprintf (msg, "%d files on the list", num);
			set_iface_status_ref (msg);
			plist_count_total_time (playlist);
			update_info_win ();
			wrefresh (info_win);
		}
		else
			clear_playlist ();
	}

	/* Delete this item from the server's playlist if it has our
	 * playlist */
	send_int_to_srv (CMD_PLIST_GET_SERIAL);
	if (get_data_int() == plist_get_serial(playlist)) {
		send_int_to_srv (CMD_DELETE);
		send_str_to_srv (menu_item_get_file(curr_menu, selected));
	}
	
	send_int_to_srv (CMD_UNLOCK);
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

/* A special value of ch -1 disables the entry. */
static int entry_search_key (const int ch)
{
	/* saved menus that we replace with a menu with filtered elements. */
	static struct menu *old_curr_menu = NULL;
	static struct menu *old_playlist_menu = NULL;
	static struct menu *old_curr_plist_menu = NULL;

	enum key_cmd cmd;
	
	/* in this entry, we also operate on the menu */
	if ((cmd = get_key_cmd(CON_ENTRY, ch)) == KEY_CMD_WRONG)
		cmd = get_key_cmd(CON_MENU, ch);

	/* isgraph() can return wrong values if ch is not in unsigned char
	 * scope */
	if ((ch >= 0 && ch <= 255 && isgraph(ch)) || ch == ' '
			|| ch == KEY_BACKSPACE) {
		if (ch == KEY_BACKSPACE)
			entry_back_space ();
		else
			entry_add_char (ch);

		if (!old_curr_menu) {
			
			/* save the old menus */
			old_curr_menu = curr_menu;
			old_playlist_menu = playlist_menu;
			old_curr_plist_menu = curr_plist_menu;
		}
		else
			menu_free (curr_menu); /* free previous filtered menu */
		
		curr_menu = menu_filter_pattern (old_curr_menu, entry.text);
		if (old_curr_menu == old_playlist_menu)
			playlist_menu = curr_menu;
		else
			curr_plist_menu = curr_menu;

		entry_draw ();
	}
	else if (cmd == KEY_CMD_MENU_UP)
		menu_driver (curr_menu, REQ_UP);
	else if (cmd == KEY_CMD_MENU_DOWN)
		menu_driver (curr_menu, REQ_DOWN);
	else if (cmd == KEY_CMD_MENU_NPAGE)
		menu_driver (curr_menu, REQ_PGDOWN);
	else if (cmd == KEY_CMD_MENU_PPAGE)
		menu_driver (curr_menu, REQ_PGUP);
	else if (cmd == KEY_CMD_MENU_FIRST)
		menu_driver (curr_menu, REQ_TOP);
	else if (cmd == KEY_CMD_MENU_LAST)
		menu_driver (curr_menu, REQ_BOTTOM);
	else if (ch == '\n' || cmd == KEY_CMD_CANCEL || ch == -1) {
		entry_disable ();
		update_info_win ();
		wrefresh (info_win);
		
		if (ch == '\n' && entry.text[0])
			go_file ();

		if (old_curr_menu) {
			
			/* restore the original menu */
			menu_free (curr_menu);
			playlist_menu = old_playlist_menu;
			curr_plist_menu = old_curr_plist_menu;
			curr_menu = old_curr_menu;
			old_curr_menu = NULL;
		}
		
		update_curr_file ();
	}

	update_menu ();
	
	/* Ignore every other key, because if something else cause exiting from
	 * the entry, we will lose the original menu. */
	return 1;
}

/* Handle keys specific to ENTRY_PLIST_SAVE. Return 1 if a key was handled. */
static int entry_plist_save_key (const int ch)
{
	if (ch == '\n') {
		if (!entry.text[0])
			entry_disable ();
		else {
			char *ext = ext_pos (entry.text);
			char *file;

			entry_disable ();
			update_info_win ();
			if (!ext || strcmp(ext, "m3u"))
				strncat (entry.text, ".m3u", sizeof(entry.text)
						- strlen(entry.text) - 1);

			file = make_dir (entry.text);

			if (file_exists(file)) {
				make_entry (ENTRY_PLIST_OVERWRITE,
						"File exists, owerwrite?");
				entry.file = file;
			}
			else {
				set_iface_status_ref ("Saving the playlist...");
				if (plist_save(playlist, file,
							strchr(entry.text, '/')
							? NULL : cwd))
					interface_message ("Playlist saved.");
				set_iface_status_ref (NULL);

				/* plist_save() also reads times for all items,
				 * so we can show times "for free" here */
				fill_times (playlist, playlist_menu);

				reread_dir (curr_plist == visible_plist);
				update_menu ();
				free (file);
			}
		}
		update_info_win ();
		return 1;
	}
	return 0;
}

static int entry_plist_overwrite_key (const int key)
{
	if (key == 'y') {
		char *file = xstrdup (entry.file);
		
		entry_disable ();
		update_info_win ();
		set_iface_status_ref ("Saving the playlist...");
		if (plist_save(playlist, file, NULL)) /* FIXME: not always NULL! */
			interface_message ("Playlist saved.");
		set_iface_status_ref (NULL);
		free (file);
		
		reread_dir (curr_plist == visible_plist);
		update_menu ();
	}
	else if (key == 'n') {
		entry_disable ();
		update_info_win ();
		set_iface_status_ref ("Not overwriting.");
	}
	
	return 1;
}

/* Handle common entry key. Return 1 if a key was handled. */
static int entry_common_key (const int ch)
{
	enum key_cmd cmd;

	if (isgraph(ch) || ch == ' ') {
		entry_add_char (ch);
		entry_draw ();
		return 1;
	}

	if (ch == KEY_LEFT) {
		entry_curs_left ();
		entry_draw ();
		return 1;
	}

	if (ch == KEY_RIGHT) {
		entry_curs_right ();
		entry_draw ();
		return 1;
	}

	if (ch == KEY_BACKSPACE) {
		entry_back_space ();
		entry_draw ();
		return 1;
	}

	if (ch == KEY_DC) {
		entry_del_char ();
		entry_draw ();
		return 1;
	}

	if (ch == KEY_HOME) {
		entry_home ();
		entry_draw ();
		return 1;
	}

	if (ch == KEY_END) {
		entry_end ();
		entry_draw ();
		return 1;
	}

	cmd = get_key_cmd (CON_ENTRY, ch);
	
	if (cmd == KEY_CMD_CANCEL) {
		entry_disable ();
		update_info_win ();
		return 1;
	}
	
	return 0;
}

static int history_add (char *history[HISTORY_MAX], int history_len,
		const char *text)
{
	if (history_len < HISTORY_MAX) {
		history[history_len++] = xstrdup (text);
	}
	else {
		memmove (history, history + 1,
				(HISTORY_MAX - 1) * sizeof(char *));
		history[history_len] = xstrdup (text);
	}

	return history_len;
}

static int entry_go_dir_key (const int ch)
{
	static int history_pos = 0;
	enum key_cmd cmd;

	cmd = get_key_cmd (CON_ENTRY, ch);
	
	if (ch == '\t') {
		char *dir;
		char *complete_dir;
		char buf[PATH_MAX+1];
		
		if (!(dir = make_dir(entry.text)))
			return 1;
		
		complete_dir = find_match_dir (dir);
		
		strncpy (buf, complete_dir ? complete_dir : dir, sizeof(buf));
		entry.text[sizeof(buf)-1] = 0;

		if (complete_dir)
			free (complete_dir);

		entry_set_text (buf);

		free (dir);
		entry_draw ();

		return 1;
	}
	else if (ch == '\n') {
		if (entry.text[0]) {
			char *dir = make_dir (entry.text);

			files_history_len = history_add (files_history,
					files_history_len, entry.text);
			history_pos = files_history_len;
			
			/* strip trailing slash */
			if (dir[strlen(dir)-1] == '/' && strcmp(dir, "/"))
				dir[strlen(dir)-1] = 0;

			entry_disable ();
			update_info_win ();

			if (dir) {
				go_to_dir (dir);
				update_menu ();
				free (dir);
			}
		}
		else
			entry_disable ();
		
		update_info_win ();
		
		return 1;
	}
	else if (cmd == KEY_CMD_HISTORY_UP) {
		if (history_pos > 0) {
			history_pos--;
			strcpy (entry.text, files_history[history_pos]); 
			entry.cur_pos = 0;
			update_info_win ();
		}
		
		return 1;
	}
	else if (cmd == KEY_CMD_HISTORY_DOWN) {
		if (history_pos < files_history_len - 1) {
			history_pos++;
			strcpy (entry.text, files_history[history_pos]); 
			entry.cur_pos = 0;
			update_info_win ();
		}
		return 1;
	}

	return 0;
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

static int entry_go_url (const int ch)
{
	static int history_pos = 0;
	enum key_cmd cmd;

	cmd = get_key_cmd (CON_ENTRY, ch);

	if (ch == '\n') {
		if (entry.text[0]) {
			urls_history_len = history_add (urls_history,
					urls_history_len, entry.text);
			history_pos = urls_history_len;

			if (file_type(entry.text) == F_URL)
				play_from_url (entry.text);
			else
				error ("Not a valid URL.");
		}
		entry_disable ();
		update_info_win ();

		return 1;
	}
	else if (cmd == KEY_CMD_HISTORY_UP) {
		if (history_pos > 0) {
			history_pos--;
			strcpy (entry.text, urls_history[history_pos]); 
			entry.cur_pos = 0;
			update_info_win ();
		}
		
		return 1;
	}
	else if (cmd == KEY_CMD_HISTORY_DOWN) {
		if (history_pos < urls_history_len - 1) {
			history_pos++;
			strcpy (entry.text, urls_history[history_pos]); 
			entry.cur_pos = 0;
			update_info_win ();
		}
		return 1;
	}

	return 0;
}

static void entry_key (const int ch)
{
	int handled = 0;

	if (entry.type == ENTRY_GO_DIR)
		handled = entry_go_dir_key (ch);
	if (entry.type == ENTRY_GO_URL)
		handled = entry_go_url (ch);
	if (entry.type == ENTRY_SEARCH)
		handled = entry_search_key (ch);
	if (entry.type == ENTRY_PLIST_SAVE)
		handled = entry_plist_save_key (ch);
	if (entry.type == ENTRY_PLIST_OVERWRITE)
		handled = entry_plist_overwrite_key (ch);
	if (!handled)
		entry_common_key (ch);
	wrefresh (info_win);
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
		fill_times (playlist, playlist_menu);
		fill_times (curr_plist, curr_plist_menu);
		update_info_win ();
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

static void go_to_file_dir ()
{
	if (file_info.curr_file && file_type(file_info.curr_file) == F_SOUND) {
		int i;
		
		if ((i = plist_find_fname(playlist, file_info.curr_file))
				!= -1) {
			if (visible_plist != playlist)
				toggle_plist ();
		}
		else {
			char *slash;
			char *file = xstrdup (file_info.curr_file);

			slash = strrchr (file, '/');
			assert (slash != NULL);
			*slash = 0;

			if (strcmp(file, cwd) || visible_plist == playlist)
				go_to_dir (file);
			free (file);
		}

		if ((i = plist_find_fname(visible_plist, file_info.curr_file))
				!= -1)
			menu_setcurritem_by_plistnum (curr_menu, i);
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
	if (file_info.state_code == STATE_PLAY) {
		if (silent_seek_pos == -1) {
			silent_seek_pos = file_info.curr_time_num + sec;
		}
		else
			silent_seek_pos += sec;
			
		if (silent_seek_pos < 0)
			silent_seek_pos = 0;
		else if (silent_seek_pos > file_info.time_num)
			silent_seek_pos = file_info.time_num;

		silent_seek_key_last = rounded_time ();
		update_ctime ();
	}
}

/* Handle releasing silent seek key. */
static void do_silent_seek ()
{
	time_t curr_time = time(NULL);
	
	if (silent_seek_pos != -1 && silent_seek_key_last < curr_time) {
		send_int_to_srv (CMD_SEEK);
		send_int_to_srv (silent_seek_pos - file_info.curr_time_num - 1);
		silent_seek_pos = -1;
	}
}

static void go_to_music_dir ()
{
	if (options_get_str("MusicDir")) {
		if (file_type(options_get_str("MusicDir")) == F_DIR)
			go_to_dir (options_get_str("MusicDir"));
		else if (file_type(options_get_str("MusicDir")) == F_PLAYLIST)
			go_to_playlist (options_get_str("MusicDir"));
		else
			error ("MusicDir is neither a directory nor a "
					"playlist.");
	}
	else
		error ("MusicDir not defined");
}

/* Handle key */
static void menu_key (const int ch)
{
	int do_update_menu = 0;

	if (main_win_mode == WIN_HELP) {
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
	}
	else if (entry.type != ENTRY_DISABLED)
		entry_key (ch);
	else if (ch != KEY_RESIZE) {
		enum key_cmd cmd = get_key_cmd (CON_MENU, ch);
		
		switch (cmd) {
			case KEY_CMD_QUIT_CLIENT:
				want_quit = 1;
				break;
			case KEY_CMD_GO:
				go_file ();
				do_update_menu = 1;
				break;
			case KEY_CMD_MENU_DOWN:
				menu_driver (curr_menu, REQ_DOWN);
				do_update_menu = 1;
				break;
			case KEY_CMD_MENU_UP:
				menu_driver (curr_menu, REQ_UP);
				do_update_menu = 1;
				break;
			case KEY_CMD_MENU_NPAGE:
				menu_driver (curr_menu, REQ_PGDOWN);
				do_update_menu = 1;
				break;
			case KEY_CMD_MENU_PPAGE:
				menu_driver (curr_menu, REQ_PGUP);
				do_update_menu = 1;
				break;
			case KEY_CMD_MENU_FIRST:
				menu_driver (curr_menu, REQ_TOP);
				do_update_menu = 1;
				break;
			case KEY_CMD_MENU_LAST:
				menu_driver (curr_menu, REQ_BOTTOM);
				do_update_menu = 1;
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
			case KEY_CMD_TOGGLE_READ_TAGS:
				switch_read_tags ();
				do_update_menu = 1;
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
			case KEY_CMD_TOGGLE_PLAYLIST:
				toggle_plist ();
				do_update_menu = 1;
				break;
			case KEY_CMD_PLIST_ADD_FILE:
				add_file_plist ();
				break;
			case KEY_CMD_PLIST_CLEAR:
				cmd_clear_playlist ();
				do_update_menu = 1;
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
				help_screen ();
				break;
			case KEY_CMD_HIDE_MESSAGE:
				interface_message (NULL);
				update_info_win ();
				wrefresh (info_win);
				break;
			case KEY_CMD_REFRESH:
				wclear (info_win);
				update_info_win ();
				wrefresh (info_win);
				wclear (main_win);
				do_update_menu = 1;
				break;
			case KEY_CMD_RELOAD:
				if (visible_plist == curr_plist) {
					reread_dir (1);
					do_update_menu = 1;
				}
				break;
			case KEY_CMD_TOGGLE_SHOW_HIDDEN_FILES:
				option_set_int ("ShowHiddenFiles",
						!options_get_int(
							"ShowHiddenFiles"));
				if (visible_plist == curr_plist) {
					reread_dir (1);
					do_update_menu = 1;
				}
				break;
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
			case KEY_CMD_TOGGLE_SHOW_TIME:
				toggle_show_time ();
				do_update_menu = 1;
				break;
			case KEY_CMD_TOGGLE_SHOW_FORMAT:
				toggle_show_format ();
				do_update_menu = 1;
				break;
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
			case KEY_CMD_GO_DIR_UP:
				go_dir_up ();
				do_update_menu = 1;
				break;
			case KEY_CMD_WRONG:
				error ("Bad command");
				break;
			case KEY_CMD_SEEK_FORWARD_5:
				seek_silent (5);
				break;
			case KEY_CMD_SEEK_BACKWARD_5:
				seek_silent (-5);
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
				if (options_get_str("FastDir1")) {
					go_to_dir (options_get_str(
								"FastDir1"));
					do_update_menu = 1;
				}
				else
					error ("FastDir1 not "
							"defined");
				break;
			case KEY_CMD_FAST_DIR_2:
				if (options_get_str("FastDir2")) {
					go_to_dir (options_get_str(
								"FastDir2"));
					do_update_menu = 1;
				}
				else
					error ("FastDir2 not "
							"defined");
				break;
			case KEY_CMD_FAST_DIR_3:
				if (options_get_str("FastDir3")) {
					go_to_dir (options_get_str(
								"FastDir3"));
					do_update_menu = 1;
				}
				else
					error ("FastDir3 not "
							"defined");
				break;
			case KEY_CMD_FAST_DIR_4:
				if (options_get_str("FastDir4")) {
					go_to_dir (options_get_str(
								"FastDir4"));
					do_update_menu = 1;
				}
				else
					error ("FastDir4 not "
							"defined");
				break;
			case KEY_CMD_FAST_DIR_5:
				if (options_get_str("FastDir5")) {
					go_to_dir (options_get_str(
								"FastDir5"));
					do_update_menu = 1;
				}
				else
					error ("FastDir5 not "
							"defined");
				break;
			case KEY_CMD_FAST_DIR_6:
				if (options_get_str("FastDir6")) {
					go_to_dir (options_get_str(
								"FastDir6"));
					do_update_menu = 1;
				}
				else
					error ("FastDir6 not "
							"defined");
				break;
			case KEY_CMD_FAST_DIR_7:
				if (options_get_str("FastDir7")) {
					go_to_dir (options_get_str(
								"FastDir7"));
					do_update_menu = 1;
				}
				else
					error ("FastDir7 not "
							"defined");
				break;
			case KEY_CMD_FAST_DIR_8:
				if (options_get_str("FastDir8")) {
					go_to_dir (options_get_str(
								"FastDir8"));
					do_update_menu = 1;
				}
				else
					error ("FastDir8 not "
							"defined");
				break;
			case KEY_CMD_FAST_DIR_9:
				if (options_get_str("FastDir9")) {
					go_to_dir (options_get_str(
								"FastDir9"));
					do_update_menu = 1;
				}
				else
					error ("FastDir9 not "
							"defined");
				break;
			case KEY_CMD_FAST_DIR_10:
				if (options_get_str("FastDir10")) {
					go_to_dir (options_get_str(
								"FastDir10"));
					do_update_menu = 1;
				}
				else
					error ("FastDir10 not "
							"defined");
				break;
			case KEY_CMD_TOGGLE_MIXER:
				send_int_to_srv (CMD_TOGGLE_MIXER_CHANNEL);
				break;
			default:
				debug ("Unhandled command: %d", cmd);
				fatal ("Unhandled command");
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
	entry_end ();

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
	struct event *e;
	
	debug ("Dequeuing events...");

	while ((e = event_get_first(&events))) {
		server_event (e->type, e->data);
		event_pop (&events);
	}

	debug ("done");
}

/* Handle interrupt (CTRL-C) */
static void handle_interrupt ()
{
	if (entry.type == ENTRY_SEARCH) {
		
		/* ENTRY_SEARCH requires more than just ENTRY_DISABLE,
		 * so simulate exit key */
		entry_search_key (-1);
	}
	else if (entry.type != ENTRY_DISABLED) {
		entry_disable ();
		update_info_win ();
		wrefresh (info_win);
	}
}

/* Get event from the server and handle it. */
static void get_and_handle_event (const int no_block)
{
	int type;
	void *data;

	if (no_block) {
		if (!get_int_from_srv_noblock(&type)) {
			debug ("Getting event would block.");
			return;
		}
	}
	else
		type = get_int_from_srv ();

	/* some events contail data */
	if (type == EV_PLIST_ADD)
		data = recv_item_from_srv ();
	else if (type == EV_PLIST_DEL || type == EV_STATUS_MSG)
		data = get_str_from_srv ();
	else
		data = NULL;

	server_event (type, data);
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

			do_silent_seek ();
		}
		else if (ret == -1 && !want_quit && errno != EINTR)
			interface_fatal ("select() failed: %s", strerror(errno));

#ifdef SIGWINCH
		if (want_resize)
			do_resize ();
#endif

		if (ret > 0) {
			if (FD_ISSET(STDIN_FILENO, &fds)) {
				int meta;
				int ch = wgetch(main_win);
				
				clear_interrupt ();

				/* Recognize meta sequences */
				if (ch == KEY_ESCAPE
						&& (meta = wgetch(main_win))
						!= ERR)
					ch = meta | META_KEY_FLAG;
				
				menu_key (ch);
				dequeue_events ();
			}

			if (!want_quit) {
				if (FD_ISSET(srv_sock, &fds))
					get_and_handle_event (1);
				do_silent_seek ();
				dequeue_events ();
			}
		}
		else if (user_wants_interrupt())
			handle_interrupt ();
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
static void save_playlist ()
{
	char *plist_file = create_file_name("playlist.m3u");

	if (plist_count(playlist) && options_get_int("SavePlaylist")) {
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
	send_int (srv_sock, CMD_DISCONNECT);
	close (srv_sock);
	
	/* endwin() sometimes fails on x terminals when we get SIGCHLD
	 * at this moment. Double invokation seems to solve this. */
	if (endwin() == ERR && endwin() == ERR)
		logit ("endwin() failed!");
	
	keys_cleanup ();
	xterm_clear_title ();
	plist_free (curr_plist);
	plist_free (playlist);
	if (playlist_menu)
		menu_free (playlist_menu);
	if (curr_plist_menu)
		menu_free (curr_plist_menu);
	free (curr_plist);
	free (playlist);
	event_queue_free (&events);
	iconv_cleanup ();

	/* Make sure that the next line after we exit will be "clear". */
	putchar ('\n');

	logit ("Interface exited");
}

/* Clear the playlist. Command was given from the command line and the
 * interface is not initialized. */
void interface_cmdline_clear_plist (int server_sock)
{
	int serial;
	srv_sock = server_sock; /* the interface is not initialized, so set it
				   here */
	
	send_int_to_srv (CMD_LOCK);
	if (options_get_int("SyncPlaylist"))
		send_int_to_srv (CMD_CLI_PLIST_CLEAR);

	send_int_to_srv (CMD_GET_SERIAL);
	serial = get_data_int ();
	send_int_to_srv (CMD_PLIST_SET_SERIAL);
	send_int_to_srv (serial);
	send_int_to_srv (CMD_LIST_CLEAR);
	send_int_to_srv (CMD_UNLOCK);
}

/* Append files given on command line. The interface is not initialized. */
void interface_cmdline_append (int server_sock, char **args,
		const int arg_num)
{
	int i;
	struct plist plist;

	srv_sock = server_sock; /* the interface is not initialized, so set it
				   here */

	plist_init (&plist);

	for (i = 0; i < arg_num; i++) {
		int dir = isdir(args[i]);

		if (dir == 1)
			read_directory_recurr (args[i], &plist, 1);
		else if (dir == 0 && is_sound_file(args[i]))
			plist_add (&plist, args[i]);
	}

	if (plist_count(&plist)) {
		if (options_get_int("SyncPlaylist")) {
			struct plist clients_plist;

			plist_init (&clients_plist);
			if (recv_server_plist(&clients_plist)) {
				int serial;

				send_int_to_srv (CMD_LOCK);
				send_all_items (&plist);

				send_int_to_srv (CMD_PLIST_GET_SERIAL);
				serial = get_data_int ();
				if (serial == plist_get_serial(&clients_plist))
					send_playlist (&plist, 0);
			}
			else {
				send_int_to_srv (CMD_LOCK);
				send_playlist (&plist, 0);
				send_int_to_srv (CMD_UNLOCK);
			}

			plist_free (&clients_plist);
		}
		else {
			send_int_to_srv (CMD_LOCK);
			change_srv_plist_serial ();
			send_playlist (&plist, 0);
			send_int_to_srv (CMD_UNLOCK);
		}
	}
	else
		fatal ("No files could be added");

	plist_free (&plist);
}

void interface_cmdline_play_first (int server_sock)
{
	struct plist clients_plist;

	srv_sock = server_sock; /* the interface is not initialized, so set it
				   here */

	plist_init (&clients_plist);
	if (recv_server_plist(&clients_plist)) {
		int serial;

		send_int_to_srv (CMD_LOCK);
		send_int_to_srv (CMD_PLIST_GET_SERIAL);
		serial = get_data_int ();
		if (serial != plist_get_serial(&clients_plist)) {
			send_playlist (&clients_plist, 1);
			send_int_to_srv (CMD_PLIST_SET_SERIAL);
			send_int_to_srv (plist_get_serial(&clients_plist));
		}
		
		send_int_to_srv (CMD_UNLOCK);
	}
	
	send_int_to_srv (CMD_PLAY);
	send_str_to_srv ("");

	plist_free (&clients_plist);
}

/* Print all information about the currentply played file.
 * 
 * Based on the code by Michael Banks.
 */
void interface_cmdline_file_info (const int server_sock)
{
	int state;
	has_xterm = 0;

	srv_sock = server_sock;	/* the interface is not initialized, so set it
				   here */
	init_playlists ();
	
	send_int_to_srv (CMD_GET_STATE);
	state = get_data_int ();
	if (state == STATE_STOP)
		puts ("State: STOP");
	else {
		int rate;
		char *file;
		int bitrate;
		int left;
		int curr_time;
		struct file_tags *tags = NULL;
		
		if (state == STATE_PLAY)
			puts ("State: PLAY");
		else if (state == STATE_PAUSE)
			puts ("State: PAUSE");

		send_int_to_srv (CMD_GET_SNAME);
		file = get_data_str ();

		if (file[0]) {

			/* get tags */
			if (file_type(file) == F_URL) {
				send_int_to_srv (CMD_GET_TAGS);
				wait_for_data ();
				tags = get_tags_from_srv ();
			}
			else
				tags = read_file_tags (file, NULL,
						TAGS_COMMENTS);
			
			/* get the title */
			if (tags->title) {
				char *title = build_title (tags);

				strncpy (file_info.title, title,
						sizeof(file_info.title) - 1);
				file_info.title[sizeof (file_info.title) - 1] = 0;
				free (title);
			}
			else
				file_info.title[0] = 0;
		}
		else
			file_info.title[0] = 0;

		send_int_to_srv (CMD_GET_CHANNELS);
		file_info.channels = get_data_int ();
		
		send_int_to_srv (CMD_GET_RATE);
		if ((rate = get_data_int ()) > 0)
			sprintf (file_info.rate, "%d", rate);
		else
			file_info.rate[0] = 0;

		send_int_to_srv (CMD_GET_BITRATE);
		if ((bitrate = get_data_int ()) > 0) {
			if (bitrate < 9999) {
				snprintf (file_info.bitrate,
						sizeof(file_info.bitrate),
						"%d", bitrate);
				file_info.bitrate[sizeof(file_info.bitrate)
					- 1] = 0;
			}
		}
		else
			file_info.bitrate[0] = 0;

		send_int_to_srv (CMD_GET_CTIME);
		curr_time = file_info.curr_time_num = get_data_int ();
		set_time (read_file_time(file));

		if (file_info.time_num != -1) {
			sec_to_min (file_info.curr_time, curr_time);
			left = file_info.time_num - curr_time;
			sec_to_min (file_info.time_left, left > 0 ? left : 0);
		}
		else {
			strcpy (file_info.curr_time, "00:00");
			file_info.time_left[0] = 0;
		}

		printf ("File: %s\n", file);
		printf ("Title: %s\n", file_info.title);

		if (tags) {
			printf ("Artist: %s\n",
					tags->artist ? tags->artist : "");
			printf ("SongTitle: %s\n",
					tags->title ? tags->title : "");
			printf ("Album: %s\n",
					tags->album ? tags->album : "");
		}

		if (file_info.time_num >= 0) {
			printf ("TotalTime: %s\n", file_info.time);
			printf ("CurrentTime: %s\n", file_info.curr_time);
			printf ("TimeLeft: %s\n", file_info.time_left);
			printf ("TotalSec: %d\n", file_info.time_num);
			printf ("CurrentSec: %d\n", file_info.curr_time_num);
		}

		printf ("Bitrate: %sKbps\n", file_info.bitrate);
		printf ("Rate: %sKHz\n", file_info.rate);
		
		free (file);
		if (tags)
			tags_free (tags);
	}

	plist_free (curr_plist);
	plist_free (playlist);
}
