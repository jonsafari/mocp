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
#include <unistd.h>
#include <ncurses.h>
#include <string.h>
#include <signal.h>
#include <time.h>
#include <errno.h>
#include <assert.h>
#include "protocol.h"
#include "main.h"
#include "playlist.h"
#include "log.h"
#include "menu.h"
#include "files.h"
#include "options.h"

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

static char cwd[PATH_MAX];

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
	char title[100];
	char bitrate[4];
	char rate[3];
	char time[6];
	char curr_time[6];
	char time_left[6];
	int time_num;
	int channels;
	char state[3];
} file_info;

/* When we are waiting for data from the server an event can occur. The event
 * we don't want to forget is EV_STATE, so set this variable to 1 if that event
 * occur. */
static int must_update_state = 0;

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

/* Update the info win */
static void update_info_win ()
{
	werase (info_win);
	info_border ();
	wattron (info_win, A_BOLD);

	/* Show message it it didn't expire yet */
	if (time(NULL) <= msg_timeout) {
		if (has_colors())
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
	/* FIXME: check for colors */
	wmove (info_win, 2, 38);
	if (file_info.channels == 2)
		wattron (info_win, COLOR_PAIR(CLR_ITEM) | A_BOLD);
	else
		wattron (info_win, COLOR_PAIR(CLR_DISABLED) | A_BOLD);
	waddstr (info_win, "[STEREO]");
	
	/* Shuffle & repeat */
	/* FIXME: check for colors */
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
	
	/* FIXME: check for colors */
	wattron (info_win, COLOR_PAIR(CLR_ITEM));

	/* Status line */
	wattroff (info_win, A_BOLD);
	mvwaddch (info_win, 0, 5, ACS_RTEE);
	wattroff (info_win, A_BOLD);
	mvwaddch (info_win, 0, 5 + STATUS_LINE_LEN + 1, ACS_LTEE);
	draw_interface_status ();
			
	draw_mixer ();
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
	update_info_win ();
	wrefresh (info_win);

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
	file_info.time[0] = 0;
	strcpy (file_info.curr_time, "00:00");
	strcpy (file_info.time_left, "00:00");
	strcpy (file_info.time, "00:00");
	file_info.bitrate[0] = 0;
	file_info.rate[0] = 0;
	file_info.time_num = 0;
	file_info.channels = 1;
	strcpy (file_info.state, "[]");
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

	//strcpy (cwd, "/home/daper/mp3/metal/heavy/Iron Maiden/2000 - Brave New World");
}

static int qsort_dirs_func (const void *a, const void *b)
{
	char *sa = *(char **)a;
	char *sb = *(char **)b;
	
	if (!strcmp(sa, "../"))
		return -1;
	if (!strcmp(sb, "../"))
		return 1;
	return strcmp (*(char **)a, *(char **)b);
}	

/* Make menu using the playlist and directory table. */
static struct menu *make_menu (struct plist *plist, char **dirs, int ndirs)
{
	int i;
	struct menu_item **menu_items;
	
	menu_items = (struct menu_item **)xmalloc (sizeof(struct menu_item *)
			* (plist->num + ndirs));
	
	/* directories */
	for (i = 0; i < ndirs; i++) {
		menu_items[i] = menu_newitem (dirs[i], -1);
		menu_items[i]->attr_normal = COLOR_PAIR(CLR_ITEM) | A_BOLD;
		menu_items[i]->attr_sel = COLOR_PAIR(CLR_SELECTED) | A_BOLD;
	}
	
	/* playlist items */
	for (i = ndirs; i < plist->num + ndirs; i++) {
		menu_items[i] = menu_newitem (plist->items[i-ndirs].title,
				i - ndirs);
		menu_items[i]->attr_normal = COLOR_PAIR(CLR_ITEM);
		menu_items[i]->attr_sel = COLOR_PAIR(CLR_SELECTED);
	}
	
	return menu_new (main_win, menu_items, ndirs + plist->num,
			COLOR_PAIR(CLR_ITEM), COLOR_PAIR(CLR_SELECTED),
			COLOR_PAIR(CLR_MARKED) | A_BOLD,
			COLOR_PAIR(CLR_MARKED_SELECTED) | A_BOLD);
}

/* Check if dir2 is in dir1 */
static int is_subdir (char *dir1, char *dir2)
{
	char *slash = strrchr (dir2, '/');

	assert (slash != NULL);

	/* FIXME: '/dir/dir' is not subdir of '/dir' */

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
		file_info.title[COLS-5] = 0;
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
	char **dirs;
	int ndirs;
	struct plist *old_curr_plist;
	char last_dir[PATH_MAX];
	char *new_dir = dir ? dir : cwd;
	int going_up = 0;
	char msg[50];

	set_interface_status ("reading directory...");
	wrefresh (info_win);

	if (chdir(new_dir)) {
		set_interface_status (NULL);
		wrefresh (info_win);
		interface_error ("Can't chdir(), %s", strerror(errno));
		return 0;
	}

	if (dir && is_subdir(dir, cwd)) {
		strcpy (last_dir, strrchr(cwd, '/') + 1);
		strcat (last_dir, "/");
		going_up = 1;
	}

	old_curr_plist = curr_plist;
	curr_plist = (struct plist *)xmalloc (sizeof(struct plist));
	plist_init (curr_plist);

	if (!read_directory(new_dir, curr_plist, &dirs, &ndirs)) {
		if (chdir(cwd))
			/* FIXME: something smarter? */
			interface_fatal ("Can't go to the previous directory.");
		set_interface_status (NULL);
		wrefresh (info_win);
		plist_free (curr_plist);
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
	
	qsort (dirs, ndirs, sizeof(char *), qsort_dirs_func);
	plist_sort_fname (curr_plist);
	
	menu = make_menu (curr_plist, dirs, ndirs);
	if (going_up)
		menu_setcurritem_title (menu, last_dir);
	
	free_dir_tab (dirs, ndirs);
	set_title (cwd);

	update_state ();
	sprintf (msg, "%d files and directories", curr_plist->num + ndirs - 1);
	set_interface_status (msg);
	wrefresh (info_win);

	return 1;
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

/* Initialize the interface */
void init_interface (const int sock, const int debug)
{
	srv_sock = sock;
	set_start_dir ();
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

	if (has_colors()) {
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
	}

	/* windows */
	main_win = newwin (LINES - 4, COLS, 0, 0);
	keypad (main_win, TRUE);
	info_win = newwin (4, COLS, LINES - 4, 0);
	if (has_colors()) {
		wbkgd (main_win, COLOR_PAIR(CLR_ITEM));
		wbkgd (info_win, COLOR_PAIR(CLR_ITEM));
	}


	msg_timeout = time(NULL) + 3;
	reset_file_info ();
	set_interface_status (NULL);
	xterm_set_state (STATE_STOP);
	xterm_set_title ("");
	
	main_border ();
	get_server_options ();
	
	if (!go_to_dir(NULL))
		interface_fatal ("Can't go to the initial directory.");
	menu_draw (menu);
	wrefresh (main_win);
	update_state ();
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
		default:
			interface_message ("Unknown event: 0x%02x", event);
			logit ("Unknown event 0x%02x", event);
	}

	if (must_update_state) {
		update_state ();
		must_update_state = 0;
	}
}

static void send_playlist (struct plist *plist)
{
	int i;
	
	send_int_to_srv (CMD_LIST_CLEAR);
	
	for (i = 0; i < plist->num; i++) {
		send_int_to_srv (CMD_LIST_ADD);
		send_str_to_srv (plist->items[i].file);
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

/* Action when the user selected a file. */
static void go_file ()
{
	struct menu_item *menu_item = menu_curritem (menu);

	if (menu_item->plist_pos != -1) {

		/* It's a file */
		play_it (menu_item->plist_pos);
	}
	else {
		/* it's a directory */
		char dir[PATH_MAX + 1];

		strcpy (dir, cwd);
		resolve_path (dir, sizeof(dir), menu_item->title);
		
		go_to_dir (dir);
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

/* Switch between the current playlist and the playlist
 * (curr_plist/playlist). */
static void toggle_plist ()
{
	if (visible_plist == playlist) {
		visible_plist = curr_plist;
		menu_free (menu);
		menu = saved_menu;
		set_title (cwd);
		update_curr_file ();
	}
	else if (playlist && playlist->num) {
		visible_plist = playlist;
		saved_menu = menu;
		menu = make_menu (playlist, NULL, 0);
		set_title ("Playlist");
		update_curr_file ();
	}
	else
		interface_error ("The playlist is empty.");
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

	/* TODO: dont allow to add a file that is already in the list */
	plist_add_from_item (playlist,
			&curr_plist->items[menu_item->plist_pos]);
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
	read_directory_recurr (dir, playlist); /* TODO: don't allow to add
						  a file if it's already
						  on the list */
	if (options_get_int("ReadTags")) {
		read_tags (playlist);
		make_titles_tags (playlist);
	}
	else
		make_titles_file (playlist);
	
	plist_sort_fname (playlist);

	sprintf (msg, "%d files on the list", playlist->num);
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
	if (has_colors())
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
"  q            Detach MOC from the server\n"
"  Q            Quit\n");

	wrefresh (main_win);
}

static void help_screen ()
{
	main_win_mode = WIN_HELP;
	print_help_screen ();
}

/* Handle key */
static void menu_key (const int ch)
{
	int update_menu = 0;

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
		return;
	}
	
	switch (ch) {
		case 'q':
			want_quit = 1;
			send_int (srv_sock, CMD_DISCONNECT);
			break;
		case '\n':
			go_file ();
			update_menu = 1;
			break;
		case KEY_DOWN:
			menu_driver (menu, REQ_DOWN);
			update_menu = 1;
			break;
		case KEY_UP:
			menu_driver (menu, REQ_UP);
			update_menu = 1;
			break;
		case KEY_NPAGE:
			menu_driver (menu, REQ_PGDOWN);
			update_menu = 1;
			break;
		case KEY_PPAGE:
			menu_driver (menu, REQ_PGUP);
			update_menu = 1;
			break;
		case KEY_HOME:
			menu_driver (menu, REQ_TOP);
			update_menu = 1;
			break;
		case KEY_END:
			menu_driver (menu, REQ_BOTTOM);
			update_menu = 1;
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
		case 'f': /* TODO */
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
			update_menu = 1;
			break;
		case 'a':
			add_file_plist ();
			break;
		case 'C':
			clear_playlist ();
			update_menu = 1;
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
		case KEY_RESIZE:
			break;
		default:
			interface_error ("Bad key");
	}

	if (update_menu) {
		werase (main_win);
		main_border ();
		menu_draw (menu);
		wrefresh (main_win);
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
			if (msg_timeout && msg_timeout < time(NULL)) {
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

void interface_end ()
{
	close (srv_sock);
	endwin ();
	xterm_clear_title ();
}
