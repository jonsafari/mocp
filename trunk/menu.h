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
	int attr_normal;	/* Attributes in normal mode */
	int attr_sel;		/* Attributes in selected mode */
	int plist_pos;		/* Position of that item on the playlist */
	char *file;		/* File associated with the item. */ 
	enum file_type type;	/* Type of the associated file. */
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

	/* ncurses attributes for normal items */
	int attr_normal;
	int attr_normal_sel;

	/* ncurses attributes for marked item */
	int attr_marked;
	int attr_marked_sel;
};

struct menu *menu_new (WINDOW *win, struct menu_item **items, const int nitems,
		const int attr_normal, const int attr_normal_sel,
		const int attr_marked, const int attr_marked_sel);
struct menu_item *menu_newitem (char *title, const int plist_pos,
		const enum file_type type, const char *file);
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

#endif
