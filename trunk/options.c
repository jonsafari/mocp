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

#include <assert.h>
#include <string.h>
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include "main.h"
#include "log.h"
#include "options.h"

#define OPTIONS_MAX	64
#define OPTION_NAME_MAX	32

enum option_type
{
	OPTION_INT,
	OPTION_STR,
	OPTION_ANY
};

union option_value
{
	char *str;
	int num;
};

struct option
{
	char name[OPTION_NAME_MAX];
	enum option_type type;
	union option_value value;
	int ignore_in_config;
};

static struct option options[OPTIONS_MAX];
static int options_num = 0;

/* Add an integer option to the options table. This is intended to be used at
 * initialization to make a table of valid options and its default values. */
static void option_add_int (const char *name, const int value)
{
	assert (strlen(name) < OPTION_NAME_MAX);
	assert (options_num < OPTIONS_MAX);

	strcpy (options[options_num].name, name);
	options[options_num].type = OPTION_INT;
	options[options_num].value.num = value;
	options[options_num].ignore_in_config = 0;

	options_num++;
}

/* Add an string option to the options table. This is intended to be used at
 * initialization to make a table of valid options and its default values. */
static void option_add_str (const char *name, const char *value)
{
	assert (strlen(name) < OPTION_NAME_MAX);
	assert (options_num < OPTIONS_MAX);

	strcpy (options[options_num].name, name);
	options[options_num].type = OPTION_STR;
	options[options_num].value.str = xstrdup (value);
	options[options_num].ignore_in_config = 0;

	options_num++;
}
/* Return an index on an option in the options table. If there is no such
 * option return -1. */
static int find_option (const char *name, enum option_type type)
{
	int i;

	for (i = 0; i < options_num; i++) {
		if (!strcasecmp(options[i].name, name)) {
			if (type != OPTION_ANY && options[i].type != type)
				return -1;
			return i;
		}
	}

	return -1;
}

/* Set an integer option to the value. */
void option_set_int (const char *name, const int value)
{
	int i = find_option(name, OPTION_INT);

	if (i == -1)
		fatal ("Tried to set wrong option '%s'!", name);
	options[i].value.num = value;
}

/* Set a string option to the value. The string is duplicated. */
void option_set_str (const char *name, const char *value)
{
	int opt = find_option(name, OPTION_STR);

	if (opt == -1)
		fatal ("Tried to set wrong option '%s'!", name);
	
	if (options[opt].value.str)
		free (options[opt].value.str);
	options[opt].value.str = xstrdup (value);
}

void option_ignore_config (const char *name)
{
	int opt = find_option(name, OPTION_ANY);

	if (opt == -1)
		fatal ("Tried to set wrong option '%s'!", name);

	options[opt].ignore_in_config = 1;
}

/* Make a table of options and its default values. */
void options_init ()
{
	memset (options, 0, sizeof(options));

	option_add_int ("ReadTags", 1);
	option_add_str ("MusicDir", NULL);
	option_add_int ("StartInMusicDir", 0);
	option_add_int ("ShowStreamErrors", 0);
	option_add_int ("Repeat", 0);
	option_add_int ("Shuffle", 0);
	option_add_int ("AutoNext", 1);
	option_add_str ("Sort", "FileName");
	option_add_str ("FormatString",
			"%(n:%n :)%(a:%a - :)%(t:%t:)%(A: \\(%A\\):)");
	option_add_int ("OutputBuffer", 512);
	option_add_str ("OSSDevice", "/dev/dsp");
	option_add_str ("OSSMixerDevice", "/dev/mixer");
	option_add_str ("OSSMixerChannel", "pcm");
	option_add_str ("SoundDriver", "OSS");
	option_add_int ("ShowHiddenFiles", 1);
	option_add_str ("AlsaDevice", "default");
	option_add_str ("AlsaMixer", "PCM");
	option_add_int ("HideFileExtension", 0);
	option_add_int ("ShowFormat", 1);
	option_add_str ("ShowTime", "IfAvailable");
	option_add_str ("Theme", NULL);
}

/* Return 1 if a parameter to an integer option is valid. */
int check_int_option (const char *name, const int val)
{
	/* YES/NO options */
	if (!strcasecmp(name, "ReadTags")
			|| !strcasecmp(name, "ShowStreamErrors")
			|| !strcasecmp(name, "Repeat")
			|| !strcasecmp(name, "Shuffle")
			|| !strcasecmp(name, "AutoNext")
			|| !strcasecmp(name, "ShowHiddenFiles")
			|| !strcasecmp(name, "StartInMusicDir")
			|| !strcasecmp(name, "HideFileExtension")
			|| !strcasecmp(name, "ShowFormat")
			) {
		if (!(val == 1 || val == 0))
			return 0;
	}
	else if (!strcasecmp(name, "OutputBuffer")) {
		if (val < 128)
			return 0;
	}
	return 1;
}

