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

#include <string.h>
#include <assert.h>
#include <ncurses.h>
#include <stdio.h>
#include <errno.h>

#define DEBUG

#include "keys.h"
#include "interface.h"
#include "options.h"
#include "log.h"
#include "main.h"
#include "files.h"

#define CTRL_KEY_CODE	0x1F

/* ^c version of c */
#ifndef CTRL
# define CTRL(c) ((c) & CTRL_KEY_CODE)
#endif


struct command
{
	enum key_cmd cmd;
	char *name;
	enum key_context context;
	int keys[6]; /* array of keys ended with -1 */
	int default_keys_set; /* Are the keys default? */
};

/* Array of commands - each element is a list of keys for this command. */
static struct command commands[] = {
	{
		KEY_CMD_QUIT_CLIENT,
		"quit client",
		CON_MENU,
		{ 'q', -1 },
		1
	},
	{
		KEY_CMD_GO,
		"go",
		CON_MENU,
		{ '\n',	-1 },
		1
	},
	{
		KEY_CMD_MENU_DOWN,
		"menu down",
		CON_MENU,
		{ KEY_DOWN, -1 },
		1
	},
	{
		KEY_CMD_MENU_UP,
		"menu up",
		CON_MENU,
		{ KEY_UP, -1 },
		1
	},
	{
		KEY_CMD_MENU_NPAGE,
		"menu page down",
		CON_MENU,
		{ KEY_NPAGE, -1},
		1
	},
	{
		KEY_CMD_MENU_PPAGE,
		"menu page up",
		CON_MENU,
		{ KEY_PPAGE, -1},
		1
	},
	{
		KEY_CMD_MENU_FIRST,
		"menu first item",
		CON_MENU,
		{ KEY_HOME, -1 },
		1
	},
	{
		KEY_CMD_MENU_LAST,
		"menu last item",
		CON_MENU,
		{ KEY_END, -1 },
		1
	},
	{
		KEY_CMD_QUIT,
		"quit",
		CON_MENU,
		{ 'Q', -1 },
		1
	},
	{
		KEY_CMD_STOP,
		"stop",
		CON_MENU,
		{ 's', -1 },
		1
	},
	{
		KEY_CMD_NEXT,
		"next",
		CON_MENU,
		{ 'n', -1 },
		1
	},
	{
		KEY_CMD_PREVIOUS,
		"previous",
		CON_MENU,
		{ 'b', -1 },
		1
	},
	{
		KEY_CMD_PAUSE,
		"pause",
		CON_MENU,
		{ 'p', ' ', -1 },
		1
	},
	{
		KEY_CMD_TOGGLE_READ_TAGS,
		"toggle read tags",
		CON_MENU,
		{ 'f', -1 },
		1
	},
	{
		KEY_CMD_TOGGLE_SHUFFLE,
		"toggle shuffle",
		CON_MENU,
		{ 'S', -1 },
		1
	},
	{
		KEY_CMD_TOGGLE_REPEAT,
		"toggle repeat",
		CON_MENU,
		{ 'R', -1 },
		1
	},
	{
		KEY_CMD_TOGGLE_AUTO_NEXT,
		"toggle auto next",
		CON_MENU,
		{ 'X', -1 },
		1
	},
	{
		KEY_CMD_TOGGLE_PLAYLIST,
		"toggle playlist",
		CON_MENU,
		{ 'l', -1 },
		1
	},
	{
		KEY_CMD_PLIST_ADD_FILE,
		"add file",
		CON_MENU,
		{ 'a', -1 },
		1
	},
	{
		KEY_CMD_PLIST_CLEAR,
		"clear playlist",
		CON_MENU,
		{ 'C', -1 },
		1
	},
	{
		KEY_CMD_PLIST_ADD_DIR,
		"add directory",
		CON_MENU,
		{ 'A', -1 },
		1
	},
	{
		KEY_CMD_MIXED_DEC_1,
		"volume down 1%",
		CON_MENU,
		{ '<', -1 },
		1
	},
	{
		KEY_CMD_MIXER_INC_1,
		"volume up 1%",
		CON_MENU,
		{ '>', -1 },
		1
	},
	{
		KEY_CMD_MIXER_DEC_5,
		"volume down 5%",
		CON_MENU,
		{ ',', -1 },
		1
	},
	{
		KEY_CMD_MIXER_INC_5,
		"volume up 5%",
		CON_MENU,
		{ '.', -1 },
		1
	},
	{
		KEY_CMD_SEEK_FORWARD_1,
		"seek forward 1s",
		CON_MENU,
		{ KEY_RIGHT, -1 },
		1
	},
	{
		KEY_CMD_SEEK_BACKWARD_1,
		"seek backward 1s",
		CON_MENU,
		{ KEY_LEFT, -1},
		1
	},
	{
		KEY_CMD_HELP,
		"help",
		CON_MENU,
		{ 'h', '?', -1 },
		1
	},
	{
		KEY_CMD_HIDE_MESSAGE,
		"hide message",
		CON_MENU,
		{ 'M', -1 },
		1
	},
	{
		KEY_CMD_REFRESH,
		"refresh",
		CON_MENU,
		{ CTRL('r'), -1},
		1
	},
	{
		KEY_CMD_RELOAD,
		"reload",
		CON_MENU,
		{ 'r', -1 },
		1
	},
	{
		KEY_CMD_TOGGLE_SHOW_HIDDEN_FILES,
		"toggle hidden files",
		CON_MENU,
		{ 'H', -1 },
		1
	},
	{
		KEY_CMD_GO_MUSIC_DIR,
		"go to music directory",
		CON_MENU,
		{ 'm', -1 },
		1
	},
	{
		KEY_CMD_PLIST_DEL,
		"delete from playlist",
		CON_MENU,
		{ 'd', -1 },
		1
	},
	{
		KEY_CMD_MENU_SEARCH,
		"search menu",
		CON_MENU,
		{ 'g', '/', -1 },
		1
	},
	{
		KEY_CMD_PLIST_SAVE,
		"save playlist",
		CON_MENU,
		{ 'V', -1 },
		1
	},
	{
		KEY_CMD_TOGGLE_SHOW_TIME,
		"toggle show time",
		CON_MENU,
		{ CTRL('t'), -1},
		1
	},
	{
		KEY_CMD_TOGGLE_SHOW_FORMAT,
		"toggle show format",
		CON_MENU,
		{ CTRL('f'), -1 },
		1
	},
	{
		KEY_CMD_GO_TO_PLAYING_FILE,
		"go to playing file",
		CON_MENU,
		{ 'G', -1 },
		1
	},
	{
		KEY_CMD_GO_DIR,
		"go to a directory",
		CON_MENU,
		{ 'i', -1 },
		1
	},
	{
		KEY_CMD_GO_DIR_UP,
		"go to '..'",
		CON_MENU,
		{ 'U', -1 },
		1
	},
	{
		KEY_CMD_NEXT_SEARCH,
		"next search",
		CON_ENTRY_SEARCH,
		{ CTRL('g'), CTRL('n'), -1 },
		1
	},
	{
		KEY_CMD_CANCEL,
		"cancel",
		CON_ENTRY,
		{ CTRL('x'), KEY_ESCAPE, -1 },
		1
	}
};

