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

#ifdef HAVE_NCURSES_H
# include <ncurses.h>
#elif HAVE_CURSES_H
# include <curses.h>
#endif

#include <assert.h>
#include <string.h>
#include <errno.h>

#include "interface.h"
#include "themes.h"
#include "main.h"
#include "files.h"
#include "options.h"

/* ncurses extension */
#ifndef COLOR_DEFAULT
# define COLOR_DEFAULT	-2
#endif

/* hidden color? */
#ifndef COLOR_GREY
# define COLOR_GREY	10
#endif

static int colors[CLR_LAST];

/* Initialize a color item of given index (CLR_*) with colors and
 * attributes. Do nothing if the item is already initialized. */
static void make_color (const enum color_index index, const short foreground,
		const short background,	const attr_t attr)
{
	static int pair = 1;

	assert (pair < COLOR_PAIRS);
	assert (index < CLR_LAST);

	if (colors[index] == -1) {
		init_pair (pair, foreground, background);
		colors[index] = COLOR_PAIR (pair) | attr;

		pair++;
	}
}

static void set_default_colors ()
{
	make_color (CLR_BACKGROUND, COLOR_WHITE, COLOR_BLUE, A_NORMAL);
	make_color (CLR_FRAME, COLOR_WHITE, COLOR_BLUE, A_NORMAL);
	make_color (CLR_WIN_TITLE, COLOR_WHITE, COLOR_BLUE, A_NORMAL);
	make_color (CLR_MENU_ITEM_DIR, COLOR_WHITE, COLOR_BLUE, A_BOLD);
	make_color (CLR_MENU_ITEM_DIR_SELECTED, COLOR_WHITE, COLOR_BLACK,
			A_BOLD);
	make_color (CLR_MENU_ITEM_PLAYLIST, COLOR_WHITE, COLOR_BLUE, A_BOLD);
	make_color (CLR_MENU_ITEM_PLAYLIST_SELECTED, COLOR_WHITE, COLOR_BLACK,
			A_BOLD);
	make_color (CLR_MENU_ITEM_FILE, COLOR_WHITE, COLOR_BLUE, A_NORMAL);
	make_color (CLR_MENU_ITEM_FILE_SELECTED, COLOR_WHITE,
			COLOR_BLACK, A_NORMAL);
	make_color (CLR_MENU_ITEM_FILE_MARKED, COLOR_GREEN, COLOR_BLUE,
			A_BOLD);
	make_color (CLR_MENU_ITEM_FILE_MARKED_SELECTED, COLOR_GREEN,
			COLOR_BLACK, A_BOLD);
	make_color (CLR_MENU_ITEM_INFO, COLOR_BLUE, COLOR_BLUE, A_BOLD);
	make_color (CLR_STATUS, COLOR_WHITE, COLOR_BLUE, A_NORMAL);
	make_color (CLR_TITLE, COLOR_WHITE, COLOR_BLUE, A_BOLD);
	make_color (CLR_STATE, COLOR_WHITE, COLOR_BLUE, A_BOLD);
	make_color (CLR_TIME_CURRENT, COLOR_WHITE, COLOR_BLUE, A_BOLD);
	make_color (CLR_TIME_LEFT, COLOR_WHITE, COLOR_BLUE, A_BOLD);
	make_color (CLR_TIME_TOTAL_FRAMES, COLOR_WHITE, COLOR_BLUE, A_NORMAL);
	make_color (CLR_TIME_TOTAL, COLOR_WHITE, COLOR_BLUE, A_BOLD);
	make_color (CLR_SOUND_PARAMS, COLOR_WHITE, COLOR_BLUE, A_BOLD);
	make_color (CLR_LEGEND, COLOR_WHITE, COLOR_BLUE, A_NORMAL);
	make_color (CLR_INFO_DISABLED, COLOR_BLUE, COLOR_BLUE, A_BOLD);
	make_color (CLR_INFO_ENABLED, COLOR_WHITE, COLOR_BLUE, A_BOLD);
	make_color (CLR_MIXER_BAR_EMPTY, COLOR_WHITE, COLOR_BLUE, A_NORMAL);
	make_color (CLR_MIXER_BAR_FILL, COLOR_BLACK, COLOR_CYAN, A_NORMAL);
	make_color (CLR_TIME_BAR_EMPTY, COLOR_WHITE, COLOR_BLUE, A_NORMAL);
	make_color (CLR_TIME_BAR_FILL, COLOR_BLACK, COLOR_CYAN, A_NORMAL);
	make_color (CLR_ENTRY, COLOR_WHITE, COLOR_BLUE, A_NORMAL);
	make_color (CLR_ENTRY_TITLE, COLOR_BLACK, COLOR_CYAN, A_BOLD);
	make_color (CLR_ERROR, COLOR_RED, COLOR_BLUE, A_BOLD);
	make_color (CLR_MESSAGE, COLOR_GREEN, COLOR_BLUE, A_BOLD);
	make_color (CLR_PLIST_TIME, COLOR_WHITE, COLOR_BLUE, A_NORMAL);
}

