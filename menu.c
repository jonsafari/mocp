/*
 * MOC - music on console
 * Copyright (C) 2002-2005 Damian Pietras <daper@daper.net>
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

#ifdef HAVE_NCURSES_H
# include <ncurses.h>
#elif HAVE_CURSES_H
# include <curses.h>
#endif

#include <assert.h>
#include <ctype.h>
#include "main.h"
#include "menu.h"
#include "files.h"

/* Initial number of items to allocate in a new menu. */
#define INIT_NUM_ITEMS	64

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

/* Draw menu item on a given position from the top of the menu. */
static void draw_item (const struct menu *menu, const int num, const int pos,
		const int item_info_pos, const int title_space)
{
	int title_len;
	const struct menu_item *item = &menu->items[num];
	int j;

	wmove (menu->win, pos, menu->posx);
	
	/* Set attributes */
	if (num == menu->selected && num == menu->marked)
		wattrset (menu->win, item->attr_sel_marked);
	else if (num == menu->selected)
		wattrset (menu->win, item->attr_sel);
	else if (num == menu->marked)
		wattrset (menu->win, item->attr_marked);
	else
		wattrset (menu->win, item->attr_normal);
	
	waddnstr (menu->win, item->title, title_space);
	title_len = strlen (item->title);
	
	/* Make blank line to the right side of the screen */
	if (num == menu->selected)
		for (j = title_len + 1; j <= title_space; j++)
			waddch (menu->win, ' ');

	/* Description */
	wattrset (menu->win, menu->info_attr);
	wmove (menu->win, pos, item_info_pos);

	if (menu->show_time && menu->show_format
			&& (*item->time || *item->format))
		wprintw (menu->win, "[%5s|%3s]",
				item->time ? item->time : "     ",
				item->format);
	else if (menu->show_time && item->time[0])
		wprintw (menu->win, "[%5s]", item->time);
	else if (menu->show_format && item->format[0])
		wprintw (menu->win, "[%3s]", item->format);
}

void menu_draw (struct menu *menu)
{
	int i;
	int title_width;
	int info_pos;

	assert (menu != NULL);

	if (menu->show_time || menu->show_format) {
		title_width = menu->width - 2; /* -2 for brackets */
		if (menu->show_time)
			title_width -= 5; /* 00:00 */
		if (menu->show_format)
			title_width -= 3; /* MP3 */
		if (menu->show_time && menu->show_format)
			title_width--; /* for | */
		info_pos = title_width;
	}
	else {
		title_width = menu->width;
		info_pos = 0;
	}

	for (i = menu->top; i < menu->nitems && i - menu->top < menu->height;
			i++)
		draw_item (menu, i, i - menu->top + menu->posy,
				menu->posx + info_pos,
				title_width);
}

/* menu_items must be malloc()ed memory! */
struct menu *menu_new (WINDOW *win, const int posx, const int posy,
		const int width, const int height)
{
	struct menu *menu;

	assert (win != NULL);
	assert (posx >= 0);
	assert (posy >= 0);
	assert (width > 0);
	assert (height > 0);

	menu = (struct menu *)xmalloc (sizeof(struct menu));

	menu->win = win;
	menu->items = (struct menu_item *)xmalloc(
			sizeof(struct menu_item) * INIT_NUM_ITEMS);
	menu->allocated = INIT_NUM_ITEMS;
	menu->nitems = 0;
	menu->top = 0;
	menu->selected = 0;
	menu->posx = posx;
	menu->posy = posy;
	menu->width = width;
	menu->height = height;
	menu->marked = -1;
	menu->show_time = 0;
	menu->show_format = 0;
	menu->info_attr = A_NORMAL;

	return menu;
}

int menu_add (struct menu *menu, char *title, const enum file_type type,
		const char *file)
{
	assert (menu != NULL);
	assert (title != NULL);

	if (menu->allocated == menu->nitems) {
		menu->allocated *= 2;
		menu->items = (struct menu_item *)xrealloc (menu->items,
				sizeof(struct menu_item) * menu->allocated);
	}

	menu->items[menu->nitems].title = xstrdup (title);
	menu->items[menu->nitems].type = type;
	menu->items[menu->nitems].file = xstrdup (file);

	menu->items[menu->nitems].attr_normal = A_NORMAL;
	menu->items[menu->nitems].attr_sel = A_NORMAL;
	menu->items[menu->nitems].attr_marked = A_NORMAL;
	menu->items[menu->nitems].attr_sel_marked = A_NORMAL;
	menu->items[menu->nitems].time[0] = 0;
	menu->items[menu->nitems].format[0] = 0;

	return menu->nitems++;
}

