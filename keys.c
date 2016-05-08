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

#include <string.h>
#include <strings.h>
#include <assert.h>
#include <stdio.h>
#include <errno.h>
#include <ctype.h>

#define DEBUG

#include "common.h"
#include "keys.h"
#include "interface.h"
#include "interface_elements.h"
#include "options.h"
#include "log.h"
#include "files.h"

/* ^c version of c */
#ifndef CTRL
# define CTRL(c) ((c) & CTRL_KEY_CODE)
#endif

struct command
{
	enum key_cmd cmd;	/* the command */
	char *name;		/* name of the command (in keymap file) */
	char *help;		/* help string for the command */
	enum key_context context; /* context - where the command isused */
	int keys[6];		/* array of keys ended with -1 */
	int default_keys;	/* number of default keys */
};

/* Array of commands - each element is a list of keys for this command. */
static struct command commands[] = {
	{
		KEY_CMD_QUIT_CLIENT,
		"quit_client",
		"Detach MOC from the server",
		CON_MENU,
		{ 'q', -1 },
		1
	},
	{
		KEY_CMD_GO,
		"go",
		"Start playing at this file or go to this directory",
		CON_MENU,
		{ '\n',	-1 },
		1
	},
	{
		KEY_CMD_MENU_DOWN,
		"menu_down",
		"Move down in the menu",
		CON_MENU,
		{ KEY_DOWN, -1 },
		1
	},
	{
		KEY_CMD_MENU_UP,
		"menu_up",
		"Move up in the menu",
		CON_MENU,
		{ KEY_UP, -1 },
		1
	},
	{
		KEY_CMD_MENU_NPAGE,
		"menu_page_down",
		"Move one page down",
		CON_MENU,
		{ KEY_NPAGE, -1},
		1
	},
	{
		KEY_CMD_MENU_PPAGE,
		"menu_page_up",
		"Move one page up",
		CON_MENU,
		{ KEY_PPAGE, -1},
		1
	},
	{
		KEY_CMD_MENU_FIRST,
		"menu_first_item",
		"Move to the first item in the menu",
		CON_MENU,
		{ KEY_HOME, -1 },
		1
	},
	{
		KEY_CMD_MENU_LAST,
		"menu_last_item",
		"Move to the last item in the menu",
		CON_MENU,
		{ KEY_END, -1 },
		1
	},
	{
		KEY_CMD_QUIT,
		"quit",
		"Quit",
		CON_MENU,
		{ 'Q', -1 },
		1
	},
	{
		KEY_CMD_STOP,
		"stop",
		"Stop",
		CON_MENU,
		{ 's', -1 },
		1
	},
	{
		KEY_CMD_NEXT,
		"next",
		"Play next file",
		CON_MENU,
		{ 'n', -1 },
		1
	},
	{
		KEY_CMD_PREVIOUS,
		"previous",
		"Play previous file",
		CON_MENU,
		{ 'b', -1 },
		1
	},
	{
		KEY_CMD_PAUSE,
		"pause",
		"Pause",
		CON_MENU,
		{ 'p', ' ', -1 },
		2
	},
	{
		KEY_CMD_TOGGLE_READ_TAGS,
		"toggle_read_tags",
		"Toggle ReadTags option",
		CON_MENU,
		{ 'f', -1 },
		1
	},
	{
		KEY_CMD_TOGGLE_SHUFFLE,
		"toggle_shuffle",
		"Toggle Shuffle",
		CON_MENU,
		{ 'S', -1 },
		1
	},
	{
		KEY_CMD_TOGGLE_REPEAT,
		"toggle_repeat",
		"Toggle Repeat",
		CON_MENU,
		{ 'R', -1 },
		1
	},
	{
		KEY_CMD_TOGGLE_AUTO_NEXT,
		"toggle_auto_next",
		"Toggle AutoNext",
		CON_MENU,
		{ 'X', -1 },
		1
	},
	{
		KEY_CMD_TOGGLE_MENU,
		"toggle_menu",
		"Switch between playlist and file list",
		CON_MENU,
		{ '\t', -1 },
		1
	},
	{
		KEY_CMD_TOGGLE_LAYOUT,
		"toggle_layout",
		"Switch between layouts",
		CON_MENU,
		{ 'l', -1 },
		1
	},
	{
		KEY_CMD_TOGGLE_PERCENT,
		"toggle_percent",
		"Switch on/off play time percentage",
		CON_MENU,
		{ -1 },
		0
	},
	{
		KEY_CMD_PLIST_ADD_FILE,
		"add_file",
		"Add a file/directory to the playlist",
		CON_MENU,
		{ 'a', -1 },
		1
	},
	{
		KEY_CMD_PLIST_CLEAR,
		"clear_playlist",
		"Clear the playlist",
		CON_MENU,
		{ 'C', -1 },
		1
	},
	{
		KEY_CMD_PLIST_ADD_DIR,
		"add_directory",
		"Add a directory recursively to the playlist",
		CON_MENU,
		{ 'A', -1 },
		1
	},
	{
		KEY_CMD_PLIST_REMOVE_DEAD_ENTRIES,
		"remove_dead_entries",
		"Remove playlist entries for non-existent files",
		CON_MENU,
		{ 'Y', -1 },
		1
	},
	{
		KEY_CMD_MIXER_DEC_1,
		"volume_down_1",
		"Decrease volume by 1%",
		CON_MENU,
		{ '<', -1 },
		1
	},
	{
		KEY_CMD_MIXER_INC_1,
		"volume_up_1",
		"Increase volume by 1%",
		CON_MENU,
		{ '>', -1 },
		1
	},
	{
		KEY_CMD_MIXER_DEC_5,
		"volume_down_5",
		"Decrease volume by 5%",
		CON_MENU,
		{ ',', -1 },
		1
	},
	{
		KEY_CMD_MIXER_INC_5,
		"volume_up_5",
		"Increase volume by 5%",
		CON_MENU,
		{ '.', -1 },
		1
	},
	{
		KEY_CMD_SEEK_FORWARD,
		"seek_forward",
		"Seek forward by n-s",
		CON_MENU,
		{ KEY_RIGHT, -1 },
		1
	},
	{
		KEY_CMD_SEEK_BACKWARD,
		"seek_backward",
		"Seek backward by n-s",
		CON_MENU,
		{ KEY_LEFT, -1},
		1
	},
	{
		KEY_CMD_HELP,
		"help",
		"Show the help screen",
		CON_MENU,
		{ 'h', '?', -1 },
		2
	},
	{
		KEY_CMD_HIDE_MESSAGE,
		"hide_message",
		"Hide error/informative message",
		CON_MENU,
		{ 'M', -1 },
		1
	},
	{
		KEY_CMD_REFRESH,
		"refresh",
		"Refresh the screen",
		CON_MENU,
		{ CTRL('r'), CTRL('l'), -1},
		2
	},
	{
		KEY_CMD_RELOAD,
		"reload",
		"Reread directory content",
		CON_MENU,
		{ 'r', -1 },
		1
	},
	{
		KEY_CMD_TOGGLE_SHOW_HIDDEN_FILES,
		"toggle_hidden_files",
		"Toggle ShowHiddenFiles option",
		CON_MENU,
		{ 'H', -1 },
		1
	},
	{
		KEY_CMD_GO_MUSIC_DIR,
		"go_to_music_directory",
		"Go to the music directory (requires config option)",
		CON_MENU,
		{ 'm', -1 },
		1
	},
	{
		KEY_CMD_PLIST_DEL,
		"delete_from_playlist",
		"Delete an item from the playlist",
		CON_MENU,
		{ 'd', -1 },
		1
	},
	{
		KEY_CMD_MENU_SEARCH,
		"search_menu",
		"Search the menu",
		CON_MENU,
		{ 'g', '/', -1 },
		2
	},
	{
		KEY_CMD_PLIST_SAVE,
		"save_playlist",
		"Save the playlist",
		CON_MENU,
		{ 'V', -1 },
		1
	},
	{
		KEY_CMD_TOGGLE_SHOW_TIME,
		"toggle_show_time",
		"Toggle ShowTime option",
		CON_MENU,
		{ CTRL('t'), -1},
		1
	},
	{
		KEY_CMD_TOGGLE_SHOW_FORMAT,
		"toggle_show_format",
		"Toggle ShowFormat option",
		CON_MENU,
		{ CTRL('f'), -1 },
		1
	},
	{
		KEY_CMD_GO_URL,
		"go_url",
		"Play from the URL",
		CON_MENU,
		{ 'o', -1 },
		1
	},
	{
		KEY_CMD_GO_TO_PLAYING_FILE,
		"go_to_playing_file",
		"Go to the currently playing file's directory",
		CON_MENU,
		{ 'G', -1 },
		1
	},
	{
		KEY_CMD_GO_DIR,
		"go_to_a_directory",
		"Go to a directory",
		CON_MENU,
		{ 'i', -1 },
		1
	},
	{
		KEY_CMD_GO_DIR_UP,
		"go_up",
		"Go to '..'",
		CON_MENU,
		{ 'U', -1 },
		1
	},
	{
		KEY_CMD_CANCEL,
		"cancel",
		"Exit from an entry",
		CON_ENTRY,
		{ CTRL('x'), KEY_ESCAPE, -1 },
		2
	},
	{
		KEY_CMD_SEEK_FORWARD_5,
		"seek_forward_fast",
		"Silent seek forward by 5s",
		CON_MENU,
		{ ']', -1 },
		1
	},
	{
		KEY_CMD_SEEK_BACKWARD_5,
		"seek_backward_fast",
		"Silent seek backward by 5s",
		CON_MENU,
		{ '[', -1 },
		1
	},
	{
		KEY_CMD_VOLUME_10,
		"volume_10",
		"Set volume to 10%",
		CON_MENU,
		{ '1' | META_KEY_FLAG, -1 },
		1
	},
	{
		KEY_CMD_VOLUME_20,
		"volume_20",
		"Set volume to 20%",
		CON_MENU,
		{ '2' | META_KEY_FLAG, -1 },
		1
	},
	{
		KEY_CMD_VOLUME_30,
		"volume_30",
		"Set volume to 30%",
		CON_MENU,
		{ '3' | META_KEY_FLAG, -1 },
		1
	},
	{
		KEY_CMD_VOLUME_40,
		"volume_40",
		"Set volume to 40%",
		CON_MENU,
		{ '4' | META_KEY_FLAG, -1 },
		1
	},
	{
		KEY_CMD_VOLUME_50,
		"volume_50",
		"Set volume to 50%",
		CON_MENU,
		{ '5' | META_KEY_FLAG, -1 },
		1
	},
	{
		KEY_CMD_VOLUME_60,
		"volume_60",
		"Set volume to 60%",
		CON_MENU,
		{ '6' | META_KEY_FLAG, -1 },
		1
	},
	{
		KEY_CMD_VOLUME_70,
		"volume_70",
		"Set volume to 70%",
		CON_MENU,
		{ '7' | META_KEY_FLAG, -1 },
		1
	},
	{
		KEY_CMD_VOLUME_80,
		"volume_80",
		"Set volume to 80%",
		CON_MENU,
		{ '8' | META_KEY_FLAG, -1 },
		1
	},
	{
		KEY_CMD_VOLUME_90,
		"volume_90",
		"Set volume to 90%",
		CON_MENU,
		{ '9' | META_KEY_FLAG, -1 },
		1
	},
 	{
 		KEY_CMD_MARK_START,
 		"mark_start",
 		"Mark the start of a block",
 		CON_MENU,
 		{ '\'', -1 },
 		1
 	},
 	{
 		KEY_CMD_MARK_END,
 		"mark_end",
 		"Mark the end of a block",
 		CON_MENU,
 		{ '\"', -1 },
 		1
 	},
 	{
 		KEY_CMD_FAST_DIR_1,
 		"go_to_fast_dir1",
 		"Go to a fast dir 1",
 		CON_MENU,
 		{ '!', -1 },
 		1
 	},
 	{
 		KEY_CMD_FAST_DIR_2,
 		"go_to_fast_dir2",
 		"Go to a fast dir 2",
 		CON_MENU,
 		{ '@', -1 },
 		1
 	},
 	{
 		KEY_CMD_FAST_DIR_3,
 		"go_to_fast_dir3",
 		"Go to a fast dir 3",
 		CON_MENU,
 		{ '#', -1 },
 		1
 	},
 	{
 		KEY_CMD_FAST_DIR_4,
 		"go_to_fast_dir4",
 		"Go to a fast dir 4",
 		CON_MENU,
 		{ '$', -1 },
 		1
 	},
 	{
 		KEY_CMD_FAST_DIR_5,
 		"go_to_fast_dir5",
 		"Go to a fast dir 5",
 		CON_MENU,
 		{ '%', -1 },
 		1
 	},
 	{
 		KEY_CMD_FAST_DIR_6,
 		"go_to_fast_dir6",
 		"Go to a fast dir 6",
 		CON_MENU,
 		{ '^', -1 },
 		1
 	},
 	{
 		KEY_CMD_FAST_DIR_7,
 		"go_to_fast_dir7",
 		"Go to a fast dir 7",
 		CON_MENU,
 		{ '&', -1 },
 		1
 	},
 	{
 		KEY_CMD_FAST_DIR_8,
 		"go_to_fast_dir8",
 		"Go to a fast dir 8",
 		CON_MENU,
 		{ '*', -1 },
 		1
 	},
 	{
 		KEY_CMD_FAST_DIR_9,
 		"go_to_fast_dir9",
 		"Go to a fast dir 9",
 		CON_MENU,
 		{ '(', -1 },
 		1
 	},
 	{
 		KEY_CMD_FAST_DIR_10,
 		"go_to_fast_dir10",
 		"Go to a fast dir 10",
 		CON_MENU,
 		{ ')', -1 },
 		1
  	},
 	{
 		KEY_CMD_HISTORY_UP,
 		"history_up",
 		"Go to the previous entry in the history (entry)",
 		CON_ENTRY,
 		{ KEY_UP, -1 },
 		1
 	},
 	{
 		KEY_CMD_HISTORY_DOWN,
 		"history_down",
 		"Go to the next entry in the history (entry)",
 		CON_ENTRY,
 		{ KEY_DOWN, -1 },
 		1
  	},
 	{
 		KEY_CMD_DELETE_START,
 		"delete_to_start",
 		"Delete to start of line (entry)",
 		CON_ENTRY,
 		{ CTRL('u'), -1 },
 		1
  	},
 	{
 		KEY_CMD_DELETE_END,
 		"delete_to_end",
 		"Delete to end of line (entry)",
 		CON_ENTRY,
 		{ CTRL('k'), -1 },
 		1
  	},
 	{
 		KEY_CMD_TOGGLE_MIXER,
 		"toggle_mixer",
 		"Toggles the mixer channel",
 		CON_MENU,
 		{ 'x', -1 },
 		1
  	},
 	{
 		KEY_CMD_TOGGLE_SOFTMIXER,
 		"toggle_softmixer",
 		"Toggles the software-mixer",
 		CON_MENU,
 		{ 'w', -1 },
 		1
  	},
 	{
 		KEY_CMD_TOGGLE_EQUALIZER,
 		"toggle_equalizer",
 		"Toggles the equalizer",
 		CON_MENU,
 		{ 'E', -1 },
 		1
  	},
 	{
 		KEY_CMD_EQUALIZER_REFRESH,
 		"equalizer_refresh",
 		"Reload EQ-presets",
 		CON_MENU,
 		{ 'e', -1 },
 		1
  	},
 	{
 		KEY_CMD_EQUALIZER_PREV,
 		"equalizer_prev",
 		"Select previous equalizer-preset",
 		CON_MENU,
 		{ 'K', -1 },
 		1
  	},
 	{
 		KEY_CMD_EQUALIZER_NEXT,
 		"equalizer_next",
 		"Select next equalizer-preset",
 		CON_MENU,
 		{ 'k', -1 },
 		1
  	},
 	{
 		KEY_CMD_TOGGLE_MAKE_MONO,
 		"toggle_make_mono",
		"Toggle mono-mixing",
 		CON_MENU,
 		{ 'J', -1 },
 		1
  	},
 	{
 		KEY_CMD_PLIST_MOVE_UP,
 		"plist_move_up",
 		"Move playlist item up",
 		CON_MENU,
 		{ 'u', -1 },
 		1
  	},
 	{
 		KEY_CMD_PLIST_MOVE_DOWN,
 		"plist_move_down",
 		"Move playlist item down",
 		CON_MENU,
 		{ 'j', -1 },
 		1
  	},
 	{
 		KEY_CMD_ADD_STREAM,
 		"plist_add_stream",
 		"Add a URL to the playlist using entry",
 		CON_MENU,
 		{ CTRL('U'), -1 },
 		1
  	},
 	{
 		KEY_CMD_THEME_MENU,
 		"theme_menu",
 		"Switch to the theme selection menu",
 		CON_MENU,
 		{ 'T', -1 },
 		1
  	},
 	{
 		KEY_CMD_EXEC1,
 		"exec_command1",
 		"Execute ExecCommand1",
 		CON_MENU,
 		{ KEY_F(1), -1 },
 		1
  	},
 	{
 		KEY_CMD_EXEC2,
 		"exec_command2",
 		"Execute ExecCommand2",
 		CON_MENU,
 		{ KEY_F(2), -1 },
 		1
  	},
 	{
 		KEY_CMD_EXEC3,
 		"exec_command3",
 		"Execute ExecCommand3",
 		CON_MENU,
 		{ KEY_F(3), -1 },
 		1
  	},
 	{
 		KEY_CMD_EXEC4,
 		"exec_command4",
 		"Execute ExecCommand4",
 		CON_MENU,
 		{ KEY_F(4), -1 },
 		1
  	},
 	{
 		KEY_CMD_EXEC5,
 		"exec_command5",
 		"Execute ExecCommand5",
 		CON_MENU,
 		{ KEY_F(5), -1 },
 		1
  	},
 	{
 		KEY_CMD_EXEC6,
 		"exec_command6",
 		"Execute ExecCommand6",
 		CON_MENU,
 		{ KEY_F(6), -1 },
 		1
  	},
 	{
 		KEY_CMD_EXEC7,
 		"exec_command7",
 		"Execute ExecCommand7",
 		CON_MENU,
 		{ KEY_F(7), -1 },
 		1
  	},
 	{
 		KEY_CMD_EXEC8,
 		"exec_command8",
 		"Execute ExecCommand8",
 		CON_MENU,
 		{ KEY_F(8), -1 },
 		1
  	},
 	{
 		KEY_CMD_EXEC9,
 		"exec_command9",
 		"Execute ExecCommand9",
 		CON_MENU,
 		{ KEY_F(9), -1 },
 		1
  	},
 	{
 		KEY_CMD_EXEC10,
 		"exec_command10",
 		"Execute ExecCommand10",
 		CON_MENU,
 		{ KEY_F(10), -1 },
 		1
  	},
	{
		KEY_CMD_LYRICS,
		"show_lyrics",
		"Display lyrics of the current song (if available)",
		CON_MENU,
		{ 'L',	-1 },
		1
	},
 	{
 		KEY_CMD_TOGGLE_PLAYLIST_FULL_PATHS,
 		"playlist_full_paths",
 		"Toggle displaying full paths in the playlist",
 		CON_MENU,
 		{ 'P', -1 },
 		1
  	},
	{
		KEY_CMD_QUEUE_TOGGLE_FILE,
		"enqueue_file",
		"Add (or remove) a file to (from) queue",
		CON_MENU,
		{ 'z', -1 },
		1
	},
	{
		KEY_CMD_QUEUE_CLEAR,
		"clear_queue",
		"Clear the queue",
		CON_MENU,
		{ 'Z', -1 },
		1
	}
};