static void theme_parse_error (const int line, const char *msg)
{
	interface_fatal ("Parse error in theme file line %d: %s", line, msg);
}

/* Find the index of a color element by name. Return CLR_WRONG if not found. */
static enum color_index find_color_element_name (const char *name)
{
	int i;
	static struct
	{
		char *name;
		enum color_index idx;
	} color_tab[] = {
		{ "background",		CLR_BACKGROUND },
		{ "frame",		CLR_FRAME },
		{ "window_title",	CLR_WIN_TITLE },
		{ "directory",		CLR_MENU_ITEM_DIR },
		{ "selected_directory", CLR_MENU_ITEM_DIR_SELECTED },
		{ "playlist",		CLR_MENU_ITEM_PLAYLIST },
		{ "selected_playlist",	CLR_MENU_ITEM_PLAYLIST_SELECTED },
		{ "file",		CLR_MENU_ITEM_FILE },
		{ "selected_file",	CLR_MENU_ITEM_FILE_SELECTED },
		{ "marked_file",	CLR_MENU_ITEM_FILE_MARKED },
		{ "marked_selected_file", CLR_MENU_ITEM_FILE_MARKED_SELECTED },
		{ "info",		CLR_MENU_ITEM_INFO },
		{ "status",		CLR_STATUS },
		{ "title",		CLR_TITLE },
		{ "state",		CLR_STATE },
		{ "current_time",	CLR_TIME_CURRENT },
		{ "time_left",		CLR_TIME_LEFT },
		{ "total_time",		CLR_TIME_TOTAL },
		{ "time_total_frames",	CLR_TIME_TOTAL_FRAMES },
		{ "sound_parameters",	CLR_SOUND_PARAMS },
		{ "legend",		CLR_LEGEND },
		{ "disabled",		CLR_INFO_DISABLED },
		{ "enabled",		CLR_INFO_ENABLED },
		{ "empty_mixer_bar",	CLR_MIXER_BAR_EMPTY },
		{ "filled_mixer_bar",	CLR_MIXER_BAR_FILL },
		{ "empty_time_bar",	CLR_TIME_BAR_EMPTY },
		{ "filled_time_bar",	CLR_TIME_BAR_FILL },
		{ "entry",		CLR_ENTRY },
		{ "entry_title",	CLR_ENTRY_TITLE },
		{ "error",		CLR_ERROR },
		{ "message",		CLR_MESSAGE },
		{ "plist_time",		CLR_PLIST_TIME }
	};
	assert (name != NULL);

	for (i = 0; i < (int)(sizeof(color_tab)/sizeof(color_tab[0])); i++)
		if (!strcasecmp(color_tab[i].name, name))
			return color_tab[i].idx;

	return CLR_WRONG;
}

/* Find the curses color by name. Return -1 if the color is unknown. */
static short find_color_name (const char *name)
{
	int i;
	static struct
	{
		char *name;
		short color;
	} color_tab[] = {
		{ "black",	COLOR_BLACK },
		{ "red",	COLOR_RED },
		{ "green",	COLOR_GREEN },
		{ "yellow",	COLOR_YELLOW },
		{ "blue",	COLOR_BLUE },
		{ "magenta",	COLOR_MAGENTA },
		{ "cyan",	COLOR_CYAN },
		{ "white",	COLOR_WHITE },
		{ "default",	COLOR_DEFAULT },
		{ "grey",	COLOR_GREY }
	};
	
	for (i = 0; i < (int)(sizeof(color_tab)/sizeof(color_tab[0])); i++)
		if (!strcasecmp(color_tab[i].name, name))
			return color_tab[i].color;
	
	return -1;
}

