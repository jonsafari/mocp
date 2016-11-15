/*
 * MOC - music on console
 * Copyright (C) 2004 - 2006 Damian Pietras <daper@daper.net>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * Other authors:
 *  - Kamil Tarkowski <kamilt@interia.pl> - sec_to_min_plist()
 */

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <assert.h>
#include <errno.h>
#include <math.h>
#include <time.h>
#include <unistd.h>
#include <ctype.h>
#include <wctype.h>
#include <wchar.h>

#include "common.h"
#include "menu.h"
#include "themes.h"
#include "lists.h"
#include "options.h"
#include "interface_elements.h"
#include "log.h"
#include "files.h"
#include "decoder.h"
#include "keys.h"
#include "playlist.h"
#include "protocol.h"
#include "interface.h"
#include "utf8.h"
#include "rcc.h"
#include "lyrics.h"

#ifndef PACKAGE_REVISION
#define STARTUP_MESSAGE "Welcome to " PACKAGE_NAME \
                        " (version " PACKAGE_VERSION ")!"
#else
#define STARTUP_MESSAGE "Welcome to " PACKAGE_NAME \
                        " (version " PACKAGE_VERSION \
                        ", revision " PACKAGE_REVISION ")!"
#endif
#define HISTORY_SIZE	50


/* TODO: removing/adding a char to the entry may increase width of the text
 * by more than one column. */

/* Type of the side menu. */
enum side_menu_type
{
	MENU_DIR,	/* list of files in a directory */
	MENU_PLAYLIST,	/* a playlist of files */
	MENU_THEMES,	/* list of available themes */
	MENU_TREE	/* tree of directories */
};

struct side_menu
{
	enum side_menu_type type;
	int visible;	/* is it visible (are the other fields initialized) ? */
	WINDOW *win; 	/* window for the menu */
	char *title;	/* title of the window */

	/* Position and size of the menu in the window. */
	int posx;
	int posy;
	int width;
	int height;

	int total_time; /* total time of the files on the playlist */
	int total_time_for_all; /* is the total file counted for all files? */

	union
	{
		struct {
			struct menu *main;    /* visible menu */
			struct menu *copy;    /* copy of the menu when we display
			                         matching items while searching */
		} list;
		/* struct menu_tree *tree;*/
	} menu;
};

/* State of the side menu that can be read/restored.  It remembers the state
 * (position of the view, which file is selected, etc) of the menu. */
struct side_menu_state
{
	struct menu_state menu_state;
};

/* When used instead of the size parameter it means: fill to the end of the
 * window. */
#define LAYOUT_SIZE_FILL	(-1)

struct window_params
{
	int x, y;
	int width, height;
};

struct main_win_layout
{
	struct window_params menus[3];
};

static struct main_win
{
	WINDOW *win;
	char *curr_file; /* currently played file. */

	int in_help; /* are we displaying help screen? */
	int too_small; /* is the terminal window too small to display mocp? */
	int help_screen_top; /* first visible line of the help screen. */
	int in_lyrics; /* are we displaying lyrics screen? */
	int lyrics_screen_top; /* first visible line of the lyrics screen. */

	struct side_menu menus[3];
	lists_t_strs *layout_fmt;
	int selected_menu; /* which menu is currently selected by the user */
} main_win;

/* Bar for displaying mixer state or progress. */
struct bar
{
	int width;	/* width in chars */
	float filled;	/* how much is it filled in percent */
	char *orig_title;	/* optional title */
	char title[512];	/* title with the percent value */
	int show_val;	/* show the title and the value? */
	int show_pct;	/* show percentage in the title value? */
	int fill_color;	/* color (ncurses attributes) of the filled part */
	int empty_color;	/* color of the empty part */
};

/* History for entries' values. */
struct entry_history
{
	char *items[HISTORY_SIZE];
	int num;	/* number of items */
};

/* An input area where a user can type text to enter a file name etc. */
struct entry
{
	enum entry_type type;
	int width;		/* width of the entry part for typing */

	/* The text the user types: */
	wchar_t text_ucs[512];	/* unicode */
	wchar_t saved_ucs[512];	/* unicode saved during history scrolling */

	char *title;		/* displayed title */
	char *file;		/* optional: file associated with the entry */
	int cur_pos;		/* cursor position */
	int display_from;	/* displaying from this char */
	struct entry_history *history;	/* history to use with this entry or
					   NULL is history is not used */
	int history_pos;	/* current position in the history */
};

/* Type of message. */
enum message_type
{
	NORMAL_MSG,
	ERROR_MSG,
	QUERY_MSG
};

/* Save a new message for display. */
struct queued_message
{
	struct queued_message *next;
	/* What type is this message? */
	enum message_type type;
	/* Message to be displayed instead of the file's title. */
	char *msg;
	/* Prompt to use for user query menu. */
	char *prompt;
	/* How many seconds does the message linger? */
	time_t timeout;
	/* The callback function and opaque data for user replies. */
	t_user_reply_callback *callback;
	void *data;
};

static struct info_win
{
	WINDOW *win;

	struct queued_message *current_message;	/* Message currently being displayed */

	struct queued_message *queued_message_head;	/* FIFO queue on which pending */
	struct queued_message *queued_message_tail;	/*          messages get saved */
	int queued_message_total;					/* Total messages on queue */
	int queued_message_errors;					/* Error messages on queue */

	int too_small; /* is the current window too small to display this widget? */

	struct entry entry;
	int in_entry;		/* are we using the entry (is the above
				   structure initialized)?  */
	struct entry_history urls_history;
	struct entry_history dirs_history;
	struct entry_history user_history;

	/* true/false options values */
	bool state_stereo;
	bool state_shuffle;
	bool state_repeat;
	bool state_next;
	bool state_net;

	int bitrate;		/* in kbps */
	int rate;		/* in kHz */

	int files_in_queue;

	/* time in seconds */
	int curr_time;
	int total_time;
	int block_start;
	int block_end;

	int plist_time;		/* total time of files displayed in the menu */
	int plist_time_for_all;	/* is the above time for all files? */

	char *title;		/* title of the played song. */
	char status_msg[26];	/* status message */
	int state_play;		/* STATE_(PLAY | STOP | PAUSE) */

	/* Saved user reply callback data. */
	t_user_reply_callback *callback;
	void *data;

	struct bar mixer_bar;
	struct bar time_bar;
} info_win;

/* Are we running on xterm? */
static bool has_xterm = false;

/* Are we running inside screen? */
static bool has_screen = false;

/* Was the interface initialized? */
static int iface_initialized = 0;

/* Was initscr() called? */
static int screen_initialized = 0;

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

static void entry_history_init (struct entry_history *h)
{
	assert (h != NULL);

	h->num = 0;
}

static void entry_history_add (struct entry_history *h,	const char *text)
{
	assert (h != NULL);
	assert (text != NULL);

	if (strlen (text) != strspn (text, " ")) {
		if (h->num == 0 || strcmp (text, h->items[h->num - 1])) {
			if (h->num < HISTORY_SIZE)
				h->items[h->num++] = xstrdup (text);
			else {
				free (h->items[0]);
				memmove (h->items, h->items + 1,
						(HISTORY_SIZE - 1) * sizeof (char *));
				h->items[h->num - 1] = xstrdup (text);
			}
		}
	}
}

static void entry_history_replace (struct entry_history *h, int num, const char *text)
{
	assert (h != NULL);
	assert (LIMIT(num, h->num));
	assert (text != NULL);

	if (strlen (text) != strspn (text, " ") &&
	    strcmp (h->items[num], text)) {
		free (h->items[num]);
		h->items[num] = xstrdup (text);
	}
}

static void entry_history_clear (struct entry_history *h)
{
	int i;

	assert (h != NULL);

	for (i = 0; i < h->num; i++)
		free (h->items[i]);

	h->num = 0;
}

static int entry_history_nitems (const struct entry_history *h)
{
	assert (h != NULL);

	return h->num;
}

static char *entry_history_get (const struct entry_history *h, const int num)
{
	assert (h != NULL);
	assert (LIMIT(num, h->num));

	return xstrdup (h->items[num]);
}

/* Draw the entry.  Use this function at the end of screen drawing
 * because it sets the cursor position in the right place. */
static void entry_draw (const struct entry *e, WINDOW *w, const int posx,
		const int posy)
{
	char *text;
	wchar_t *text_ucs;
	int len;

	assert (e != NULL);
	assert (w != NULL);
	assert (posx >= 0);
	assert (posy >= 0);

	wmove (w, posy, posx);
	wattrset (w, get_color(CLR_ENTRY_TITLE));
	xwprintw (w, "%s", e->title);

	wattrset (w, get_color(CLR_ENTRY));
	len = wcslen(e->text_ucs) - e->display_from;

	text_ucs = (wchar_t *)xmalloc(sizeof(wchar_t) * (len + 1));
	memcpy (text_ucs, e->text_ucs + e->display_from,
			sizeof(wchar_t) * (len + 1));
	if (len > e->width)
		text_ucs[e->width] = L'\0';
	len = wcstombs (NULL, text_ucs, -1) + 1;
	assert (len >= 1);

	text = (char *)xmalloc (len);
	wcstombs (text, text_ucs, len);

	xwprintw (w, " %-*s", e->width, text);

	/* Move the cursor */
	wmove (w, posy, e->cur_pos - e->display_from + strwidth(e->title)
			+ posx + 1);

	free (text);
	free (text_ucs);
}

static void entry_init (struct entry *e, const enum entry_type type,
		const int width, struct entry_history *history, const char *prompt)
{
	const char *title;

	assert (e != NULL);

	switch (type) {
		case ENTRY_SEARCH:
			title = "SEARCH";
			break;
		case ENTRY_PLIST_SAVE:
			title = "SAVE PLAYLIST";
			break;
		case ENTRY_GO_DIR:
			title = "GO";
			break;
		case ENTRY_GO_URL:
			title = "URL";
			break;
		case ENTRY_ADD_URL:
			title = "ADD URL";
			break;
		case ENTRY_PLIST_OVERWRITE:
			title = "File exists, overwrite?";
			break;
		case ENTRY_USER_QUERY:
			title = prompt;
			break;
		default:
			abort ();
	}

	e->type = type;
	e->text_ucs[0] = L'\0';
	e->saved_ucs[0] = L'\0';
	e->file = NULL;
	e->title = xmalloc (strlen (title) + 2);
	strcpy (e->title, title);
	if (e->title[strlen (e->title) - 1] != ':' &&
	    e->title[strlen (e->title) - 1] != '?')
		strcat (e->title, ":");
	e->width = width - strwidth(title);
	e->cur_pos = 0;
	e->display_from = 0;
	e->history = history;

	if (history)
		e->history_pos = history->num;
}

static enum entry_type entry_get_type (const struct entry *e)
{
	assert (e != NULL);

	return e->type;
}

/* Set the entry text as UCS.  Move the cursor to the end. */
static void entry_set_text_ucs (struct entry *e, const wchar_t *text)
{
	int width, len;

	assert (e != NULL);

	len = MIN (wcslen (text) + 1, ARRAY_SIZE (e->text_ucs));
	wmemcpy (e->text_ucs, text, len);
	e->text_ucs[ARRAY_SIZE (e->text_ucs) - 1] = L'\0';

	width = wcswidth (e->text_ucs, WIDTH_MAX);
	e->cur_pos = wcslen (e->text_ucs);

	e->display_from = 0;
	if (e->cur_pos > e->width)
		e->display_from = width - e->width;
}

/* Set the entry text. */
static void entry_set_text (struct entry *e, const char *text)
{
	wchar_t text_ucs[ARRAY_SIZE (e->text_ucs)];

	assert (e != NULL);

	mbstowcs (text_ucs, text, ARRAY_SIZE (e->text_ucs));
	e->text_ucs[ARRAY_SIZE (e->text_ucs) - 1] = L'\0';

	entry_set_text_ucs (e, text_ucs);
}

/* Add a char to the entry where the cursor is placed. */
static void entry_add_char (struct entry *e, const wchar_t c)
{
	size_t len;

	assert (e != NULL);

	len = wcslen (e->text_ucs);
	if (len >= ARRAY_SIZE(e->text_ucs) - sizeof(wchar_t))
		return;

	memmove (e->text_ucs + e->cur_pos + 1,
			e->text_ucs + e->cur_pos,
			(len - e->cur_pos + 1) * sizeof(e->text_ucs[0]));
	e->text_ucs[e->cur_pos] = c;
	e->cur_pos++;

	if (e->cur_pos - e->display_from > e->width)
		e->display_from++;
}

/* Delete 'count' chars before the cursor. */
static void entry_del_chars (struct entry *e, int count)
{
	assert (e != NULL);
	assert (e->cur_pos > 0);

	int width = wcslen (e->text_ucs);
	if (e->cur_pos < count)
		count = e->cur_pos;

	memmove (e->text_ucs + e->cur_pos - count,
	         e->text_ucs + e->cur_pos,
	         (width - e->cur_pos) * sizeof (e->text_ucs[0]));
	width -= count;
	e->text_ucs[width] = L'\0';
	e->cur_pos -= count;

	if (e->cur_pos < e->display_from)
		e->display_from = e->cur_pos;

	/* Can we show more after deleting the chars? */
	if (e->display_from > 0 && width - e->display_from < e->width)
		e->display_from = width - e->width;
	if (e->display_from < 0)
		e->display_from = 0;
}

/* Delete the char before the cursor. */
static void entry_back_space (struct entry *e)
{
	assert (e != NULL);

	if (e->cur_pos > 0)
		entry_del_chars (e, 1);
}

/* Delete the char under the cursor. */
static void entry_del_char (struct entry *e)
{
	int len;

	assert (e != NULL);

	len = wcslen (e->text_ucs);
	if (e->cur_pos < len) {
		e->cur_pos += 1;
		entry_del_chars (e, 1);
	}
}

/* Delete the chars from cursor to start of line. */
static void entry_del_to_start (struct entry *e)
{
	assert (e != NULL);

	if (e->cur_pos > 0)
		entry_del_chars (e, e->cur_pos);
}

/* Delete the chars from cursor to end of line. */
static void entry_del_to_end (struct entry *e)
{
	int len;

	assert (e != NULL);

	len = wcslen (e->text_ucs);
	if (e->cur_pos < len) {
		int count;

		count = len - e->cur_pos;
		e->cur_pos = len;
		entry_del_chars (e, count);
	}
}

/* Move the cursor one char left. */
static void entry_curs_left (struct entry *e)
{
	assert (e != NULL);

	if (e->cur_pos > 0) {
		e->cur_pos--;

		if (e->cur_pos < e->display_from)
			e->display_from--;
	}
}

/* Move the cursor one char right. */
static void entry_curs_right (struct entry *e)
{
	int width;

	assert (e != NULL);

	width = wcslen (e->text_ucs);

	if (e->cur_pos < width) {
		e->cur_pos++;

		if (e->cur_pos > e->width + e->display_from)
			e->display_from++;
	}
}

/* Move the cursor to the end of the entry text. */
static void entry_end (struct entry *e)
{
	int width;

	assert (e != NULL);

	width = wcslen (e->text_ucs);

	e->cur_pos = width;

	if (width > e->width)
		e->display_from = width - e->width;
	else
		e->display_from = 0;
}

/* Move the cursor to the beginning of the entry field. */
static void entry_home (struct entry *e)
{
	assert (e != NULL);

	e->display_from = 0;
	e->cur_pos = 0;
}