static struct special_keys
{
	char *name;
	int key;
} special_keys[] = {
	{ "DOWN",		KEY_DOWN },
	{ "UP",			KEY_UP },
	{ "LEFT",		KEY_LEFT },
	{ "RIGHT",		KEY_RIGHT },
	{ "HOME",		KEY_HOME },
	{ "BACKSPACE",		KEY_BACKSPACE },
	{ "DEL",		KEY_DC },
	{ "INS",		KEY_IC },
	{ "ENTER",		'\n' },
	{ "PAGE_UP",		KEY_PPAGE },
	{ "PAGE_DOWN",		KEY_NPAGE },
	{ "TAB",		'\t' },
	{ "END",		KEY_END },
	{ "KEYPAD_CENTER",	KEY_B2 },
	{ "SPACE",		' ' },
	{ "ESCAPE",		KEY_ESCAPE },
	{ "F1",			KEY_F(1) },
	{ "F2",			KEY_F(2) },
	{ "F3",			KEY_F(3) },
	{ "F4",			KEY_F(4) },
	{ "F5",			KEY_F(5) },
	{ "F6",			KEY_F(6) },
	{ "F7",			KEY_F(7) },
	{ "F8",			KEY_F(8) },
	{ "F9",			KEY_F(9) },
	{ "F10",		KEY_F(10) },
	{ "F11",		KEY_F(11) },
	{ "F12",		KEY_F(12) }
};