/* Return 1 if a parameter to a string option is valid. */
int check_str_option (const char *name, const char *val)
{
	if (!strcasecmp(name, "Sort")) {
		if (strcasecmp(val, "FileName"))
			return 0;
	}
	else if (!strcasecmp(name, "OSSMixerChannel")) {
		if (strcasecmp(val, "master") && strcasecmp(val, "pcm"))
			return 0;
	}
	else if (!strcasecmp(name, "OutputDriver")) {
		if (!proper_sound_driver(val))
			return 0;
	}
	else if (!strcasecmp(name, "ShowTime")) {
		if (strcasecmp(val, "yes") && strcasecmp(val, "no")
				&& strcasecmp(val, "IfAvailable"))
			return 0;
	}
	
	return 1;
}

/* Set an option read from the configuration file. Return 0 on error. */
static int set_option (const char *name, const char *value)
{
	int i = find_option (name, OPTION_ANY);

	if (i == -1)
		return 0;

	if (options[i].ignore_in_config)
		return 1;

	if (options[i].type == OPTION_INT) {
		int num;

		if (!strcasecmp(value, "yes"))
			num = 1;
		else if (!strcasecmp(value, "no"))
			num = 0;
		else {
			char *end;

			num = strtol (value, &end, 10);
			if (*end)
				return 0;
		}
		
		if (!check_int_option(name, num))
			return 0;
		option_set_int (name, num);
	}
	else {
		if (!check_str_option(name, value))
			return 0;
		option_set_str (name, value);
	}
	
	return 1;
}

/* Parse the configuration file. */
void options_parse (const char *config_file)
{
	int ch;
	int comm = 0; /* comment? */
	int eq = 0; /* equal character appeard? */
	int quote = 0; /* are we in quotes? */
	int esc = 0;
	char opt_name[30];
	char opt_value[100];
	int line = 1;
	int name_pos = 0;
	int value_pos = 0;
	FILE *file;

	if (!(file = fopen(config_file, "r"))) {
		logit ("Can't open config file: %s", strerror(errno));
		return;
	}

	while ((ch = getc(file)) != EOF) {

		/* Skip comment */
		if (comm && ch != '\n')
			continue;
		
		/* Interpret parameter */
		if (ch == '\n') {
			comm = 0;

			opt_name[name_pos] = 0;
			opt_value[value_pos] = 0;

			if (name_pos) {
				if (value_pos == 0
						|| !set_option(opt_name,
							opt_value))
					fatal ("Error in config file, line %d.",
							line);
			}

			opt_name[0] = 0;
			opt_value[0] = 0;
			name_pos = 0;
			value_pos = 0;
			eq = 0;
			quote = 0;
			esc = 0;

			line++;
		}

		/* Turn on comment */
		else if (ch == '#' && !quote)
			comm = 1;

		/* Turn on quote */
		else if (!quote && !esc && (ch == '"'))
			quote = 1;

		/* Turn off quote */
		else if (!esc && quote && ch == '"')
			quote = 0;

		else if (ch == '=' && !quote) {
			if (eq)
				fatal ("Error in config file, line %d.",
						line);
			if (!opt_name[0])
				fatal ("Error in config file, line %d.",
						line);
				eq = 1;
			opt_value[0] = 0;
		}

		/* Turn on escape */
		else if (ch == '\\' && !esc)
			esc = 1;

		/* Add char to parameter value */
		else if ((!isblank(ch) || quote) && eq) {
			if (esc && ch != '"') {
				if (sizeof(opt_value) == value_pos)
					fatal ("Error in config file, line %d "
							"is too long.", line);
				opt_value[value_pos++] = '\\';
			}
			
			if (sizeof(opt_value) == value_pos)
				fatal ("Error in config file, line %d is "
						"too long.", line);
			opt_value[value_pos++] = ch;
			esc = 0;
		}

		/* Add char to parameter name */
		else if (!isblank(ch) || quote) {
			if (sizeof(opt_name) == name_pos)
				fatal ("Error in config file, line %d is "
						"too long.", line);
			opt_name[name_pos++] = ch;
			esc = 0;
		}
	}

	if (opt_name[0] || opt_value[0])
		fatal ("Parse error at the end of the config file (need end of "
				"line?).");

	fclose (file);
}

void options_free ()
{
	int i;
	
	for (i = 0; i < options_num; i++)
		if (options[i].type == OPTION_STR && options[i].value.str)
			free (options[i].value.str);
}

int options_get_int (const char *name)
{
	int i = find_option(name, OPTION_INT);

	if (i == -1)
		fatal ("Tried to get wrong option '%s'!", name);
	
	return options[i].value.num;
}

char *options_get_str (const char *name)
{
	int i = find_option(name, OPTION_STR);

	if (i == -1)
		fatal ("Tried to get wrong option '%s'!", name);

	return options[i].value.str;
}
