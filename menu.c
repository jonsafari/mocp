/*
 * MOC - music on console
 * Copyright (C) 2002 - 2006 Damian Pietras <daper@daper.net>
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

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>

#include "common.h"
#include "options.h"
#include "menu.h"
#include "files.h"
#include "rbtree.h"
#include "utf8.h"

/* Draw menu item on a given position from the top of the menu. */
static void draw_item (const struct menu *menu, const struct menu_item *mi,
		const int pos, const int item_info_pos, int title_space,
		const int number_space, const int draw_selected)
{
	int title_width, queue_pos_len = 0;
	int ix, x;
	int y ATTR_UNUSED;		/* OpenBSD flags this as unused. */
	char buf[32];

	assert (menu != NULL);
	assert (mi != NULL);
	assert (pos >= 0);
	assert (item_info_pos > menu->posx
			|| (!menu->show_time && !menu->show_format));
	assert (title_space > 0);
	assert (number_space == 0 || number_space >= 2);

	wmove (menu->win, pos, menu->posx);

	if (number_space) {
		if (draw_selected && mi == menu->selected && mi == menu->marked)
			wattrset (menu->win, menu->info_attr_sel_marked);
		else if (draw_selected && mi == menu->selected)
			wattrset (menu->win, menu->info_attr_sel);
		else if (mi == menu->marked)
			wattrset (menu->win, menu->info_attr_marked);
		else
			wattrset (menu->win, menu->info_attr_normal);
		xwprintw (menu->win, "%*d ", number_space - 1, mi->num + 1);
	}

	/* Set attributes */
	if (draw_selected && mi == menu->selected && mi == menu->marked)
		wattrset (menu->win, mi->attr_sel_marked);
	else if (draw_selected && mi == menu->selected)
		wattrset (menu->win, mi->attr_sel);
	else if (mi == menu->marked)
		wattrset (menu->win, mi->attr_marked);
	else
		wattrset (menu->win, mi->attr_normal);

	/* Compute the length of the queue position if nonzero */
	if (mi->queue_pos) {
		sprintf (buf, "%d", mi->queue_pos);
		queue_pos_len = strlen(buf) + 2;
		title_space -= queue_pos_len;
	}

	title_width = strwidth (mi->title);

	getyx (menu->win, y, x);
	if (title_width <= title_space || mi->align == MENU_ALIGN_LEFT)
		xwaddnstr (menu->win, mi->title, title_space);
	else {
		char *ptr;

		ptr = xstrtail (mi->title, title_space);
		xwaddstr (menu->win, ptr);
		free (ptr);
	}

	/* Fill the remainder of the title field with spaces. */
	if (mi == menu->selected) {
		getyx (menu->win, y, ix);
		while (ix < x + title_space) {
			waddch (menu->win, ' ');
			ix += 1;
		}
	}

	/* Description. */
	if (draw_selected && mi == menu->selected && mi == menu->marked)
		wattrset (menu->win, menu->info_attr_sel_marked);
	else if (draw_selected && mi == menu->selected)
		wattrset (menu->win, menu->info_attr_sel);
	else if (mi == menu->marked)
		wattrset (menu->win, menu->info_attr_marked);
	else
		wattrset (menu->win, menu->info_attr_normal);
	wmove (menu->win, pos, item_info_pos - queue_pos_len);

	/* Position in queue. */
	if (mi->queue_pos) {
		xwaddstr (menu->win, "[");
		xwaddstr (menu->win, buf);
		xwaddstr (menu->win, "]");
	}

	if (menu->show_time && menu->show_format
			&& (*mi->time || *mi->format))
		xwprintw (menu->win, "[%5s|%3s]",
				mi->time ? mi->time : "	 ",
				mi->format);
	else if (menu->show_time && mi->time[0])
		xwprintw (menu->win, "[%5s]", mi->time);
	else if (menu->show_format && mi->format[0])
		xwprintw (menu->win, "[%3s]", mi->format);
}

void menu_draw (const struct menu *menu, const int active)
{
	struct menu_item *mi;
	int title_width;
	int info_pos;
	int number_space = 0;

	assert (menu != NULL);

	if (menu->number_items) {
		int count = menu->nitems / 10;

		number_space = 2; /* begin from 1 digit and a space char */
		while (count) {
			count /= 10;
			number_space++;
		}
	}
	else
		number_space = 0;

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
		info_pos = title_width;
	}

	title_width -= number_space;

	for (mi = menu->top; mi && mi->num - menu->top->num < menu->height;
			mi = mi->next)
		draw_item (menu, mi, mi->num - menu->top->num + menu->posy,
				menu->posx + info_pos, title_width,
				number_space, active);
}

