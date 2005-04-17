#ifndef MENU_H
#define MENU_H

#ifdef HAVE_NCURSES_H
# include <ncurses.h>
#elif HAVE_CURSES_H
# include <curses.h>
#endif

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
	struct menu_item *items;
	int nitems; /* number of present items */
	int allocated; /* number of allocated items */
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

struct menu *menu_new (WINDOW *win);
int menu_add (struct menu *menu, char *title, const int plist_pos,
		const enum file_type type, const char *file);

void menu_item_set_attr_normal (struct menu *menu, const int num,
		const int attr);
void menu_item_set_attr_sel (struct menu *menu, const int num, const int attr);
void menu_item_set_attr_sel_marked (struct menu *menu, const int num,
		const int attr);
void menu_item_set_attr_marked (struct menu *menu, const int num,
		const int attr);

void menu_item_set_time_plist (struct menu *menu, const int plist_num,
		const char *time);
void menu_item_set_time (struct menu *menu, const int num, const char *time);
void menu_item_set_format (struct menu *menu, const int num,
		const char *format);

void menu_free (struct menu *menu);
void menu_driver (struct menu *menu, enum menu_request req);
int menu_curritem (struct menu *menu);
void menu_setcurritem (struct menu *menu, int num);
void menu_setcurritem_title (struct menu *menu, const char *title);
void menu_draw (struct menu *menu);
void menu_mark_plist_item (struct menu *menu, const int plist_item);
void set_menu_state (struct menu *menu, int selected, int top);
void menu_update_size (struct menu *menu, WINDOW *win);
void menu_unmark_item (struct menu *menu);
void menu_set_top_item (struct menu *menu, const int num);
struct menu *menu_filter_pattern (struct menu *menu, const char *pattern);
void menu_set_show_time (struct menu *menu, const int t);
void menu_set_show_format (struct menu *menu, const int t);
void menu_set_info_attr (struct menu *menu, const int attr);
enum file_type menu_item_get_type (const struct menu *menu, const int num);
char *menu_item_get_file (struct menu *menu, const int num);
int menu_item_get_plist_pos (struct menu *menu, const int num);
void menu_item_set_title (struct menu *menu, const int num, const char *title);

#endif