/* Find path to the theme for the given name. Returned memory is static. */
static char *find_theme_file (const char *name)
{
	static char path[PATH_MAX];
	
	path[sizeof(path)-1] = 0;
	if (name[0] == '/') {

		/* Absolute path */
		strncpy (path, name, sizeof(path));
		if (path[sizeof(path)-1])
			interface_fatal ("Theme path too long!");
		return path;
	}

	/* Try the user directory */
	if (snprintf(path, sizeof(path), "%s/%s", create_file_name("themes"),
				name) >= (int)sizeof(path))
		interface_fatal ("Theme path too long!");
	if (file_exists(path))
		return path;
	
	/* Try the system directory */
	if (snprintf(path, sizeof(path), "%s/%s", SYSTEM_THEMES_DIR,
				name) >= (int)sizeof(path))
		interface_fatal ("Theme path too long!");
	if (file_exists(path))
		return path;

	/* File related to the current directory? */
	strncpy (path, name, sizeof(path));
	if (path[sizeof(path)-1])
		interface_fatal ("Theme path too long!");
	return path;
}

static void load_color_theme (const char *name)
{
	FILE *file;
	char *line;
	int line_num = 0;
	char *theme_file = find_theme_file (name);

	if (!(file = fopen(theme_file, "r")))
		interface_fatal ("Can't open theme file: %s", strerror(errno));

	/* The lines should be in format:
	 * ELEMENT = FOREGROUND BACKGROUND [ATTRIBUTE[,ATTRIBUTE,..]]
	 * Blank lines and beginning with # are ignored, see example_theme. */

	while ((line = read_line(file))) {
		char *name;
		char *tmp;
		char *foreground, *background, *attributes;
		attr_t curses_attr = 0;
		enum color_index element;
		short clr_fore, clr_back;
		
		line_num++;
		if (line[0] == '#' || !(name = strtok(line, " \t"))) {

			/* empty line or a comment */
			free (line);
			continue;
		}

		if (!(tmp = strtok(NULL, " \t")) || strcmp(tmp, "="))
			theme_parse_error (line_num, "expected '='");
		if (!(foreground = strtok(NULL, " \t")))
			theme_parse_error (line_num, "foreground color not "
					"specified");
		if (!(background = strtok(NULL, " \t")))
			theme_parse_error (line_num, "background color not "
					"specified");
		if ((attributes = strtok(NULL, " \t"))) {
			char *attr;

			if ((tmp = strtok(NULL, " \t")))
				theme_parse_error (line_num,
						"unexpected chars at the end "
						"of line");
			
			attr = strtok (attributes, ",");

			do {
				if (!strcasecmp(attr, "normal"))
					curses_attr |= A_NORMAL;
				else if (!strcasecmp(attr, "standout"))
					curses_attr |= A_STANDOUT;
				else if (!strcasecmp(attr, "underline"))
					curses_attr |= A_UNDERLINE;
				else if (!strcasecmp(attr, "reverse"))
					curses_attr |= A_REVERSE;
				else if (!strcasecmp(attr, "blink"))
					curses_attr |= A_BLINK;
				else if (!strcasecmp(attr, "dim"))
					curses_attr |= A_DIM;
				else if (!strcasecmp(attr, "bold"))
					curses_attr |= A_BOLD;
				else if (!strcasecmp(attr, "protected"))
					curses_attr |= A_PROTECT;
				else
					theme_parse_error (line_num,
							"unknown attribute");
			} while ((attr = strtok(NULL, ",")));
		}

		if ((element = find_color_element_name(name)) == CLR_WRONG)
			theme_parse_error (line_num, "unknown element");
		if ((clr_fore = find_color_name(foreground)) == -1)
			theme_parse_error (line_num,
					"bad foreground color name");
		if ((clr_back = find_color_name(background)) == -1)
			theme_parse_error (line_num,
					"bad background color name");

		make_color (element, clr_fore, clr_back, curses_attr);
		
		free (line);
	}

	fclose (file);
}

static void reset_colors_table ()
{
	int i;

	for (i = 0; i < CLR_LAST; i++)
		colors[i] = -1;
}

void theme_init (const int has_xterm)
{
	reset_colors_table ();

	if (options_get_str("ForceTheme"))
		load_color_theme (options_get_str("ForceTheme"));
	else if (has_xterm && options_get_str("XTermTheme"))
		load_color_theme (options_get_str("XTermTheme"));
	else if (options_get_str("Theme"))
		load_color_theme (options_get_str("Theme"));

	set_default_colors ();
}

int get_color (const enum color_index index)
{
	return colors[index];
}