/* Move the cursor to the selected file. */
void menu_set_cursor (const struct menu *m)
{
	assert (m != NULL);

	if (m->selected)
		wmove (m->win, m->selected->num - m->top->num + m->posy, m->posx);
}

static int rb_compare (const void *a, const void *b,
                       const void *unused ATTR_UNUSED)
{
	struct menu_item *mia = (struct menu_item *)a;
	struct menu_item *mib = (struct menu_item *)b;

	return strcmp (mia->file, mib->file);
}

static int rb_fname_compare (const void *key, const void *data,
                             const void *unused ATTR_UNUSED)
{
	const char *fname = (const char *)key;
	const struct menu_item *mi = (const struct menu_item *)data;

	return strcmp (fname, mi->file);
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
	menu->items = NULL;
	menu->nitems = 0;
	menu->top = NULL;
	menu->last = NULL;
	menu->selected = NULL;
	menu->posx = posx;
	menu->posy = posy;
	menu->width = width;
	menu->height = height;
	menu->marked = NULL;
	menu->show_time = 0;
	menu->show_format = false;
	menu->info_attr_normal = A_NORMAL;
	menu->info_attr_sel = A_NORMAL;
	menu->info_attr_marked = A_NORMAL;
	menu->info_attr_sel_marked = A_NORMAL;
	menu->number_items = 0;

	menu->search_tree = rb_tree_new (rb_compare, rb_fname_compare, NULL);

	return menu;
}

struct menu_item *menu_add (struct menu *menu, const char *title,
		const enum file_type type, const char *file)
{
	struct menu_item *mi;

	assert (menu != NULL);
	assert (title != NULL);

	mi = (struct menu_item *)xmalloc (sizeof(struct menu_item));

	mi->title = xstrdup (title);
	mi->type = type;
	mi->file = xstrdup (file);
	mi->num = menu->nitems;

	mi->attr_normal = A_NORMAL;
	mi->attr_sel = A_NORMAL;
	mi->attr_marked = A_NORMAL;
	mi->attr_sel_marked = A_NORMAL;
	mi->align = MENU_ALIGN_LEFT;

	mi->time[0] = 0;
	mi->format[0] = 0;
	mi->queue_pos = 0;

	mi->next = NULL;
	mi->prev = menu->last;
	if (menu->last)
		menu->last->next = mi;

	if (!menu->items)
		menu->items = mi;
	if (!menu->top)
		menu->top = menu->items;
	if (!menu->selected)
		menu->selected = menu->items;

	if (file)
		rb_insert (menu->search_tree, (void *)mi);

	menu->last = mi;
	menu->nitems++;

	return mi;
}

static struct menu_item *menu_add_from_item (struct menu *menu,
		const struct menu_item *mi)
{
	struct menu_item *new;

	assert (menu != NULL);
	assert (mi != NULL);

	new = menu_add (menu, mi->title, mi->type, mi->file);

	new->attr_normal = mi->attr_normal;
	new->attr_sel = mi->attr_sel;
	new->attr_marked = mi->attr_marked;
	new->attr_sel_marked = mi->attr_sel_marked;

	strncpy(new->time, mi->time, FILE_TIME_STR_SZ);
	strncpy(new->format, mi->format, FILE_FORMAT_SZ);

	return new;
}

static struct menu_item *get_item_relative (struct menu_item *mi,
		int to_move)
{
	assert (mi != NULL);

	while (to_move) {
		struct menu_item *prev = mi;

		if (to_move > 0) {
			mi = mi->next;
			to_move--;
		}
		else {
			mi = mi->prev;
			to_move++;
		}

		if (!mi) {
			mi = prev;
			break;
		}
	}

	return mi;
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

	if (menu->selected && menu->top
			&& menu->selected->num >= menu->top->num + menu->height)
		menu->selected = get_item_relative (menu->top,
				menu->height - 1);
}

static void menu_item_free (struct menu_item *mi)
{
	assert (mi != NULL);
	assert (mi->title != NULL);

	free (mi->title);
	if (mi->file)
		free (mi->file);

	free (mi);
}

void menu_free (struct menu *menu)
{
	struct menu_item *mi;

	assert (menu != NULL);

	mi = menu->items;
	while (mi) {
		struct menu_item *next = mi->next;

		menu_item_free (mi);
		mi = next;
	}

	rb_tree_free (menu->search_tree);

	free (menu);
}

