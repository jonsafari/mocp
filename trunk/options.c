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

#include <assert.h>
#include <string.h>
#include <strings.h>
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <limits.h>
#include <stdarg.h>

#include "common.h"
#include "log.h"
#include "options.h"

#define OPTIONS_MAX	181
#define OPTION_NAME_MAX	32

enum option_type
{
	OPTION_FREE=0,
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
	int set_in_config;
	unsigned int hash;
	int (*check) (int, ...);
	int count;
	void *constraints;
};

static struct option options[OPTIONS_MAX];
static int options_num = 0;


/* Returns the str's hash using djb2 algorithm */
static unsigned int hash(const char * str)
{
	unsigned int hash = 5381;

	while (*str)
		hash = ((hash << 5) + hash) + tolower(*(str++));
	return hash;
}

/* Return an index to an option in the options hashtable. If there is no such
 * option return -1. */
static int find_option (const char *name, enum option_type type)
{
	unsigned int h=hash(name),i,init_pos=h%OPTIONS_MAX;
	
	
	for(i=init_pos;i<OPTIONS_MAX;i++)
		if(options[i].type==OPTION_FREE)
			return -1;
		else if(h == options[i].hash && (type == options[i].type||type == OPTION_ANY))
			if(!strcasecmp(name, options[i].name))
		    return i;

	for(i=0;i<init_pos;i++)
		if(options[i].type==OPTION_FREE)
			return -1;
		else if(h == options[i].hash && (type == options[i].type || type == OPTION_ANY))
			if(!strcasecmp(name, options[i].name))
		    return i;

	return -1;
}

/* Return an index on a free slot in the options hashtable. If there is no such
 * slot return -1. */
static int find_free (unsigned int h)
{
	unsigned int i;

	assert (options_num < OPTIONS_MAX);
	h%=OPTIONS_MAX;

	for(i=h;i<OPTIONS_MAX;i++)
		if(options[i].type==OPTION_FREE)
			return i;

	for(i=0;i<h;i++)
		if(options[i].type==OPTION_FREE)
			return i;

	return -1;
}

/* Check that a value falls within the specified range(s). */
int option_check_range (int opt, ...)
{
	int rc, ix, int_val;
	char *str_val;
	va_list va;

	assert (opt != -1);
	assert (options[opt].count % 2 == 0);

	rc = 0;
	va_start (va, opt);
	switch (options[opt].type) {
		case OPTION_INT:
			int_val = va_arg (va, int);
			for (ix = 0; ix < options[opt].count; ix += 2) {
				if (int_val >= ((int *) options[opt].constraints)[ix] &&
		    		int_val <= ((int *) options[opt].constraints)[ix + 1])
					rc = 1;
			}
			break;
		case OPTION_STR:
			str_val = va_arg (va, char *);
			for (ix = 0; ix < options[opt].count; ix += 2) {
				if (strcasecmp (str_val, (((char **) options[opt].constraints)[ix])) >= 0 &&
				    strcasecmp (str_val, (((char **) options[opt].constraints)[ix + 1])) <= 0)
					rc = 1;
			}
			break;
		case OPTION_ANY:
		case OPTION_FREE:
			break;
	}
	va_end (va);

	return rc;
}

/* Check that a value is one of the specified values. */
int option_check_discrete (int opt, ...)
{
	int rc, ix, int_val;
	char *str_val;
	va_list va;

	assert (opt != -1);

	rc = 0;
	va_start (va, opt);
	switch (options[opt].type) {
		case OPTION_INT:
			int_val = va_arg (va, int);
			for (ix = 0; ix < options[opt].count; ix += 1) {
				if (int_val == ((int *) options[opt].constraints)[ix])
					rc = 1;
			}
			break;
		case OPTION_STR:
			str_val = va_arg (va, char *);
			for (ix = 0; ix < options[opt].count; ix += 1) {
				if (!strcasecmp(str_val, (((char **) options[opt].constraints)[ix])))
					rc = 1;
			}
			break;
		case OPTION_ANY:
		case OPTION_FREE:
			break;
	}
	va_end (va);

	return rc;
}