static void entry_resize (struct entry *e, const int width)
{
	assert (e != NULL);
	assert (width > 0);

	e->width = width - strlen (e->title);
	entry_end (e);
}

static char *entry_get_text (const struct entry *e)
{
	char *text;
	int len;

	assert (e != NULL);

	len = wcstombs (NULL, e->text_ucs, -1) + 1;
	assert (len >= 1);
	text = (char *) xmalloc (sizeof (char) * len);
	wcstombs (text, e->text_ucs, len);

	return text;
}

/* Copy the previous history item to the entry if available, move the entry
 * history position down. */
static void entry_set_history_up (struct entry *e)
{
	assert (e != NULL);
	assert (e->history != NULL);

	if (e->history_pos > 0) {
		char *t;

		if (e->history_pos == entry_history_nitems (e->history))
			wmemcpy (e->saved_ucs, e->text_ucs, wcslen (e->text_ucs) + 1);
		else {
			t = entry_get_text (e);
			entry_history_replace (e->history, e->history_pos, t);
			free (t);
		}
		e->history_pos--;

		t = entry_history_get (e->history, e->history_pos);
		entry_set_text (e, t);
		free (t);
	}
}

/* Copy the next history item to the entry if available, move the entry history
 * position down. */
static void entry_set_history_down (struct entry *e)
{
	assert (e != NULL);
	assert (e->history != NULL);

	if (e->history_pos < entry_history_nitems (e->history)) {
		char *t;

		t = entry_get_text (e);
		entry_history_replace (e->history, e->history_pos, t);
		free (t);

		e->history_pos++;
		if (e->history_pos == entry_history_nitems (e->history))
			entry_set_text_ucs (e, e->saved_ucs);
		else {
			t = entry_history_get (e->history, e->history_pos);
			entry_set_text (e, t);
			free (t);
		}
	}
}

static void entry_set_file (struct entry *e, const char *file)
{
	assert (e != NULL);
	assert (file != NULL);

	if (e->file)
		free (e->file);
	e->file = xstrdup (file);
}

static char *entry_get_file (const struct entry *e)
{
	return xstrdup (e->file);
}

static void entry_destroy (struct entry *e)
{
	assert (e != NULL);

	if (e->file)
		free (e->file);
	if (e->title)
		free (e->title);
}

static void entry_add_text_to_history (struct entry *e)
{
	char *text;

	assert (e != NULL);
	assert (e->history);

	text = entry_get_text (e);
	entry_history_add (e->history, text);
	free (text);
}

/* Return the list menu height inside the side menu. */
static int side_menu_get_menu_height (const struct side_menu *m)
{
	if (m->posy + m->height == LINES - 4)
		return m->height - 1;
	return m->height - 2;
}

static void side_menu_init_menu (struct side_menu *m)
{
	assert (m != NULL);

	m->menu.list.main = menu_new (m->win, m->posx + 1, m->posy + 1,
			m->width - 2, side_menu_get_menu_height (m));
}

static void side_menu_init (struct side_menu *m, const enum side_menu_type type,
		WINDOW *parent_win, const struct window_params *wp)
{
	assert (m != NULL);
	assert (parent_win != NULL);
	assert (wp != NULL);
	assert (wp->width >= 8);
	assert (wp->height >= 3);

	m->type = type;
	m->win = parent_win;
	m->posx = wp->x;
	m->posy = wp->y;
	m->height = wp->height;
	m->width = wp->width;

	m->title = NULL;

	m->total_time = 0;
	m->total_time_for_all = 0;

	if (type == MENU_DIR || type == MENU_PLAYLIST) {
		side_menu_init_menu (m);
		m->menu.list.copy = NULL;

		menu_set_items_numbering (m->menu.list.main,
				type == MENU_PLAYLIST
				&& options_get_bool("PlaylistNumbering"));
		menu_set_show_format (m->menu.list.main,
				options_get_bool("ShowFormat"));
		menu_set_show_time (m->menu.list.main,
				strcasecmp(options_get_symb("ShowTime"), "no"));
		menu_set_info_attr_normal (m->menu.list.main,
				get_color(CLR_MENU_ITEM_INFO));
		menu_set_info_attr_sel (m->menu.list.main,
				get_color(CLR_MENU_ITEM_INFO_SELECTED));
		menu_set_info_attr_marked (m->menu.list.main,
				get_color(CLR_MENU_ITEM_INFO_MARKED));
		menu_set_info_attr_sel_marked (m->menu.list.main,
				get_color(CLR_MENU_ITEM_INFO_MARKED_SELECTED));
	}
	else if (type == MENU_THEMES) {
		side_menu_init_menu (m);
		m->menu.list.copy = NULL;
	}
	else
		abort ();

	m->visible = 1;
}

static void side_menu_destroy (struct side_menu *m)
{
	assert (m != NULL);

	if (m->visible) {
		if (m->type == MENU_DIR || m->type == MENU_PLAYLIST
				|| m->type == MENU_THEMES) {
			menu_free (m->menu.list.main);
			if (m->menu.list.copy)
				menu_free (m->menu.list.copy);
		}
		else
			abort ();

		if (m->title)
			free (m->title);
		m->visible = 0;
	}
}

static void side_menu_set_title (struct side_menu *m, const char *title)
{
	assert (m != NULL);
	assert (title != NULL);

	if (m->title)
		free (m->title);
	m->title = xstrdup (title);
}

/* Parse one layout coordinate from "0,2,54%,1" and put it in val.
 * Max is the maximum value of the field.  It's also used when processing
 * percent values.
 * Return false on error. */
static bool parse_layout_coordinate (const char *fmt, int *val, const int max)
{
	long v;
	const char *e = fmt;

	if (!strcasecmp (fmt, "FILL")) {
		*val = LAYOUT_SIZE_FILL;
		return true;
	}

	v = strtol (fmt, (char **)&e, 10);
	if (e == fmt)
		return false;

	if (*e == '%')
		v = lroundf (max * v / 100.0 - 0.1);
	*val = v;

	if (!RANGE(0, *val, max)) {
		logit ("Coordinate out of range - %d is not in (0, %d)", *val, max);
		return false;
	}

	return true;
}

/* Parse the layout string. Return false on error. */
static bool parse_layout (struct main_win_layout *l, lists_t_strs *fmt)
{
	int ix;
	bool result = false;
	lists_t_strs *format;

	assert (l != NULL);
	assert (fmt != NULL);

	/* default values */
	l->menus[0].x = 0;
	l->menus[0].y = 0;
	l->menus[0].width = COLS;
	l->menus[0].height = LINES - 4;
	l->menus[1] = l->menus[0];
	l->menus[2] = l->menus[0];

	format = lists_strs_new (6);
	for (ix = 0; ix < lists_strs_size (fmt); ix += 1) {
		const char *menu, *name;
		struct window_params p;

		lists_strs_clear (format);
		menu = lists_strs_at (fmt, ix);
		if (lists_strs_split (format, menu, "(,)") != 5)
			goto err;

		name = lists_strs_at (format, 0);

		if (!parse_layout_coordinate (lists_strs_at (format, 1), &p.x, COLS)) {
			logit ("Coordinate parse error when parsing X");
			goto err;
		}
		if (!parse_layout_coordinate (lists_strs_at (format, 2), &p.y, LINES - 4)) {
			logit ("Coordinate parse error when parsing Y");
			goto err;
		}
		if (!parse_layout_coordinate (lists_strs_at (format, 3), &p.width, COLS)) {
			logit ("Coordinate parse error when parsing width");
			goto err;
		}
		if (!parse_layout_coordinate (lists_strs_at (format, 4), &p.height, LINES - 4)) {
			logit ("Coordinate parse error when parsing height");
			goto err;
		}

		if (p.width == LAYOUT_SIZE_FILL)
			p.width = COLS - p.x;
		if (p.height == LAYOUT_SIZE_FILL)
			p.height = LINES - 4 - p.y;

		if (p.width < 15) {
			logit ("Width is less than 15");
			goto err;
		}
		if (p.height < 2) {
			logit ("Height is less than 2");
			goto err;
		}
		if (p.x + p.width > COLS) {
			logit ("X + width is more than COLS (%d)", COLS);
			goto err;
		}
		if (p.y + p.height > LINES - 4) {
			logit ("Y + height is more than LINES - 4 (%d)", LINES - 4);
			goto err;
		}

		if (!strcmp(name, "directory"))
			l->menus[MENU_DIR] = p;
		else if (!strcmp(name, "playlist"))
			l->menus[MENU_PLAYLIST] = p;
		else {
			logit ("Bad subwindow name '%s'", name);
			goto err;
		}
	}

	result = true;

err:
	lists_strs_free (format);
	return result;
}

static void main_win_init (struct main_win *w, lists_t_strs *layout_fmt)
{
	struct main_win_layout l;
	bool rc ASSERT_ONLY;

	assert (w != NULL);

	w->win = newwin (LINES - 4, COLS, 0, 0);
	wbkgd (w->win, get_color(CLR_BACKGROUND));
	nodelay (w->win, TRUE);
	keypad (w->win, TRUE);

	w->curr_file = NULL;
	w->in_help = 0;
	w->in_lyrics = 0;
	w->too_small = 0;
	w->help_screen_top = 0;
	w->lyrics_screen_top = 0;
	w->layout_fmt = layout_fmt;

	rc = parse_layout (&l, layout_fmt);
	assert (rc);

	side_menu_init (&w->menus[0], MENU_DIR, w->win, &l.menus[0]);
	side_menu_init (&w->menus[1], MENU_PLAYLIST, w->win, &l.menus[1]);
	side_menu_set_title (&w->menus[1], "Playlist");
	w->menus[2].visible = 0;

	w->selected_menu = 0;
}

static void main_win_destroy (struct main_win *w)
{
	assert (w != NULL);

	side_menu_destroy (&w->menus[0]);
	side_menu_destroy (&w->menus[1]);
	side_menu_destroy (&w->menus[2]);

	if (w->win)
		delwin (w->win);
	if (w->curr_file)
		free (w->curr_file);
}

/* Make a title suitable to display in a menu from the title of a playlist item.
 * Returned memory is malloc()ed.
 * made_from tags - was the playlist title made from tags?
 * full_paths - If the title is the file name, use the full path?
 */
static char *make_menu_title (const char *plist_title,
		const int made_from_tags, const int full_path)
{
	char *title = xstrdup (plist_title);

	if (!made_from_tags) {
		if (!full_path && !is_url (title)) {

			/* Use only the file name instead of the full path. */
			char *slash = strrchr (title, '/');

			if (slash && slash != title) {
				char *old_title = title;

				title = xstrdup (slash + 1);
				free (old_title);
			}
		}
	}

	return title;
}

/* Add an item from the playlist to the menu.
 * If full_paths has non-zero value, full paths will be displayed instead of
 * just file names.
 * Return a non-zero value if the added item is visible on the screen. */
static int add_to_menu (struct menu *menu, const struct plist *plist,
		const int num, const int full_paths)
{
	bool made_from_tags;
	struct menu_item *added;
	const struct plist_item *item = &plist->items[num];
	char *title;
	const char *type_name;

	made_from_tags = (options_get_bool ("ReadTags") && item->title_tags);

	if (made_from_tags)
		title = make_menu_title (item->title_tags, 1, 0);
	else
		title = make_menu_title (item->title_file, 0, full_paths);
	added = menu_add (menu, title, plist_file_type (plist, num), item->file);
	free (title);

	if (item->tags && item->tags->time != -1) {
		char time_str[6];

		sec_to_min (time_str, item->tags->time);
		menu_item_set_time (added, time_str);
	}

	menu_item_set_attr_normal (added, get_color(CLR_MENU_ITEM_FILE));
	menu_item_set_attr_sel (added, get_color(CLR_MENU_ITEM_FILE_SELECTED));
	menu_item_set_attr_marked (added, get_color(CLR_MENU_ITEM_FILE_MARKED));
	menu_item_set_attr_sel_marked (added,
			get_color(CLR_MENU_ITEM_FILE_MARKED_SELECTED));

	if (!(type_name = file_type_name(item->file)))
		type_name = "";
	menu_item_set_format (added, type_name);
	menu_item_set_queue_pos (added, item->queue_pos);

	if (full_paths && !made_from_tags)
		menu_item_set_align (added, MENU_ALIGN_RIGHT);

	return menu_is_visible (menu, added);
}

static void side_menu_clear (struct side_menu *m)
{
	assert (m != NULL);
	assert (m->visible);
	assert (m->type == MENU_DIR || m->type == MENU_PLAYLIST);
	assert (m->menu.list.main != NULL);
	assert (m->menu.list.copy == NULL);

	menu_free (m->menu.list.main);
	side_menu_init_menu (m);
	menu_set_items_numbering (m->menu.list.main, m->type == MENU_PLAYLIST
			&& options_get_bool("PlaylistNumbering"));

	menu_set_show_format (m->menu.list.main, options_get_bool("ShowFormat"));
	menu_set_show_time (m->menu.list.main,
			strcasecmp(options_get_symb("ShowTime"), "no"));
	menu_set_info_attr_normal (m->menu.list.main, get_color(CLR_MENU_ITEM_INFO));
	menu_set_info_attr_sel (m->menu.list.main, get_color(CLR_MENU_ITEM_INFO_SELECTED));
	menu_set_info_attr_marked (m->menu.list.main, get_color(CLR_MENU_ITEM_INFO_MARKED));
	menu_set_info_attr_sel_marked (m->menu.list.main, get_color(CLR_MENU_ITEM_INFO_MARKED_SELECTED));
}

/* Fill the directory or playlist side menu with this content. */
static void side_menu_make_list_content (struct side_menu *m,
		const struct plist *files, const lists_t_strs *dirs,
		const lists_t_strs *playlists, const int add_up_dir)
{
	struct menu_item *added;
	int i;

	assert (m != NULL);
	assert (m->type == MENU_DIR || m->type == MENU_PLAYLIST);
	assert (m->menu.list.main != NULL);
	assert (m->menu.list.copy == NULL);

	side_menu_clear (m);

	if (add_up_dir) {
		added = menu_add (m->menu.list.main, "../", F_DIR, "..");
		menu_item_set_attr_normal (added, get_color(CLR_MENU_ITEM_DIR));
		menu_item_set_attr_sel (added,
				get_color(CLR_MENU_ITEM_DIR_SELECTED));
	}

	if (dirs)
		for (i = 0; i < lists_strs_size (dirs) ; i++) {
			char title[PATH_MAX];

#ifdef HAVE_RCC
			char *t_str = NULL;
			if (options_get_bool("UseRCCForFilesystem")) {
				strcpy (title, strrchr (lists_strs_at (dirs, i), '/') + 1);
				strcat (title, "/");
				t_str = xstrdup (title);
				t_str = rcc_reencode (t_str);
				snprintf(title, PATH_MAX, "%s", t_str);
				free(t_str);
			}
			else
#endif
			if (options_get_bool ("FileNamesIconv"))
			{
				char *conv_title = files_iconv_str (
						strrchr (lists_strs_at (dirs, i), '/') + 1);

				strcpy (title, conv_title);
				strcat (title, "/");

				free (conv_title);
			}
			else
			{
				strcpy (title, strrchr (lists_strs_at (dirs, i), '/') + 1);
				strcat (title, "/");
			}

			added = menu_add (m->menu.list.main, title, F_DIR,
					lists_strs_at (dirs, i));
			menu_item_set_attr_normal (added,
					get_color(CLR_MENU_ITEM_DIR));
			menu_item_set_attr_sel (added,
					get_color(CLR_MENU_ITEM_DIR_SELECTED));
		}

	if (playlists)
		for (i = 0; i < lists_strs_size (playlists); i++){
			added = menu_add (m->menu.list.main,
					strrchr (lists_strs_at (playlists, i), '/') + 1,
					F_PLAYLIST, lists_strs_at (playlists, i));
			menu_item_set_attr_normal (added,
					get_color(CLR_MENU_ITEM_PLAYLIST));
			menu_item_set_attr_sel (added,
					get_color(
					CLR_MENU_ITEM_PLAYLIST_SELECTED));
		}

	/* playlist items */
	for (i = 0; i < files->num; i++) {
		if (!plist_deleted(files, i))
			add_to_menu (m->menu.list.main, files, i,
					m->type == MENU_PLAYLIST
					&& options_get_bool("PlaylistFullPaths"));
	}

	m->total_time = plist_total_time (files, &m->total_time_for_all);
}

