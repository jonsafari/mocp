/*
 * MOC - music on console
 * Copyright (C) 2004,2005 Damian Pietras <daper@daper.net>
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
#include <assert.h>
#include <time.h>

#ifdef HAVE_NCURSES_H
# include <ncurses.h>
#elif HAVE_CURSES_H
# include <curses.h>
#endif

#include "menu.h"
#include "themes.h"
#include "main.h"
#include "options.h"
#include "interface_elements.h"
#include "log.h"
#include "files.h"
#include "decoder.h"
#include "keys.h"
#include "playlist.h"
#include "protocol.h"

#define STARTUP_MESSAGE	"The interface is being rewritten and thus is " \
	"not fully functional."
#define HISTORY_SIZE	50

/* TODO:
 * - xterm title (state, title of the song)
 */

/* Type of the side menu. */
enum side_menu_type
{
	MENU_DIR,	/* list of files in a directory */
	MENU_PLAYLIST,	/* a playlist of files */
	MENU_TREE	/* tree of directories */
};

struct side_menu
{
	enum side_menu_type type;
	int visible;	/* is it visible (are the other fields initialized) ? */
	WINDOW *win; 	/* window for the menu */

	/* Position and size of tme menu in the window */
	int posx;
	int posy;
	int width;
	int height;

	int total_time; /* total time of the files on the plalist */
	int total_time_for_all; /* is the total file counted for all files? */
	
	union
	{
		struct menu *list;
		/* struct menu_tree *tree;*/
	} menu;
};

/* State of the side menu that can be read/restored. It remembers the state
 * (position of the view, which file is selected etc.) of the menu. */
struct side_menu_state
{
	struct menu_state menu_state;
};

static struct main_win
{
	WINDOW *win;
	char *curr_file; /* currently played file. */
	struct side_menu menus[3];
	int selected_menu; /* which menu is currently selected by the user */
} main_win;

/* Bar for displaying mixer state or progress. */
struct bar
{
	int width;	/* width in chars */
	int filled;	/* how much is it filled in percent */
	char orig_title[40];	/* optional title */
	char title[256];	/* title with the percent value */
	int show_val;	/* show the title and the value? */
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
	char text[512];		/* the text the user types */
	char title[32];		/* displayed title */
	char *file;		/* optional: file associated with the entry */
				/* TODO: is it needed? */
	int cur_pos;		/* cursor position */
	int display_from;	/* displaying from this char */
	struct entry_history *history;	/* history to use with this entry or
					   NULL is history is not used */
	int history_pos;	/* current position in the history */
};

static struct info_win
{
	WINDOW *win;

	char *msg; /* message displayed instead of the file's title */
	int msg_is_error; /* is the above message an error? */
	time_t msg_timeout; /* how many seconds remain before the message
				disapperars */

	struct entry entry;
	int in_entry;		/* are we using the entry (is the above
				   structure initialized)?  */
	struct entry_history urls_history;
	struct entry_history dirs_history;
	
	/* true/false options values */
	int state_stereo;
	int state_shuffle;
	int state_repeat;
	int state_next;
	int state_net;

	int bitrate;		/* in Kbps */
	int rate;		/* in KHz */

	/* time in seconds */
	int curr_time;
	int total_time;

	int plist_time;		/* total time of files displayed in the menu */
	int plist_time_for_all;	/* is the above time for all files? */

	char *title;		/* title of the played song. */
	char status_msg[26];	/* status message */
	int state_play;		/* STATE_(PLAY | STOP | PAUSE) */

	struct bar mixer_bar;
	struct bar time_bar;
} info_win;

/* Are we running on xterm? */
static int has_xterm = 0;

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

	if (h->num < HISTORY_SIZE)
		h->items[h->num++] = xstrdup (text);
	else {
		free (h->items[0]);
		memmove (h->items, h->items + 1,
				(HISTORY_SIZE - 1) * sizeof(char *));
		h->items[h->num] = xstrdup (text);
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
	assert (num >= 0 && num < h->num);

	return xstrdup (h->items[num]);
}

/* Draw the entry. Use this function at the end of screen drawing, because
 * Set the cursor position in the right place. */
static void entry_draw (const struct entry *e, WINDOW *w, const int posx,
		const int posy)
{
	char text[sizeof(e->text)];
	
	assert (e != NULL);
	assert (w != NULL);
	assert (posx >= 0);
	assert (posy >= 0);
	
	wmove (w, posy, posx);
	wattrset (w, get_color(CLR_ENTRY_TITLE));
	wprintw (w, "%s:", e->title);
	
	wattrset (w, get_color(CLR_ENTRY));

	strncpy (text, e->text + e->display_from, e->width);
	text[e->width] = 0;
	
	wprintw (w, " %-*s", e->width, text);
	wmove (w, posy, e->cur_pos - e->display_from + strlen(e->title) + posx
			+ 2);
}

static void entry_init (struct entry *e, const enum entry_type type,
		const int width, struct entry_history *history)
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
		case ENTRY_PLIST_OVERWRITE:
			title = "File exists, overwrite?";
			break;
		default:
			abort ();
	}
	
	e->type = type;
	e->text[0] = 0;
	e->file = NULL;
	strcpy (e->title, title);
	e->width = width - strlen(title);
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

/* Set the entry text. Move the cursor to the end. */
static void entry_set_text (struct entry *e, const char *text)
{
	int len;
	
	assert (e != NULL);

	strncpy (e->text, text, sizeof(e->text));
	e->text[sizeof(e->text)-1] = 0;
	len = strlen (e->text);
	e->cur_pos = len;
	
	if (e->cur_pos - e->display_from > e->width)
		e->display_from = len - e->width;
}

/* Add a char to the entry where the cursor is placed. */
static void entry_add_char (struct entry *e, const char c)
{
	unsigned int len = strlen(e->text);

	assert (e != NULL);

	if (len < sizeof(e->text) - 1) {
		memmove (e->text + e->cur_pos + 1,
				e->text + e->cur_pos,
				len - e->cur_pos + 1);
		
		e->text[e->cur_pos] = c;
		e->cur_pos++;

		if (e->cur_pos - e->display_from > e->width)
			e->display_from++;
	}
}

/* Delete the char before the cursor. */
static void entry_back_space (struct entry *e)
{
	assert (e != NULL);

	if (e->cur_pos > 0) {
		int len = strlen (e->text);
		
		memmove (e->text + e->cur_pos - 1,
				e->text + e->cur_pos,
				len - e->cur_pos);
		e->text[--len] = 0;
		e->cur_pos--;

		if (e->cur_pos < e->display_from)
			e->display_from--;

		/* Can we show more after deleting the char? */
		if (e->display_from > 0
				&& len - e->display_from < e->width)
			e->display_from--;
	}
}