void menu_driver (struct menu *menu, const enum menu_request req)
{
	assert (menu != NULL);

	if (menu->nitems == 0)
		return;

	if (req == REQ_DOWN && menu->selected->next) {
		menu->selected = menu->selected->next;
		if (menu->selected->num >= menu->top->num + menu->height) {
			menu->top = get_item_relative (menu->selected,
					-menu->height / 2);
			if (menu->top->num > menu->nitems - menu->height)
				menu->top = get_item_relative (menu->last,
						-menu->height + 1);
		}
	}
	else if (req == REQ_UP && menu->selected->prev) {
		menu->selected = menu->selected->prev;
		if (menu->top->num > menu->selected->num)
			menu->top = get_item_relative (menu->selected,
					-menu->height / 2);
	}
	else if (req == REQ_PGDOWN && menu->selected->num < menu->nitems - 1) {
		if (menu->selected->num + menu->height - 1 < menu->nitems - 1) {
			menu->selected = get_item_relative (menu->selected,
					menu->height - 1);
			menu->top = get_item_relative (menu->top,
					menu->height - 1);
			if (menu->top->num > menu->nitems - menu->height)
				menu->top = get_item_relative (menu->last,
						-menu->height + 1);
		}
		else {
			menu->selected = menu->last;
			menu->top = get_item_relative (menu->last,
					-menu->height + 1);
		}
	}
	else if (req == REQ_PGUP && menu->selected->prev) {
		if (menu->selected->num - menu->height + 1 > 0) {
			menu->selected = get_item_relative (menu->selected,
					-menu->height + 1);
			menu->top = get_item_relative (menu->top,
					-menu->height + 1);
		}
		else {
			menu->selected = menu->items;
			menu->top = menu->items;
		}
	}
	else if (req == REQ_TOP) {
		menu->selected = menu->items;
		menu->top = menu->items;
	}
	else if (req == REQ_BOTTOM) {
		menu->selected = menu->last;
		menu->top = get_item_relative (menu->selected,
				-menu->height + 1);
	}
}

/* Return the index of the currently selected item. */
struct menu_item *menu_curritem (struct menu *menu)
{
	assert (menu != NULL);

	return menu->selected;
}

static void make_item_visible (struct menu *menu, struct menu_item *mi)
{
	assert (menu != NULL);
	assert (mi != NULL);

	if (mi->num < menu->top->num || mi->num >= menu->top->num + menu->height) {
		menu->top = get_item_relative(mi, -menu->height/2);

		if (menu->top->num > menu->nitems - menu->height)
			menu->top = get_item_relative (menu->last,
					-menu->height + 1);
	}

	if (menu->selected) {
		if (menu->selected->num < menu->top->num ||
				menu->selected->num >= menu->top->num + menu->height)
			menu->selected = mi;
	}
}

/* Make this item selected */
static void menu_setcurritem (struct menu *menu, struct menu_item *mi)
{
	assert (menu != NULL);
	assert (mi != NULL);

	menu->selected = mi;
	make_item_visible (menu, mi);
}

/* Make the item with this title selected. */
void menu_setcurritem_title (struct menu *menu, const char *title)
{
	struct menu_item *mi;

	/* Find it */
	for (mi = menu->top; mi; mi = mi->next)
		if (!strcmp(mi->title, title))
			break;

	if (mi)
		menu_setcurritem (menu, mi);
}

static struct menu_item *menu_find_by_position (struct menu *menu,
		const int num)
{
	struct menu_item *mi;

	assert (menu != NULL);

	mi = menu->top;
	while (mi && mi->num != num)
		mi = mi->next;

	return mi;
}

void menu_set_state (struct menu *menu, const struct menu_state *st)
{
	assert (menu != NULL);

	if (!(menu->selected = menu_find_by_position(menu, st->selected_item)))
		menu->selected = menu->last;

	if (!(menu->top = menu_find_by_position(menu, st->top_item)))
		menu->top = get_item_relative (menu->last, menu->height + 1);
}

void menu_set_items_numbering (struct menu *menu, const int number)
{
	assert (menu != NULL);

	menu->number_items = number;
}

void menu_get_state (const struct menu *menu, struct menu_state *st)
{
	assert (menu != NULL);

	st->top_item = menu->top ? menu->top->num : -1;
	st->selected_item = menu->selected ? menu->selected->num : -1;
}

