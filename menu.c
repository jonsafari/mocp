/*
 * MOC - music on console
 * Copyright (C) 2002-2004 Damian Pietras <daper@daper.net>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#define _GNU_SOURCE
#include <stdlib.h>
#include <string.h>
#include <ncurses.h>
#include <assert.h>
#include <ctype.h>
#include "main.h"
#include "menu.h"
#include "files.h"

#ifndef HAVE_STRCASESTR
/* Case insensitive version od strstr(). */
static char *strcasestr (const char *haystack, const char *needle)
{
	char *haystack_i, *needle_i;
	char *c;
	char *res;

	haystack_i = xstrdup (haystack);
	needle_i = xstrdup (needle);

	c = haystack_i;
	while (*c) {
		*c = tolower (*c);
		c++;
	}
		
	c = needle_i;
	while (*c) {
		*c = tolower (*c);
		c++;
	}

	res = strstr (haystack_i, needle_i);
	free (haystack_i);
	free (needle_i);
	return res ? res - haystack_i + (char *)haystack : NULL;
}
#endif

void menu_draw (struct menu *menu)
{
	int i, j;
	int title_width;
	int info_pos = 0;

	assert (menu != NULL);

	if (menu->show_time || menu->show_format) {
		title_width = menu->maxx - 2; /* for brackets */
		if (menu->show_time)
			title_width -= 5; /* 00:00 */
		if (menu->show_format)
			title_width -= 3; /* MP3 */
		if (menu->show_time && menu->show_format)
			title_width--; /* for | */
		info_pos = title_width + 1; /* +1 for the frame */
	}
	else
		title_width = menu->maxx;

	for (i = menu->top; i < menu->nitems && i - menu->top < menu->maxy;
			i++) {
		int title_len;
		
		wmove (menu->win, i - menu->top + 1, 1);
		
		/* Set attributes */
		if (i == menu->selected && i == menu->marked)
			wattrset (menu->win, menu->items[i]->attr_sel_marked);
		else if (i == menu->selected)
			wattrset (menu->win, menu->items[i]->attr_sel);
		else if (i == menu->marked)
			wattrset (menu->win, menu->items[i]->attr_marked);
		else
			wattrset (menu->win, menu->items[i]->attr_normal);
		
		waddnstr (menu->win, menu->items[i]->title, title_width);
		title_len = strlen (menu->items[i]->title);
		
		
		/* Make blank line to the right side of the screen */
		if (i == menu->selected) {
			for (j = title_len + 1; j <= title_width; j++)
				waddch (menu->win, ' ');
		}

		/* Description */
		wattrset (menu->win, menu->info_attr);
		wmove (menu->win, i - menu->top + 1, info_pos);

		if (menu->show_time && menu->show_format
				&& menu->items[i]->time[0])
			wprintw (menu->win, "[%5s|%3s]", menu->items[i]->time,
					menu->items[i]->format);
		else if (menu->show_time && menu->items[i]->time[0])
			wprintw (menu->win, "[%5s]", menu->items[i]->time);
		else if (menu->show_format && menu->items[i]->format[0])
			wprintw (menu->win, "[%3s]", menu->items[i]->format);
	}
}

/* menu_items must be malloc()ed memory! */
struct menu *menu_new (WINDOW *win, struct menu_item **items, const int nitems)
{
	struct menu *menu;

	assert (items != NULL);
	assert (win != NULL);
	
	menu = (struct menu *)xmalloc (sizeof(struct menu));

	menu->win = win;
	menu->items = items;
	menu->top = 0;
	menu->selected = 0;
	getmaxyx (win, menu->maxy, menu->maxx);
	menu->maxx -= 2; /* The window contains border */
	menu->maxy--; /* Border without bottom line */
	menu->nitems = nitems;
	menu->marked = -1;
	menu->show_time = 0;
	menu->show_format = 0;
	menu->info_attr = A_NORMAL;

	return menu;
}