static void clear_area (WINDOW *w, const int posx, const int posy,
		const int width, const int height)
{
	int y;
	char line[512];

	assert (width < ssizeof(line));

	memset (line, ' ', width);
	line[width] = 0;

	wattrset (w, get_color(CLR_BACKGROUND));
	for (y = posy; y < posy + height; y++) {
		wmove (w, y, posx);
		xwaddstr (w, line);
	}
}

static void side_menu_draw_frame (const struct side_menu *m)
{
	char *title;

	assert (m != NULL);
	assert (m->visible);

	if (m->title) {
		if ((int)strwidth(m->title) > m->width - 4) {
			char *tail;

			tail = xstrtail (m->title, m->width - 7);
			title = (char *)xmalloc (strlen(tail) + 4);
			sprintf (title, "...%s", tail);
			free (tail);
		}
		else
			title = xstrdup (m->title);
	}
	else
		title = NULL;

	/* Border */
	wattrset (m->win, get_color(CLR_FRAME));

	/* upper left corner */
	wmove (m->win, m->posy, m->posx);
	waddch (m->win, lines.ulcorn);

	/* upper line */
	whline (m->win, lines.horiz, m->width - 2);

	/* upper right corner */
	wmove (m->win, m->posy, m->posx + m->width - 1);
	waddch (m->win, lines.urcorn);

	/* left line */
	wmove (m->win, m->posy + 1, m->posx);
	wvline (m->win, lines.vert, m->height - 1);

	/* right line */
	wmove (m->win, m->posy + 1, m->posx + m->width - 1);
	wvline (m->win, lines.vert, m->height - 1);

	if (m->posy + m->height < LINES - 4) {

		/* bottom left corner */
		wmove (m->win, m->posy + m->height - 1, m->posx);
		waddch (m->win, lines.llcorn);

		/* bottom line */
		whline (m->win, lines.horiz, m->width - 2);

		/* bottom right corner */
		wmove (m->win, m->posy + m->height - 1, m->posx + m->width - 1);
		waddch (m->win, lines.lrcorn);
	}

	/* The title */
	if (title) {
		wmove (m->win, m->posy, m->posx + m->width / 2
				- strwidth(title) / 2 - 1);

		wattrset (m->win, get_color(CLR_FRAME));
		waddch (m->win, lines.rtee);

		wattrset (m->win, get_color(CLR_WIN_TITLE));
		xwaddstr (m->win, title);

		wattrset (m->win, get_color(CLR_FRAME));
		waddch (m->win, lines.ltee);

		free (title);
	}
}

static void side_menu_draw (const struct side_menu *m, const int active)
{
	assert (m != NULL);
	assert (m->visible);

	clear_area (m->win, m->posx, m->posy, m->width, m->height);
	side_menu_draw_frame (m);

	if (m->type == MENU_DIR || m->type == MENU_PLAYLIST
			|| m->type == MENU_THEMES) {
		menu_draw (m->menu.list.main, active);
		if (options_get_bool("UseCursorSelection"))
			menu_set_cursor (m->menu.list.main);
	}
	else
		abort ();
}

static void side_menu_cmd (struct side_menu *m, const enum key_cmd cmd)
{
	assert (m != NULL);
	assert (m->visible);

	if (m->type == MENU_DIR || m->type == MENU_PLAYLIST
			|| m->type == MENU_THEMES) {
		switch (cmd) {
			case KEY_CMD_MENU_DOWN:
				menu_driver (m->menu.list.main, REQ_DOWN);
				break;
			case KEY_CMD_MENU_UP:
				menu_driver (m->menu.list.main, REQ_UP);
				break;
			case KEY_CMD_MENU_NPAGE:
				menu_driver (m->menu.list.main, REQ_PGDOWN);
				break;
			case KEY_CMD_MENU_PPAGE:
				menu_driver (m->menu.list.main, REQ_PGUP);
				break;
			case KEY_CMD_MENU_FIRST:
				menu_driver (m->menu.list.main, REQ_TOP);
				break;
			case KEY_CMD_MENU_LAST:
				menu_driver (m->menu.list.main, REQ_BOTTOM);
				break;
			default:
				abort ();
		}
	}
	else
		abort ();
}

static enum file_type side_menu_curritem_get_type (const struct side_menu *m)
{
	struct menu_item *mi;

	assert (m != NULL);
	assert (m->visible);
	assert (m->type == MENU_DIR || m->type == MENU_PLAYLIST
			|| m->type == MENU_THEMES);

	mi = menu_curritem (m->menu.list.main);

	if (mi)
		return menu_item_get_type (mi);

	return F_OTHER;
}

static char *side_menu_get_curr_file (const struct side_menu *m)
{
	struct menu_item *mi;

	assert (m != NULL);
	assert (m->visible);
	assert (m->type == MENU_DIR || m->type == MENU_PLAYLIST
			|| m->type == MENU_THEMES);

	mi = menu_curritem (m->menu.list.main);

	if (mi)
		return menu_item_get_file (mi);

	return NULL;
}

static struct side_menu *find_side_menu (struct main_win *w,
		const enum side_menu_type type)
{
	size_t ix;

	assert (w != NULL);

	for (ix = 0; ix < ARRAY_SIZE(w->menus); ix += 1) {
		struct side_menu *m = &w->menus[ix];

		if (m->visible && m->type == type)
			return m;
	}

	abort (); /* menu not found - BUG */
}

static void side_menu_set_curr_item_title (struct side_menu *m,
		const char *title)
{
	assert (m != NULL);
	assert (m->visible);
	assert (title != NULL);

	menu_setcurritem_title (m->menu.list.main, title);
}

/* Update menu item using the playlist item. */
static void update_menu_item (struct menu_item *mi,
		const struct plist *plist,
		const int n, const int full_path)
{
	bool made_from_tags;
	char *title;
	const struct plist_item *item;

	assert (mi != NULL);
	assert (plist != NULL);
	assert (n >= 0);

	item = &plist->items[n];

	if (item->tags && item->tags->time != -1) {
		char time_str[6];

		sec_to_min (time_str, item->tags->time);
		menu_item_set_time (mi, time_str);
	}
	else
		menu_item_set_time (mi, "");

	made_from_tags = (options_get_bool ("ReadTags") && item->title_tags);

	if (made_from_tags)
		title = make_menu_title (item->title_tags, 1, 0);
	else
		title = make_menu_title (item->title_file, 0, full_path);

	menu_item_set_title (mi, title);

	if (full_path && !made_from_tags)
		menu_item_set_align (mi, MENU_ALIGN_RIGHT);
	else
		menu_item_set_align (mi, MENU_ALIGN_LEFT);

	menu_item_set_queue_pos (mi, item->queue_pos);

	free (title);

}

/* Update item title and time for this item if it's present on this menu.
 * Return a non-zero value if the item is visible. */
static int side_menu_update_item (struct side_menu *m,
		const struct plist *plist, const int n)
{
	struct menu_item *mi;
	int visible = 0;
	char *file;

	assert (m != NULL);
	assert (m->visible);
	assert (m->type == MENU_DIR || m->type == MENU_PLAYLIST);
	assert (plist != NULL);
	assert (LIMIT(n, plist->num));

	file = plist_get_file (plist, n);
	assert (file != NULL);

	if ((mi = menu_find(m->menu.list.main, file))) {
		update_menu_item (mi, plist, n, m->type == MENU_PLAYLIST
				&& options_get_bool("PlaylistFullpaths"));
		visible = menu_is_visible (m->menu.list.main, mi);
	}
	if (m->menu.list.copy
			&& (mi = menu_find(m->menu.list.copy, file))) {
		update_menu_item (mi, plist, n, m->type == MENU_PLAYLIST
				&& options_get_bool("PlaylistFullpaths"));
		visible = visible || menu_is_visible (m->menu.list.main, mi);
	}

	free (file);

	m->total_time = plist_total_time (plist, &m->total_time_for_all);

	return visible;
}

static void side_menu_unmark_file (struct side_menu *m)
{
	assert (m != NULL);
	assert (m->visible);
	assert (m->type == MENU_DIR || m->type == MENU_PLAYLIST);

	menu_unmark_item (m->menu.list.main);
	if (m->menu.list.copy)
		menu_unmark_item (m->menu.list.copy);
}

static void side_menu_mark_file (struct side_menu *m, const char *file)
{
	assert (m != NULL);
	assert (m->visible);
	assert (m->type == MENU_DIR || m->type == MENU_PLAYLIST);

	menu_mark_item (m->menu.list.main, file);
	if (m->menu.list.copy)
		menu_mark_item (m->menu.list.copy, file);
}

static void side_menu_add_file (struct side_menu *m, const char *file,
		const char *title, const enum file_type type)
{
	struct menu_item *added;

	added = menu_add (m->menu.list.main, title, type, file);

	menu_item_set_attr_normal (added, get_color(CLR_MENU_ITEM_FILE));
	menu_item_set_attr_sel (added, get_color(CLR_MENU_ITEM_FILE_SELECTED));
	menu_item_set_attr_marked (added, get_color(CLR_MENU_ITEM_FILE_MARKED));
	menu_item_set_attr_sel_marked (added,
			get_color(CLR_MENU_ITEM_FILE_MARKED_SELECTED));
}

static int side_menu_add_plist_item (struct side_menu *m,
		const struct plist *plist, const int num)
{
	int visible;

	assert (m != NULL);
	assert (plist != NULL);
	assert (m->visible);
	assert (m->type == MENU_DIR || m->type == MENU_PLAYLIST);

	visible = add_to_menu (m->menu.list.copy ? m->menu.list.copy
			: m->menu.list.main,
			plist, num,
			m->type == MENU_PLAYLIST
			&& options_get_bool("PlaylistFullPaths"));
	m->total_time = plist_total_time (plist, &m->total_time_for_all);

	return visible;
}

static int side_menu_is_time_for_all (const struct side_menu *m)
{
	assert (m != NULL);
	assert (m->visible);

	return m->total_time_for_all;
}

static int side_menu_get_files_time (const struct side_menu *m)
{
	assert (m != NULL);
	assert (m->visible);

	return m->total_time;
}

static void side_menu_update_show_time (struct side_menu *m)
{
	assert (m != NULL);
	assert (m->visible);
	assert (m->type == MENU_DIR || m->type == MENU_PLAYLIST);

	menu_set_show_time (m->menu.list.main,
				strcasecmp(options_get_symb("ShowTime"), "no"));
}

static void side_menu_update_show_format (struct side_menu *m)
{
	assert (m != NULL);
	assert (m->visible);
	assert (m->type == MENU_DIR || m->type == MENU_PLAYLIST);

	menu_set_show_format (m->menu.list.main, options_get_bool("ShowFormat"));
}

static void side_menu_get_state (const struct side_menu *m,
		struct side_menu_state *st)
{
	assert (m != NULL);
	assert (st != NULL);
	assert (m->visible);
	assert (m->type == MENU_DIR || m->type == MENU_PLAYLIST);

	menu_get_state (m->menu.list.main, &st->menu_state);
}

static void side_menu_set_state (struct side_menu *m,
		const struct side_menu_state *st)
{
	assert (m != NULL);
	assert (st != NULL);
	assert (m->visible);
	assert (m->type == MENU_DIR || m->type == MENU_PLAYLIST);

	menu_set_state (m->menu.list.main, &st->menu_state);
}

static void side_menu_del_item (struct side_menu *m, const char *file)
{
	assert (m != NULL);
	assert (m->visible);
	assert (m->type == MENU_DIR || m->type == MENU_PLAYLIST);

	menu_del_item (m->menu.list.copy ? m->menu.list.copy : m->menu.list.main,
			file);
}

static void side_menu_set_plist_time (struct side_menu *m, const int time,
		const int time_for_all)
{
	assert (m != NULL);
	assert (time >= 0);
	assert (m->type == MENU_DIR || m->type == MENU_PLAYLIST);

	m->total_time = time;
	m->total_time_for_all = time_for_all;
}

/* Replace the menu with one having only those items which contain 'pattern'.
 * If no items match, don't do anything.
 * Return the number of matching items. */
static int side_menu_filter (struct side_menu *m, const char *pattern)
{
	struct menu *filtered_menu;

	assert (m != NULL);
	assert (pattern != NULL);
	assert (m->menu.list.main != NULL);

	filtered_menu = menu_filter_pattern (m->menu.list.copy
			? m->menu.list.copy : m->menu.list.main, pattern);

	if (menu_nitems(filtered_menu) == 0) {
		menu_free (filtered_menu);
		return 0;
	}

	if (m->menu.list.copy)
		menu_free (m->menu.list.main);
	else
		m->menu.list.copy = m->menu.list.main;

	m->menu.list.main = filtered_menu;

	return menu_nitems (filtered_menu);
}

static void side_menu_use_main (struct side_menu *m)
{
	assert (m != NULL);
	assert (m->menu.list.main != NULL);

	if (m->menu.list.copy) {
		menu_free (m->menu.list.main);
		m->menu.list.main = m->menu.list.copy;
		m->menu.list.copy = NULL;
	}
}

static void side_menu_make_visible (struct side_menu *m, const char *file)
{
	assert (m != NULL);
	assert (m->visible);
	assert (m->type == MENU_PLAYLIST || m->type == MENU_DIR);
	assert (file != NULL);

	if (!m->menu.list.copy)
		menu_make_visible (m->menu.list.main, file);
}

static void side_menu_swap_items (struct side_menu *m, const char *file1,
		const char *file2)
{
	assert (m != NULL);
	assert (m->visible);
	assert (m->type == MENU_PLAYLIST || m->type == MENU_DIR);
	assert (file1 != NULL);
	assert (file2 != NULL);
	assert (m->menu.list.main != NULL);
	assert (m->menu.list.copy == NULL);

	menu_swap_items (m->menu.list.main, file1, file2);
}

static void side_menu_select_file (struct side_menu *m, const char *file)
{
	assert (m != NULL);
	assert (file != NULL);

	if (m->type == MENU_DIR || m->type == MENU_PLAYLIST)
		menu_setcurritem_file (m->menu.list.main, file);
	else
		abort ();
}