#define COMMANDS_NUM		(ARRAY_SIZE(commands))
#define SPECIAL_KEYS_NUM	(ARRAY_SIZE(special_keys))

/* Number of chars from the left where the help message starts
 * (skipping the key list). */
#define HELP_INDENT	15

static char *help[COMMANDS_NUM];

enum key_cmd get_key_cmd (const enum key_context context,
                          const struct iface_key *key)
{
	int k;
	size_t i;

	k = (key->type == IFACE_KEY_CHAR) ? key->key.ucs : key->key.func;

	for (i = 0; i < COMMANDS_NUM; i += 1) {
		if (commands[i].context == context) {
			int j = 0;

			while (commands[i].keys[j] != -1) {
				if (commands[i].keys[j++] == k)
					return commands[i].cmd;
			}
		}
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
	size_t i;

	if (strlen(symbol) == 1) {
		/* Just a regular char */
		static bool digit_key_warned = false;
		if (!digit_key_warned && isdigit (symbol[0])) {
			fprintf (stderr,
			         "\n\tUsing digits as keys is deprecated as they may"
			         "\n\tbe used for specific purposes in release 2.6.\n");
			xsleep (5, 1);
			digit_key_warned = true;
		}
		return symbol[0];
	}

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

		return symbol[2] | META_KEY_FLAG;
	}

	/* Special keys. */
	for (i = 0; i < SPECIAL_KEYS_NUM; i += 1) {
		if (!strcasecmp(special_keys[i].name, symbol))
			return special_keys[i].key;
	}

	return -1;
}