/* Check that a string length falls within the specified range(s). */
int option_check_length (int opt, ...)
{
	int rc, ix, str_len;
	va_list va;

	assert (opt != -1);
	assert (options[opt].count % 2 == 0);
	assert (options[opt].type == OPTION_STR);

	rc = 0;
	va_start (va, opt);
	str_len = strlen (va_arg (va, char *));
	for (ix = 0; ix < options[opt].count; ix += 1) {
		if (str_len >= ((int *) options[opt].constraints)[ix] &&
    		str_len <= ((int *) options[opt].constraints)[ix + 1])
			rc = 1;
	}
	va_end (va);

	return rc;
}

/* Always pass a value as valid. */
int option_check_true (int opt ATTR_UNUSED, ...)
{
	return 1;
}

/* Initializes a position on the options table. This is intended to be used at
 * initialization to make a table of valid options and its default values. */
static int option_init(const char *name, enum option_type type)
{
	unsigned int h=hash(name);
	int pos=find_free(h);
	
	assert (strlen(name) < OPTION_NAME_MAX);
	assert(pos>=0);
	
	strcpy (options[pos].name, name);
	options[pos].hash=h;
	options[pos].type = type;
	options[pos].ignore_in_config = 0;
	options[pos].set_in_config = 0;
	options[pos].check = option_check_true;
	options[pos].count = 0;
	options[pos].constraints = NULL;

	options_num++;
	return pos;
}

/* Add an integer option to the options table. This is intended to be used at
 * initialization to make a table of valid options and their default values. */
static void option_add_int (const char *name, const int value, int (*check) (int, ...), const int count, ...)
{
	int ix, pos;
	va_list va;

	pos = option_init (name, OPTION_INT);
	options[pos].value.num = value;
	options[pos].check = check;
	options[pos].count = count;
	if (count > 0) {
		options[pos].constraints = xcalloc (count, sizeof (int));
		va_start (va, count);
		for (ix = 0; ix < count; ix += 1)
			((int *) options[pos].constraints)[ix] = va_arg (va, int);
		va_end (va);
	}
}

/* Add a string option to the options table. This is intended to be used at
 * initialization to make a table of valid options and their default values. */