static void side_menu_resize (struct side_menu *m,
		const struct window_params *wp)
{
	assert (m != NULL);

	m->posx = wp->x;
	m->posy = wp->y;
	m->height = wp->height;
	m->width = wp->width;

	if (m->type == MENU_DIR || m->type == MENU_PLAYLIST
			|| m->type == MENU_THEMES) {
		menu_update_size (m->menu.list.main, m->posx + 1, m->posy + 1,
				m->width - 2, side_menu_get_menu_height(m));
		if (m->menu.list.copy)
			menu_update_size (m->menu.list.copy, m->posx + 1,
					m->posy + 1, m->width - 2,
					side_menu_get_menu_height(m));
	}
	else
		abort ();
}

static void main_win_draw_too_small_screen (const struct main_win *w)
{
	assert (w != NULL);
	assert (w->too_small);

	werase (w->win);
	wbkgd (w->win, get_color(CLR_BACKGROUND));

	wmove (w->win, 0, 0);
	wattrset (w->win, get_color(CLR_MESSAGE));
	xmvwaddstr (w->win, LINES/2,
			COLS/2 - (sizeof("...TERMINAL IS TOO SMALL...")-1)/2,
			"...TERMINAL TOO SMALL...");
}

static void main_win_draw_help_screen (const struct main_win *w)
{
	int i;
	int max_lines;
	int help_lines;
	char **help;

	assert (w != NULL);
	assert (w->in_help);

	max_lines = w->help_screen_top + LINES - 6;

	help = get_keys_help (&help_lines);

	werase (w->win);
	wbkgd (w->win, get_color(CLR_BACKGROUND));

	wmove (w->win, 0, 0);
	if (w->help_screen_top != 0) {
		wattrset (w->win, get_color(CLR_MESSAGE));
		xmvwaddstr (w->win, 0, COLS/2 - (sizeof("...MORE...")-1)/2,
				"...MORE...");
	}
	wmove (w->win, 1, 0);
	wattrset (w->win, get_color(CLR_LEGEND));
	for (i = w->help_screen_top; i < max_lines && i < help_lines; i++) {
		xwaddstr (w->win, help[i]);
		waddch (w->win, '\n');
	}
	if (i != help_lines) {
		wattrset (w->win, get_color(CLR_MESSAGE));
		xmvwaddstr (w->win, LINES-5,
				COLS/2 - (sizeof("...MORE...")-1)/2,
				"...MORE...");
	}
}

static void main_win_draw_lyrics_screen (const struct main_win *w)
{
	int i;
	int max_lines;
	int height, width;
	lists_t_strs *lyrics_array;

	assert (w != NULL);
	assert (w->in_lyrics);

	max_lines = w->lyrics_screen_top + LINES - 6;

	werase (w->win);
	wbkgd (w->win, get_color(CLR_BACKGROUND));

	wmove (w->win, 0, 0);
	if (w->lyrics_screen_top != 0) {
		wattrset (w->win, get_color(CLR_MESSAGE));
		xmvwaddstr (w->win, 0, COLS/2 - (sizeof("...MORE...")-1)/2,
				"...MORE...");
	}
	wmove (w->win, 1, 0);
	wattrset (w->win, get_color(CLR_LEGEND));
	getmaxyx (w->win, height, width);
	lyrics_array = lyrics_format (height, width);
	for (i = w->lyrics_screen_top; i < max_lines && i < lists_strs_size (lyrics_array); i++)
		xwaddstr (w->win, lists_strs_at (lyrics_array, i));
	if (i != lists_strs_size (lyrics_array)) {
		wattrset (w->win, get_color(CLR_MESSAGE));
		xmvwaddstr (w->win, LINES-5,
				COLS/2 - (sizeof("...MORE...")-1)/2,
				"...MORE...");
	}
	lists_strs_free (lyrics_array);
}

static void main_win_draw (struct main_win *w)
{
	size_t ix;

	if (w->in_help)
		main_win_draw_help_screen (w);
	else if (w->in_lyrics)
		main_win_draw_lyrics_screen (w);
	else if (w->too_small)
		main_win_draw_too_small_screen (w);
	else {
		werase (w->win);

		/* Draw all visible menus.  Draw the selected menu last. */
		for (ix = 0; ix < ARRAY_SIZE(w->menus); ix += 1)
			if (w->menus[ix].visible && ix != (size_t)w->selected_menu)
				side_menu_draw (&w->menus[ix], 0);

		side_menu_draw (&w->menus[w->selected_menu], 1);
	}
}

static enum side_menu_type iface_to_side_menu (const enum iface_menu iface_menu)
{
	switch (iface_menu) {
		case IFACE_MENU_PLIST:
			return MENU_PLAYLIST;
		case IFACE_MENU_DIR:
			return MENU_DIR;
		default:
			abort (); /* BUG */
	}
}

static void main_win_set_dir_content (struct main_win *w,
		const enum iface_menu iface_menu, const struct plist *files,
		const lists_t_strs *dirs, const lists_t_strs *playlists)
{
	struct side_menu *m;

	assert (w != NULL);

	m = find_side_menu (w, iface_to_side_menu(iface_menu));

	side_menu_make_list_content (m, files, dirs, playlists,
			iface_menu == IFACE_MENU_DIR);
	if (w->curr_file)
		side_menu_mark_file (m, w->curr_file);
	main_win_draw (w);
}

static void main_win_set_title (struct main_win *w,
		const enum side_menu_type type,
		const char *title)
{
	struct side_menu *m;

	assert (w != NULL);
	assert (title != NULL);

	m = find_side_menu (w, type);
	side_menu_set_title (m, title);
	main_win_draw (w);
}

static void main_win_update_dir_content (struct main_win *w,
		const enum iface_menu iface_menu, const struct plist *files,
		const lists_t_strs *dirs, const lists_t_strs *playlists)
{
	struct side_menu *m;
	struct side_menu_state ms;

	assert (w != NULL);

	m = find_side_menu (w, iface_menu == IFACE_MENU_DIR ? MENU_DIR
			: MENU_PLAYLIST);

	side_menu_get_state (m, &ms);
	side_menu_make_list_content (m, files, dirs, playlists, 1);
	side_menu_set_state (m, &ms);
	if (w->curr_file)
		side_menu_mark_file (m, w->curr_file);
	main_win_draw (w);
}

static void main_win_switch_to (struct main_win *w,
		const enum side_menu_type menu)
{
	size_t ix;

	assert (w != NULL);

	if (w->selected_menu == 2) /* if the themes menu is selected */
		side_menu_destroy (&w->menus[2]);

	for (ix = 0; ix < ARRAY_SIZE(w->menus); ix += 1)
		if (w->menus[ix].type == menu) {
			w->selected_menu = ix;
			break;
		}

	assert (ix < ARRAY_SIZE(w->menus));

	main_win_draw (w);
}

static void main_win_switch_to_help (struct main_win *w)
{
	assert (w != NULL);

	w->in_help = 1;
	main_win_draw (w);
}

static void main_win_switch_to_lyrics (struct main_win *w)
{
	assert (w != NULL);

	w->in_lyrics = 1;
	main_win_draw (w);
}

static void main_win_create_themes_menu (struct main_win *w)
{
	struct window_params p;

	assert (w != NULL);

	p.x = 0;
	p.y = 0;
	p.width = COLS;
	p.height = LINES - 4;

	side_menu_init (&w->menus[2], MENU_THEMES, w->win, &p);
	side_menu_set_title (&w->menus[2], "Themes");
}

static void main_win_menu_cmd (struct main_win *w, const enum key_cmd cmd)
{
	assert (w != NULL);

	side_menu_cmd (&w->menus[w->selected_menu], cmd);
	main_win_draw (w);
}

static enum file_type main_win_curritem_get_type (const struct main_win *w)
{
	assert (w != NULL);

	return side_menu_curritem_get_type (&w->menus[w->selected_menu]);
}

static char *main_win_get_curr_file (const struct main_win *w)
{
	assert (w != NULL);

	return side_menu_get_curr_file (&w->menus[w->selected_menu]);
}

static int main_win_in_dir_menu (const struct main_win *w)
{
	assert (w != NULL);

	return w->menus[w->selected_menu].type == MENU_DIR;
}

static int main_win_in_help (const struct main_win *w)
{
	assert (w != NULL);

	return w->in_help;
}

static int main_win_in_lyrics (const struct main_win *w)
{
	assert (w != NULL);

	return w->in_lyrics;
}

static int main_win_in_plist_menu (const struct main_win *w)
{
	assert (w != NULL);

	return w->menus[w->selected_menu].type == MENU_PLAYLIST;
}

static int main_win_in_theme_menu (const struct main_win *w)
{
	assert (w != NULL);

	return w->menus[w->selected_menu].type == MENU_THEMES;
}

static void main_win_set_curr_item_title (struct main_win *w, const char *title)
{
	assert (w != NULL);
	assert (title != NULL);

	side_menu_set_curr_item_title (&w->menus[w->selected_menu], title);
	main_win_draw (w);
}

/* Update item title and time on all menus where it's present. */
static void main_win_update_item (struct main_win *w,
		const enum iface_menu iface_menu, const struct plist *plist,
		const int n)
{
	struct side_menu *m;

	assert (w != NULL);
	assert (plist != NULL);
	assert (LIMIT(n, plist->num));

	m = find_side_menu (w, iface_to_side_menu(iface_menu));

	if (side_menu_update_item(m, plist, n))
		main_win_draw (w);
}

/* Mark the played file on all lists of files or unmark it when file is NULL. */
static void main_win_set_played_file (struct main_win *w, const char *file)
{
	size_t ix;

	assert (w != NULL);

	if (w->curr_file)
		free (w->curr_file);
	w->curr_file = xstrdup (file);

	for (ix = 0; ix < ARRAY_SIZE(w->menus); ix += 1) {
		struct side_menu *m = &w->menus[ix];

		if (m->visible && (m->type == MENU_DIR
					|| m->type == MENU_PLAYLIST)) {
			side_menu_unmark_file (m);
			if (file)
				side_menu_mark_file (m, file);
		}
	}

	main_win_draw (w);
}

static int main_win_menu_filter (struct main_win *w, const char *pattern)
{
	int num;

	assert (w != NULL);
	assert (pattern != NULL);

	num = side_menu_filter (&w->menus[w->selected_menu], pattern);

	if (num)
		main_win_draw (w);

	return num;
}

static void main_win_clear_filter_menu (struct main_win *w)
{
	assert (w != NULL);

	side_menu_use_main (&w->menus[w->selected_menu]);
	main_win_draw (w);
}

static void main_win_set_plist_time (struct main_win *w, const int time,
		const int time_for_all)
{
	struct side_menu *m;

	assert (w != NULL);

	m = find_side_menu (w, MENU_PLAYLIST);
	side_menu_set_plist_time (m, time, time_for_all);
}

static void main_win_add_to_plist (struct main_win *w, const struct plist *plist,
		const int num)
{
	struct side_menu *m;
	int need_redraw;

	assert (plist != NULL);

	m = find_side_menu (w, MENU_PLAYLIST);
	need_redraw = side_menu_add_plist_item (m, plist, num);
	if (w->curr_file)
		side_menu_mark_file (m, w->curr_file);
	if (need_redraw)
		main_win_draw (w);
}

static void main_win_add_file (struct main_win *w, const char *file,
		const char *title, const enum file_type type)
{
	assert (w != NULL);
	assert (file != NULL);
	assert (title != NULL);

	side_menu_add_file (&w->menus[w->selected_menu], file, title, type);
	main_win_draw (w);
}

static int main_win_get_files_time (const struct main_win *w,
		const enum iface_menu menu)
{
	struct side_menu *m;

	assert (w != NULL);

	m = find_side_menu ((struct main_win *)w, iface_to_side_menu(menu));

	return side_menu_get_files_time (m);
}

static int main_win_is_time_for_all (const struct main_win *w,
		const enum iface_menu menu)
{
	struct side_menu *m;

	assert (w != NULL);

	m = find_side_menu ((struct main_win *)w, iface_to_side_menu(menu));

	return side_menu_is_time_for_all (m);
}

static int main_win_get_curr_files_time (const struct main_win *w)
{
	assert (w != NULL);

	return side_menu_get_files_time (&w->menus[w->selected_menu]);
}

static int main_win_is_curr_time_for_all (const struct main_win *w)
{
	assert (w != NULL);

	return side_menu_is_time_for_all (&w->menus[w->selected_menu]);
}

static void main_win_handle_help_key (struct main_win *w,
		const struct iface_key *k)
{
	int help_lines;

	assert (w != NULL);
	assert (w->in_help);

	get_keys_help (&help_lines);

	if ((k->type == IFACE_KEY_FUNCTION && (
					k->key.func == KEY_DOWN
					|| k->key.func == KEY_NPAGE))
			|| (k->key.ucs == '\n')) {
		if (w->help_screen_top + LINES - 5 <= help_lines)
			w->help_screen_top++;
	}
	else {
		if (k->type == IFACE_KEY_FUNCTION && (k->key.func == KEY_UP
					|| k->key.func == KEY_PPAGE)) {
			if (w->help_screen_top > 0)
				w->help_screen_top--;
		}
		else if (k->key.func != KEY_RESIZE)
			w->in_help = 0;
	}

	main_win_draw (w);
}

static void main_win_handle_lyrics_key (struct main_win *w,
		const struct iface_key *k)
{
	int height, width;
	lists_t_strs *lyrics_array;

	assert (w != NULL);
	assert (w->in_lyrics);

	if ((k->type == IFACE_KEY_FUNCTION && (
					k->key.func == KEY_DOWN
					|| k->key.func == KEY_NPAGE))
			|| (k->key.ucs == '\n')) {
		getmaxyx (w->win, height, width);
		lyrics_array = lyrics_format (height, width);
		if (w->lyrics_screen_top + LINES - 5 <= lists_strs_size (lyrics_array))
			w->lyrics_screen_top++;
		lists_strs_free (lyrics_array);
	}
	else {
		if (k->type == IFACE_KEY_FUNCTION && (k->key.func == KEY_UP
					|| k->key.func == KEY_PPAGE)) {
			if (w->lyrics_screen_top > 0)
				w->lyrics_screen_top--;
		}
		else if (k->key.func != KEY_RESIZE)
			w->in_lyrics = 0;
	}

	main_win_draw (w);
}

static void main_win_swap_plist_items (struct main_win *w, const char *file1,
		const char *file2)
{
	struct side_menu *m;

	assert (w != NULL);
	assert (file1 != NULL);
	assert (file2 != NULL);

	m = find_side_menu (w, MENU_PLAYLIST);
	side_menu_swap_items (m, file1, file2);
	main_win_draw (w);
}

static void main_win_use_layout (struct main_win *w, lists_t_strs *layout_fmt)
{
	struct main_win_layout l;
	bool rc ASSERT_ONLY;

	assert (w != NULL);
	assert (layout_fmt != NULL);

	w->layout_fmt = layout_fmt;

	rc = parse_layout (&l, layout_fmt);
	assert (rc);

	side_menu_resize (&w->menus[0], &l.menus[0]);
	side_menu_resize (&w->menus[1], &l.menus[1]);

	main_win_draw (w);
}

static void validate_layouts ()
{
	struct main_win_layout l;
	lists_t_strs *layout_fmt;

	layout_fmt = options_get_list ("Layout1");
	if (lists_strs_empty (layout_fmt) || !parse_layout(&l, layout_fmt))
		interface_fatal ("Layout1 is malformed!");

	layout_fmt = options_get_list ("Layout2");
	if (!lists_strs_empty (layout_fmt) && !parse_layout(&l, layout_fmt))
		interface_fatal ("Layout2 is malformed!");

	layout_fmt = options_get_list ("Layout3");
	if (!lists_strs_empty (layout_fmt) && !parse_layout(&l, layout_fmt))
		interface_fatal ("Layout3 is malformed!");
}