static int menu_add_from_item (struct menu *menu, const struct menu_item *item)
{
	assert (menu != NULL);
	assert (item != NULL);

	if (menu->allocated == menu->nitems) {
		menu->allocated *= 2;
		menu->items = (struct menu_item *)xrealloc (menu->items,
				sizeof(struct menu_item) * menu->allocated);
	}

	menu->items[menu->nitems].title = xstrdup (item->title);
	menu->items[menu->nitems].type = item->type;
	menu->items[menu->nitems].file = xstrdup (item->file);

	menu->items[menu->nitems].attr_normal = item->attr_normal;
	menu->items[menu->nitems].attr_sel = item->attr_sel;
	menu->items[menu->nitems].attr_marked = item->attr_marked;
	menu->items[menu->nitems].attr_sel_marked = item->attr_sel_marked;
	strcpy (menu->items[menu->nitems].time, item->time);
	strcpy (menu->items[menu->nitems].format, item->format);

	return menu->nitems++;
}

void menu_update_size (struct menu *menu, const int posx, const int posy,
		const int width, const int height)
{
	assert (menu != NULL);
	assert (posx >= 0);
	assert (posy >= 0);
	assert (width > 0);
	assert (height > 0);

	menu->posx = posx;
	menu->posy = posy;
	menu->width = width;
	menu->height = height;
}

void menu_free (struct menu *menu)
{
	int i;

	assert (menu != NULL);

	/* Free items */
	for (i = 0; i < menu->nitems; i++) {
		free (menu->items[i].title);
		if (menu->items[i].file)
			free (menu->items[i].file);
	}

	free (menu->items);
	free (menu);
}

