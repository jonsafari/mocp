/*
 * MOC - music on console
 * Copyright (C) 2004 - 2006 Damian Pietras <daper@daper.net>
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

#if defined HAVE_NCURSESW_CURSES_H
# include <ncursesw/curses.h>
#elif defined HAVE_NCURSESW_H
# include <ncursesw.h>
#elif defined HAVE_NCURSES_CURSES_H
# include <ncurses/curses.h>
#elif defined HAVE_NCURSES_H
# include <ncurses.h>
#elif defined HAVE_CURSES_H
# include <curses.h>
#endif

#include <stdio.h>
#include <assert.h>
#include <string.h>
#include <strings.h>
#include <errno.h>

#include "common.h"
#include "interface.h"
#include "themes.h"
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

static char current_theme[PATH_MAX];

static int colors[CLR_LAST];

/* Counter used for making colors (init_pair()) */
static short pair_count = 1;

/* Initialize a color item of given index (CLR_*) with colors and
 * attributes. Do nothing if the item is already initialized. */
static void make_color (const enum color_index index, const short foreground,
		const short background,	const attr_t attr)
{
	assert (pair_count < COLOR_PAIRS);
	assert (index < CLR_LAST);

	if (colors[index] == -1) {
		init_pair (pair_count, foreground, background);
		colors[index] = COLOR_PAIR (pair_count) | attr;

		pair_count++;
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
	make_color (CLR_MENU_ITEM_INFO_SELECTED, COLOR_BLUE, COLOR_BLACK, A_BOLD);
	make_color (CLR_MENU_ITEM_INFO_MARKED, COLOR_BLUE, COLOR_BLUE, A_BOLD);
	make_color (CLR_MENU_ITEM_INFO_MARKED_SELECTED, COLOR_BLUE, COLOR_BLACK, A_BOLD);
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

/* Set default colors for black and white terminal. */
static void set_bw_colors ()
{
	colors[CLR_BACKGROUND] = A_NORMAL;
	colors[CLR_FRAME] = A_NORMAL;
	colors[CLR_WIN_TITLE] = A_NORMAL;
	colors[CLR_MENU_ITEM_DIR] = A_NORMAL;
	colors[CLR_MENU_ITEM_DIR_SELECTED] = A_REVERSE;
	colors[CLR_MENU_ITEM_PLAYLIST] = A_NORMAL;
	colors[CLR_MENU_ITEM_PLAYLIST_SELECTED] = A_REVERSE;
	colors[CLR_MENU_ITEM_FILE] = A_NORMAL;
	colors[CLR_MENU_ITEM_FILE_SELECTED] = A_REVERSE;
	colors[CLR_MENU_ITEM_FILE_MARKED] = A_BOLD;
	colors[CLR_MENU_ITEM_FILE_MARKED_SELECTED] = A_BOLD | A_REVERSE;
	colors[CLR_MENU_ITEM_INFO] = A_NORMAL;
	colors[CLR_MENU_ITEM_INFO_SELECTED] = A_REVERSE;
	colors[CLR_MENU_ITEM_INFO_MARKED] = A_BOLD;
	colors[CLR_MENU_ITEM_INFO_MARKED_SELECTED] = A_BOLD | A_REVERSE;
	colors[CLR_STATUS] = A_NORMAL;
	colors[CLR_TITLE] = A_BOLD;
	colors[CLR_STATE] = A_BOLD;
	colors[CLR_TIME_CURRENT] = A_BOLD;
	colors[CLR_TIME_LEFT] = A_BOLD;
	colors[CLR_TIME_TOTAL_FRAMES] = A_NORMAL;
	colors[CLR_TIME_TOTAL] = A_BOLD;
	colors[CLR_SOUND_PARAMS] = A_BOLD;
	colors[CLR_LEGEND] = A_NORMAL;
	colors[CLR_INFO_DISABLED] = A_BOLD;
	colors[CLR_INFO_ENABLED] = A_BOLD;
	colors[CLR_MIXER_BAR_EMPTY] = A_NORMAL;
	colors[CLR_MIXER_BAR_FILL] = A_REVERSE;
	colors[CLR_TIME_BAR_EMPTY] = A_NORMAL;
	colors[CLR_TIME_BAR_FILL] = A_REVERSE;
	colors[CLR_ENTRY] = A_NORMAL;
	colors[CLR_ENTRY_TITLE] = A_BOLD;
	colors[CLR_ERROR] = A_BOLD;
	colors[CLR_MESSAGE] = A_BOLD;
	colors[CLR_PLIST_TIME] = A_NORMAL;
}

static void theme_parse_error (const int line, const char *msg)
{
	interface_fatal ("Parse error in theme file line %d: %s", line, msg);
}

/* Find the index of a color element by name. Return CLR_WRONG if not found. */
static enum color_index find_color_element_name (const char *name)
{
	size_t ix;
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
		{ "selected_info",	CLR_MENU_ITEM_INFO_SELECTED },
		{ "marked_info",	CLR_MENU_ITEM_INFO_MARKED },
		{ "marked_selected_info", CLR_MENU_ITEM_INFO_MARKED_SELECTED },
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

	for (ix = 0; ix < ARRAY_SIZE(color_tab); ix += 1) {
		if (!strcasecmp(color_tab[ix].name, name))
			return color_tab[ix].idx;
	}

	return CLR_WRONG;
}

/* Find the curses color by name. Return -1 if the color is unknown. */
static short find_color_name (const char *name)
{
	size_t ix;
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

	for (ix = 0; ix < ARRAY_SIZE(color_tab); ix += 1) {
		if (!strcasecmp(color_tab[ix].name, name))
			return color_tab[ix].color;
	}

	return -1;
}

static int new_colordef (const int line_num, const char *name, const short red,
		const short green, const short blue, const int errors_are_fatal)
{
	short color = find_color_name (name);

	if (color == -1) {
		if (errors_are_fatal)
			theme_parse_error (line_num, "bad color name");
		return 0;
	}
	if (can_change_color())
		init_color (color, red, green, blue);

	return 1;
}

/* Find path to the theme for the given name. Returned memory is static. */
static char *find_theme_file (const char *name)
{
	int rc;
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
	rc = snprintf(path, sizeof(path), "%s/%s",
	              create_file_name("themes"), name);
	if (rc >= ssizeof(path))
		interface_fatal ("Theme path too long!");
	if (file_exists(path))
		return path;

	/* Try the system directory */
	rc = snprintf(path, sizeof(path), "%s/%s", SYSTEM_THEMES_DIR, name);
	if (rc >= ssizeof(path))
		interface_fatal ("Theme path too long!");
	if (file_exists(path))
		return path;

	/* File related to the current directory? */
	strncpy (path, name, sizeof(path));
	if (path[sizeof(path)-1])
		interface_fatal ("Theme path too long!");
	return path;
}

/* Parse a theme element line. strtok() should be already invoked and consumed
 * the element name.
 * On error: if errors_are_fatal is true,
 * theme_parse_error() is invoked, otherwise 0 is returned. */
static int parse_theme_element (const int line_num, const char *name,
		const int errors_are_fatal)
{
	char *tmp;
	char *foreground, *background, *attributes;
	attr_t curses_attr = 0;
	enum color_index element;
	short clr_fore, clr_back;

	if (!(tmp = strtok(NULL, " \t")) || strcmp(tmp, "=")) {
		if (errors_are_fatal)
			theme_parse_error (line_num, "expected '='");
		return 0;
	}
	if (!(foreground = strtok(NULL, " \t"))) {
		if (errors_are_fatal)
			theme_parse_error (line_num,
					"foreground color not specified");
		return 0;
	}
	if (!(background = strtok(NULL, " \t"))) {
		if (errors_are_fatal)
			theme_parse_error (line_num,
					"background color not specified");
		return 0;
	}
	if ((attributes = strtok(NULL, " \t"))) {
		char *attr;

		if ((tmp = strtok(NULL, " \t"))) {
			if (errors_are_fatal)
				theme_parse_error (line_num,
					"unexpected chars at the end of line");
			return 0;
		}

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
			else if (!strcasecmp(attr, "protect"))
				curses_attr |= A_PROTECT;
			else {
				if (errors_are_fatal)
					theme_parse_error (line_num,
						"unknown attribute");
				return 0;
			}
		} while ((attr = strtok(NULL, ",")));
	}

	if ((element = find_color_element_name(name)) == CLR_WRONG) {
		if (errors_are_fatal)
			theme_parse_error (line_num, "unknown element");
		return 0;
	}
	if ((clr_fore = find_color_name(foreground)) == -1) {
		if (errors_are_fatal)
			theme_parse_error (line_num,
					"bad foreground color name");
		return 0;
	}
	if ((clr_back = find_color_name(background)) == -1) {
		if (errors_are_fatal)
			theme_parse_error (line_num,
					"bad background color name");
		return 0;
	}

	make_color (element, clr_fore, clr_back, curses_attr);

	return 1;
}

/* Parse a color value. strtok() should be already invoked and should "point"
 * to the number. If errors_are_fatal, use theme_parse_error() on error,
 * otherwise return -1. */
static short parse_rgb_color_value (const int line_num,
		const int errors_are_fatal)
{
	char *tmp;
	char *end;
	long color;

	if (!(tmp = strtok(NULL, " \t"))) {
		if (errors_are_fatal)
			theme_parse_error (line_num, "3 color values expected");
		return -1;
	}
	color = strtol (tmp, &end, 10);
	if (*end) {
		if (errors_are_fatal)
			theme_parse_error (line_num,
					"color value is not a valid number");
		return -1;
	}
	if (!RANGE(0, color, 1000)) {
		if (errors_are_fatal)
			theme_parse_error (line_num,
					"color value should be in range 0-1000");
		return -1;
	}

	return color;
}

/* Parse a theme color definition. strtok() should be already invoked and
 * consumed 'colordef'. On error: if errors_are_fatal is true,
 * theme_parse_error() is invoked, otherwise 0 is returned. */
static int parse_theme_colordef (const int line_num,
		const int errors_are_fatal)
{
	char *name;
	char *tmp;
	short red, green, blue;

	if (!(name = strtok(NULL, " \t"))) {
		if (errors_are_fatal)
			theme_parse_error (line_num, "expected color name");
		return 0;
	}
	if (!(tmp = strtok(NULL, " \t")) || strcmp(tmp, "=")) {
		if (errors_are_fatal)
			theme_parse_error (line_num, "expected '='");
		return 0;
	}

	red = parse_rgb_color_value (line_num, errors_are_fatal);
	green = parse_rgb_color_value (line_num, errors_are_fatal);
	blue = parse_rgb_color_value (line_num, errors_are_fatal);
	if (red == -1 || green == -1 || blue == -1)
		return 0;

	if (!new_colordef(line_num, name, red, green, blue, errors_are_fatal))
		return 0;

	return 1;
}

/* The lines should be in format:
 *
 * ELEMENT = FOREGROUND BACKGROUND [ATTRIBUTE[,ATTRIBUTE,..]]
 * or:
 * colordef COLORNAME = RED GREEN BLUE
 *
 * Blank lines and beginning with # are ignored, see example_theme.
 *
 * On error: if errors_are_fatal is true, interface_fatal() is invoked,
 * otherwise 0 is returned. */
static int parse_theme_line (const int line_num, char *line,
		const int errors_are_fatal)
{
	char *name;

	if (line[0] == '#' || !(name = strtok(line, " \t"))) {

		/* empty line or a comment */
		return 1;
	}

	if (!strcasecmp(name, "colordef"))
		return parse_theme_colordef (line_num, errors_are_fatal);
	return parse_theme_element (line_num, name, errors_are_fatal);
}

/* Load a color theme. If errors_are_fatal is true, errors cause
 * interface_fatal(), otherwise 0 is returned on error. */
static int load_color_theme (const char *name, const int errors_are_fatal)
{
	FILE *file;
	char *line;
	int result = 1;
	int line_num = 0;
	char *theme_file = find_theme_file (name);

	if (!(file = fopen(theme_file, "r"))) {
		if (errors_are_fatal)
			interface_fatal ("Can't open theme file: %s", xstrerror (errno));
		return 0;
	}

	while (result && (line = read_line (file))) {
		line_num++;
		result = parse_theme_line (line_num, line, errors_are_fatal);
		free (line);
	}

	fclose (file);

	return result;
}

static void reset_colors_table ()
{
	int i;

	pair_count = 1;
	for (i = 0; i < CLR_LAST; i++)
		colors[i] = -1;
}

void theme_init (bool has_xterm)
{
	reset_colors_table ();

	if (has_colors ()) {
		char *file;

		if ((file = options_get_str ("ForceTheme"))) {
			load_color_theme (file, 1);
			strncpy (current_theme, find_theme_file (file), PATH_MAX);
		}
		else if (has_xterm && (file = options_get_str ("XTermTheme"))) {
			load_color_theme (file, 1);
			strncpy (current_theme, find_theme_file (file), PATH_MAX);
		}
		else if ((file = options_get_str ("Theme"))) {
			load_color_theme (file, 1);
			strncpy (current_theme, find_theme_file (file), PATH_MAX);
		}
		else
			snprintf (current_theme, PATH_MAX, "%s/example_theme",
			                                   SYSTEM_THEMES_DIR);

		set_default_colors ();
	}
	else
		set_bw_colors ();
}

int get_color (const enum color_index index)
{
	return colors[index];
}

void themes_switch_theme (const char *file)
{
	if (has_colors()) {
		reset_colors_table ();
		if (!load_color_theme(file, 0)) {
			interface_error ("Error loading theme!");
			reset_colors_table ();
		}
		else
			strncpy (current_theme, file, PATH_MAX);

		set_default_colors ();
	}
}

const char *get_current_theme ()
{
	return current_theme;
}