void menu_update_size (struct menu *menu, WINDOW *win)
{
	assert (menu != NULL);
	
	getmaxyx (win, menu->maxy, menu->maxx);
	menu->maxx -= 2; /* The window contains border */
	menu->maxy--; /* Border without bottom line */
}

struct menu_item *menu_newitem (char *title, const int plist_pos,
		const enum file_type type, const char *file)
{
	struct menu_item *item;

	assert (title != NULL);
	
	item = (struct menu_item *)xmalloc (sizeof(struct menu_item));

	item->title = xstrdup (title);
	item->plist_pos = plist_pos;
	item->attr_normal = A_NORMAL;
	item->attr_sel = A_NORMAL;
	item->attr_sel_marked = A_NORMAL;
	item->type = type;
	item->time[0] = 0;
	item->format[0] = 0;
	item->file = xstrdup (file);

	return item;
}

void menu_free (struct menu *menu)
{
	int i;

	assert (menu != NULL);

	/* Free items */
	for (i = 0; i < menu->nitems; i++) {
		free (menu->items[i]->title);
		if (menu->items[i]->file)
			free (menu->items[i]->file);
		free (menu->items[i]);
	}

	free (menu->items);
	free (menu);
}

void menu_driver (struct menu *menu, enum menu_request req)
{
	assert (menu != NULL);
	
	if (req == REQ_DOWN && menu->selected < menu->nitems - 1) {
		menu->selected++;
		if (menu->selected > menu->top + menu->maxy - 1) {
			menu->top = menu->selected - (menu->maxy - 1) / 2;
			if (menu->top > menu->nitems - menu->maxy)
				menu->top = menu->nitems - menu->maxy;
		}
	}
	else if (req == REQ_UP && menu->selected > 0) {
		menu->selected--;
		if (menu->top > menu->selected) {
			menu->top = menu->selected - (menu->maxy - 1) / 2;
			if (menu->top < 0)
				menu->top = 0;
		}
	}
	else if (req == REQ_PGDOWN && menu->selected < menu->nitems - 1) {
		if (menu->selected + menu->maxy - 1 < menu->nitems - 1) {
			menu->selected += menu->maxy - 1;
			menu->top += menu->maxy - 1;
			if (menu->top > menu->nitems - menu->maxy)
				menu->top = menu->nitems - menu->maxy;
		}
		else {
			menu->selected = menu->nitems - 1;
			menu->top = menu->nitems - menu->maxy;
		}

		if (menu->top < 0)
			menu->top = 0;

	}
	else if (req == REQ_PGUP && menu->selected > 0) {
		if (menu->selected - menu->maxy + 1 > 0) {
			menu->selected -= menu->maxy - 1;
			menu->top -= menu->maxy - 1;
			if (menu->top < 0)
				menu->top = 0;
		}
		else {
			menu->selected = 0;
			menu->top = 0;
		}
	}
	else if (req == REQ_TOP) {
		menu->selected = 0;
		menu->top = 0;
	}
	else if (req == REQ_BOTTOM) {
		menu->selected = menu->nitems - 1;
		menu->top = menu->selected - menu->maxy + 1;
		if (menu->top < 0)
			menu->top = 0;
	}
}

struct menu_item *menu_curritem (struct menu *menu)
{
	assert (menu != NULL);
	
	return menu->items[menu->selected];
}

/* Make this item selected */
void menu_setcurritem (struct menu *menu, int num)
{
	assert (menu != NULL);
	
	if (num < menu->nitems)
		menu->selected = num;
	else
		menu->selected = menu->nitems - 1;

	if (menu->selected < menu->top)
		menu->top = menu->selected;
	else if (menu->selected > menu->top + menu->maxy - 1)
		menu->top = menu->selected - menu->maxy + 1;	
}

/* Make the item with this title selected. */
void menu_setcurritem_title (struct menu *menu, const char *title)
{
	int i;

	/* Find it */
	for (i = 0; i < menu->nitems; i++)
		if (!strcmp(menu->items[i]->title, title))
			break;

	/* Not found? */
	if (i == menu->nitems)
		return;

	menu_setcurritem (menu, i);
}