/* Delete the char under the cursor. */
static void entry_del_char (struct entry *e)
{
	int len;

	assert (e != NULL);

	len = strlen (e->text);

	if (e->cur_pos < len) {
		len--;
		memmove (e->text + e->cur_pos,
				e->text + e->cur_pos + 1,
				len - e->cur_pos);
		e->text[len] = 0;
		
		/* Can we show more after deleting the char? */
		if (e->display_from > 0
				&& len - e->display_from < e->width)
			e->display_from--;
	
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
	int len;
	
	assert (e != NULL);

	len = strlen (e->text);

	if (e->cur_pos < len) {
		e->cur_pos++;

		if (e->cur_pos > e->width + e->display_from)
			e->display_from++;
	}
}

/* Move the cursor to the end of the entry text. */
static void entry_end (struct entry *e)
{
	int len;
	
	assert (e != NULL);

	len = strlen (e->text);

	e->cur_pos = len;
	
	if (len > e->width)
		e->display_from = len - e->width;
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

/* Copy the previous history item to the entry if available, move the entry
 * history position down. */
static void entry_set_history_up (struct entry *e)
{
	assert (e != NULL);
	assert (e->history != NULL);

	if (e->history_pos > 0) {
		char *t;
		
		e->history_pos--;
		t = entry_history_get (e->history, e->history_pos);
		entry_set_text (e, t);
		free (t);
		e->cur_pos = 0;
	}
}

/* Copy the next history item to the entry if available, move the entry history
 * position down. */
static void entry_set_history_down (struct entry *e)
{
	assert (e != NULL);
	assert (e->history != NULL);

	if (e->history_pos < entry_history_nitems(e->history) - 1) {
		char *t;
		
		e->history_pos++;
		t = entry_history_get (e->history, e->history_pos);
		entry_set_text (e, t);
		free (t);
		e->cur_pos = 0;
	}
}
static void entry_destroy (struct entry *e)
{
	assert (e != NULL);

	if (e->file)
		free (e->file);
}

static char *entry_get_text (const struct entry *e)
{
	assert (e != NULL);

	return xstrdup (e->text);
}

static void entry_add_text_to_history (struct entry *e)
{
	assert (e != NULL);
	assert (e->history);

	entry_history_add (e->history, e->text);
}

static void side_menu_init (struct side_menu *m, const enum side_menu_type type,
		WINDOW *parent_win, const int height, const int width,
		const int posy, const int posx)
{
	assert (m != NULL);
	assert (parent_win != NULL);
	
	m->type = type;
	m->win = parent_win;
	m->posx = posx;
	m->posy = posy;
	m->height = height;
	m->width = width;

	m->total_time = 0;
	m->total_time_for_all = 0;

	if (type == MENU_DIR || type == MENU_PLAYLIST) {
		m->menu.list = menu_new (m->win, posx, posy, width, height);
		menu_set_items_numbering (m->menu.list,
				type == MENU_PLAYLIST
				&& options_get_int("PlaylistNumbering"));
		menu_set_show_format (m->menu.list,
				options_get_int("ShowFormat"));
		menu_set_show_time (m->menu.list,
				strcasecmp(options_get_str("ShowTime"), "no"));
		menu_set_info_attr (m->menu.list,
				get_color(CLR_MENU_ITEM_INFO));
	}
	else
		abort ();
	
	m->visible = 1;
}

static void side_menu_destroy (struct side_menu *m)
{
	assert (m != NULL);

	if (m->visible) {
		if (m->type == MENU_DIR || m->type == MENU_PLAYLIST)
			menu_free (m->menu.list);
		else
			abort ();
		
		m->visible = 0;
	}
}

static void main_win_init (struct main_win *w)
{
	assert (w != NULL);
	
	w->win = newwin (LINES - 4, COLS, 0, 0);
	wbkgd (w->win, get_color(CLR_BACKGROUND));
	nodelay (w->win, TRUE);
	keypad (w->win, TRUE);

	w->curr_file = NULL;

	side_menu_init (&w->menus[0], MENU_DIR, w->win, LINES - 5, COLS - 2,
			1, 1);
	/*side_menu_init (&w->menus[0], MENU_DIR, w->win, 5, 40,
			1, 1);*/
	side_menu_init (&w->menus[1], MENU_PLAYLIST, w->win, LINES - 5,
			COLS - 2, 1, 1);
	w->menus[2].visible = 0;

	w->selected_menu = 0;
}

static void main_win_destroy (struct main_win *w)
{
	assert (w != NULL);

	side_menu_destroy (&w->menus[0]);
	side_menu_destroy (&w->menus[1]);

	if (w->win)
		delwin (w->win);
	if (w->curr_file)
		free (w->curr_file);
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

/* Make a title sutible to display in a menu from the title of a playlist item.
 * Returned memory is malloc()ed.
 * made_from tags - was the playlist title made from tags? 
 * for_plist - is this title to be displayed in the playlist?
 */
static char *make_menu_title (const char *plist_title,
		const int made_from_tags, const int for_plist)
{
	char *title = xstrdup (plist_title);
	
	if (made_from_tags)
		title = iconv_str (title, 0);
	else {
		if (!for_plist) {

			/* Use only the file name instead of the full path. */
			char *slash = strrchr (title, '/');
			
			if (slash && slash != title) {
				char *old_title = title;
				
				title = xstrdup (slash + 1);
				free (old_title);
			}
		}
		
		title = iconv_str (title, 1);
	}

	return title;
}

/* Add an item from the playlist to the menu.
 * If for_playlist has non-zero value, full paths will be displayed instead of
 * just file names. */
static void add_to_menu (struct menu *menu, const struct plist *plist,
		const int num, const int for_playlist)
{
	struct menu_item *added;
	const struct plist_item *item = &plist->items[num];
	char *title;

	title = make_menu_title (item->title, item->title == item->title_tags,
			for_playlist);
	added = menu_add (menu, title, plist_file_type(plist, num), item->file);
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
	
	menu_item_set_format (added, file_type_name(item->file));

	if (for_playlist && item->title == item->title_file)
		menu_item_set_align (added, MENU_ALIGN_RIGHT);
}

static void side_menu_clear (struct side_menu *m)
{
	assert (m != NULL);
	assert (m->visible);
	assert (m->type == MENU_DIR || m->type == MENU_PLAYLIST);
	assert (m->menu.list != NULL);

	menu_free (m->menu.list);
	m->menu.list = menu_new (m->win, m->posx, m->posy, m->width, m->height);
	menu_set_items_numbering (m->menu.list,	m->type == MENU_PLAYLIST
			&& options_get_int("PlaylistNumbering"));
	
	menu_set_show_format (m->menu.list, options_get_int("ShowFormat"));
	menu_set_show_time (m->menu.list,
			strcasecmp(options_get_str("ShowTime"), "no"));
	menu_set_info_attr (m->menu.list, get_color(CLR_MENU_ITEM_INFO));
}

/* Fill the directory or playlist side menu with this content. */
static void side_menu_make_list_content (struct side_menu *m,
		const struct plist *files, const struct file_list *dirs,
		const struct file_list *playlists)
{
	struct menu_item *added;
	int i;

	assert (m != NULL);
	assert (m->type == MENU_DIR || m->type == MENU_PLAYLIST);
	assert (m->menu.list != NULL);

	side_menu_clear (m);

	added = menu_add (m->menu.list, "../", F_DIR, "..");
	menu_item_set_attr_normal (added, get_color(CLR_MENU_ITEM_DIR));
	menu_item_set_attr_sel (added, get_color(CLR_MENU_ITEM_DIR_SELECTED));
	
	if (dirs)
		for (i = 0; i < dirs->num; i++) {
			char title[PATH_MAX];

			strcpy (title, strrchr(dirs->items[i], '/') + 1);
			strcat (title, "/");
			
			added = menu_add (m->menu.list, title, F_DIR,
					dirs->items[i]);
			menu_item_set_attr_normal (added,
					get_color(CLR_MENU_ITEM_DIR));
			menu_item_set_attr_sel (added,
					get_color(CLR_MENU_ITEM_DIR_SELECTED));
		}

	if (playlists)
		for (i = 0; i < playlists->num; i++){
			added = menu_add (m->menu.list,
					strrchr(playlists->items[i], '/') + 1,
					F_PLAYLIST, playlists->items[i]);
			menu_item_set_attr_normal (added,
					get_color(CLR_MENU_ITEM_PLAYLIST));
			menu_item_set_attr_sel (added,
					get_color(
					CLR_MENU_ITEM_PLAYLIST_SELECTED));
		}
	
	/* playlist items */
	for (i = 0; i < files->num; i++) {
		if (!plist_deleted(files, i))
			add_to_menu (m->menu.list, files, i,
					m->type == MENU_PLAYLIST);
	}

	m->total_time = plist_total_time (files, &m->total_time_for_all);
}

static void clear_area (WINDOW *w, const int posx, const int posy,
		const int width, const int height)
{
	int x, y;

	for (y = posy; y < posy + height; y++) {
		wmove (w, y, posx);
		for (x = 0; x < width; x++)
			waddch (w, ' ');
	}
}

static void side_menu_draw (const struct side_menu *m)
{
	assert (m != NULL);
	assert (m->visible);

	clear_area (m->win, m->posx, m->posy, m->width, m->height);
	
	if (m->type == MENU_DIR || m->type == MENU_PLAYLIST)
		menu_draw (m->menu.list);
	else
		abort ();
}

static void side_menu_cmd (struct side_menu *m, const enum key_cmd cmd)
{
	assert (m != NULL);
	assert (m->visible);

	if (m->type == MENU_DIR || m->type == MENU_PLAYLIST) {
		switch (cmd) {
			case KEY_CMD_MENU_DOWN:
				menu_driver (m->menu.list, REQ_DOWN);
				break;
			case KEY_CMD_MENU_UP:
				menu_driver (m->menu.list, REQ_UP);
				break;
			case KEY_CMD_MENU_NPAGE:
				menu_driver (m->menu.list, REQ_PGDOWN);
				break;
			case KEY_CMD_MENU_PPAGE:
				menu_driver (m->menu.list, REQ_PGUP);
				break;
			case KEY_CMD_MENU_FIRST:
				menu_driver (m->menu.list, REQ_TOP);
				break;
			case KEY_CMD_MENU_LAST:
				menu_driver (m->menu.list, REQ_BOTTOM);
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
	assert (m->type == MENU_DIR || m->type == MENU_PLAYLIST);

	mi = menu_curritem(m->menu.list);

	if (mi)
		return menu_item_get_type (mi);

	return F_OTHER;
}

static char *side_menu_get_curr_file (const struct side_menu *m)
{
	struct menu_item *mi;
	
	assert (m != NULL);
	assert (m->visible);
	assert (m->type == MENU_DIR || m->type == MENU_PLAYLIST);

	mi = menu_curritem (m->menu.list);

	if (mi)
		return menu_item_get_file (mi);

	return NULL;
}

static struct side_menu *find_side_menu (struct main_win *w,
		const enum side_menu_type type)
{
	int i;

	assert (w != NULL);

	for (i = 0; i < (int)(sizeof(w->menus)/sizeof(w->menus[0])); i++) {
		struct side_menu *m = &w->menus[i];
	
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

	menu_setcurritem_title (m->menu.list, title);
}

/* Update item title and time for this item if ti's present on this menu. */
static void side_menu_update_item (struct side_menu *m,
		const struct plist *plist, const int n)
{
	struct menu_item *mi;
	const struct plist_item *item;
	
	assert (m != NULL);
	assert (m->visible);
	assert (m->type == MENU_DIR || m->type == MENU_PLAYLIST);
	assert (plist != NULL);
	assert (n >= 0 && n < plist->num);

	item = &plist->items[n];
	
	if ((mi = menu_find(m->menu.list, item->file))) {
		char *title;
		
		if (item->tags && item->tags->time != -1) {
			char time_str[6];
		
			sec_to_min (time_str, item->tags->time);
			menu_item_set_time (mi, time_str);
		}
		else
			menu_item_set_time (mi, "");

		title = make_menu_title (item->title,
				item->title == item->title_tags,
				m->type == MENU_PLAYLIST);

		menu_item_set_title (mi, title);
		
		if (m->type == MENU_PLAYLIST && item->title == item->title_tags)
			menu_item_set_align (mi, MENU_ALIGN_RIGHT);

		m->total_time = plist_total_time (plist,
				&m->total_time_for_all);

		free (title);
	}
}

static void side_menu_unmark_file (struct side_menu *m)
{
	assert (m != NULL);
	assert (m->visible);
	assert (m->type == MENU_DIR || m->type == MENU_PLAYLIST);

	menu_unmark_item (m->menu.list);
}

static void side_menu_mark_file (struct side_menu *m, const char *file)
{
	assert (m != NULL);
	assert (m->visible);
	assert (m->type == MENU_DIR || m->type == MENU_PLAYLIST);

	menu_mark_item (m->menu.list, file);
}

static void side_menu_add_plist_item (struct side_menu *m,
		const struct plist *plist, const int num)
{
	assert (m != NULL);
	assert (plist != NULL);
	assert (m->visible);
	assert (m->type == MENU_DIR || m->type == MENU_PLAYLIST);

	add_to_menu (m->menu.list, plist, num, m->type == MENU_PLAYLIST);
	m->total_time = plist_total_time (plist, &m->total_time_for_all);
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

	menu_set_show_time (m->menu.list,
				strcasecmp(options_get_str("ShowTime"), "no"));
}

static void side_menu_update_show_format (struct side_menu *m)
{
	assert (m != NULL);
	assert (m->visible);
	assert (m->type == MENU_DIR || m->type == MENU_PLAYLIST);

	menu_set_show_format (m->menu.list, options_get_int("ShowFormat"));
}

static void side_menu_get_state (const struct side_menu *m,
		struct side_menu_state *st)
{
	assert (m != NULL);
	assert (st != NULL);
	assert (m->visible);
	assert (m->type == MENU_DIR || m->type == MENU_PLAYLIST);

	menu_get_state (m->menu.list, &st->menu_state);
}

static void side_menu_set_state (struct side_menu *m,
		const struct side_menu_state *st)
{
	assert (m != NULL);
	assert (st != NULL);
	assert (m->visible);
	assert (m->type == MENU_DIR || m->type == MENU_PLAYLIST);

	menu_set_state (m->menu.list, &st->menu_state);
}

static void side_menu_del_item (struct side_menu *m, const char *file)
{
	assert (m != NULL);
	assert (m->visible);
	assert (m->type == MENU_DIR || m->type == MENU_PLAYLIST);

	menu_del_item (m->menu.list, file);
}

static void side_menu_set_plist_time (struct side_menu *m, const int time,
		const int time_for_all)
{
	assert (m != NULL);
	assert (time >= 0);
	
	m->total_time = time;
	m->total_time_for_all = time_for_all;
}

static void side_menu_resize (struct side_menu *m, const int height,
		const int width, const int posy, const int posx)
{
	assert (m != NULL);

	m->posx = posx;
	m->posy = posy;
	m->height = height;
	m->width = width;

	if (m->type == MENU_DIR || m->type == MENU_PLAYLIST)
		menu_update_size (m->menu.list, posx, posy, width, height);
	else
		abort ();
}

static void main_win_draw (const struct main_win *w)
{
	int i;

	/* Draw all visible menus, draw the selected menu as the last menu. */
	for (i = 0; i < (int)(sizeof(w->menus)/sizeof(w->menus[0])); i++)
		if (w->menus[i].visible && i != w->selected_menu)
			side_menu_draw (&w->menus[i]);

	side_menu_draw (&w->menus[w->selected_menu]);
}

static void main_win_set_dir_content (struct main_win *w,
		const enum iface_menu iface_menu, const struct plist *files,
		const struct file_list *dirs, const struct file_list *playlists)
{
	struct side_menu *m;
	
	assert (w != NULL);
	
	m = find_side_menu (w, iface_menu == IFACE_MENU_DIR ? MENU_DIR
			: MENU_PLAYLIST);

	side_menu_make_list_content (m, files, dirs, playlists);
	if (w->curr_file)
		side_menu_mark_file (m, w->curr_file);
	main_win_draw (w);
}

static void main_win_update_dir_content (struct main_win *w,
		const enum iface_menu iface_menu, const struct plist *files,
		const struct file_list *dirs, const struct file_list *playlists)
{
	struct side_menu *m;
	struct side_menu_state ms;

	assert (w != NULL);

	m = find_side_menu (w, iface_menu == IFACE_MENU_DIR ? MENU_DIR
			: MENU_PLAYLIST);

	side_menu_get_state (m, &ms);
	side_menu_make_list_content (m, files, dirs, playlists);
	side_menu_set_state (m, &ms);
	if (w->curr_file)
		side_menu_mark_file (m, w->curr_file);
	main_win_draw (w);
}

static void main_win_switch_to (struct main_win *w,
		const enum side_menu_type menu)
{
	int i;
	
	assert (w != NULL);

	for (i = 0; i < (int)(sizeof(w->menus)/sizeof(w->menus[0])); i++)
		if (w->menus[i].type == menu) {
			w->selected_menu = i;
			break;
		}

	assert (i < (int)(sizeof(w->menus)/sizeof(w->menus[0])));

	main_win_draw (w);
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

static int main_win_in_plist_menu (const struct main_win *w)
{
	assert (w != NULL);

	return w->menus[w->selected_menu].type == MENU_PLAYLIST;
}

static void main_win_set_curr_item_title (struct main_win *w, const char *title)
{
	assert (w != NULL);
	assert (title != NULL);

	side_menu_set_curr_item_title (&w->menus[w->selected_menu], title);
	main_win_draw (w);
}

/* Update item title and time on all menus where it's present. */
static void main_win_update_item (struct main_win *w, const struct plist *plist,
		const int n)
{
	int i;

	assert (w != NULL);
	assert (plist != NULL);
	assert (n >= 0 && n < plist->num);

	for (i = 0; i < (int)(sizeof(w->menus)/sizeof(w->menus[0])); i++) {
		struct side_menu *m = &w->menus[i];
		
		if (m->visible && (m->type == MENU_DIR
					|| m->type == MENU_PLAYLIST))
			side_menu_update_item (m, plist, n);
	}

	main_win_draw (w);
}

/* Mark the played file on all lists of files or unmark it when file is NULL. */
static void main_win_set_played_file (struct main_win *w, const char *file)
{
	int i;
	
	assert (w != NULL);

	if (w->curr_file)
		free (w->curr_file);
	w->curr_file = xstrdup (file);

	for (i = 0; i < (int)(sizeof(w->menus)/sizeof(w->menus[0])); i++) {
		struct side_menu *m = &w->menus[i];

		if (m->visible && (m->type == MENU_DIR
					|| m->type == MENU_PLAYLIST)) {
			side_menu_unmark_file (m);
			if (file)
				side_menu_mark_file (m, file);
		}
	}

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
	
	assert (plist != NULL);

	m = find_side_menu (w, MENU_PLAYLIST);
	side_menu_add_plist_item (m, plist, num);
	if (w->curr_file)
		side_menu_mark_file (m, w->curr_file);
	main_win_draw (w);
}

static int main_win_get_files_time (const struct main_win *w)
{
	assert (w != NULL);

	return side_menu_get_files_time (&w->menus[w->selected_menu]);
}

static int main_win_is_time_for_all (const struct main_win *w)
{
	assert (w != NULL);

	return side_menu_is_time_for_all (&w->menus[w->selected_menu]);
}

/* Handle terminal size change. */
static void main_win_resize (struct main_win *w)
{
	assert (w != NULL);

	keypad (w->win, TRUE);
	wresize (w->win, LINES - 4, COLS);
	werase (w->win);

	side_menu_resize (&w->menus[0], LINES - 5, COLS - 2, 1, 1);
	side_menu_resize (&w->menus[1], LINES - 5, COLS - 2, 1, 1);

	main_win_draw (w);
}

static void main_win_update_show_time (struct main_win *w)
{
	int i;
	
	assert (w != NULL);

	for (i = 0; i < (int)(sizeof(w->menus)/sizeof(w->menus[0])); i++) {
		struct side_menu *m = &w->menus[i];

		if (m->visible && (m->type == MENU_DIR
					|| m->type == MENU_PLAYLIST))
			side_menu_update_show_time (&w->menus[i]);
	}

	main_win_draw (w);
}

static void main_win_update_show_format (struct main_win *w)
{
	int i;
	
	assert (w != NULL);

	for (i = 0; i < (int)(sizeof(w->menus)/sizeof(w->menus[0])); i++) {
		struct side_menu *m = &w->menus[i];

		if (m->visible && (m->type == MENU_DIR
					|| m->type == MENU_PLAYLIST))
			side_menu_update_show_format (&w->menus[i]);
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

/* End the program if the terminal is too small. */
static void check_term_size ()
{
	if (COLS < 79 || LINES < 7)
		fatal ("The terminal is too small after resizeing.");
}

/* Update the title with the current fill. */
static void bar_update_title (struct bar *b)
{
	assert (b != NULL);
	assert (b->show_val);
	
	if (b->filled < 100)
		sprintf (b->title, "%*s  %02d%%  ", b->width - 7, b->orig_title,
				b->filled);
	else
		sprintf (b->title, "%*s 100%%  ", b->width - 7, b->orig_title);
}

static void bar_set_title (struct bar *b, const char *title)
{
	assert (b != NULL);
	assert (b->show_val);
	assert (title != NULL);
	assert (strlen(title) < sizeof(b->title) - 5);

	strcpy (b->orig_title, title);
	bar_update_title (b);
}

static void bar_init (struct bar *b, const int width, const char *title,
		const int show_val, const int fill_color,
		const int empty_color)
{
	assert (b != NULL);
	assert (width > 5 && width < (int)sizeof(b->title));
	assert (title != NULL || !show_val);
	
	b->width = width;
	b->filled = 0;
	b->show_val = show_val;
	b->fill_color = fill_color;
	b->empty_color = empty_color;
	
	if (show_val)
		bar_set_title (b, title);
	else {
		int i;

		for (i = 0; i < b->width; i++)
			b->title[i] = ' ';
		b->title[b->width] = 0;
	}
}

static void bar_draw (const struct bar *b, WINDOW *win, const int pos_x,
		const int pos_y)
{
	int fill_chars; /* how many chars are "filled" */

	assert (b != NULL);
	assert (win != NULL);
	assert (pos_x >= 0 && pos_x < COLS - b->width);
	assert (pos_y >= 0 && pos_y < LINES);

	fill_chars = b->filled * b->width / 100;
	
	wattrset (win, b->fill_color);
	mvwaddnstr (win, pos_y, pos_x, b->title, fill_chars);

	wattrset (win, b->empty_color);
	waddstr (win, b->title + fill_chars);
}

static void bar_set_fill (struct bar *b, const int fill)
{
	assert (b != NULL);
	assert (fill >= 0);
	
	b->filled = fill <= 100 ? fill : 100;

	if (b->show_val)
		bar_update_title (b);
}

static void bar_resize (struct bar *b, const int width)
{
	assert (b != NULL);
	assert (width > 5 && width < (int)sizeof(b->title));
	b->width = width;

	if (b->show_val)
		bar_update_title (b);
	else {
		int i;

		for (i = 0; i < b->width; i++)
			b->title[i] = ' ';
		b->title[b->width] = 0;
	}	
}

static void info_win_init (struct info_win *w)
{
	assert (w != NULL);

	w->win = newwin (4, COLS, LINES - 4, 0);
	wbkgd (w->win, get_color(CLR_BACKGROUND));

	w->state_stereo = 0;
	w->state_shuffle = 0;
	w->state_repeat = 0;
	w->state_next = 0;
	w->state_play = STATE_STOP;
	w->state_net = 0;

	w->bitrate = -1;
	w->rate = -1;

	w->curr_time = -1;
	w->total_time = -1;

	w->title = NULL;
	w->status_msg[0] = 0;
	
	w->in_entry = 0;
	entry_history_init (&w->urls_history);
	entry_history_init (&w->dirs_history);

	w->msg = xstrdup (STARTUP_MESSAGE);
	w->msg_is_error = 0;
	w->msg_timeout = time(NULL) + 3;

	bar_init (&w->mixer_bar, 20, "", 1, get_color(CLR_MIXER_BAR_FILL),
			get_color(CLR_MIXER_BAR_EMPTY));
	bar_init (&w->time_bar, COLS - 4, NULL, 0, get_color(CLR_TIME_BAR_FILL),
			get_color(CLR_TIME_BAR_EMPTY));
}

static void info_win_destroy (struct info_win *w)
{
	assert (w != NULL);

	if (w->win)
		delwin (w->win);
	if (w->msg)
		free (w->msg);
	if (w->in_entry)
		entry_destroy (&w->entry);

	entry_history_clear (&w->urls_history);
	entry_history_clear (&w->dirs_history);
}

/* Set the cursor position in the right place if needed. */
static void info_win_update_curs (const struct info_win *w)
{
	assert (w != NULL);
	
	if (w->in_entry)
		entry_draw (&w->entry, w->win, 1, 0);
}

static void info_win_set_mixer_name (struct info_win *w, const char *name)
{
	assert (w != NULL);
	assert (name != NULL);
	
	bar_set_title (&w->mixer_bar, name);
	if (!w->in_entry) {
		bar_draw (&w->mixer_bar, w->win, COLS - 37, 0);
		info_win_update_curs (w);
	}
}

static void info_win_draw_status (const struct info_win *w)
{
	assert (w != NULL);

	if (!w->in_entry) {
		wattrset (w->win, get_color(CLR_STATUS));
		mvwprintw (w->win, 0, 6, "%-*s", sizeof(w->status_msg) - 1,
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

	wattrset (w->win, get_color(CLR_STATE));
	mvwaddstr (w->win, 1, 1, state_symbol);
	info_win_update_curs (w);
}

/* Draw the title or the message (informative or error). */
static void info_win_draw_title (const struct info_win *w)
{
	assert (w != NULL);

	clear_area (w->win, 4, 1, COLS - 5, 1);

	if (w->msg && w->msg_timeout >= time(NULL)) {
		wattrset (w->win, w->msg_is_error ? get_color(CLR_ERROR)
				: get_color(CLR_MESSAGE));
		mvwaddnstr (w->win, 1, 4, w->msg, COLS - 5);
	}
	else {
		wattrset (w->win, get_color(CLR_TITLE));
		mvwaddnstr (w->win, 1, 4, w->title, COLS - 5);
	}

	info_win_update_curs (w);
}

static void info_win_set_state (struct info_win *w, const int state)
{
	assert (w != NULL);
	assert (state == STATE_PLAY || state == STATE_STOP
			|| state == STATE_PAUSE);

	w->state_play = state;
	info_win_draw_state (w);
}

static void info_win_draw_time (const struct info_win *w)
{
	char time_str[6];

	assert (w != NULL);
	
	/* current time */
	sec_to_min (time_str, w->curr_time != -1 ? w->curr_time : 0);
	wattrset (w->win, get_color(CLR_TIME_CURRENT));
	mvwaddstr (w->win, 2, 1, time_str);

	/* time left */
	if (w->total_time > 0 && w->curr_time >= 0
			&& w->total_time >= w->curr_time) {
		sec_to_min (time_str, w->total_time - w->curr_time);
		wmove (w->win, 2, 7);
		wattrset (w->win, get_color(CLR_TIME_LEFT));
		waddstr (w->win, time_str);
	}
	else
		mvwaddstr (w->win, 2, 7, "     ");

	/* total time */
	sec_to_min (time_str, w->total_time != -1 ? w->total_time : 0);
	wmove (w->win, 2, 14);
	wattrset (w->win, get_color(CLR_TIME_TOTAL));
	waddstr (w->win, time_str);

	bar_draw (&w->time_bar, w->win, 2, 3);
	info_win_update_curs (w);
}

static void info_win_set_curr_time (struct info_win *w, const int time)
{
	assert (w != NULL);
	assert (time >= -1);

	w->curr_time = time;
	if (w->total_time > 0 && w->curr_time >= 0)
		bar_set_fill (&w->time_bar, w->curr_time * 100 / w->total_time);
	else
		bar_set_fill (&w->time_bar, 0);
	
	info_win_draw_time (w);
}

static void info_win_set_total_time (struct info_win *w, const int time)
{
	assert (w != NULL);
	assert (time >= -1);

	w->total_time = time;
	
	if (w->total_time > 0 && w->curr_time >= 0)
		bar_set_fill (&w->time_bar, w->curr_time * 100 / w->total_time);
	else
		bar_set_fill (&w->time_bar, 0);

	info_win_draw_time (w);
}

static void info_win_set_played_title (struct info_win *w, const char *title)
{
	assert (w != NULL);

	if (w->title)
		free (w->title);
	w->title = xstrdup (title);
	//TODO: iconv()
	info_win_draw_title (w);
}

static void info_win_draw_rate (const struct info_win *w)
{
	assert (w != NULL);

	wattrset (w->win, get_color(CLR_SOUND_PARAMS));
	if (w->rate != -1)
		mvwprintw (w->win, 2, 22, "%3d", w->rate);
	else
		mvwaddstr (w->win, 2, 22, "   ");
}

static void info_win_draw_bitrate (const struct info_win *w)
{
	assert (w != NULL);

	wattrset (w->win, get_color(CLR_SOUND_PARAMS));
	if (w->bitrate != -1)
		mvwprintw (w->win, 2, 29, "%4d", w->bitrate);
	else
		mvwaddstr (w->win, 2, 29, "    ");
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
	assert (value >= 0 && value <= 100);

	bar_set_fill (&w->mixer_bar, value);
	if (!w->in_entry)
		bar_draw (&w->mixer_bar, w->win, COLS - 37, 0);
}

/* Draw a switch that is turned on or off in form of [TITLE]. */
static void info_win_draw_switch (const struct info_win *w, const int posx,
		const int posy, const char *title, const int value)
{
	assert (w != NULL);
	assert (title != NULL);

	wattrset (w->win, get_color(
				value ? CLR_INFO_ENABLED : CLR_INFO_DISABLED));
	mvwprintw (w->win, posy, posx, "[%s]", title);
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

static void info_win_msg (struct info_win *w, const char *msg,
		const int is_error)
{
	if (w->msg)
		free (w->msg);
	w->msg = xstrdup (msg);
	w->msg_is_error = is_error;
	w->msg_timeout = time(NULL) + 3;
	info_win_draw_title (w);
}

static void info_win_disable_msg (struct info_win *w)
{
	assert (w != NULL);

	if (w->msg) {
		free (w->msg);
		w->msg = NULL;
	}

	info_win_draw_title (w);
}

static void info_win_set_channels (struct info_win *w, const int channels)
{
	assert (w != NULL);
	assert (channels == 1 || channels == 2);

	w->state_stereo = (channels == 2) ? 1 : 0;
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
		const int value)
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

	if (!w->in_entry) {
		char buf[10];

		sec_to_min_plist (buf, w->plist_time);
		wmove (w->win, 0, COLS - 12);
		wattrset (w->win, get_color(CLR_PLIST_TIME));
		waddch (w->win, w->plist_time_for_all ? ' ' : '>');
		waddstr (w->win, buf);
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
	if (w->msg && time(NULL) > w->msg_timeout) {
		free (w->msg);
		w->msg = NULL;

		info_win_draw_title (w);
	}
}

/* Draw static elements of info_win: frames, legend etc. */
static void info_win_draw_static_elements (const struct info_win *w)
{
	assert (w != NULL);
	
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
	mvwaddch (w->win, 3, COLS - 2, lines.ltee);
	mvwaddch (w->win, 3, 1, lines.rtee);
	
	/* status line frame */
	mvwaddch (w->win, 0, 5, lines.rtee);
	mvwaddch (w->win, 0, 5 + sizeof(w->status_msg), lines.ltee);
	
	/* rate and bitrate units */
	wmove (w->win, 2, 25);
	wattrset (w->win, get_color(CLR_LEGEND));
	waddstr (w->win, "KHz     Kbps");

	info_win_update_curs (w);
}

static void info_win_make_entry (struct info_win *w, const enum entry_type type)
{
	struct entry_history *history;
	
	assert (w != NULL);
	assert (!w->in_entry);

	switch (type) {
		case ENTRY_GO_DIR:
			history = &w->dirs_history;
			break;
		case ENTRY_GO_URL:
			history = &w->urls_history;
			break;
		default:
			history = NULL;
	}

	entry_init (&w->entry, type, COLS - 4, history);
	w->in_entry = 1;
	curs_set (1);
	entry_draw (&w->entry, w->win, 1, 0);
}

static void info_win_entry_handle_key (struct info_win *w, const int ch)
{
	enum key_cmd cmd;
	enum entry_type type;

	assert (w != NULL);
	assert (w->in_entry);

	cmd = get_key_cmd (CON_ENTRY, ch);
	type = entry_get_type (&w->entry);
	
	if (isgraph(ch) || ch == ' ')
		entry_add_char (&w->entry, ch);
	else if (ch == KEY_LEFT)
		entry_curs_left (&w->entry);
	else if (ch == KEY_RIGHT)
		entry_curs_right (&w->entry);
	else if (ch == KEY_BACKSPACE)
		entry_back_space (&w->entry);
	else if (ch == KEY_DC)
		entry_del_char (&w->entry);
	else if (ch == KEY_HOME)
		entry_home (&w->entry);
	else if (ch == KEY_END)
		entry_end (&w->entry);
	else if (type == ENTRY_GO_DIR || type == ENTRY_GO_URL) {
		if (cmd == KEY_CMD_HISTORY_UP)
			entry_set_history_up (&w->entry);
		else if (cmd == KEY_CMD_HISTORY_DOWN)
			entry_set_history_down (&w->entry);
	}

	entry_draw (&w->entry, w->win, 1, 0);
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

static void info_win_draw (const struct info_win *w)
{
	assert (w != NULL);
	
	info_win_draw_static_elements (w);
	info_win_draw_state (w);
	info_win_draw_time (w);
	info_win_draw_title (w);
	info_win_draw_options_state (w);
	info_win_draw_status (w);
	info_win_draw_files_time (w);
	info_win_draw_bitrate (w);
	info_win_draw_rate (w);
	
	if (w->in_entry)
		entry_draw (&w->entry, w->win, 1, 0);
	else
		bar_draw (&w->mixer_bar, w->win, COLS - 37, 0);

	bar_draw (&w->time_bar, w->win, 2, 3);
	info_win_update_curs (w);
}

static void info_win_entry_disable (struct info_win *w)
{
	assert (w != NULL);
	assert (w->in_entry);
	
	entry_destroy (&w->entry);
	w->in_entry = 0;
	curs_set (0);
	info_win_draw (w);
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

	if (w->in_entry)
		entry_resize (&w->entry, COLS - 4);

	info_win_draw (w);
}

void windows_init ()
{
	initscr ();
	cbreak ();
	noecho ();
	curs_set (0);
	use_default_colors ();

	check_term_size ();
	
	detect_term ();
	start_color ();
	theme_init (has_xterm);
	init_lines ();

	main_win_init (&main_win);
	info_win_init (&info_win);

	main_win_draw (&main_win);
	info_win_draw (&info_win);
	
	wrefresh (main_win.win);
	wrefresh (info_win.win);
}

void windows_end ()
{
	main_win_destroy (&main_win);
	info_win_destroy (&info_win);
	
	/* endwin() sometimes fails on x terminals when we get SIGCHLD
	 * at this moment. Double invokation seems to solve this. */
	if (endwin() == ERR && endwin() == ERR)
		logit ("endwin() failed!");

	/* Make sure that the next line after we exit will be "clear". */
	putchar ('\n');
}

/* Set state of the options displayed in the information window. */
void iface_set_option_state (const char *name, const int value)
{
	assert (name != NULL);

	info_win_set_option_state (&info_win, name, value);
}

/* Set the mixer name. */
void iface_set_mixer_name (const char *name)
{
	assert (name != NULL);
	
	info_win_set_mixer_name (&info_win, name);
	wrefresh (info_win.win);
}

/* Set the status message in the info window. */
void iface_set_status (const char *msg)
{
	assert (msg != NULL);

	info_win_set_status (&info_win, msg);
	wrefresh (info_win.win);
}

/* Change the content of the directory menu to these files, directories, and
 * playlists. */
void iface_set_dir_content (const enum iface_menu iface_menu,
		const struct plist *files, const struct file_list *dirs,
		const struct file_list *playlists)
{
	main_win_set_dir_content (&main_win, iface_menu, files, dirs,
			playlists);
	info_win_set_files_time (&info_win, main_win_get_files_time(&main_win),
			main_win_is_time_for_all(&main_win));
	wrefresh (info_win.win);
	wrefresh (main_win.win);
	
	/* TODO: also display the number of items */
}

/* Like iface_set_dir_content(), but before replacing the menu content, save
 * the menu state (selected file, view position) and restore it after making
 * a new menu. */
void iface_update_dir_content (const enum iface_menu iface_menu,
		const struct plist *files, const struct file_list *dirs,
		const struct file_list *playlists)
{
	main_win_update_dir_content (&main_win, iface_menu, files, dirs,
			playlists);
	info_win_set_files_time (&info_win, main_win_get_files_time(&main_win),
			main_win_is_time_for_all(&main_win));
	wrefresh (info_win.win);
	wrefresh (main_win.win);

	/* TODO: also display the number of items */
}

/* Update item title and time on all menus where it's present. */
void iface_update_item (const struct plist *plist, const int n)
{
	assert (plist != NULL);

	main_win_update_item (&main_win, plist, n);
	info_win_set_files_time (&info_win, main_win_get_files_time(&main_win),
			main_win_is_time_for_all(&main_win));
	wrefresh (info_win.win);
	wrefresh (main_win.win);
}

/* Chenge the current item in the directory menu to this item. */
void iface_set_curr_item_title (const char *title)
{
	assert (title != NULL);
	
	main_win_set_curr_item_title (&main_win, title);
	wrefresh (main_win.win);
}

/* Set the title for the directory menu. */
void iface_set_dir_title (const char *title)
{
	// TODO
}

/* Get the char code from the user with meta flag set if necessary. */
int iface_get_char ()
{
	int meta;
	int ch = wgetch (main_win.win);
	
	/* Recognize meta sequences */
	if (ch == KEY_ESCAPE
			&& (meta = wgetch(main_win.win))
			!= ERR)
		ch = meta | META_KEY_FLAG;

	return ch;
}

/* Return a non zero value if the help screen is displayed. */
int iface_in_help ()
{
	return 0;
}

/* Return a non zero value if the key is not a real key - KEY_RESIZE. */
int iface_key_is_resize (const int ch)
{
	return ch == KEY_RESIZE;
}

/* Handle a key command for the menu. */
void iface_menu_key (const enum key_cmd cmd)
{
	main_win_menu_cmd (&main_win, cmd);
	wrefresh (main_win.win);
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
	wrefresh (info_win.win);
}

/* Set the total time for the currently played file. */
void iface_set_total_time (const int time)
{
	info_win_set_total_time (&info_win, time);
	wrefresh (info_win.win);
}

/* Set the state (STATE_(PLAY|STOP|PAUSE)). */
void iface_set_state (const int state)
{
	info_win_set_state (&info_win, state);
	wrefresh (info_win.win);
}

/* Set the bitrate (in Kbps). 0 or -1 means no bitrate information. */
void iface_set_bitrate (const int bitrate)
{
	assert (bitrate >= -1);
	
	info_win_set_bitrate (&info_win, bitrate);
	wrefresh (info_win.win);
}

/* Set the rate (in KHz). 0 or -1 means no rate information. */
void iface_set_rate (const int rate)
{
	assert (rate >= -1);
	
	info_win_set_rate (&info_win, rate);
	wrefresh (info_win.win);
}

/* Set the number of channels. */
void iface_set_channels (const int channels)
{
	assert (channels == 1 || channels == 2);
	
	info_win_set_channels (&info_win, channels);
	wrefresh (info_win.win);
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
		info_win_set_option_state (&info_win, "Net", 0);
		wrefresh (info_win.win);
	}
	else if (is_url(file)) {
		info_win_set_option_state (&info_win, "Net", 1);
		wrefresh (info_win.win);
	}

	wrefresh (main_win.win);
}

/* Set the title for the currently played file. */
void iface_set_played_file_title (const char *title)
{
	assert (title != NULL);

	info_win_set_played_title (&info_win, title);
	wrefresh (info_win.win);
}

/* Update timeouts, refresh the screen if needed. This should be called at
 * least once a second. */
void iface_tick ()
{
	info_win_tick (&info_win);
	wrefresh (info_win.win);
}

void iface_set_mixer_value (const int value)
{
	assert (value >= 0 && value <= 100);

	info_win_set_mixer_value (&info_win, value);
	wrefresh (info_win.win);
}

/* Switch to the playlist menu. */
void iface_switch_to_plist ()
{
	main_win_switch_to (&main_win, MENU_PLAYLIST);
	info_win_set_files_time (&info_win, main_win_get_files_time(&main_win),
			main_win_is_time_for_all(&main_win));
	wrefresh (info_win.win);
	wrefresh (main_win.win);
}

/* Switch to the directory menu. */
void iface_switch_to_dir ()
{
	main_win_switch_to (&main_win, MENU_DIR);
	info_win_set_files_time (&info_win, main_win_get_files_time(&main_win),
			main_win_is_time_for_all(&main_win));
	wrefresh (info_win.win);
	wrefresh (main_win.win);
}

/* Add the item from the playlist to the playlist menu. */
void iface_add_to_plist (const struct plist *plist, const int num)
{
	assert (plist != NULL);
	
	main_win_add_to_plist (&main_win, plist, num);
	info_win_set_files_time (&info_win, main_win_get_files_time(&main_win),
			main_win_is_time_for_all(&main_win));
	wrefresh (info_win.win);
	wrefresh (main_win.win);
}

/* Display an error message. */
void iface_error (const char *msg)
{
	info_win_msg (&info_win, msg, 1);
	wrefresh (info_win.win);
}

/* Handle screen resizing. */
void iface_resize ()
{
	check_term_size ();
	endwin ();
	refresh ();
	main_win_resize (&main_win);
	info_win_resize (&info_win);
	wrefresh (main_win.win);
	wrefresh (info_win.win);
}

void iface_refresh ()
{
	wclear (main_win.win);
	wclear (info_win.win);

	main_win_draw (&main_win);
	info_win_draw (&info_win);
	
	wrefresh (main_win.win);
	wrefresh (info_win.win);
}

void iface_update_show_time ()
{
	main_win_update_show_time (&main_win);
	wrefresh (main_win.win);
}

void iface_update_show_format ()
{
	main_win_update_show_format (&main_win);
	wrefresh (main_win.win);
}

void iface_clear_plist ()
{
	main_win_clear_plist (&main_win);
	wrefresh (main_win.win);
}

void iface_del_plist_item (const char *file)
{
	assert (file != NULL);

	main_win_del_plist_item (&main_win, file);
	info_win_set_files_time (&info_win, main_win_get_files_time(&main_win),
			main_win_is_time_for_all(&main_win));
	wrefresh (info_win.win);
	wrefresh (main_win.win);

	/* TODO: display the number of items */
}

void iface_make_entry (const enum entry_type type)
{
	info_win_make_entry (&info_win, type);
	wrefresh (info_win.win);
}

enum entry_type iface_get_entry_type ()
{
	return info_win_get_entry_type (&info_win);
}

int iface_in_entry ()
{
	return info_win_in_entry (&info_win);
}

void iface_entry_handle_key (const int ch)
{
	info_win_entry_handle_key (&info_win, ch);
}

void iface_entry_set_text (const char *text)
{
	assert (text != NULL);

	info_win_entry_set_text (&info_win, text);
	wrefresh (info_win.win);
}

/* Get text from the entry. Returned memory is amlloc()ed. */
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
	info_win_entry_disable (&info_win);
	wrefresh (info_win.win);
}

void iface_message (const char *msg)
{
	assert (msg != NULL);
	
	info_win_msg (&info_win, msg, 0);
	wrefresh (info_win.win);
}

void iface_disable_message ()
{
	info_win_disable_msg (&info_win);
	wrefresh (info_win.win);
}

void iface_plist_set_total_time (const int time, const int for_all_files)
{
	if (iface_in_plist_menu())
		info_win_set_files_time (&info_win, time, for_all_files);
	main_win_set_plist_time (&main_win, time, for_all_files);
	wrefresh (info_win.win);
}