void menu_driver (struct menu *menu, enum menu_request req)
{
	assert (menu != NULL);
	
	if (req == REQ_DOWN && menu->selected < menu->nitems - 1) {
		menu->selected++;
		if (menu->selected >= menu->top + menu->height) {
			menu->top = menu->selected - menu->height / 2;
			if (menu->top > menu->nitems - menu->height)
				menu->top = menu->nitems - menu->height;
		}
	}
	else if (req == REQ_UP && menu->selected > 0) {
		menu->selected--;
		if (menu->top > menu->selected) {
			menu->top = menu->selected - menu->height / 2;
			if (menu->top < 0)
				menu->top = 0;
		}
	}
	else if (req == REQ_PGDOWN && menu->selected < menu->nitems - 1) {
		if (menu->selected + menu->height - 1 < menu->nitems - 1) {
			menu->selected += menu->height - 1;
			menu->top += menu->height - 1;
			if (menu->top > menu->nitems - menu->height)
				menu->top = menu->nitems - menu->height;
		}
		else {
			menu->selected = menu->nitems - 1;
			menu->top = menu->nitems - menu->height;
		}

		if (menu->top < 0)
			menu->top = 0;

	}
	else if (req == REQ_PGUP && menu->selected > 0) {
		if (menu->selected - menu->height + 1 > 0) {
			menu->selected -= menu->height - 1;
			menu->top -= menu->height - 1;
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
		menu->top = menu->selected - menu->height + 1;
		if (menu->top < 0)
			menu->top = 0;
	}
}

/* Return the index of the currently selected item. */
int menu_curritem (struct menu *menu)
{
	assert (menu != NULL);
	
	return menu->selected;
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
	else if (menu->selected >= menu->top + menu->height)
		menu->top = menu->selected - menu->height + 1;
}

/* Make the item with this title selected. */
void menu_setcurritem_title (struct menu *menu, const char *title)
{
	int i;

	/* Find it */
	for (i = 0; i < menu->nitems; i++)
		if (!strcmp(menu->items[i].title, title))
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

	if (top > menu->nitems - menu->height)
		menu->top = menu->nitems - menu->height;
	else
		menu->top = top;

	if (menu->top < 0)
		menu->top = 0;
}

void menu_unmark_item (struct menu *menu)
{
	assert (menu != NULL);
	menu->marked = -1;
}

/* Set the top item to num. */
void menu_set_top_item (struct menu *menu, const int num)
{
	assert (menu != NULL);
	
	if (num > menu->nitems - menu->height) {
		menu->top = menu->nitems - menu->height;
		if (menu->top < 0)
			menu->top = 0;
	}
	else
		menu->top = num;
}

/* Make a new menu from elements matching pattern. */
struct menu *menu_filter_pattern (struct menu *menu, const char *pattern)
{
	int i;
	struct menu *new;

	new = menu_new (menu->win, menu->posx, menu->posy, menu->width,
			menu->height);
	new->show_time = menu->show_time;
	new->show_format = menu->show_format;
	new->info_attr = menu->info_attr;
	
	assert (menu != NULL);
	assert (pattern != NULL);

	for (i = 0; i < menu->nitems; i++) {
		struct menu_item *item = &menu->items[i];
		
		if ((item->type == F_SOUND || item->type == F_URL)
				&& strcasestr(menu->items[i].title, pattern)) {
			int added = menu_add_from_item (new, item);
			
			if (menu->marked == i)
				new->marked = added;
		}
	}

	return new;
}

void menu_item_set_attr_normal (struct menu *menu, const int num,
		const int attr)
{
	assert (menu != NULL);
	assert (num >= 0 && num < menu->nitems);
	
	menu->items[num].attr_normal = attr;
}

void menu_item_set_attr_sel (struct menu *menu, const int num, const int attr)
{
	assert (menu != NULL);
	assert (num >= 0 && num < menu->nitems);
	
	menu->items[num].attr_sel = attr;
}

void menu_item_set_attr_sel_marked (struct menu *menu, const int num,
		const int attr)
{
	assert (menu != NULL);
	assert (num >= 0 && num < menu->nitems);
	
	menu->items[num].attr_sel_marked = attr;
}

void menu_item_set_attr_marked (struct menu *menu, const int num,
		const int attr)
{
	assert (menu != NULL);
	assert (num >= 0 && num < menu->nitems);
	
	menu->items[num].attr_marked = attr;
}

void menu_item_set_time (struct menu *menu, const int num, const char *time)
{
	assert (menu != NULL);
	assert (num >= 0 && num < menu->nitems);
	
	menu->items[num].time[sizeof(menu->items[num].time)-1] = 0;
	strncpy (menu->items[num].time, time, sizeof(menu->items[num].time));
	assert (menu->items[num].time[sizeof(menu->items[num].time)-1] == 0);
}

void menu_item_set_format (struct menu *menu, const int num,
		const char *format)
{
	assert (menu != NULL);
	assert (num >= 0 && num < menu->nitems);

	menu->items[num].format[sizeof(menu->items[num].format)-1] = 0;
	strncpy (menu->items[num].format, format,
			sizeof(menu->items[num].format));
	assert (menu->items[num].format[sizeof(menu->items[num].format)-1]
			== 0);
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

enum file_type menu_item_get_type (const struct menu *menu, const int num)
{
	assert (menu != NULL);
	assert (num >= 0 && num < menu->nitems);

	return menu->items[num].type;
}

char *menu_item_get_file (const struct menu *menu, const int num)
{
	assert (menu != NULL);
	assert (num >= 0 && num < menu->nitems);

	return xstrdup (menu->items[num].file);
}

void menu_item_set_title (struct menu *menu, const int num, const char *title)
{
	assert (menu != NULL);
	assert (num >= 0 && num < menu->nitems);

	if (menu->items[num].title)
		free (menu->items[num].title);
	menu->items[num].title = xstrdup (title);
}

int menu_nitems (const struct menu *menu)
{
	assert (menu != NULL);

	return menu->nitems;
}

int menu_find (const struct menu *menu, const char *fname)
{
	/* TODO: linear search is too slow */
	
	int i;
	
	assert (menu != NULL);
	assert (fname != NULL);

	for (i = 0; i < menu->nitems; i++)
		if (!strcmp(menu->items[i].file, fname))
			return i;

	return -1;
}

void menu_mark_item (struct menu *menu, const char *file)
{
	int item;

	assert (menu != NULL);
	assert (file != NULL);

	item = menu_find (menu, file);
	if (item != -1)
		menu->marked = item;
}