/* Remove a single key from the default key definition for a command. */
static void clear_default_key (int key)
{
	size_t cmd_ix;

	for (cmd_ix = 0; cmd_ix < COMMANDS_NUM; cmd_ix += 1) {
		int key_ix;

		for (key_ix = 0; key_ix < commands[cmd_ix].default_keys; key_ix++) {
			if (commands[cmd_ix].keys[key_ix] == key)
				break;
		}

		if (key_ix == commands[cmd_ix].default_keys)
				continue;

		while (commands[cmd_ix].keys[key_ix] != -1) {
			commands[cmd_ix].keys[key_ix] = commands[cmd_ix].keys[key_ix + 1];
			key_ix += 1;
		}

		commands[cmd_ix].default_keys -= 1;

		break;
	}
}

/* Remove default keys definition for a command. Return 0 on error. */
static void clear_default_keys (size_t cmd_ix)
{
	assert (cmd_ix < COMMANDS_NUM);

	commands[cmd_ix].default_keys = 0;
	commands[cmd_ix].keys[0] = -1;
}

/* Add a key to the command defined in the keymap file in line
 * line_num (used only when reporting an error). */
static void add_key (const int line_num, size_t cmd_ix, const char *key_symbol)
{
	int i, key;

	assert (cmd_ix < COMMANDS_NUM);

	key = parse_key (key_symbol);
	if (key == -1)
		keymap_parse_error (line_num, "bad key sequence");

	clear_default_key (key);

	for (i = commands[cmd_ix].default_keys;
	     commands[cmd_ix].keys[i] != -1;
	     i += 1) {
		if (commands[cmd_ix].keys[i] == key)
			return;
	}

	if (i == ARRAY_SIZE(commands[cmd_ix].keys) - 1)
		keymap_parse_error (line_num, "too many keys defined");

	commands[cmd_ix].keys[i] = key;
	commands[cmd_ix].keys[i + 1] = -1;
}