void set_menu_state (struct menu *menu, int selected, int top)
{
	assert (menu != NULL);
	
	if (selected >= menu->nitems)
		menu->selected = menu->nitems - 1;
	else
		menu->selected = selected;

	if (top > menu->nitems - menu->maxy + 1)
		menu->top = menu->nitems - menu->maxy;
	else
		menu->top = top;

	if (menu->top < 0)
		menu->top = 0;
}

void menu_unmark_item (struct menu *menu)
{
	menu->marked = -1;
}

/* Find the item for the given plist_num */
static int find_item_plist (const struct menu *menu, const int plist_num)
{
	int i;

	for (i = 0; i < menu->nitems; i++)
		if (menu->items[i]->plist_pos == plist_num)
			return i;
	return -1;
}

/* Mark the item that is plist_item on the playlist. */
void menu_mark_plist_item (struct menu *menu, const int plist_item)
{
	int i;

	if (menu->marked != -1)
		menu_unmark_item (menu);

	if ((i = find_item_plist(menu, plist_item)) != -1)
		menu->marked = i;
}

/* Set the top item to num. */
void menu_set_top_item (struct menu *menu, const int num)
{
	assert (menu != NULL);
	
	if (num > menu->nitems - menu->maxy) {
		menu->top = menu->nitems - menu->maxy;
		if (menu->top < 0)
			menu->top = 0;
	}
	else
		menu->top = num;
}

/* Find an item that title's contain the string, begin searching from current.
 * Return the item index or -1 if not found. */
int menu_find_pattern_next (struct menu *menu, const char *pattern,
		const int current)
{
	int i;
	
	assert (menu != NULL);
	assert (pattern != NULL);
	assert (current >= 0 && current < menu->nitems);

	for (i = current; i < menu->nitems; i++)
		if (strcasestr(menu->items[i]->title, pattern))
			return i;

	/* Not found? */
	for (i = 0; i < current; i++)
		if (strcasestr(menu->items[i]->title, pattern))
			return i;

	return -1;
}

int menu_get_selected (const struct menu *menu)
{
	assert (menu != NULL);
	
	return menu->selected;
}

/* Get index of the next item after selected. If last item is selected, return
 * 0. */
int menu_next_turn (const struct menu *menu)
{
	assert (menu != NULL);

	if (menu->selected < menu->nitems - 1)
		return menu->selected + 1;
	else
		return 0;
}

void menu_item_set_attr_normal (struct menu_item *item, const int attr)
{
	item->attr_normal = attr;
}

void menu_item_set_attr_sel (struct menu_item *item, const int attr)
{
	item->attr_sel = attr;
}

void menu_item_set_attr_sel_marked (struct menu_item *item, const int attr)
{
	item->attr_sel_marked = attr;
}

void menu_item_set_attr_marked (struct menu_item *item, const int attr)
{
	item->attr_marked = attr;
}

void menu_item_set_time (struct menu_item *item, const char *time)
{
	item->time[sizeof(item->time)-1] = 0;
	strncpy (item->time, time, sizeof(item->time));
	assert (item->time[sizeof(item->time)-1] == 0);
}

void menu_item_set_time_plist (struct menu *menu, const int plist_num,
		const char *time)
{
	int i;
	
	assert (menu != NULL);
	
	i = find_item_plist (menu, plist_num);
	assert (i != -1);
	
	menu_item_set_time (menu->items[i], time);
}

void menu_item_set_format (struct menu_item *item, const char *format)
{
	assert (item != NULL);
	assert (format != NULL);

	item->format[sizeof(item->format)-1] = 0;
	strncpy (item->format, format, sizeof(item->format));
	assert (item->format[sizeof(item->format)-1] == 0);
}

void menu_set_show_time (struct menu *menu, const int t)
{
	assert (menu != NULL);
	menu->show_time = t;
}

void menu_set_show_format (struct menu *menu, const int t)
{
	assert (menu != NULL);
	menu->show_format = t;
}

void menu_set_info_attr (struct menu *menu, const int attr)
{
	assert (menu != NULL);
	menu->info_attr = attr;
}