/* Handle terminal size change. */
static void main_win_resize (struct main_win *w)
{
	struct main_win_layout l;
	bool rc ASSERT_ONLY;

	assert (w != NULL);

	keypad (w->win, TRUE);
	wresize (w->win, LINES - 4, COLS);
	werase (w->win);

	rc = parse_layout (&l, w->layout_fmt);
	assert (rc);

	side_menu_resize (&w->menus[0], &l.menus[0]);
	side_menu_resize (&w->menus[1], &l.menus[1]);

	if (w->menus[2].visible) { /* Themes menu */
		struct window_params p;

		p.x = 0;
		p.y = 0;
		p.width = COLS;
		p.height = LINES - 4;

		side_menu_resize (&w->menus[2], &p);
	}

	main_win_draw (w);
}

static void main_win_make_visible (struct main_win *w,
		const enum side_menu_type type, const char *file)
{
	struct side_menu *m;

	assert (w != NULL);
	assert (file != NULL);

	m = find_side_menu (w, type);
	side_menu_make_visible (m, file);
	main_win_draw (w);
}

static void main_win_update_show_time (struct main_win *w)
{
	size_t ix;

	assert (w != NULL);

	for (ix = 0; ix < ARRAY_SIZE(w->menus); ix += 1) {
		struct side_menu *m = &w->menus[ix];

		if (m->visible && (m->type == MENU_DIR
					|| m->type == MENU_PLAYLIST))
			side_menu_update_show_time (&w->menus[ix]);
	}

	main_win_draw (w);
}

static void main_win_select_file (struct main_win *w, const char *file)
{
	assert (w != NULL);
	assert (file != NULL);

	side_menu_select_file (&w->menus[w->selected_menu], file);
	main_win_draw (w);
}

static void main_win_update_show_format (struct main_win *w)
{
	size_t ix;

	assert (w != NULL);

	for (ix = 0; ix < ARRAY_SIZE(w->menus); ix += 1) {
		struct side_menu *m = &w->menus[ix];

		if (m->visible && (m->type == MENU_DIR
					|| m->type == MENU_PLAYLIST))
			side_menu_update_show_format (&w->menus[ix]);
	}

	main_win_draw (w);
}

static void main_win_del_plist_item (struct main_win *w, const char *file)
{
	struct side_menu *m;

	assert (w != NULL);
	assert (file != NULL);

	m = find_side_menu (w, MENU_PLAYLIST);
	side_menu_del_item (m, file);
	main_win_draw (w);
}

static void main_win_clear_plist (struct main_win *w)
{
	struct side_menu *m;

	assert (w != NULL);

	m = find_side_menu (w, MENU_PLAYLIST);
	side_menu_clear (m);
	main_win_draw (w);
}

/* Write to a file and log but otherwise ignore any error. */
static void soft_write (int fd, const void *buf, size_t count)
{
	ssize_t rc;

	rc = write (fd, buf, count);
	if (rc < 0)
		log_errno ("write() failed", errno);
}

/* Set the has_xterm variable. */
static void detect_term ()
{
	char *term;

	term = getenv ("TERM");
	if (term) {
		lists_t_strs *xterms;

		xterms = options_get_list ("XTerms");
		has_xterm = lists_strs_exists (xterms, term);
	}
}

static void xterm_set_title (const int state, const char *title)
{
	if (has_xterm && options_get_bool("SetXtermTitle")) {
		soft_write (1, "\033]0;", sizeof("\033]0;")-1);
		soft_write (1, "MOC ", sizeof("MOC ")-1);

		switch (state) {
			case STATE_PLAY:
				soft_write (1, "[play]", sizeof("[play]")-1);
				break;
			case STATE_STOP:
				soft_write (1, "[stop]", sizeof("[stop]")-1);
				break;
			case STATE_PAUSE:
				soft_write (1, "[pause]", sizeof("[pause]")-1);
				break;
		}

        if (title)
        {
            soft_write (1, " - ", sizeof(" - ")-1);
            if (options_get_bool ("NonUTFXterm"))
            {
                char *iconv_title = xterm_iconv_str (title);
                soft_write (1, iconv_title, strlen(iconv_title));
                free (iconv_title);
            }
            else
            {
                soft_write (1, title, strlen(title));
            }
        }

        soft_write (1, "\007", 1);
    }
}


static void xterm_clear_title ()
{
	if (has_xterm && options_get_bool("SetXtermTitle"))
		soft_write (1, "\033]2;\007", sizeof("\033]2;\007")-1);
}

/* Set the has_screen variable. */
static void detect_screen ()
{
	char *term, *window;

	term = getenv ("TERM");
	window = getenv ("WINDOW");
	if (term && window && isdigit (*window)) {
		lists_t_strs *screen_terms;

		screen_terms = options_get_list ("ScreenTerms");
		has_screen = lists_strs_exists (screen_terms, term);
	}
}

#define SCREEN_TITLE_START "\033k"
#define SCREEN_TITLE_END "\033\\"
static void screen_set_title (const int state, const char *title)
{
	if (has_screen && options_get_bool("SetScreenTitle")) {
		soft_write (1, SCREEN_TITLE_START, sizeof(SCREEN_TITLE_START)-1);
		soft_write (1, "MOC ", sizeof("MOC ")-1);

		switch (state) {
			case STATE_PLAY:
				soft_write (1, "[play]", sizeof("[play]")-1);
				break;
			case STATE_STOP:
				soft_write (1, "[stop]", sizeof("[stop]")-1);
				break;
			case STATE_PAUSE:
				soft_write (1, "[pause]", sizeof("[pause]")-1);
				break;
		}

		if (title) {
			soft_write (1, " - ", sizeof(" - ")-1);
			soft_write (1, title, strlen(title));
		}

		soft_write (1, SCREEN_TITLE_END, sizeof(SCREEN_TITLE_END)-1);
	}
}

static void screen_clear_title ()
{
	if (has_screen && options_get_bool("SetScreenTitle"))
	{
		soft_write (1, SCREEN_TITLE_START, sizeof(SCREEN_TITLE_START)-1);
		soft_write (1, SCREEN_TITLE_END, sizeof(SCREEN_TITLE_END)-1);
	}
}

/* Based on ASCIILines option initialize line characters with curses lines or
 * ASCII characters. */