/* Find command entry by command name; return COMMANDS_NUM if not found. */
static size_t find_command_name (const char *command)
{
	size_t result;

	for (result = 0; result < COMMANDS_NUM; result += 1) {
		if (!(strcasecmp(commands[result].name, command)))
			break;
	}

	return result;
}

/* Load a key map from the file. */
static void load_key_map (const char *file_name)
{
	FILE *file;
	char *line;
	int line_num = 0;
	size_t cmd_ix;

	if (!(file = fopen(file_name, "r")))
		fatal ("Can't open keymap file: %s", xstrerror (errno));

	/* Read lines in format:
	 * COMMAND = KEY [KEY ...]
	 * Blank lines and beginning with # are ignored, see example_keymap. */
	while ((line = read_line(file))) {
		char *command, *tmp, *key;

		line_num++;
		if (line[0] == '#' || !(command = strtok(line, " \t"))) {

			/* empty line or a comment */
			free (line);
			continue;
		}

		cmd_ix = find_command_name (command);
		if (cmd_ix == COMMANDS_NUM)
			keymap_parse_error (line_num, "unknown command");

		tmp = strtok(NULL, " \t");
		if (!tmp || (strcmp(tmp, "=") && strcmp(tmp, "+=")))
			keymap_parse_error (line_num, "expected '=' or '+='");

		if (strcmp(tmp, "+=")) {
			if (commands[cmd_ix].keys[commands[cmd_ix].default_keys] != -1)
				keymap_parse_error (line_num, "command previously bound");
			clear_default_keys (cmd_ix);
		}

		while ((key = strtok(NULL, " \t")))
			add_key (line_num, cmd_ix, key);

		free (line);
	}

	fclose (file);
}