void menu_unmark_item (struct menu *menu)
{
	assert (menu != NULL);
	menu->marked = NULL;
}

/* Make a new menu from elements matching pattern. */
struct menu *menu_filter_pattern (const struct menu *menu, const char *pattern)
{
	struct menu *new;
	const struct menu_item *mi;

	assert (menu != NULL);
	assert (pattern != NULL);

	new = menu_new (menu->win, menu->posx, menu->posy, menu->width,
			menu->height);
	menu_set_show_time (new, menu->show_time);
	menu_set_show_format (new, menu->show_format);
	menu_set_info_attr_normal (new, menu->info_attr_normal);
	menu_set_info_attr_sel (new, menu->info_attr_sel);
	menu_set_info_attr_marked (new, menu->info_attr_marked);
	menu_set_info_attr_sel_marked (new, menu->info_attr_sel_marked);

	for (mi = menu->items; mi; mi = mi->next)
		if (strcasestr(mi->title, pattern))
			menu_add_from_item (new, mi);

	if (menu->marked)
		menu_mark_item (new, menu->marked->file);

	return new;
}

void menu_item_set_attr_normal (struct menu_item *mi, const int attr)
{
	assert (mi != NULL);

	mi->attr_normal = attr;
}

void menu_item_set_attr_sel (struct menu_item *mi, const int attr)
{
	assert (mi != NULL);

	mi->attr_sel = attr;
}

void menu_item_set_attr_sel_marked (struct menu_item *mi, const int attr)
{
	assert (mi != NULL);

	mi->attr_sel_marked = attr;
}

void menu_item_set_attr_marked (struct menu_item *mi, const int attr)
{
	assert (mi != NULL);

	mi->attr_marked = attr;
}

void menu_item_set_time (struct menu_item *mi, const char *time)
{
	assert (mi != NULL);

	mi->time[sizeof(mi->time)-1] = 0;
	strncpy (mi->time, time, sizeof(mi->time));
	assert (mi->time[sizeof(mi->time)-1] == 0);
}

void menu_item_set_format (struct menu_item *mi, const char *format)
{
	assert (mi != NULL);
	assert (format != NULL);

	mi->format[sizeof(mi->format)-1] = 0;
	strncpy (mi->format, format,
			sizeof(mi->format));
	assert (mi->format[sizeof(mi->format)-1]
			== 0);
}

void menu_item_set_queue_pos (struct menu_item *mi, const int pos)
{
	assert (mi != NULL);

	mi->queue_pos = pos;
}

void menu_set_show_time (struct menu *menu, const int t)
{
	assert (menu != NULL);

	menu->show_time = t;
}

void menu_set_show_format (struct menu *menu, const bool t)
{
	assert (menu != NULL);

	menu->show_format = t;
}

void menu_set_info_attr_normal (struct menu *menu, const int attr)
{
	assert (menu != NULL);

	menu->info_attr_normal = attr;
}

void menu_set_info_attr_sel (struct menu *menu, const int attr)
{
	assert (menu != NULL);

	menu->info_attr_sel = attr;
}

void menu_set_info_attr_marked (struct menu *menu, const int attr)
{
	assert (menu != NULL);

	menu->info_attr_marked = attr;
}

void menu_set_info_attr_sel_marked (struct menu *menu, const int attr)
{
	assert (menu != NULL);

	menu->info_attr_sel_marked = attr;
}

enum file_type menu_item_get_type (const struct menu_item *mi)
{
	assert (mi != NULL);

	return mi->type;
}

char *menu_item_get_file (const struct menu_item *mi)
{
	assert (mi != NULL);

	return xstrdup (mi->file);
}

void menu_item_set_title (struct menu_item *mi, const char *title)
{
	assert (mi != NULL);

	if (mi->title)
		free (mi->title);
	mi->title = xstrdup (title);
}

int menu_nitems (const struct menu *menu)
{
	assert (menu != NULL);

	return menu->nitems;
}

struct menu_item *menu_find (struct menu *menu, const char *fname)
{
	struct rb_node *x;

	assert (menu != NULL);
	assert (fname != NULL);

	x = rb_search (menu->search_tree, fname);
	if (rb_is_null(x))
		return NULL;

	return (struct menu_item *)rb_get_data (x);
}

void menu_mark_item (struct menu *menu, const char *file)
{
	struct menu_item *item;

	assert (menu != NULL);
	assert (file != NULL);

	item = menu_find (menu, file);
	if (item)
		menu->marked = item;
}

