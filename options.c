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

union option_value
{
	char *str;
	int num;
	bool boolean;
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
		else if(h == options[i].hash && type & options[i].type)
			if(!strcasecmp(name, options[i].name))
		    return i;

	for(i=0;i<init_pos;i++)
		if(options[i].type==OPTION_FREE)
			return -1;
		else if(h == options[i].hash && type & options[i].type)
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
	assert (options[opt].type & (OPTION_INT | OPTION_STR));

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

		case OPTION_BOOL:
		case OPTION_SYMB:
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
	assert (options[opt].type & (OPTION_INT | OPTION_SYMB));

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

		case OPTION_SYMB:
			str_val = va_arg (va, char *);
			for (ix = 0; ix < options[opt].count; ix += 1) {
				if (!strcasecmp(str_val, (((char **) options[opt].constraints)[ix])))
					rc = 1;
			}
			break;

		case OPTION_BOOL:
		case OPTION_STR:
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
	assert (is_valid_symbol (name));
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

/* Add a boolean option to the options table. This is intended to be used at
 * initialization to make a table of valid options and their default values. */
static void option_add_bool (const char *name, const bool value)
{
	int pos;

	pos = option_init (name, OPTION_BOOL);
	options[pos].value.boolean = value;
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

/* Add a symbol option to the options table. This is intended to be used at
 * initialization to make a table of valid options and their default values. */
static void option_add_symb (const char *name, const char *value, const int count, ...)
{
	int ix, pos;
	va_list va;

	assert (name != NULL);
	assert (value != NULL);
	assert (count > 0);

	pos = option_init (name, OPTION_SYMB);
	options[pos].value.str = NULL;
	options[pos].check = option_check_discrete;
	options[pos].count = count;
	va_start (va, count);
	options[pos].constraints = xcalloc (count, sizeof (char *));
	for (ix = 0; ix < count; ix += 1) {
		char *val = va_arg (va, char *);
		if (!is_valid_symbol (val))
			fatal ("Invalid symbol in '%s' constraint list.", name);
		((char **) options[pos].constraints)[ix] = xstrdup (val);
		if (!strcasecmp (val, value))
			options[pos].value.str = ((char **) options[pos].constraints)[ix];
	}
	if (!options[pos].value.str)
		fatal ("Invalid default value symbol in '%s'.", name);
	va_end (va);
}

/* Set an integer option to the value. */
void option_set_int (const char *name, const int value)
{
	int i = find_option(name, OPTION_INT | OPTION_BOOL);

	if (i == -1)
		fatal ("Tried to set wrong option '%s'!", name);
	if (options[i].type == OPTION_INT)
		options[i].value.num = value;
	else
		options[i].value.boolean = value ? true : false;
}

/* Set a boolean option to the value. */
void option_set_bool (const char *name, const bool value)
{
	int i = find_option(name, OPTION_BOOL);

	if (i == -1)
		fatal ("Tried to set wrong option '%s'!", name);
	options[i].value.boolean = value;
}

/* Set a symbol option to the value. */
void option_set_symb (const char *name, const char *value)
{
	int opt, ix;

	opt = find_option(name, OPTION_SYMB);
	if (opt == -1)
		fatal ("Tried to set wrong option '%s'!", name);
	
	options[opt].value.str = NULL;
	for (ix = 0; ix < options[opt].count; ix += 1) {
		if (!strcasecmp(value, (((char **) options[opt].constraints)[ix])))
			options[opt].value.str = ((char **) options[opt].constraints)[ix];
	}
	if (!options[opt].value.str)
		fatal ("Tried to set option '%s' to unknown symbol '%s'.", name, value);
}

/* Set a string option to the value. The string is duplicated. */
void option_set_str (const char *name, const char *value)
{
	int opt = find_option(name, OPTION_STR | OPTION_SYMB);

	if (opt == -1)
		fatal ("Tried to set wrong option '%s'!", name);
	
	if (options[opt].type == OPTION_SYMB) {
		option_set_symb (name, value);
	} else {
		if (options[opt].value.str)
			free (options[opt].value.str);
		options[opt].value.str = xstrdup (value);
	}
}

void option_ignore_config (const char *name)
{
	int opt = find_option(name, OPTION_ANY);

	if (opt == -1)
		fatal ("Tried to set wrong option '%s'!", name);

	options[opt].ignore_in_config = 1;
}

#define CHECK_DISCRETE(c)   option_check_discrete, (c)
#define CHECK_RANGE(c)      option_check_range, (2 * (c))
#define CHECK_LENGTH(c)     option_check_length, (2 * (c))
#define CHECK_SYMBOL(c)     (c)
#define CHECK_NONE          option_check_true, 0

/* Make a table of options and its default values. */
void options_init ()
{
	memset (options, 0, sizeof(options));

	option_add_bool ("ReadTags", true);
	option_add_str  ("MusicDir", NULL, CHECK_NONE);
	option_add_bool ("StartInMusicDir", false);
	option_add_bool ("ShowStreamErrors", false);
	option_add_bool ("Repeat", false);
	option_add_bool ("Shuffle", false);
	option_add_bool ("AutoNext", true);
	option_add_symb ("Sort", "FileName", CHECK_SYMBOL(1), "FileName");
	option_add_str  ("FormatString",
			"%(n:%n :)%(a:%a - :)%(t:%t:)%(A: \\(%A\\):)", CHECK_NONE);
	option_add_int  ("OutputBuffer", 512, CHECK_RANGE(1), 128, INT_MAX);
	option_add_str  ("OSSDevice", "/dev/dsp", CHECK_NONE);
	option_add_str  ("OSSMixerDevice", "/dev/mixer", CHECK_NONE);
	option_add_str  ("OSSMixerChannel", "pcm", CHECK_NONE);
	option_add_str  ("OSSMixerChannel2", "master", CHECK_NONE);
	option_add_str  ("SoundDriver", "Jack, ALSA, OSS", CHECK_NONE);
	option_add_bool ("ShowHiddenFiles", true);
	option_add_str  ("AlsaDevice", "default", CHECK_NONE);
	option_add_str  ("AlsaMixer", "PCM", CHECK_NONE);
	option_add_str  ("AlsaMixer2", "Master", CHECK_NONE);
	option_add_bool ("HideFileExtension", false);
	option_add_bool ("ShowFormat", true);
	option_add_symb ("ShowTime", "IfAvailable",
	                 CHECK_SYMBOL(3), "yes", "no", "IfAvailable");
	option_add_str  ("Theme", NULL, CHECK_NONE);
	option_add_str  ("XTermTheme", NULL, CHECK_NONE);
	option_add_str  ("ForceTheme", NULL, CHECK_NONE); /* Used when -T is set */
	option_add_bool ("AutoLoadLyrics", true);
	option_add_str  ("MOCDir", "~/.moc", CHECK_NONE);
	option_add_bool ("UseMmap", false);
	option_add_bool ("UseMimeMagic", false);
	option_add_bool ("Precache", true);
	option_add_bool ("SavePlaylist", true);
	option_add_str  ("Keymap", NULL, CHECK_NONE);
	option_add_bool ("SyncPlaylist", true);
	option_add_int  ("InputBuffer", 512, CHECK_RANGE(1), 32, INT_MAX);
	option_add_int  ("Prebuffering", 64, CHECK_RANGE(1), 0, INT_MAX);
	option_add_str  ("JackOutLeft", "alsa_pcm:playback_1", CHECK_NONE);
	option_add_str  ("JackOutRight", "alsa_pcm:playback_2", CHECK_NONE);
	option_add_bool ("ASCIILines", false);
	option_add_str  ("FastDir1", NULL, CHECK_NONE);
	option_add_str  ("FastDir2", NULL, CHECK_NONE);
	option_add_str  ("FastDir3", NULL, CHECK_NONE);
	option_add_str  ("FastDir4", NULL, CHECK_NONE);
	option_add_str  ("FastDir5", NULL, CHECK_NONE);
	option_add_str  ("FastDir6", NULL, CHECK_NONE);
	option_add_str  ("FastDir7", NULL, CHECK_NONE);
	option_add_str  ("FastDir8", NULL, CHECK_NONE);
	option_add_str  ("FastDir9", NULL, CHECK_NONE);
	option_add_str  ("FastDir10", NULL, CHECK_NONE);
	option_add_str  ("ExecCommand1", NULL, CHECK_NONE);
	option_add_str  ("ExecCommand2", NULL, CHECK_NONE);
	option_add_str  ("ExecCommand3", NULL, CHECK_NONE);
	option_add_str  ("ExecCommand4", NULL, CHECK_NONE);
	option_add_str  ("ExecCommand5", NULL, CHECK_NONE);
	option_add_str  ("ExecCommand6", NULL, CHECK_NONE);
	option_add_str  ("ExecCommand7", NULL, CHECK_NONE);
	option_add_str  ("ExecCommand8", NULL, CHECK_NONE);
	option_add_str  ("ExecCommand9", NULL, CHECK_NONE);
	option_add_str  ("ExecCommand10", NULL, CHECK_NONE);
	option_add_bool ("Mp3IgnoreCRCErrors", true);
	option_add_int  ("SeekTime", 1, CHECK_RANGE(1), 1, INT_MAX);
	option_add_int  ("SilentSeekTime", 5, CHECK_RANGE(1), 1, INT_MAX);
	option_add_symb ("ResampleMethod", "Linear",
	                 CHECK_SYMBOL(5), "SincBestQuality", "SincMediumQuality",
	                                  "SincFastest", "ZeroOrderHold", "Linear");
	option_add_int  ("ForceSampleRate", 0, CHECK_RANGE(1), 0, 500000);
	option_add_str  ("HTTPProxy", NULL, CHECK_NONE);
	option_add_bool ("UseRealtimePriority", false);
	option_add_int  ("TagsCacheSize", 256, CHECK_RANGE(1), 0, INT_MAX);
	option_add_bool ("PlaylistNumbering", true);
	option_add_str  ("Layout1",
			"directory:0,0,50%,100% playlist:50%,0,FILL,100%", CHECK_NONE);
	option_add_str  ("Layout2",
			"directory:0,0,100%,100% playlist:0,0,100%,100%", CHECK_NONE);
	option_add_str  ("Layout3", NULL, CHECK_NONE);
	option_add_bool ("FollowPlayedFile", true);
	option_add_bool ("CanStartInPlaylist", true);
	option_add_bool ("UseCursorSelection", false);
	option_add_str  ("ID3v1TagsEncoding", "WINDOWS-1250", CHECK_NONE);
	option_add_bool ("UseRCC", true);
	option_add_bool ("UseRCCForFilesystem", true);
	option_add_bool ("EnforceTagsEncoding", false);
	option_add_bool ("FileNamesIconv", false);
	option_add_bool ("NonUTFXterm", false);
	option_add_bool ("SetXtermTitle", true);
	option_add_bool ("SetScreenTitle", true);
	option_add_bool ("PlaylistFullPaths", true);
	option_add_str  ("BlockDecorators", "`\"'", CHECK_LENGTH(1), 3, 3);
	option_add_int  ("MessageLingerTime", 3, CHECK_RANGE(1), 0, INT_MAX);
	option_add_bool ("PrefixQueuedMessages", true);
	option_add_str  ("ErrorMessagesQueued", "!", CHECK_NONE);
	option_add_bool ("Allow24bitOutput", false);

	option_add_int  ("ModPlug_Channels", 2, CHECK_DISCRETE(2), 1, 2);
	option_add_int  ("ModPlug_Frequency", 44100,
	                 CHECK_DISCRETE(4), 11025, 22050, 44100, 48000);
	option_add_int  ("ModPlug_Bits", 16, CHECK_DISCRETE(3), 8, 16, 32);

	option_add_bool ("ModPlug_Oversampling", true);
	option_add_bool ("ModPlug_NoiseReduction", true);
	option_add_bool ("ModPlug_Reverb", false);
	option_add_bool ("ModPlug_MegaBass", false);
	option_add_bool ("ModPlug_Surround", false);

	option_add_symb ("ModPlug_ResamplingMode", "FIR",
	                 CHECK_SYMBOL(4), "FIR", "SPLINE", "LINEAR", "NEAREST");

	option_add_int  ("ModPlug_ReverbDepth", 0, CHECK_RANGE(1), 0, 100);
	option_add_int  ("ModPlug_ReverbDelay", 0, CHECK_RANGE(1), 0, INT_MAX);
	option_add_int  ("ModPlug_BassAmount", 0, CHECK_RANGE(1), 0, 100);
	option_add_int  ("ModPlug_BassRange", 10, CHECK_RANGE(1), 10, 100);
	option_add_int  ("ModPlug_SurroundDepth", 0, CHECK_RANGE(1), 0, 100);
	option_add_int  ("ModPlug_SurroundDelay", 0, CHECK_RANGE(1), 0, INT_MAX);
	option_add_int  ("ModPlug_LoopCount", 0, CHECK_RANGE(1), -1, INT_MAX);

	option_add_int  ("TiMidity_Volume", 100, CHECK_RANGE(1), 0, 800);
	option_add_int  ("TiMidity_Rate", 44100, CHECK_RANGE(1), 8000, 48000);
		// not sure about the limits... I like 44100
	option_add_int  ("TiMidity_Bits", 16, CHECK_DISCRETE(2), 8, 16);
	option_add_int  ("TiMidity_Channels", 2, CHECK_DISCRETE(2), 1, 2);
	option_add_str  ("TiMidity_Config", NULL, CHECK_NONE);

	option_add_int  ("SidPlay2_DefaultSongLength", 180,
	                 CHECK_RANGE(1), 0, INT_MAX);
	option_add_int  ("SidPlay2_MinimumSongLength", 0,
	                 CHECK_RANGE(1), 0, INT_MAX);
	option_add_str  ("SidPlay2_Database", NULL, CHECK_NONE);
	option_add_int  ("SidPlay2_Frequency", 44100, CHECK_RANGE(1), 4000, 48000);
	option_add_int  ("SidPlay2_Bits", 16, CHECK_DISCRETE(2), 8, 16);
	option_add_symb ("SidPlay2_PlayMode", "M",
	                 CHECK_SYMBOL(4), "M", "S", "L", "R");
	option_add_int  ("SidPlay2_Optimisation", 0, CHECK_RANGE(1), 0, 2);
	option_add_bool ("SidPlay2_StartAtStart", true);
	option_add_bool ("SidPlay2_PlaySubTunes", true);

	option_add_str  ("OnSongChange", NULL, CHECK_NONE);
	option_add_str  ("OnStop", NULL, CHECK_NONE);

	option_add_bool ("Softmixer_SaveState", true);

	option_add_bool ("Equalizer_SaveState", true);

	option_add_bool ("QueueNextSongReturn", false);
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

/* Return 1 if a parameter to a boolean option is valid.  This may seem
 * pointless but it provides a consistant interface, ensures the existence
 * of the option and checks the value where true booleans are emulated with
 * other types. */
int check_bool_option (const char *name, const bool val)
{
	int opt;

	opt = find_option (name, OPTION_BOOL);
	if (opt == -1)
		return 0;
	if (val == true || val == false)
		return 1;
	return 0;
}

/* Return 1 if a parameter to a string option is valid. */
int check_str_option (const char *name, const char *val)
{
	int opt;

	opt = find_option (name, OPTION_STR | OPTION_SYMB);
	if (opt == -1)
		return 0;
	return options[opt].check (opt, val);
}

/* Return 1 if a parameter to a symbol option is valid. */
int check_symb_option (const char *name, const char *val)
{
	int opt;

	opt = find_option (name, OPTION_SYMB);
	if (opt == -1)
		return 0;
	return option_check_discrete (opt, val);
}

static int is_deprecated_option (const char *name)
{
	if (!strcmp(name, "TagsIconv"))
		return 1;

	return 0;
}

/* Set an option read from the configuration file. Return 0 on error. */
static int set_option (const char *name, const char *value_x)
{
	int i, num;
	char *end;
	const char *value;
	bool val;

	i = find_option (name, OPTION_ANY);
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

	/* Handle a change of option type for QueueNextSongReturn. */
	value = NULL;
	if (!strcasecmp(options[i].name, "QueueNextSongReturn")) {
		if (!strcmp(value_x, "0"))
			value = "no";
		else if (!strcmp(value_x, "1"))
			value = "yes";
	}
	if (value) {
		fprintf (stderr, "\n\tThe valid values of '%s' have changed, "
				"\n\tplease update your configuration file.\n\n", name);
		sleep (5);
	} else {
		value = value_x;
	}

	switch (options[i].type) {

		case OPTION_INT:
			num = strtol (value, &end, 10);
			if (*end)
				return 0;
			if (!check_int_option(name, num))
				return 0;
			option_set_int (name, num);
			break;

		case OPTION_BOOL:
			if (!strcasecmp(value, "yes"))
				val = true;
			else if (!strcasecmp(value, "no"))
				val = false;
			else
				return 0;
			option_set_bool (name, val);
			break;

		case OPTION_STR:
			if (!check_str_option(name, value))
				return 0;
			option_set_str (name, value);
			break;

		case OPTION_SYMB:
			if (!check_symb_option(name, value))
				return 0;
			option_set_symb (name, value);
			break;

		case OPTION_FREE:
		case OPTION_ANY:
			break;
	}
	
	return 1;
}

/* Check if values of options make sense. This only checks options that can't
 * be checked without parsing the whole file. */
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
	int eq = 0; /* equal character appeared? */
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
		if (options[i].type == OPTION_STR && options[i].value.str)
			free (options[i].value.str);
		options[i].value.str = NULL;
		if (options[i].type & (OPTION_STR | OPTION_SYMB)) {
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
	int i = find_option(name, OPTION_INT | OPTION_BOOL);

	if (i == -1)
		fatal ("Tried to get wrong option '%s'!", name);
	
	if (options[i].type == OPTION_BOOL)
		return options[i].value.boolean ? 1 : 0;
	return options[i].value.num;
}

bool options_get_bool (const char *name)
{
	int i = find_option(name, OPTION_BOOL);

	if (i == -1)
		fatal ("Tried to get wrong option '%s'!", name);
	
	return options[i].value.boolean;
}

char *options_get_str (const char *name)
{
	int i = find_option(name, OPTION_STR | OPTION_SYMB);

	if (i == -1)
		fatal ("Tried to get wrong option '%s'!", name);

	return options[i].value.str;
}

char *options_get_symb (const char *name)
{
	int i = find_option(name, OPTION_SYMB);

	if (i == -1)
		fatal ("Tried to get wrong option '%s'!", name);

	return options[i].value.str;
}

enum option_type options_get_type (const char *name)
{
	int i = find_option(name, OPTION_ANY);

	if (i == -1)
		return OPTION_FREE;

	return options[i].type;
}