/* Get a nice key name.
 * Returned memory may be static. */
static char *get_key_name (const int key)
{
	size_t i;
	static char key_str[4];

	/* Search for special keys */
	for (i = 0; i < SPECIAL_KEYS_NUM; i += 1) {
		if (special_keys[i].key == key)
			return special_keys[i].name;
	}

	/* CTRL combination */
	if (!(key & ~CTRL_KEY_CODE)) {
		key_str[0] = '^';
		key_str[1] = key + 0x60;
		key_str[2] = 0;

		return key_str;
	}

	/* Meta keys */
	if (key & META_KEY_FLAG) {
		strcpy (key_str, "M-");
		key_str[2] = key & ~META_KEY_FLAG;
		key_str[3] = 0;

		return key_str;
	}

	/* Normal key */
	key_str[0] = key;
	key_str[1] = 0;

	return key_str;
}

/* Check if keys for cmd1 and cmd2 are different, if not, issue an error. */
static void compare_keys (struct command *cmd1, struct command *cmd2)
{
	int i = 0;

	while (cmd1->keys[i] != -1) {
		int j = 0;

		while (cmd2->keys[j] != -1 && cmd2->keys[j] != cmd1->keys[i])
			j++;
		if (cmd2->keys[j] != -1)
			fatal ("Key %s is defined for %s and %s!",
					get_key_name(cmd2->keys[j]),
					cmd1->name, cmd2->name);
		i++;
	}
}