static void option_add_str (const char *name, const char *value, int (*check) (int, ...), const int count, ...)
{
	int ix, pos;
	va_list va;

	pos = option_init (name, OPTION_STR);
	options[pos].value.str = xstrdup (value);
	options[pos].check = check;
	options[pos].count = count;
	if (count > 0) {
		va_start (va, count);
		if (check == option_check_length) {
			options[pos].constraints = xcalloc (count, sizeof (int));
			for (ix = 0; ix < count; ix += 1)
				((int *) options[pos].constraints)[ix] = va_arg (va, int);
		} else {
			options[pos].constraints = xcalloc (count, sizeof (char *));
			for (ix = 0; ix < count; ix += 1)
				((char **) options[pos].constraints)[ix] = xstrdup (va_arg (va, char *));
		}
		va_end (va);
	}
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

#define CHECK_DISCRETE(c)	option_check_discrete, (c)
#define CHECK_RANGE(c)		option_check_range, (2 * (c))
#define CHECK_LENGTH(c)		option_check_length, (2 * (c))
#define CHECK_NONE			option_check_true, 0

/* Make a table of options and its default values. */
void options_init ()
{
	memset (options, 0, sizeof(options));

	option_add_int ("ReadTags", 1, CHECK_DISCRETE(2), 0, 1);
	option_add_str ("MusicDir", NULL, CHECK_NONE);
	option_add_int ("StartInMusicDir", 0, CHECK_DISCRETE(2), 0, 1);
	option_add_int ("ShowStreamErrors", 0, CHECK_DISCRETE(2), 0, 1);
	option_add_int ("Repeat", 0, CHECK_DISCRETE(2), 0, 1);
	option_add_int ("Shuffle", 0, CHECK_DISCRETE(2), 0, 1);
	option_add_int ("AutoNext", 1, CHECK_DISCRETE(2), 0, 1);
	option_add_str ("Sort", "FileName", CHECK_DISCRETE(1), "FileName");
	option_add_str ("FormatString",
			"%(n:%n :)%(a:%a - :)%(t:%t:)%(A: \\(%A\\):)", CHECK_NONE);
	option_add_int ("OutputBuffer", 512, CHECK_RANGE(1), 128, INT_MAX);
	option_add_str ("OSSDevice", "/dev/dsp", CHECK_NONE);
	option_add_str ("OSSMixerDevice", "/dev/mixer", CHECK_NONE);
	option_add_str ("OSSMixerChannel", "pcm", CHECK_NONE);
	option_add_str ("OSSMixerChannel2", "master", CHECK_NONE);
	option_add_str ("SoundDriver", "Jack, ALSA, OSS", CHECK_NONE);
	option_add_int ("ShowHiddenFiles", 1, CHECK_DISCRETE(2), 0, 1);
	option_add_str ("AlsaDevice", "default", CHECK_NONE);
	option_add_str ("AlsaMixer", "PCM", CHECK_NONE);
	option_add_str ("AlsaMixer2", "Master", CHECK_NONE);
	option_add_int ("HideFileExtension", 0, CHECK_DISCRETE(2), 0, 1);
	option_add_int ("ShowFormat", 1, CHECK_DISCRETE(2), 0, 1);
	option_add_str ("ShowTime", "IfAvailable",
	                 CHECK_DISCRETE(3), "yes", "no", "IfAvailable");
	option_add_str ("Theme", NULL, CHECK_NONE);
	option_add_str ("XTermTheme", NULL, CHECK_NONE);
	option_add_str ("ForceTheme", NULL, CHECK_NONE); /* Used when -T is set */
	option_add_str ("MOCDir", "~/.moc", CHECK_NONE);
	option_add_int ("UseMmap", 0, CHECK_DISCRETE(2), 0, 1);
	option_add_int ("Precache", 1, CHECK_DISCRETE(2), 0, 1);
	option_add_int ("SavePlaylist", 1, CHECK_DISCRETE(2), 0, 1);
	option_add_str ("Keymap", NULL, CHECK_NONE);
	option_add_int ("SyncPlaylist", 1, CHECK_DISCRETE(2), 0, 1);
	option_add_int ("InputBuffer", 512, CHECK_RANGE(1), 32, INT_MAX);
	option_add_int ("Prebuffering", 64, CHECK_RANGE(1), 0, INT_MAX);
	option_add_str ("JackOutLeft", "alsa_pcm:playback_1", CHECK_NONE);
	option_add_str ("JackOutRight", "alsa_pcm:playback_2", CHECK_NONE);
	option_add_int ("ASCIILines", 0, CHECK_DISCRETE(2), 0, 1);
	option_add_str ("FastDir1", NULL, CHECK_NONE);
	option_add_str ("FastDir2", NULL, CHECK_NONE);
	option_add_str ("FastDir3", NULL, CHECK_NONE);
	option_add_str ("FastDir4", NULL, CHECK_NONE);
	option_add_str ("FastDir5", NULL, CHECK_NONE);
	option_add_str ("FastDir6", NULL, CHECK_NONE);
	option_add_str ("FastDir7", NULL, CHECK_NONE);
	option_add_str ("FastDir8", NULL, CHECK_NONE);
	option_add_str ("FastDir9", NULL, CHECK_NONE);
	option_add_str ("FastDir10", NULL, CHECK_NONE);
	option_add_str ("ExecCommand1", NULL, CHECK_NONE);
	option_add_str ("ExecCommand2", NULL, CHECK_NONE);
	option_add_str ("ExecCommand3", NULL, CHECK_NONE);
	option_add_str ("ExecCommand4", NULL, CHECK_NONE);
	option_add_str ("ExecCommand5", NULL, CHECK_NONE);
	option_add_str ("ExecCommand6", NULL, CHECK_NONE);
	option_add_str ("ExecCommand7", NULL, CHECK_NONE);
	option_add_str ("ExecCommand8", NULL, CHECK_NONE);
	option_add_str ("ExecCommand9", NULL, CHECK_NONE);
	option_add_str ("ExecCommand10", NULL, CHECK_NONE);
	option_add_int ("Mp3IgnoreCRCErrors", 1, CHECK_DISCRETE(2), 0, 1);
	option_add_int ("SeekTime", 1, CHECK_RANGE(1), 1, INT_MAX);
	option_add_int ("SilentSeekTime", 5, CHECK_RANGE(1), 1, INT_MAX);
	option_add_str ("ResampleMethod", "Linear",
	                 CHECK_DISCRETE(5), "SincBestQuality", "SincMediumQuality",
	                                    "SincFastest", "ZeroOrderHold", "Linear");
	option_add_int ("ForceSampleRate", 0, CHECK_RANGE(1), 0, 500000);
	option_add_str ("HTTPProxy", NULL, CHECK_NONE);
	option_add_int ("UseRealtimePriority", 0, CHECK_DISCRETE(2), 0, 1);
	option_add_int ("TagsCacheSize", 256, CHECK_RANGE(1), 0, INT_MAX);
	option_add_int ("PlaylistNumbering", 1, CHECK_DISCRETE(2), 0, 1);
	option_add_str ("Layout1",
			"directory:0,0,50%,100% playlist:50%,0,FILL,100%", CHECK_NONE);
	option_add_str ("Layout2",
			"directory:0,0,100%,100% playlist:0,0,100%,100%", CHECK_NONE);
	option_add_str ("Layout3", NULL, CHECK_NONE);
	option_add_int ("FollowPlayedFile", 1, CHECK_DISCRETE(2), 0, 1);
	option_add_int ("CanStartInPlaylist", 1, CHECK_DISCRETE(2), 0, 1);
	option_add_int ("UseCursorSelection", 0, CHECK_DISCRETE(2), 0, 1);
	option_add_str ("ID3v1TagsEncoding", "WINDOWS-1250", CHECK_NONE);
	option_add_int ("UseRCC", 1, CHECK_DISCRETE(2), 0, 1);
	option_add_int ("UseRCCForFilesystem", 1, CHECK_DISCRETE(2), 0, 1);
	option_add_int ("EnforceTagsEncoding", 0, CHECK_DISCRETE(2), 0, 1);
	option_add_int ("FileNamesIconv", 0, CHECK_DISCRETE(2), 0, 1);
	option_add_int ("NonUTFXterm", 0, CHECK_DISCRETE(2), 0, 1);
	option_add_int ("SetXtermTitle", 1, CHECK_DISCRETE(2), 0, 1);
	option_add_int ("SetScreenTitle", 1, CHECK_DISCRETE(2), 0, 1);
	option_add_int ("PlaylistFullPaths", 1, CHECK_DISCRETE(2), 0, 1);
	option_add_str ("BlockDecorators", "`\"'", CHECK_LENGTH(1), 3, 3);
	option_add_int ("MessageLingerTime", 3, CHECK_RANGE(1), 0, INT_MAX);
	option_add_int ("PrefixQueuedMessages", 1, CHECK_DISCRETE(2), 0, 1);
	option_add_str ("ErrorMessagesQueued", "!", CHECK_NONE);
	option_add_int ("Allow24bitOutput", 0, CHECK_DISCRETE(2), 0, 1);

	option_add_int ("ModPlug_Channels", 2, CHECK_DISCRETE(2), 1, 2);
	option_add_int ("ModPlug_Frequency", 44100,
	                 CHECK_DISCRETE(4), 11025, 22050, 44100, 48000);
	option_add_int ("ModPlug_Bits", 16, CHECK_DISCRETE(3), 8, 16, 32);

	option_add_int ("ModPlug_Oversampling", 1, CHECK_DISCRETE(2), 0, 1);
	option_add_int ("ModPlug_NoiseReduction", 1, CHECK_DISCRETE(2), 0, 1);
	option_add_int ("ModPlug_Reverb", 0, CHECK_DISCRETE(2), 0, 1);
	option_add_int ("ModPlug_MegaBass", 0, CHECK_DISCRETE(2), 0, 1);
	option_add_int ("ModPlug_Surround", 0, CHECK_DISCRETE(2), 0, 1);

	option_add_str ("ModPlug_ResamplingMode", "FIR",
	                 CHECK_DISCRETE(4), "FIR", "SPLINE", "LINEAR", "NEAREST");

	option_add_int ("ModPlug_ReverbDepth", 0, CHECK_RANGE(1), 0, 100);
	option_add_int ("ModPlug_ReverbDelay", 0, CHECK_RANGE(1), 0, INT_MAX);
	option_add_int ("ModPlug_BassAmount", 0, CHECK_RANGE(1), 0, 100);
	option_add_int ("ModPlug_BassRange", 10, CHECK_RANGE(1), 10, 100);
	option_add_int ("ModPlug_SurroundDepth", 0, CHECK_RANGE(1), 0, 100);
	option_add_int ("ModPlug_SurroundDelay", 0, CHECK_RANGE(1), 0, INT_MAX);
	option_add_int ("ModPlug_LoopCount", 0, CHECK_RANGE(1), -1, INT_MAX);

	option_add_int ("TiMidity_Volume", 100, CHECK_RANGE(1), 0, 800);
	option_add_int ("TiMidity_Rate", 44100, CHECK_RANGE(1), 8000, 48000);
		// not sure about the limits... I like 44100
	option_add_int ("TiMidity_Bits", 16, CHECK_DISCRETE(2), 8, 16);
	option_add_int ("TiMidity_Channels", 2, CHECK_DISCRETE(2), 1, 2);
	option_add_str ("TiMidity_Config", NULL, CHECK_NONE);

	option_add_int ("SidPlay2_DefaultSongLength", 180,
	                 CHECK_RANGE(1), 0, INT_MAX);
	option_add_int ("SidPlay2_MinimumSongLength", 0,
	                 CHECK_RANGE(1), 0, INT_MAX);
	option_add_str ("SidPlay2_Database", NULL, CHECK_NONE);
	option_add_int ("SidPlay2_Frequency", 44100, CHECK_RANGE(1), 4000, 48000);
	option_add_int ("SidPlay2_Bits", 16, CHECK_DISCRETE(2), 8, 16);
	option_add_str ("SidPlay2_PlayMode", "M",
	                 CHECK_DISCRETE(4), "M", "S", "L", "R");
	option_add_int ("SidPlay2_Optimisation", 0, CHECK_RANGE(1), 0, 2);
	option_add_int ("SidPlay2_StartAtStart", 1, CHECK_DISCRETE(2), 0, 1);
	option_add_int ("SidPlay2_PlaySubTunes", 1, CHECK_DISCRETE(2), 0, 1);

	option_add_str ("OnSongChange", NULL, CHECK_NONE);
	option_add_str ("OnStop", NULL, CHECK_NONE);

	option_add_int ("Softmixer_SaveState", 1, CHECK_DISCRETE(2), 0, 1);

	option_add_int ("Equalizer_SaveState", 1, CHECK_DISCRETE(2), 0, 1);

	option_add_int ("QueueNextSongReturn", 0, CHECK_DISCRETE(2), 0, 1);

}

/* Return 1 if a parameter to an integer option is valid. */
int check_int_option (const char *name, const int val)
{
	int opt;

	opt = find_option (name, OPTION_INT);
	if (opt == -1)
		return 0;
	return options[opt].check (opt, val);
}

/* Return 1 if a parameter to a string option is valid. */
int check_str_option (const char *name, const char *val)
{
	int opt;

	opt = find_option (name, OPTION_STR);
	if (opt == -1)
		return 0;
	return options[opt].check (opt, val);
}

static int is_deprecated_option (const char *name)
{
	if (!strcmp(name, "TagsIconv"))
		return 1;

	return 0;
}

/* Set an option read from the configuration file. Return 0 on error. */
static int set_option (const char *name, const char *value)
{
	int i = find_option (name, OPTION_ANY);

	if (is_deprecated_option(name)) {
		fprintf (stderr, "Option '%s' was ignored, remove it "
				"from the configuration file.\n", name);
		sleep (1);
		return 1;
	}

	if (i == -1) {
		fprintf (stderr, "Wrong option name: '%s'.", name);
		return 0;
	}

	if (options[i].ignore_in_config) 
		return 1;

	if (options[i].set_in_config) {
		fprintf (stderr, "Tried to set an option that has been already "
				"set in the config file ('%s').", name);
		return 0;
	}

	options[i].set_in_config = 1;

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

/* Check if values of options make sense. This only check options that can't be
 * check without parsing the whole file. */
static void options_sanity_check ()
{
	if (options_get_int("Prebuffering") > options_get_int("InputBuffer"))
		fatal ("Prebuffering is set to a value greater than "
				"InputBuffer.");
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

	options_sanity_check ();

	fclose (file);
}

void options_free ()
{
	int i, ix;
	
	for (i = 0; i < options_num; i++) {
		if (options[i].type == OPTION_STR) {
			if (options[i].value.str)
				free (options[i].value.str);
			options[i].value.str = NULL;
			if (options[i].check != option_check_length) {
				for (ix = 0; ix < options[i].count; ix += 1)
					free (((char **) options[i].constraints)[ix]);
			}
		}
		options[i].check = option_check_true;
		options[i].count = 0;
		if (options[i].constraints)
			free (options[i].constraints);
		options[i].constraints = NULL;
	}
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