static void init_lines ()
{
	if (options_get_bool("ASCIILines")) {
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

/* End the program if the terminal is too small. */
static void check_term_size (struct main_win *mw, struct info_win *iw)
{
	mw->too_small = iw->too_small = COLS < 59 || LINES < 7;
}

/* Update the title with the current fill. */
static void bar_update_title (struct bar *b)
{
	char pct[8];

	assert (b != NULL);
	assert (b->show_val);

	if (!b->show_pct)
		sprintf (b->title, "%*s", b->width, b->orig_title);
	else {
		sprintf (b->title, "%*s", b->width - 7, b->orig_title);
		strcpy (pct, " 100%  ");
		if (b->filled < 99.99)
			snprintf (pct, sizeof (pct), "  %02.0f%%  ", b->filled);
		strncpy (&b->title[b->width - 7], pct, strlen (pct));
	}
}

static void bar_set_title (struct bar *b, const char *title)
{
	assert (b != NULL);
	assert (b->show_val);
	assert (title != NULL);
	assert (strlen(title) < sizeof(b->title) - 5);

	strncpy (b->orig_title, title, b->width);
	b->orig_title[b->width] = 0;
	bar_update_title (b);
}

static void bar_init (struct bar *b, const int width, const char *title,
		const int show_val, const int show_pct,
		const int fill_color, const int empty_color)
{
	assert (b != NULL);
	assert (width > 5 && width < ssizeof(b->title));
	assert (title != NULL || !show_val);

	b->width = width;
	b->filled = 0.0;
	b->show_val = show_val;
	b->show_pct = show_pct;
	b->fill_color = fill_color;
	b->empty_color = empty_color;

	if (show_val) {
		b->orig_title = xmalloc (b->width + 1);
		bar_set_title (b, title);
	} else {
		b->orig_title = NULL;
		memset (b->title, ' ', b->width);
		b->title[b->width] = 0;
	}
}

static void bar_draw (const struct bar *b, WINDOW *win, const int pos_x,
		const int pos_y)
{
	int fill_chars; /* how many chars are "filled" */

	assert (b != NULL);
	assert (win != NULL);
	assert (LIMIT(pos_x, COLS - b->width));
	assert (LIMIT(pos_y, LINES));

	fill_chars = b->filled * b->width / 100.0;

	wattrset (win, b->fill_color);
	xmvwaddnstr (win, pos_y, pos_x, b->title, fill_chars);

	wattrset (win, b->empty_color);
	xwaddstr (win, b->title + fill_chars);
}

static void bar_set_fill (struct bar *b, const double fill)
{
	assert (b != NULL);
	assert (fill >= 0.0);

	b->filled = MIN(fill, 100.0);

	if (b->show_val)
		bar_update_title (b);
}

static void bar_resize (struct bar *b, const int width)
{
	assert (b != NULL);
	assert (width > 5 && width < ssizeof(b->title));

	if (b->show_val && b->width < width) {
		char *new_title = xmalloc (width + 1);
		strcpy (new_title, b->orig_title);
		free (b->orig_title);
		b->orig_title = new_title;
	}

	b->width = width;

	if (b->show_val)
		bar_update_title (b);
	else {
		memset (b->title, ' ', b->width);
		b->title[b->width] = 0;
	}
}

static struct queued_message *queued_message_create (enum message_type type)
{
	struct queued_message *result;

	result = (struct queued_message *) xmalloc (sizeof (struct queued_message));
	result->next = NULL;
	result->type = type;
	result->msg = NULL;
	result->prompt = NULL;
	result->timeout = 0;
	result->callback = NULL;
	result->data = NULL;

	return result;
}

static void queued_message_destroy (struct queued_message *msg)
{
	assert (msg != NULL);

	if (msg->msg)
		free (msg->msg);
	if (msg->prompt)
		free (msg->prompt);

	free (msg);
}

static void set_startup_message (struct info_win *w)
{
	assert (w != NULL);

	w->current_message = queued_message_create (NORMAL_MSG);
	w->current_message->msg = xstrdup (STARTUP_MESSAGE);
	w->current_message->timeout = time (NULL);
	w->current_message->timeout += options_get_int ("MessageLingerTime");

	if (is_help_still_h ()) {
		struct queued_message *msg;

		msg = queued_message_create (NORMAL_MSG);
		msg->msg = xstrdup ("Press 'h' for the list of commands.");
		msg->timeout = options_get_int ("MessageLingerTime");

		w->queued_message_head = msg;
		w->queued_message_tail = msg;
		w->queued_message_total = 1;
	}
}

static void info_win_init (struct info_win *w)
{
	assert (w != NULL);

	w->win = newwin (4, COLS, LINES - 4, 0);
	wbkgd (w->win, get_color(CLR_BACKGROUND));

	w->queued_message_head = NULL;
	w->queued_message_tail = NULL;
	w->queued_message_total = 0;
	w->queued_message_errors = 0;

	w->too_small = 0;

	w->state_stereo = false;
	w->state_shuffle = false;
	w->state_repeat = false;
	w->state_next = false;
	w->state_play = STATE_STOP;
	w->state_net = false;

	w->bitrate = -1;
	w->rate = -1;

	w->files_in_queue = 0;

	w->curr_time = -1;
	w->total_time = -1;
	w->block_start = -1;
	w->block_end = -1;

	w->title = NULL;
	w->status_msg[0] = 0;

	w->in_entry = 0;
	entry_history_init (&w->urls_history);
	entry_history_init (&w->dirs_history);
	entry_history_init (&w->user_history);

	set_startup_message (w);

	bar_init (&w->mixer_bar, 20, "", 1, 1,
	          get_color(CLR_MIXER_BAR_FILL),
	          get_color(CLR_MIXER_BAR_EMPTY));
	bar_init (&w->time_bar, COLS - 4, "", 1,
	          options_get_bool("ShowTimePercent") ? 1 : 0,
	          get_color(CLR_TIME_BAR_FILL),
	          get_color(CLR_TIME_BAR_EMPTY));
}

static void info_win_destroy (struct info_win *w)
{
	assert (w != NULL);

	if (w->win)
		delwin (w->win);
	if (w->in_entry)
		entry_destroy (&w->entry);

	entry_history_clear (&w->urls_history);
	entry_history_clear (&w->dirs_history);
	entry_history_clear (&w->user_history);
}

/* Set the cursor position in the right place if needed. */
static void info_win_update_curs (const struct info_win *w)
{
	assert (w != NULL);

	if (w->in_entry && !w->too_small)
		entry_draw (&w->entry, w->win, 1, 0);
}

static void info_win_set_mixer_name (struct info_win *w, const char *name)
{
	assert (w != NULL);
	assert (name != NULL);

	bar_set_title (&w->mixer_bar, name);
	if (!w->in_entry && !w->too_small) {
		bar_draw (&w->mixer_bar, w->win, COLS - 37, 0);
		info_win_update_curs (w);
	}
}

static void info_win_draw_status (const struct info_win *w)
{
	assert (w != NULL);

	if (!w->in_entry && !w->too_small) {
		wattrset (w->win, get_color(CLR_STATUS));
		wmove (w->win, 0, 6);
		xwprintw (w->win, "%-*s", (int) sizeof(w->status_msg) - 1,
				w->status_msg);
		info_win_update_curs (w);
	}
}

static void info_win_set_status (struct info_win *w, const char *msg)
{
	assert (w != NULL);
	assert (msg != NULL);
	assert (strlen(msg) < sizeof(w->status_msg));

	strcpy (w->status_msg, msg);
	info_win_draw_status (w);
}

static void info_win_draw_files_in_queue (const struct info_win *w)
{
	const int hstart = 5 + sizeof(w->status_msg) + 2;

	assert (w != NULL);

	if(!w->in_entry && !w->too_small) {
		if (w->files_in_queue) {
			wattrset (w->win, get_color(CLR_STATUS));
			mvwaddch (w->win, 0, hstart, lines.rtee);
			xwprintw (w->win, "Q:%3d", w->files_in_queue);
			waddch (w->win, lines.ltee);
		}
		else {
			wattrset (w->win, get_color(CLR_FRAME));
			mvwhline (w->win, 0, hstart, lines.horiz, 9);
		}
	}

	info_win_update_curs (w);
}

static void info_win_set_files_in_queue (struct info_win *w, const int num)
{
	assert (w != NULL);
	assert (num >= 0);

	w->files_in_queue = num;
	info_win_draw_files_in_queue (w);
}

static void info_win_draw_state (const struct info_win *w)
{
	const char *state_symbol;

	assert (w != NULL);

	switch (w->state_play) {
		case STATE_PLAY:
			state_symbol = " >";
			break;
		case STATE_STOP:
			state_symbol = "[]";
			break;
		case STATE_PAUSE:
			state_symbol = "||";
			break;
		default:
			abort (); /* BUG */
	}

	if (!w->too_small) {
		wattrset (w->win, get_color(CLR_STATE));
		xmvwaddstr (w->win, 1, 1, state_symbol);
	}

	info_win_update_curs (w);
}

/* Draw the title or the message (informative or error). */
static void info_win_draw_title (const struct info_win *w)
{
	assert (w != NULL);

	if (!w->too_small) {
		clear_area (w->win, 4, 1, COLS - 5, 1);

		if (w->current_message &&
		    w->current_message->msg &&
		    w->current_message->timeout >= time (NULL))
		{
			wattrset (w->win, w->current_message->type == ERROR_MSG
			                  ? get_color (CLR_ERROR)
			                  : get_color (CLR_MESSAGE));
			xmvwaddnstr (w->win, 1, 4, w->current_message->msg, COLS - 5);
		}
		else
		{
			wattrset (w->win, get_color (CLR_TITLE));
			xmvwaddnstr (w->win, 1, 4, w->title ? w->title : "", COLS - 5);
		}
	}

	info_win_update_curs (w);
}

static void info_win_set_state (struct info_win *w, const int state)
{
	assert (w != NULL);
	assert (state == STATE_PLAY || state == STATE_STOP
			|| state == STATE_PAUSE);

	w->state_play = state;
	xterm_set_title (state, w->title);
	screen_set_title (state, w->title);
	info_win_draw_state (w);
}

static void info_win_draw_time (const struct info_win *w)
{
	char time_str[6];

	assert (w != NULL);

	if (!w->too_small) {
		/* current time */
		sec_to_min (time_str, w->curr_time != -1 ? w->curr_time : 0);
		wattrset (w->win, get_color(CLR_TIME_CURRENT));
		xmvwaddstr (w->win, 2, 1, time_str);

		/* time left */
		if (w->total_time > 0 && w->curr_time >= 0
				&& w->total_time >= w->curr_time) {
			sec_to_min (time_str, w->total_time - w->curr_time);
			wmove (w->win, 2, 7);
			wattrset (w->win, get_color(CLR_TIME_LEFT));
			xwaddstr (w->win, time_str);
		}
		else
			xmvwaddstr (w->win, 2, 7, "     ");

		/* total time */
		sec_to_min (time_str, w->total_time != -1 ? w->total_time : 0);
		wmove (w->win, 2, 14);
		wattrset (w->win, get_color(CLR_TIME_TOTAL));
		xwaddstr (w->win, time_str);

		bar_draw (&w->time_bar, w->win, 2, 3);
	}
	info_win_update_curs (w);
}

static void info_win_draw_block (const struct info_win *w)
{
	assert (w != NULL);

	if (!w->too_small)
		bar_draw (&w->time_bar, w->win, 2, 3);
	info_win_update_curs (w);
}

static void info_win_set_curr_time (struct info_win *w, const int time)
{
	assert (w != NULL);
	assert (time >= -1);

	w->curr_time = time;
	if (w->total_time > 0 && w->curr_time >= 0)
		bar_set_fill (&w->time_bar, w->curr_time * 100.0 / w->total_time);
	else
		bar_set_fill (&w->time_bar, 0.0);

	info_win_draw_time (w);
}

static void info_win_set_total_time (struct info_win *w, const int time)
{
	assert (w != NULL);
	assert (time >= -1);

	w->total_time = time;

	if (w->total_time > 0 && w->curr_time >= 0)
		bar_set_fill (&w->time_bar, w->curr_time * 100.0 / w->total_time);
	else
		bar_set_fill (&w->time_bar, 0.0);

	info_win_draw_time (w);
}

static void info_win_set_block_title (struct info_win *w)
{
	assert (w != NULL);

	if (w->total_time == -1) {
		bar_set_title (&w->time_bar, "");
	} else if (w->block_start == -1 || w->block_end == -1) {
		bar_set_title (&w->time_bar, "");
	} else if (w->block_start == 0 && w->block_end == w->total_time) {
		bar_set_title (&w->time_bar, "");
	} else {
		int start_pos, end_pos;
		char *new_title, *decorators;

		start_pos = w->block_start * w->time_bar.width / w->total_time;
		if (w->block_end < w->total_time)
			end_pos = w->block_end * w->time_bar.width / w->total_time;
		else
			end_pos = w->time_bar.width - 1;

		new_title = xmalloc(w->time_bar.width + 1);
		memset(new_title, ' ', w->time_bar.width);
		decorators = options_get_str ("BlockDecorators");
		if (start_pos == end_pos) {
			new_title[start_pos] = decorators[1];
		} else {
			new_title[start_pos] = decorators[0];
			new_title[end_pos] = decorators[2];
		}
		new_title[w->time_bar.width] = 0x00;

		bar_set_title (&w->time_bar, new_title);
	}
}

static void info_win_set_block (struct info_win *w, const int block_start, const int block_end)
{
	assert (w != NULL);
	assert (block_start == -1 || RANGE(0, block_start, w->total_time));
	assert (block_end == -1 || RANGE(0, block_end, w->total_time));

	info_win.block_start = block_start;
	info_win.block_end = block_end;

	info_win_set_block_title (w);
	info_win_draw_block (w);
}

static void info_win_set_played_title (struct info_win *w, const char *title)
{
	assert (w != NULL);

	if (!w->title && !title)
		return;

	if (w->title && title && !strcmp(w->title, title))
		return;

	if (w->title)
		free (w->title);
	w->title = xstrdup (title);
	xterm_set_title (w->state_play, title);
	screen_set_title (w->state_play, title);
	info_win_draw_title (w);
}

static void info_win_draw_rate (const struct info_win *w)
{
	assert (w != NULL);

	wattrset (w->win, get_color(CLR_SOUND_PARAMS));
	wmove (w->win, 2, 22);
	if (w->rate != -1)
		xwprintw (w->win, "%3d", w->rate);
	else
		xwaddstr (w->win, "   ");
}

static void info_win_draw_bitrate (const struct info_win *w)
{
	assert (w != NULL);

	if (!w->too_small) {
		wattrset (w->win, get_color(CLR_SOUND_PARAMS));
		wmove (w->win, 2, 29);
		if (w->bitrate != -1)
			xwprintw (w->win, "%4d", MIN(w->bitrate, 9999));
		else
			xwaddstr (w->win, "    ");
	}
	info_win_update_curs (w);
}

static void info_win_set_bitrate (struct info_win *w, const int bitrate)
{
	assert (w != NULL);
	assert (bitrate >= -1);

	w->bitrate = bitrate > 0 ? bitrate : -1;
	info_win_draw_bitrate (w);
}

static void info_win_set_rate (struct info_win *w, const int rate)
{
	assert (w != NULL);
	assert (rate >= -1);

	w->rate = rate > 0 ? rate : -1;
	info_win_draw_rate (w);
}

static void info_win_set_mixer_value (struct info_win *w, const int value)
{
	assert (w != NULL);

	bar_set_fill (&w->mixer_bar, (double) value);
	if (!w->in_entry && !w->too_small)
		bar_draw (&w->mixer_bar, w->win, COLS - 37, 0);
}

/* Draw a switch that is turned on or off in form of [TITLE]. */
static void info_win_draw_switch (const struct info_win *w, const int posx,
		const int posy, const char *title, const bool value)
{
	assert (w != NULL);
	assert (title != NULL);

	if (!w->too_small) {
		wattrset (w->win, get_color(
					value ? CLR_INFO_ENABLED : CLR_INFO_DISABLED));
		wmove (w->win, posy, posx);
		xwprintw (w->win, "[%s]", title);
	}
	info_win_update_curs (w);
}

static void info_win_draw_options_state (const struct info_win *w)
{
	assert (w != NULL);

	info_win_draw_switch (w, 38, 2, "STEREO", w->state_stereo);
	info_win_draw_switch (w, 47, 2, "NET", w->state_net);
	info_win_draw_switch (w, 53, 2, "SHUFFLE", w->state_shuffle);
	info_win_draw_switch (w, 63, 2, "REPEAT", w->state_repeat);
	info_win_draw_switch (w, 72, 2, "NEXT", w->state_next);
}

static void info_win_make_entry (struct info_win *w, const enum entry_type type)
{
	struct entry_history *history;
	const char *prompt;

	assert (w != NULL);
	assert (!w->in_entry);

	prompt = NULL;
	switch (type) {
		case ENTRY_GO_DIR:
			history = &w->dirs_history;
			break;
		case ENTRY_GO_URL:
			history = &w->urls_history;
			break;
		case ENTRY_ADD_URL:
			history = &w->urls_history;
			break;
		case ENTRY_USER_QUERY:
			history = &w->user_history;
			prompt = w->current_message->prompt;
			break;
		default:
			history = NULL;
	}

	entry_init (&w->entry, type, COLS - 4, history, prompt);
	w->in_entry = 1;
	curs_set (1);
	entry_draw (&w->entry, w->win, 1, 0);
}

/* Display the next queued message. */
static void info_win_display_msg (struct info_win *w)
{
	int msg_changed;

	assert (w != NULL);

	msg_changed = 0;
	if (w->current_message && time (NULL) > w->current_message->timeout) {
		w->callback = w->current_message->callback;
		w->data = w->current_message->data;
		queued_message_destroy (w->current_message);
		w->current_message = NULL;
		msg_changed = 1;
	}

	if (!w->current_message && w->queued_message_head && !w->in_entry) {
		w->current_message = w->queued_message_head;
		w->queued_message_head = w->current_message->next;
		w->current_message->next = NULL;
		if (!w->queued_message_head)
			w->queued_message_tail = NULL;
		w->queued_message_total -= 1;
		if (w->current_message->type == ERROR_MSG)
			w->queued_message_errors -= 1;

		if (msg_changed &&
		    w->current_message->msg
		    && options_get_bool ("PrefixQueuedMessages"))
		{
			char *msg, *decorator;
			int len;

			msg = w->current_message->msg;
			decorator = options_get_str ("ErrorMessagesQueued");
			len = strlen (msg) + strlen (decorator) + 10;
			w->current_message->msg = (char *) xmalloc (len);
			snprintf (w->current_message->msg, len, "(%d%s) %s",
			          w->queued_message_total,
			          (w->queued_message_errors ? decorator : ""),
			          msg);
			w->current_message->msg[len - 1] = 0x00;
			free (msg);
		}

		if (w->current_message->type == QUERY_MSG) {
			info_win_make_entry (w, ENTRY_USER_QUERY);
			w->current_message->timeout = 86400;
		}

		w->current_message->timeout += time (NULL);
		msg_changed = 1;
	}

	if (msg_changed)
		info_win_draw_title (w);
}

/* Force the next queued message to be displayed. */
static void info_win_disable_msg (struct info_win *w)
{
	assert (w != NULL);

	if (w->current_message) {
		w->current_message->timeout = 0;
		info_win_display_msg (w);
	}
}

/* Clear all queued messages. */
static void info_win_clear_msg (struct info_win *w)
{
	assert (w != NULL);

	while (w->queued_message_head) {
		struct queued_message *this_msg;

		this_msg = w->queued_message_head;
		w->queued_message_head = this_msg->next;
		queued_message_destroy (this_msg);
	}

	w->queued_message_total = 0;
	w->queued_message_errors = 0;
	w->queued_message_tail = NULL;

	if (w->current_message) {
		queued_message_destroy (w->current_message);
		w->current_message = NULL;
	}
}

/* Queue a new message for display. */
static void info_win_msg (struct info_win *w, const char *msg,
                          enum message_type msg_type, const char *prompt,
                          t_user_reply_callback *callback, void *data)
{
	struct queued_message *this_msg;

	assert (w != NULL);
	assert (msg != NULL || prompt != NULL);

	this_msg = queued_message_create (msg_type);
	if (msg)
		this_msg->msg = xstrdup (msg);
	if (prompt)
		this_msg->prompt = xstrdup (prompt);
	this_msg->timeout = options_get_int ("MessageLingerTime");
	this_msg->callback = callback;
	this_msg->data = data;

	if (w->queued_message_head) {
		w->queued_message_tail->next = this_msg;
		w->queued_message_tail = this_msg;
	} else {
		w->queued_message_head = this_msg;
		w->queued_message_tail = this_msg;
	}
	w->queued_message_total += 1;
	if (msg_type == ERROR_MSG)
		w->queued_message_errors += 1;

	info_win_display_msg (w);
}

static void iface_win_user_reply (struct info_win *w, const char *reply)
{
	assert (w != NULL);

	if (w->callback)
		w->callback (reply, w->data);
}

static void info_win_user_history_add (struct info_win *w, const char *text)
{
	assert (w != NULL);

	entry_history_add (&w->user_history, text);
}

static void info_win_set_channels (struct info_win *w, const int channels)
{
	assert (w != NULL);
	assert (channels == 1 || channels == 2);

	w->state_stereo = (channels == 2);
	info_win_draw_options_state (w);
}

static int info_win_in_entry (const struct info_win *w)
{
	assert (w != NULL);

	return w->in_entry;
}

static enum entry_type info_win_get_entry_type (const struct info_win *w)
{
	assert (w != NULL);
	assert (w->in_entry);

	return entry_get_type (&w->entry);
}

static void info_win_set_option_state (struct info_win *w, const char *name,
		const bool value)
{
	assert (w != NULL);
	assert (name != NULL);

	if (!strcasecmp(name, "Shuffle"))
		w->state_shuffle = value;
	else if (!strcasecmp(name, "Repeat"))
		w->state_repeat = value;
	else if (!strcasecmp(name, "AutoNext"))
		w->state_next = value;
	else if (!strcasecmp(name, "Net"))
		w->state_net = value;
	else
		abort ();

	info_win_draw_options_state (w);
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

static void info_win_draw_files_time (const struct info_win *w)
{
	assert (w != NULL);

	if (!w->in_entry && !w->too_small) {
		char buf[10];

		sec_to_min_plist (buf, w->plist_time);
		wmove (w->win, 0, COLS - 12);
		wattrset (w->win, get_color(CLR_PLIST_TIME));
		waddch (w->win, w->plist_time_for_all ? ' ' : '>');
		xwaddstr (w->win, buf);
		info_win_update_curs (w);
	}
}

/* Set the total time for files in the displayed menu. If time_for_all
 * has a non zero value, the time is for all files. */
static void info_win_set_files_time (struct info_win *w, const int time,
		const int time_for_all)
{
	assert (w != NULL);

	w->plist_time = time;
	w->plist_time_for_all = time_for_all;

	info_win_draw_files_time (w);
}

/* Update the message timeout, redraw the window if needed. */
static void info_win_tick (struct info_win *w)
{
	info_win_display_msg (w);
}

/* Draw static elements of info_win: frames, legend etc. */
static void info_win_draw_static_elements (const struct info_win *w)
{
	assert (w != NULL);

	if (!w->too_small) {
		/* window frame */
		wattrset (w->win, get_color(CLR_FRAME));
		wborder (w->win, lines.vert, lines.vert, lines.horiz, lines.horiz,
				lines.ltee, lines.rtee, lines.llcorn, lines.lrcorn);

		/* mixer frame */
		mvwaddch (w->win, 0, COLS - 38, lines.rtee);
		mvwaddch (w->win, 0, COLS - 17, lines.ltee);

		/* playlist time frame */
		mvwaddch (w->win, 0, COLS - 13, lines.rtee);
		mvwaddch (w->win, 0, COLS - 2, lines.ltee);

		/* total time frames */
		wattrset (w->win, get_color(CLR_TIME_TOTAL_FRAMES));
		mvwaddch (w->win, 2, 13, '[');
		mvwaddch (w->win, 2, 19, ']');

		/* time bar frame */
		wattrset (w->win, get_color(CLR_FRAME));
		mvwaddch (w->win, 3, COLS - 2, lines.ltee);
		mvwaddch (w->win, 3, 1, lines.rtee);

		/* status line frame */
		mvwaddch (w->win, 0, 5, lines.rtee);
		mvwaddch (w->win, 0, 5 + sizeof(w->status_msg), lines.ltee);

		/* rate and bitrate units */
		wmove (w->win, 2, 25);
		wattrset (w->win, get_color(CLR_LEGEND));
		xwaddstr (w->win, "kHz	 kbps");
	}

	info_win_update_curs (w);
}

static void info_win_draw (const struct info_win *w)
{
	assert (w != NULL);

	if (!w->too_small) {
		info_win_draw_static_elements (w);
		info_win_draw_state (w);
		info_win_draw_time (w);
		info_win_draw_block (w);
		info_win_draw_title (w);
		info_win_draw_options_state (w);
		info_win_draw_status (w);
		info_win_draw_files_in_queue (w);
		info_win_draw_files_time (w);
		info_win_draw_bitrate (w);
		info_win_draw_rate (w);

		if (w->in_entry)
			entry_draw (&w->entry, w->win, 1, 0);
		else
			bar_draw (&w->mixer_bar, w->win, COLS - 37, 0);

		bar_draw (&w->time_bar, w->win, 2, 3);
	}
	info_win_update_curs (w);
}

static void info_win_entry_disable (struct info_win *w)
{
	assert (w != NULL);
	assert (w->in_entry);

	entry_destroy (&w->entry);
	w->in_entry = 0;

	if (!options_get_bool("UseCursorSelection"))
		curs_set (0);
	info_win_draw (w);
}

/* Handle a key while in entry. main_win is used to update the menu (filter
 * only matching items) when ENTRY_SEARCH is used. */
static void info_win_entry_handle_key (struct info_win *iw, struct main_win *mw,
		const struct iface_key *k)
{
	enum key_cmd cmd;
	enum entry_type type;

	assert (iw != NULL);
	assert (mw != NULL);
	assert (iw->in_entry);

	cmd = get_key_cmd (CON_ENTRY, k);
	type = entry_get_type (&iw->entry);

	if (type == ENTRY_SEARCH) {
		char *text;

		if (k->type == IFACE_KEY_CHAR) {
			if (iswprint(k->key.ucs)) {
				entry_add_char (&iw->entry, k->key.ucs);
				text = entry_get_text (&iw->entry);
				if (!main_win_menu_filter(mw, text))
					entry_back_space (&iw->entry);
				free (text);
			}

		}
		else if (k->key.func == KEY_BACKSPACE) {
			entry_back_space (&iw->entry);
			text = entry_get_text (&iw->entry);
			main_win_menu_filter (mw, text);
			free (text);
		}
		else if (cmd == KEY_CMD_CANCEL) {
			main_win_clear_filter_menu (mw);
			info_win_entry_disable (iw);
		}
		else {
			enum key_cmd cmd = get_key_cmd (CON_MENU, k);

			if (cmd == KEY_CMD_MENU_UP
					|| cmd == KEY_CMD_MENU_DOWN
					|| cmd == KEY_CMD_MENU_NPAGE
					|| cmd == KEY_CMD_MENU_PPAGE
					|| cmd == KEY_CMD_MENU_FIRST
					|| cmd == KEY_CMD_MENU_LAST)
				main_win_menu_cmd (mw, cmd);
		}
	}
	else {
		if (k->type == IFACE_KEY_CHAR) {
			if (iswprint(k->key.ucs))
				entry_add_char (&iw->entry, k->key.ucs);
		}
		else if (k->key.func == KEY_LEFT)
			entry_curs_left (&iw->entry);
		else if (k->key.func == KEY_RIGHT)
			entry_curs_right (&iw->entry);
		else if (k->key.func == KEY_BACKSPACE)
			entry_back_space (&iw->entry);
		else if (k->key.func == KEY_DC)
			entry_del_char (&iw->entry);
		else if (k->key.func == KEY_HOME)
			entry_home (&iw->entry);
		else if (k->key.func == KEY_END)
			entry_end (&iw->entry);
		else if (cmd == KEY_CMD_CANCEL) {
			info_win_entry_disable (iw);
			if (type == ENTRY_USER_QUERY)
				iface_user_reply (NULL);
		}
		else if ((type == ENTRY_GO_DIR || type == ENTRY_GO_URL
					|| type == ENTRY_ADD_URL || type == ENTRY_USER_QUERY)
					&& cmd != KEY_CMD_WRONG) {
			if (cmd == KEY_CMD_HISTORY_UP)
				entry_set_history_up (&iw->entry);
			else if (cmd == KEY_CMD_HISTORY_DOWN)
				entry_set_history_down (&iw->entry);
			else if (cmd == KEY_CMD_DELETE_START)
				entry_del_to_start (&iw->entry);
			else if (cmd == KEY_CMD_DELETE_END)
				entry_del_to_end (&iw->entry);
		}
	}

	if (iw->in_entry) /* the entry could be disabled above */
		entry_draw (&iw->entry, iw->win, 1, 0);
}

static void info_win_entry_set_text (struct info_win *w, const char *text)
{
	assert (w != NULL);
	assert (text != NULL);
	assert (w->in_entry);

	entry_set_text (&w->entry, text);
	entry_draw (&w->entry, w->win, 1, 0);
}

static char *info_win_entry_get_text (const struct info_win *w)
{
	assert (w != NULL);
	assert (w->in_entry);

	return entry_get_text (&w->entry);
}

static void info_win_entry_history_add (struct info_win *w)
{
	assert (w != NULL);
	assert (w->in_entry);

	entry_add_text_to_history (&w->entry);
}

static void info_win_entry_set_file (struct info_win *w, const char *file)
{
	assert (w != NULL);
	assert (w->in_entry);
	assert (file != NULL);

	entry_set_file (&w->entry, file);
}

static char *info_win_entry_get_file (const struct info_win *w)
{
	assert (w != NULL);
	assert (w->in_entry);

	return entry_get_file (&w->entry);
}

/* Handle terminal size change. */
static void info_win_resize (struct info_win *w)
{
	assert (w != NULL);
	keypad (w->win, TRUE);
	wresize (w->win, 4, COLS);
	mvwin (w->win, LINES - 4, 0);
	werase (w->win);

	bar_resize (&w->mixer_bar, 20);
	bar_resize (&w->time_bar, COLS - 4);
	info_win_set_block_title (w);

	if (w->in_entry)
		entry_resize (&w->entry, COLS - 4);

	info_win_draw (w);
}

void windows_init ()
{
	if (getenv ("ESCDELAY") == NULL) {
#ifdef HAVE_SET_ESCDELAY
		set_escdelay (25);
#else
		setenv ("ESCDELAY", "25", 0);
#endif
	}

	utf8_init ();
	if (!initscr ())
		fatal ("Can't initialize terminal!");
	screen_initialized = 1;
	validate_layouts ();
	cbreak ();
	noecho ();
	if (!options_get_bool ("UseCursorSelection"))
		curs_set (0);
	use_default_colors ();

	detect_term ();
	detect_screen ();
	start_color ();
	theme_init (has_xterm);
	init_lines ();

	main_win_init (&main_win, options_get_list ("Layout1"));
	info_win_init (&info_win);

	check_term_size (&main_win, &info_win);

	main_win_draw (&main_win);
	info_win_draw (&info_win);

	wnoutrefresh (main_win.win);
	wnoutrefresh (info_win.win);
	doupdate ();

	iface_initialized = 1;
}

void windows_reset ()
{
	if (screen_initialized) {
		screen_initialized = 0;

		/* endwin() sometimes fails on X-terminals when we get SIGCHLD
		 * at this moment.  Double invocation seems to solve this. */
		if (endwin () == ERR && endwin () == ERR)
			logit ("endwin() failed!");

		/* Make sure that the next line after we exit will be "clear". */
		printf ("\n");
		fflush (stdout);
	}
}

void windows_end ()
{
	if (iface_initialized) {
		iface_initialized = 0;

		main_win_destroy (&main_win);
		info_win_clear_msg (&info_win);
		info_win_destroy (&info_win);

		xterm_clear_title ();
		screen_clear_title ();
		utf8_cleanup ();
	}

	windows_reset ();

	lyrics_cleanup ();
}

static void iface_refresh_screen ()
{
	/* We must do it in proper order to get the right cursor position. */
	if (iface_in_entry ()) {
		wnoutrefresh (main_win.win);
		wnoutrefresh (info_win.win);
	}
	else {
		wnoutrefresh (info_win.win);
		wnoutrefresh (main_win.win);
	}
	doupdate ();
}

/* Set state of the options displayed in the information window. */
void iface_set_option_state (const char *name, const bool value)
{
	assert (name != NULL);

	info_win_set_option_state (&info_win, name, value);
	iface_refresh_screen ();
}

/* Set the mixer name. */
void iface_set_mixer_name (const char *name)
{
	assert (name != NULL);

	info_win_set_mixer_name (&info_win, name);
	iface_refresh_screen ();
}

/* Set the status message in the info window. */
void iface_set_status (const char *msg)
{
	assert (msg != NULL);

	if (iface_initialized) {
		info_win_set_status (&info_win, msg);
		iface_refresh_screen ();
	}
}

/* Set the number of files in song queue in the info window */
void iface_set_files_in_queue (const int num)
{
	assert (num >= 0);

	if (iface_initialized) {
		info_win_set_files_in_queue (&info_win, num);
		iface_refresh_screen ();
	}
}

static void iface_show_num_files (const int num)
{
	char str[20];

	snprintf (str, sizeof(str), "Files: %d", num);
	iface_set_status (str);
}

/* Change the content of the directory menu to these files, directories, and
 * playlists. */
void iface_set_dir_content (const enum iface_menu iface_menu,
		const struct plist *files, const lists_t_strs *dirs,
		const lists_t_strs *playlists)
{
	main_win_set_dir_content (&main_win, iface_menu, files, dirs,
			playlists);
	info_win_set_files_time (&info_win,
			main_win_get_files_time(&main_win, iface_menu),
			main_win_is_time_for_all(&main_win, iface_menu));

	iface_show_num_files (plist_count(files)
			+ (dirs ? lists_strs_size (dirs) : 0)
			+ (playlists ? lists_strs_size (playlists) : 0));

	iface_refresh_screen ();
}

/* Refreshes all menu structs with updated theme attributes. */
void iface_update_attrs ()
{
	size_t ix;

	info_win.mixer_bar.fill_color = get_color (CLR_MIXER_BAR_FILL);
	info_win.mixer_bar.empty_color = get_color (CLR_MIXER_BAR_EMPTY);
	info_win.time_bar.fill_color = get_color (CLR_TIME_BAR_FILL);
	info_win.time_bar.empty_color = get_color (CLR_TIME_BAR_EMPTY);

	for (ix = 0; ix < ARRAY_SIZE(main_win.menus); ix += 1) {
		int item_num;
		struct side_menu *m = &main_win.menus[ix];
		struct menu *menu = m->menu.list.main;
		struct menu_item *mi;

		if (m->type == MENU_DIR || m->type == MENU_PLAYLIST) {
			menu_set_info_attr_normal (menu, get_color (CLR_MENU_ITEM_INFO));
			menu_set_info_attr_sel (menu, get_color (CLR_MENU_ITEM_INFO_SELECTED));
			menu_set_info_attr_marked (menu, get_color (CLR_MENU_ITEM_INFO_MARKED));
			menu_set_info_attr_sel_marked (menu, get_color (CLR_MENU_ITEM_INFO_MARKED_SELECTED));

			for (mi = menu->items, item_num = 0;
			     mi && item_num < menu->nitems;
			     mi = mi->next, item_num += 1) {
				if (mi->type == F_DIR) {
					menu_item_set_attr_normal (mi, get_color (CLR_MENU_ITEM_DIR));
					menu_item_set_attr_sel (mi, get_color (CLR_MENU_ITEM_DIR_SELECTED));
				}
				else if (mi->type == F_PLAYLIST) {
					menu_item_set_attr_normal (mi, get_color (CLR_MENU_ITEM_PLAYLIST));
					menu_item_set_attr_sel (mi, get_color (CLR_MENU_ITEM_PLAYLIST_SELECTED));
				}
				else {
					menu_item_set_attr_normal (mi, get_color (CLR_MENU_ITEM_FILE));
					menu_item_set_attr_sel (mi, get_color (CLR_MENU_ITEM_FILE_SELECTED));
				}
			}
		}
		else {
			menu_set_info_attr_normal (menu, get_color (CLR_MENU_ITEM_FILE));
			menu_set_info_attr_sel (menu, get_color (CLR_MENU_ITEM_FILE_SELECTED));

			for (mi = menu->items, item_num = 0;
			     mi && item_num < menu->nitems;
			     mi = mi->next, item_num += 1) {
				menu_item_set_attr_normal (mi, get_color (CLR_MENU_ITEM_FILE));
				menu_item_set_attr_sel (mi, get_color (CLR_MENU_ITEM_FILE_SELECTED));
			}
		}
	}
}

void iface_update_theme_selection (const char *file)
{
    /* menus[2] is theme menu. */
    assert (main_win.menus[2].menu.list.main->selected);

    menu_setcurritem_file (main_win.menus[2].menu.list.main, file);
}

/* Like iface_set_dir_content(), but before replacing the menu content, save
 * the menu state (selected file, view position) and restore it after making
 * a new menu. */
void iface_update_dir_content (const enum iface_menu iface_menu,
		const struct plist *files, const lists_t_strs *dirs,
		const lists_t_strs *playlists)
{
	main_win_update_dir_content (&main_win, iface_menu, files, dirs,
			playlists);
	info_win_set_files_time (&info_win,
			main_win_get_files_time(&main_win, iface_menu),
			main_win_is_time_for_all(&main_win, iface_menu));

	iface_show_num_files (plist_count(files)
			+ (dirs ? lists_strs_size (dirs) : 0)
			+ (playlists ? lists_strs_size (playlists) : 0));

	iface_refresh_screen ();
}

/* Update item title and time in the menu. */
void iface_update_item (const enum iface_menu menu,
		const struct plist *plist, const int n)
{
	assert (plist != NULL);

	main_win_update_item (&main_win, menu, plist, n);
	info_win_set_files_time (&info_win,
			main_win_get_curr_files_time(&main_win),
			main_win_is_curr_time_for_all(&main_win));
	iface_refresh_screen ();
}

/* Change the current item in the directory menu to this item. */
void iface_set_curr_item_title (const char *title)
{
	assert (title != NULL);

	main_win_set_curr_item_title (&main_win, title);
	iface_refresh_screen ();
}

/* Set the title for the directory menu. */
void iface_set_title (const enum iface_menu menu, const char *title)
{

	assert (title != NULL);

    if (options_get_bool ("FileNamesIconv"))
    {
        char *conv_title = NULL;
        conv_title = files_iconv_str (title);

        main_win_set_title (&main_win,
                menu == IFACE_MENU_DIR ? MENU_DIR : MENU_PLAYLIST,
                conv_title);

        free (conv_title);
    }
    else
    {
        main_win_set_title (&main_win,
                menu == IFACE_MENU_DIR ? MENU_DIR : MENU_PLAYLIST,
                title);
    }
	iface_refresh_screen ();
}

/* Get the char code from the user with meta flag set if necessary. */
void iface_get_key (struct iface_key *k)
{
	wint_t ch;

	ch = wgetch (main_win.win);
	if (ch == (wint_t)ERR)
		interface_fatal ("wgetch() failed!");

	if (ch < 32 && ch != '\n' && ch != '\t' && ch != KEY_ESCAPE) {
		/* Unprintable, generally control sequences */
		k->type = IFACE_KEY_FUNCTION;
		k->key.func = ch;
	}
	else if (ch == 0x7f) {
		/* Workaround for backspace on many terminals */
		k->type = IFACE_KEY_FUNCTION;
		k->key.func = KEY_BACKSPACE;
	}
	else if (ch < 255) {
		/* Regular char */
		int meta;

#ifdef HAVE_NCURSESW
		ungetch (ch);
		if (wget_wch(main_win.win, &ch) == ERR)
			interface_fatal ("wget_wch() failed!");
#endif

		/* Recognize meta sequences */
		if (ch == KEY_ESCAPE) {
			meta = wgetch (main_win.win);
			if (meta != ERR)
				ch = meta | META_KEY_FLAG;
			k->type = IFACE_KEY_FUNCTION;
			k->key.func = ch;
		}
		else {
			k->type = IFACE_KEY_CHAR;
			k->key.ucs = ch;
		}
	}
	else {
		k->type = IFACE_KEY_FUNCTION;
		k->key.func = ch;
	}
}

/* Return a non zero value if the key is not a real key - KEY_RESIZE. */
int iface_key_is_resize (const struct iface_key *k)
{
	return k->type == IFACE_KEY_FUNCTION && k->key.func == KEY_RESIZE;
}

/* Handle a key command for the menu. */
void iface_menu_key (const enum key_cmd cmd)
{
	main_win_menu_cmd (&main_win, cmd);
	iface_refresh_screen ();
}

/* Get the type of the currently selected item. */
enum file_type iface_curritem_get_type ()
{
	return main_win_curritem_get_type (&main_win);
}

/* Return a non zero value if a directory menu is currently selected. */
int iface_in_dir_menu ()
{
	return main_win_in_dir_menu (&main_win);
}

/* Return a non zero value if the playlist menu is currently selected. */
int iface_in_plist_menu ()
{
	return main_win_in_plist_menu (&main_win);
}

/* Return a non zero value if the theme menu is currently selected. */
int iface_in_theme_menu ()
{
	return main_win_in_theme_menu (&main_win);
}

/* Return the currently selected file (malloc()ed) or NULL if the menu is
 * empty. */
char *iface_get_curr_file ()
{
	return main_win_get_curr_file (&main_win);
}

/* Set the current time of playing. */
void iface_set_curr_time (const int time)
{
	info_win_set_curr_time (&info_win, time);
	iface_refresh_screen ();
}

/* Set the total time for the currently played file. */
void iface_set_total_time (const int time)
{
	info_win_set_total_time (&info_win, time);
	info_win_set_block (&info_win, -1, -1);
	iface_refresh_screen ();
}

/* Set the start and end marks for the currently played file. */
void iface_set_block (const int block_start, const int block_end)
{
	info_win_set_block (&info_win, block_start, block_end);
	iface_refresh_screen ();
}

/* Set the state (STATE_(PLAY|STOP|PAUSE)). */
void iface_set_state (const int state)
{
	info_win_set_state (&info_win, state);
	iface_refresh_screen ();
}

/* Set the bitrate (in kbps). 0 or -1 means no bitrate information. */
void iface_set_bitrate (const int bitrate)
{
	assert (bitrate >= -1);

	info_win_set_bitrate (&info_win, bitrate);
	iface_refresh_screen ();
}

/* Set the rate (in kHz). 0 or -1 means no rate information. */
void iface_set_rate (const int rate)
{
	assert (rate >= -1);

	info_win_set_rate (&info_win, rate);
	iface_refresh_screen ();
}

/* Set the number of channels. */
void iface_set_channels (const int channels)
{
	assert (channels == 1 || channels == 2);

	info_win_set_channels (&info_win, channels);
	iface_refresh_screen ();
}

/* Set the currently played file. If file is NULL, nothing is played. */
void iface_set_played_file (const char *file)
{
	main_win_set_played_file (&main_win, file);

	if (!file) {
		info_win_set_played_title (&info_win, NULL);
		info_win_set_bitrate (&info_win, -1);
		info_win_set_rate (&info_win, -1);
		info_win_set_curr_time (&info_win, -1);
		info_win_set_total_time (&info_win, -1);
		info_win_set_block (&info_win, -1, -1);
		info_win_set_option_state (&info_win, "Net", 0);
	}
	else if (is_url(file)) {
		info_win_set_option_state (&info_win, "Net", 1);
	}

	iface_refresh_screen ();
}

/* Set the title for the currently played file. */
void iface_set_played_file_title (const char *title)
{
	assert (title != NULL);

	info_win_set_played_title (&info_win, title);
	iface_refresh_screen ();
}

/* Update timeouts, refresh the screen if needed. This should be called at
 * least once a second. */
void iface_tick ()
{
	info_win_tick (&info_win);
	iface_refresh_screen ();
}

void iface_set_mixer_value (const int value)
{
	assert (value >= 0);

	info_win_set_mixer_value (&info_win, value);
	iface_refresh_screen ();
}

/* Switch to the playlist menu. */
void iface_switch_to_plist ()
{
	main_win_switch_to (&main_win, MENU_PLAYLIST);
	info_win_set_files_time (&info_win,
			main_win_get_curr_files_time(&main_win),
			main_win_is_curr_time_for_all(&main_win));

	iface_refresh_screen ();
}

/* Switch to the directory menu. */
void iface_switch_to_dir ()
{
	main_win_switch_to (&main_win, MENU_DIR);
	info_win_set_files_time (&info_win,
			main_win_get_curr_files_time(&main_win),
			main_win_is_curr_time_for_all(&main_win));

	iface_refresh_screen ();
}

/* Add the item from the playlist to the playlist menu. */
void iface_add_to_plist (const struct plist *plist, const int num)
{
	assert (plist != NULL);

	main_win_add_to_plist (&main_win, plist, num);
	info_win_set_files_time (&info_win,
			main_win_get_curr_files_time(&main_win),
			main_win_is_curr_time_for_all(&main_win));

	iface_show_num_files (plist_count(plist));

	iface_refresh_screen ();
}

/* Display an error message. */
void iface_error (const char *msg)
{
	if (iface_initialized) {
		info_win_msg (&info_win, msg, ERROR_MSG, NULL, NULL, NULL);
		iface_refresh_screen ();
	}
	else
		fprintf (stderr, "ERROR: %s", msg);
}

/* Handle screen resizing. */
void iface_resize ()
{
	check_term_size (&main_win, &info_win);
	validate_layouts ();
	endwin ();
	refresh ();
	main_win_resize (&main_win);
	info_win_resize (&info_win);
	iface_refresh_screen ();
}

void iface_refresh ()
{
	wclear (main_win.win);
	wclear (info_win.win);

	main_win_draw (&main_win);
	info_win_draw (&info_win);

	iface_refresh_screen ();
}

void iface_update_show_time ()
{
	main_win_update_show_time (&main_win);
	iface_refresh_screen ();
}

void iface_update_show_format ()
{
	main_win_update_show_format (&main_win);
	iface_refresh_screen ();
}

void iface_clear_plist ()
{
	main_win_clear_plist (&main_win);
	iface_refresh_screen ();
}

void iface_del_plist_item (const char *file)
{
	assert (file != NULL);

	main_win_del_plist_item (&main_win, file);
	info_win_set_files_time (&info_win,
			main_win_get_curr_files_time(&main_win),
			main_win_is_curr_time_for_all(&main_win));
	iface_refresh_screen ();
}

void iface_make_entry (const enum entry_type type)
{
	info_win_make_entry (&info_win, type);
	iface_refresh_screen ();
}

enum entry_type iface_get_entry_type ()
{
	return info_win_get_entry_type (&info_win);
}

int iface_in_entry ()
{
	return info_win_in_entry (&info_win);
}

void iface_entry_handle_key (const struct iface_key *k)
{
	info_win_entry_handle_key (&info_win, &main_win, k);
	iface_refresh_screen ();
}

void iface_entry_set_text (const char *text)
{
	assert (text != NULL);

	info_win_entry_set_text (&info_win, text);
	iface_refresh_screen ();
}

/* Get text from the entry. Returned memory is malloc()ed. */
char *iface_entry_get_text ()
{
	return info_win_entry_get_text (&info_win);
}

void iface_entry_history_add ()
{
	info_win_entry_history_add (&info_win);
}

void iface_entry_disable ()
{
	if (iface_get_entry_type() == ENTRY_SEARCH)
		main_win_clear_filter_menu (&main_win);
	info_win_entry_disable (&info_win);
	iface_refresh_screen ();
}

void iface_entry_set_file (const char *file)
{
	assert (file != NULL);

	info_win_entry_set_file (&info_win, file);
}

/* Returned memory is malloc()ed. */
char *iface_entry_get_file ()
{
	return info_win_entry_get_file (&info_win);
}

void iface_message (const char *msg)
{
	assert (msg != NULL);

	info_win_msg (&info_win, msg, NORMAL_MSG, NULL, NULL, NULL);
	iface_refresh_screen ();
}

void iface_disable_message ()
{
	info_win_disable_msg (&info_win);
	iface_refresh_screen ();
}

void iface_user_query (const char *msg, const char *prompt,
                       t_user_reply_callback *callback, void *data)
{
	assert (prompt != NULL);

	info_win_msg (&info_win, msg, QUERY_MSG, prompt, callback, data);
	iface_refresh_screen ();
}

void iface_user_reply (const char *reply)
{
	info_win_disable_msg (&info_win);
	iface_win_user_reply (&info_win, reply);
}

void iface_user_history_add (const char *text)
{
	info_win_user_history_add (&info_win, text);
}

void iface_plist_set_total_time (const int time, const int for_all_files)
{
	if (iface_in_plist_menu())
		info_win_set_files_time (&info_win, time, for_all_files);
	main_win_set_plist_time (&main_win, time, for_all_files);
	iface_refresh_screen ();
}

void iface_select_file (const char *file)
{
	assert (file != NULL);

	main_win_select_file (&main_win, file);
	iface_refresh_screen ();
}

int iface_in_help ()
{
	return main_win_in_help (&main_win);
}

void iface_switch_to_help ()
{
	main_win_switch_to_help (&main_win);
	iface_refresh_screen ();
}

void iface_handle_help_key (const struct iface_key *k)
{
	main_win_handle_help_key (&main_win, k);
	iface_refresh_screen ();
}

int iface_in_lyrics ()
{
	return main_win_in_lyrics (&main_win);
}

void iface_switch_to_lyrics ()
{
	main_win_switch_to_lyrics (&main_win);
	iface_refresh_screen ();
}

void iface_handle_lyrics_key (const struct iface_key *k)
{
	main_win_handle_lyrics_key (&main_win, k);
	iface_refresh_screen ();
}

void iface_toggle_layout ()
{
	static int curr_layout = 1;
	char layout_option[10];
	lists_t_strs *layout_fmt;

	if (++curr_layout > 3)
		curr_layout = 1;

	sprintf (layout_option, "Layout%d", curr_layout);
	layout_fmt = options_get_list (layout_option);
	if (lists_strs_empty (layout_fmt)) {
		curr_layout = 1;
		layout_fmt = options_get_list ("Layout1");
	}

	main_win_use_layout (&main_win, layout_fmt);
	iface_refresh_screen ();
}

void iface_toggle_percent ()
{
	info_win.time_bar.show_pct = !info_win.time_bar.show_pct;
	bar_update_title (&info_win.time_bar);
	info_win_draw_block (&info_win);
	iface_refresh_screen ();
}

void iface_swap_plist_items (const char *file1, const char *file2)
{
	main_win_swap_plist_items (&main_win, file1, file2);
	iface_refresh_screen ();
}

/* Make sure that this file in this menu is visible. */
void iface_make_visible (const enum iface_menu menu, const char *file)
{
	assert (file != NULL);

	main_win_make_visible (&main_win,
			menu == IFACE_MENU_DIR ? MENU_DIR : MENU_PLAYLIST,
			file);
	iface_refresh_screen ();
}

void iface_switch_to_theme_menu ()
{
	main_win_create_themes_menu (&main_win);
	main_win_switch_to (&main_win, MENU_THEMES);
	iface_refresh_screen ();
}

/* Add a file to the current menu. */
void iface_add_file (const char *file, const char *title,
		const enum file_type type)
{
	assert (file != NULL);
	assert (file != NULL);

	main_win_add_file (&main_win, file, title, type);
	iface_refresh_screen ();
}

/* Temporary exit the interface (ncurses mode). */
void iface_temporary_exit ()
{
	endwin ();
}

/* Restore the interface after iface_temporary_exit(). */
void iface_restore ()
{
	iface_refresh ();
	if (!options_get_bool("UseCursorSelection"))
		curs_set (0);
}

void iface_load_lyrics (const char *file)
{
	lyrics_cleanup ();
	lyrics_autoload (file);
	main_win.lyrics_screen_top = 0;
	main_win_draw (&main_win);
}

static void update_queue_position (struct plist *playlist,
		struct plist *dir_list, const char *file, const int pos)
{
	int i;

	assert (file != NULL);
	assert (pos >= 0);

	if (playlist && (i = plist_find_fname(playlist, file)) != -1) {
		playlist->items[i].queue_pos = pos;
		main_win_update_item (&main_win, IFACE_MENU_PLIST,
				playlist, i);
	}

	if (dir_list && (i = plist_find_fname(dir_list, file)) != -1) {
		dir_list->items[i].queue_pos = pos;
		main_win_update_item (&main_win, IFACE_MENU_DIR,
				dir_list, i);
	}
}

/* Update queue positions in the playlist and directory menus. Only those items
 * which are in the queue (and whose position has therefore changed are
 * updated. One exception is the item which was deleted from the queue --
 * this one can be passed as the deleted_file parameter */
void iface_update_queue_positions (const struct plist *queue,
		struct plist *playlist, struct plist *dir_list,
		const char *deleted_file)
{
	int i;
	int pos = 1;

	assert (queue != NULL);

	for (i = 0; i < queue->num; i++) {
		if (!plist_deleted(queue,i)) {
			update_queue_position (playlist, dir_list,
					queue->items[i].file, pos);
			pos++;
		}
	}

	if (deleted_file)
		update_queue_position (playlist, dir_list, deleted_file, 0);

	iface_refresh_screen ();
}

/* Clear the queue -- zero the queue positions in playlist and directory
 * menus. */
void iface_clear_queue_positions (const struct plist *queue,
		struct plist *playlist, struct plist *dir_list)
{
	int i;

	assert (queue != NULL);
	assert (playlist != NULL);
	assert (dir_list != NULL);

	for (i = 0; i < queue->num; i++) {
		if (!plist_deleted(queue,i)) {
			update_queue_position (playlist, dir_list,
					queue->items[i].file, 0);
		}
	}

	iface_refresh_screen ();
}

void iface_update_queue_position_last (const struct plist *queue,
		struct plist *playlist, struct plist *dir_list)
{
	int i;
	int pos;

	assert (queue != NULL);

	i = plist_last (queue);
	pos = plist_get_position (queue, i);
	update_queue_position (playlist, dir_list, queue->items[i].file, pos);
	iface_refresh_screen ();
}