/* Check that no key is defined more than once. */
static void check_keys ()
{
	size_t i, j;

	for (i = 0; i < COMMANDS_NUM; i += 1) {
		for (j = 0; j < COMMANDS_NUM; j += 1) {
			if (j != i && commands[i].context == commands[j].context)
				compare_keys (&commands[i], &commands[j]);
		}
	}
}

/* Return a string contains the list of keys used for command.
 * Returned memory is static. */
static char *get_command_keys (const int idx)
{
	static char keys[64];
	int i = 0;

	keys[0] = 0;

	while (commands[idx].keys[i] != -1) {
		strncat (keys, get_key_name(commands[idx].keys[i]),
				sizeof(keys) - strlen(keys) - 1);
		strncat (keys, " ", sizeof(keys) - strlen(keys) - 1);
		i++;
	}

	/* strip the last space */
	if (keys[0] != 0)
		keys[strlen (keys) - 1] = 0;

	return keys;
}

/* Make the help message for keys. */
static void make_help ()
{
	size_t i;
	const char unassigned[] = " [unassigned]";

	for (i = 0; i < COMMANDS_NUM; i += 1) {
		size_t len;

		len = HELP_INDENT + strlen (commands[i].help) + 1;
		if (commands[i].keys[0] == -1)
			len += strlen (unassigned);
		help[i] = xcalloc (sizeof(char), len);
		strncpy (help[i], get_command_keys(i), HELP_INDENT);
		if (strlen(help[i]) < HELP_INDENT)
			memset (help[i] + strlen(help[i]), ' ',
					HELP_INDENT - strlen(help[i]));
		strcpy (help[i] + HELP_INDENT, commands[i].help);
		if (commands[i].keys[0] == -1)
			strcat (help[i], unassigned);
	}
}

/* Load key map. Set default keys if necessary. */
void keys_init ()
{
	char *file = find_keymap_file ();

	if (file) {
		load_key_map (file);
		check_keys ();
	}

	make_help ();
}

/* Free the help message. */
void keys_cleanup ()
{
	size_t i;

	for (i = 0; i < COMMANDS_NUM; i += 1)
		free (help[i]);
}

/* Return an array of strings with the keys help. The number of lines is put
 * in num. */
char **get_keys_help (int *num)
{
	*num = (int) COMMANDS_NUM;
	return help;
}

/* Find command entry by key command; return COMMANDS_NUM if not found. */
static size_t find_command_cmd (const enum key_cmd cmd)
{
	size_t result;

	for (result = 0; result < COMMANDS_NUM; result += 1) {
		if (commands[result].cmd == cmd)
			break;
	}

	return result;
}

/* Return true iff the help key is still 'h'. */
bool is_help_still_h ()
{
	size_t cmd_ix;

	cmd_ix = find_command_cmd (KEY_CMD_HELP);

	assert (cmd_ix < COMMANDS_NUM);

	return commands[cmd_ix].keys[0] == 'h';
}