static void menu_renumber_items (struct menu *menu)
{
	int i = 0;
	struct menu_item *mi;

	assert (menu != NULL);

	for (mi = menu->items; mi; mi = mi->next)
		mi->num = i++;

	assert (i == menu->nitems);
}

static void menu_delete (struct menu *menu, struct menu_item *mi)
{
	assert (menu != NULL);
	assert (mi != NULL);

	if (mi->prev)
		mi->prev->next = mi->next;
	if (mi->next)
		mi->next->prev = mi->prev;

	if (menu->items == mi)
		menu->items = mi->next;
	if (menu->last == mi)
		menu->last = mi->prev;

	if (menu->marked == mi)
		menu->marked = NULL;
	if (menu->selected == mi)
		menu->selected = mi->next ? mi->next : mi->prev;
	if (menu->top == mi)
		menu->top = mi->next ? mi->next : mi->prev;

	if (mi->file)
		rb_delete (menu->search_tree, mi->file);

	menu->nitems--;
	menu_renumber_items (menu);

	menu_item_free (mi);
}

void menu_del_item (struct menu *menu, const char *fname)
{
	struct menu_item *mi;

	assert (menu != NULL);
	assert (fname != NULL);

	mi = menu_find (menu, fname);
	assert (mi != NULL);

	menu_delete (menu, mi);
}

void menu_item_set_align (struct menu_item *mi, const enum menu_align align)
{
	assert (mi != NULL);

	mi->align = align;
}

void menu_setcurritem_file (struct menu *menu, const char *file)
{
	struct menu_item *mi;

	assert (menu != NULL);
	assert (file != NULL);

	mi = menu_find (menu, file);
	if (mi)
		menu_setcurritem (menu, mi);
}
/* Return non-zero value if the item in in the visible part of the menu. */
int menu_is_visible (const struct menu *menu, const struct menu_item *mi)
{
	assert (menu != NULL);
	assert (mi != NULL);

	if (mi->num >= menu->top->num
			&& mi->num < menu->top->num + menu->height)
		return 1;

	return 0;
}

static void menu_items_swap (struct menu *menu, struct menu_item *mi1,
		struct menu_item *mi2)
{
	int t;

	assert (menu != NULL);
	assert (mi1 != NULL);
	assert (mi2 != NULL);
	assert (mi1 != mi2);

	/* if they are next to each other, change the pointers so that mi2
	 * is the second one */
	if (mi2->next == mi1) {
		struct menu_item *i = mi1;

		mi1 = mi2;
		mi2 = i;
	}

	if (mi1->next == mi2) {
		if (mi2->next)
			mi2->next->prev = mi1;
		if (mi1->prev)
			mi1->prev->next = mi2;

		mi1->next = mi2->next;
		mi2->prev = mi1->prev;
		mi1->prev = mi2;
		mi2->next = mi1;
	}
	else {
		if (mi2->next)
			mi2->next->prev = mi1;
		if (mi2->prev)
			mi2->prev->next = mi1;
		mi2->next = mi1->next;
		mi2->prev = mi1->prev;

		if (mi1->next)
			mi1->next->prev = mi2;
		if (mi1->prev)
			mi1->prev->next = mi2;
		mi1->next = mi2->next;
		mi1->prev = mi2->prev;
	}

	t = mi1->num;
	mi1->num = mi2->num;
	mi2->num = t;

	if (menu->top == mi1)
		menu->top = mi2;
	else if (menu->top == mi2)
		menu->top = mi1;

	if (menu->last == mi1)
		menu->last = mi2;
	else if (menu->last == mi2)
		menu->last = mi1;

	if (menu->items == mi1)
		menu->items = mi2;
	else if (menu->items == mi2)
		menu->items = mi1;
}

void menu_swap_items (struct menu *menu, const char *file1, const char *file2)
{
	struct menu_item *mi1, *mi2;

	assert (menu != NULL);
	assert (file1 != NULL);
	assert (file2 != NULL);

	if ((mi1 = menu_find(menu, file1)) && (mi2 = menu_find(menu, file2))
			&& mi1 != mi2) {
		menu_items_swap (menu, mi1, mi2);

		/* make sure that the selected item is visible */
		menu_setcurritem (menu, menu->selected);
	}
}

/* Make sure that this file is visible in the menu. */
void menu_make_visible (struct menu *menu, const char *file)
{
	struct menu_item *mi;

	assert (menu != NULL);
	assert (file != NULL);

	if ((mi = menu_find(menu, file)))
		make_item_visible (menu, mi);
}
