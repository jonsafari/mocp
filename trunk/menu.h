#ifndef MENU_H
#define MENU_H

#include <ncurses.h>
#include "files.h"

enum menu_request
{
	REQ_UP,
	REQ_DOWN,
	REQ_PGUP,
	REQ_PGDOWN,
	REQ_TOP,
	REQ_BOTTOM
};

struct menu_item
{
	char *title;		/* Titile of the item */

	/* Curses attributes in different states: */
	int attr_normal;
	int attr_sel;
	int attr_marked;
	int attr_sel_marked;
	
	int plist_pos;		/* Position of the item on the playlist */

	/* Associated file: */
	char *file;
	enum file_type type;

	/* Additional information shown: */
	char time[6];		/* File time string */
	char format[4];		/* File format */
};

struct menu
{
	WINDOW *win;
	struct menu_item **items;
	int nitems; /* number of items */
	int top; /* first visible item */
	int maxx; /* maximum x position */
	int maxy; /* maximum y position */
	int selected; /* selected item */
	int marked; /* index of the marked item or -1 */

	/* Flags for displaying information about the file. */
	int show_time;
	int show_format;

	int info_attr; /* Attributes for information about the file */
};

struct menu *menu_new (WINDOW *win, struct menu_item **items, const int nitems);

struct menu_item *menu_newitem (char *title, const int plist_pos,
		const enum file_type type, const char *file);
void menu_item_set_attr_normal (struct menu_item *item, const int attr);
void menu_item_set_attr_sel (struct menu_item *item, const int attr);
void menu_item_set_attr_sel_marked (struct menu_item *item, const int attr);
void menu_item_set_attr_marked (struct menu_item *item, const int attr);
void menu_item_set_time_plist (struct menu *menu, const int plist_num,
		const char *time);
void menu_item_set_time (struct menu_item *item, const char *time);
void menu_item_set_format (struct menu_item *item, const char *format);

void menu_free (struct menu *menu);
void menu_driver (struct menu *menu, enum menu_request req);
struct menu_item *menu_curritem (struct menu *menu);
void menu_setcurritem (struct menu *menu, int num);
int menu_get_selected (const struct menu *menu);
void menu_setcurritem_title (struct menu *menu, const char *title);
void menu_draw (struct menu *menu);
void menu_mark_plist_item (struct menu *menu, const int plist_item);
void set_menu_state (struct menu *menu, int selected, int top);
void menu_update_size (struct menu *menu, WINDOW *win);
void menu_unmark_item (struct menu *menu);
void menu_set_top_item (struct menu *menu, const int num);
int menu_find_pattern_next (struct menu *menu, const char *pattern,
		const int current);
int menu_next_turn (const struct menu *menu);
void menu_set_show_time (struct menu *menu, const int t);
void menu_set_show_format (struct menu *menu, const int t);
void menu_set_info_attr (struct menu *menu, const int attr);

#endif