#define COMMANDS_NUM	(sizeof(commands)/sizeof(commands[0]))

enum key_cmd get_key_cmd (const enum key_context context, const int key)
{
	unsigned int i;
	
	for (i = 0; i < sizeof(commands)/sizeof(commands[0]); i++)
		if (commands[i].context == context) {
			int j = 0;

			while (commands[i].keys[j] != -1)
				if (commands[i].keys[j++] == key)
					return commands[i].cmd;
		}
	
	return KEY_CMD_WRONG;
}

/* Return the path to the keymap file or NULL if none was specified. */
static char *find_keymap_file ()
{
	char *file;
	static char path[PATH_MAX];
	
	if ((file = options_get_str("Keymap"))) {
		if (file[0] == '/') {

			/* Absolute path */
			strncpy (path, file, sizeof(path));
			if (path[sizeof(path)-1])
				fatal ("Keymap path too long!");
			return path;
		}

		strncpy (path, create_file_name(file), sizeof(path));
		if (path[sizeof(path)-1])
			fatal ("Keymap path too long!");

		return path;
	}

	return NULL;
}

static void keymap_parse_error (const int line, const char *msg)
{
	fatal ("Parse error in the keymap file line %d: %s", line, msg);
}

/* Return a key for the symbolic key name (^c, M-F, etc.).
 * Return -1 on error. */
static int parse_key (const char *symbol)
{
	/* TODO: function keys, other (home, tab?) */

	if (strlen(symbol) == 1)
		
		/* Just a regular char */
		return symbol[0];

	if (symbol[0] == '^') {

		/* CTRL sequence */
		if (strlen(symbol) != 2)
			return -1;

		return CTRL(symbol[1]);
	}

	if (!strncasecmp(symbol, "M-", 2)) {

		/* Meta char */
		if (strlen(symbol) != 3)
			return -1;

		return symbol[2] & META_KEY_FLAG;
	}

	/* TODO: Special keys */

	return -1;
}

/* Add a key to the command defined in the keypap file in line
 * line_num (used only when reporting an error). */
static void add_key (const int line_num, const char *command,
		const char *key_symbol)
{
	unsigned int cmd_idx;
	int i;
	int key;

	/* Find the command */
	for (cmd_idx = 0; cmd_idx < COMMANDS_NUM; cmd_idx++)
		if (!(strcasecmp(commands[cmd_idx].name, command)))
			break;

	if (cmd_idx == COMMANDS_NUM)
		keymap_parse_error (line_num, "unknown command");

	/* Remove default keys */
	if (commands[cmd_idx].default_keys_set) {
		commands[cmd_idx].default_keys_set = 0;
		commands[cmd_idx].keys[0] = -1;
	}

	/* Go to the last key */
	for (i = 0; commands[cmd_idx].keys[i] != -1; i++)
		;

	if (i == sizeof(commands[cmd_idx].keys)
			/sizeof(commands[cmd_idx].keys[0]) - 2)
		keymap_parse_error (line_num, "too much keys defined");

	if ((key = parse_key(key_symbol)) == -1)
		keymap_parse_error (line_num, "bad key sequence");

	commands[cmd_idx].keys[i] = key;
	commands[cmd_idx].keys[i+1] = -1;
}

/* Load a key map from the file. */
static void load_key_map (const char *file_name)
{
	FILE *file;
	char *line;
	int line_num = 0;
	
	if (!(file = fopen(file_name, "r")))
		fatal ("Can't open keymap file: %s", strerror(errno));

	/* Read lines in format: 
	 * COMMAND = KEY [KEY ...]
	 * Blank lines and beginning with # are ignored, see example_keymap. */
	while ((line = read_line(file))) {
		char *command;
		char *tmp;
		char *key;

		line_num++;
		if (line[0] == '#' || !(command = strtok(line, " \t"))) {

			/* empty line or a comment */
			free (line);
			continue;
		}

		if (!(tmp = strtok(NULL, " \t")) || strcmp(tmp, "="))
			keymap_parse_error (line_num, "expected '='");

		while ((key = strtok(NULL, " \t")))
			add_key (line_num, command, key);

		free (line);
	}

	fclose (file);

//	fatal ("DEBUG");
}

/* Check if any used default key is not defined for something else. */
static void check_keys ()
{
	/* TODO */
}

/* Load key map. Set default keys if necessary. */
void keys_init ()
{
	char *file = find_keymap_file ();

	if (file) {
		load_key_map (file);
		check_keys ();
	}
}
