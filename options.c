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
#include <ctype.h>
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <limits.h>
#include <stdarg.h>
#include <sys/types.h>
#include <regex.h>

#include "common.h"
#include "files.h"
#include "log.h"
#include "options.h"
#include "lists.h"

#define OPTIONS_MAX	181
#define OPTION_NAME_MAX	32

typedef int options_t_check (int, ...);

union option_value
{
	char *str;
	int num;
	bool boolean;
	lists_t_strs *list;
};

struct option
{
	char name[OPTION_NAME_MAX];
	enum option_type type;
	union option_value value;
	int ignore_in_config;
	int set_in_config;
	unsigned int hash;
	options_t_check *check;
	int count;
	void *constraints;
};

static struct option options[OPTIONS_MAX];
static int options_num = 0;


/* Returns the str's hash using djb2 algorithm. */
static unsigned int hash (const char * str)
{
	unsigned int hash = 5381;

	while (*str)
		hash = ((hash << 5) + hash) + tolower(*(str++));
	return hash;
}

/* Return an index to an option in the options hashtable.
 * If there is no such option return -1. */
static int find_option (const char *name, enum option_type type)
{
	unsigned int h = hash (name), i, init_pos = h % OPTIONS_MAX;

	for (i = init_pos; i < OPTIONS_MAX; i += 1) {
		if (options[i].type == OPTION_FREE)
			return -1;

		if (h == options[i].hash && type & options[i].type) {
			if (!strcasecmp (name, options[i].name))
				return i;
		}
	}

	for (i = 0; i < init_pos; i += 1) {
		if (options[i].type == OPTION_FREE)
			return -1;

		if (h == options[i].hash && type & options[i].type) {
			if (!strcasecmp (name, options[i].name))
		    	return i;
		}
	}

	return -1;
}

/* Return an index of a free slot in the options hashtable.
 * If there is no such slot return -1. */
static int find_free (unsigned int h)
{
	unsigned int i;

	assert (options_num < OPTIONS_MAX);
	h %= OPTIONS_MAX;

	for (i = h; i < OPTIONS_MAX; i += 1) {
		if (options[i].type == OPTION_FREE)
			return i;
	}

	for (i = 0; i < h; i += 1) {
		if (options[i].type == OPTION_FREE)
			return i;
	}

	return -1;
}

/* Check that a value falls within the specified range(s). */
static int check_range (int opt, ...)
{
	int rc, ix, int_val;
	char *str_val;
	va_list va;

	assert (opt != -1);
	assert (options[opt].count % 2 == 0);
	assert (options[opt].type & (OPTION_INT | OPTION_STR | OPTION_LIST));

	rc = 0;
	va_start (va, opt);
	switch (options[opt].type) {

		case OPTION_INT:
			int_val = va_arg (va, int);
			for (ix = 0; ix < options[opt].count; ix += 2) {
				if (int_val >= ((int *) options[opt].constraints)[ix] &&
		    		int_val <= ((int *) options[opt].constraints)[ix + 1]) {
					rc = 1;
					break;
				}
			}
			break;

		case OPTION_STR:
		case OPTION_LIST:
			str_val = va_arg (va, char *);
			for (ix = 0; ix < options[opt].count; ix += 2) {
				if (strcasecmp (str_val, (((char **) options[opt].constraints)[ix])) >= 0 &&
				    strcasecmp (str_val, (((char **) options[opt].constraints)[ix + 1])) <= 0) {
					rc = 1;
					break;
				}
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
static int check_discrete (int opt, ...)
{
	int rc, ix, int_val;
	char *str_val;
	va_list va;

	assert (opt != -1);
	assert (options[opt].type & (OPTION_INT | OPTION_SYMB | OPTION_LIST));

	rc = 0;
	va_start (va, opt);
	switch (options[opt].type) {

		case OPTION_INT:
			int_val = va_arg (va, int);
			for (ix = 0; ix < options[opt].count; ix += 1) {
				if (int_val == ((int *) options[opt].constraints)[ix]) {
					rc = 1;
					break;
				}
			}
			break;

		case OPTION_SYMB:
		case OPTION_LIST:
			str_val = va_arg (va, char *);
			for (ix = 0; ix < options[opt].count; ix += 1) {
				if (!strcasecmp(str_val, (((char **) options[opt].constraints)[ix]))) {
					rc = 1;
					break;
				}
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
static int check_length (int opt, ...)
{
	int rc, ix, str_len;
	va_list va;

	assert (opt != -1);
	assert (options[opt].count % 2 == 0);
	assert (options[opt].type & (OPTION_STR | OPTION_LIST));

	rc = 0;
	va_start (va, opt);
	str_len = strlen (va_arg (va, char *));
	for (ix = 0; ix < options[opt].count; ix += 2) {
		if (str_len >= ((int *) options[opt].constraints)[ix] &&
    		str_len <= ((int *) options[opt].constraints)[ix + 1]) {
			rc = 1;
			break;
		}
	}
	va_end (va);

	return rc;
}

/* Check that a string has a function-like syntax. */
static int check_function (int opt, ...)
{
	int rc;
	const char *str;
	const char regex[] = "^[a-z0-9/-]+\\([^,) ]*(,[^,) ]*)*\\)$";
	static regex_t *preg = NULL;
	va_list va;

	assert (opt != -1);
	assert (options[opt].count == 0);
	assert (options[opt].type & (OPTION_STR | OPTION_LIST));

	if (preg == NULL) {
		preg = (regex_t *)xmalloc (sizeof (regex_t));
		rc = regcomp (preg, regex, REG_EXTENDED | REG_ICASE | REG_NOSUB);
		assert (rc == 0);
	}

	va_start (va, opt);
	str = va_arg (va, const char *);
	rc = regexec (preg, str, 0, NULL, 0);
	va_end (va);

	return (rc == 0) ? 1 : 0;
}

/* Always pass a value as valid. */
static int check_true (int unused ATTR_UNUSED, ...)
{
	return 1;
}

/* Initializes a position on the options table. This is intended to be used at
 * initialization to make a table of valid options and its default values. */
static int init_option (const char *name, enum option_type type)
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
	options[pos].check = check_true;
	options[pos].count = 0;
	options[pos].constraints = NULL;

	options_num++;
	return pos;
}

/* Add an integer option to the options table. This is intended to be used at
 * initialization to make a table of valid options and their default values. */
static void add_int (const char *name, const int value, options_t_check *check, const int count, ...)
{
	int ix, pos;
	va_list va;

	pos = init_option (name, OPTION_INT);
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
static void add_bool (const char *name, const bool value)
{
	int pos;

	pos = init_option (name, OPTION_BOOL);
	options[pos].value.boolean = value;
}

/* Add a string option to the options table. This is intended to be used at
 * initialization to make a table of valid options and their default values. */
static void add_str (const char *name, const char *value, options_t_check *check, const int count, ...)
{
	int ix, pos;
	va_list va;

	pos = init_option (name, OPTION_STR);
	options[pos].value.str = xstrdup (value);
	options[pos].check = check;
	options[pos].count = count;
	if (count > 0) {
		va_start (va, count);
		if (check == check_length) {
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
static void add_symb (const char *name, const char *value, const int count, ...)
{
	int ix, pos;
	va_list va;

	assert (name != NULL);
	assert (value != NULL);
	assert (count > 0);

	pos = init_option (name, OPTION_SYMB);
	options[pos].value.str = NULL;
	options[pos].check = check_discrete;
	options[pos].count = count;
	va_start (va, count);
	options[pos].constraints = xcalloc (count, sizeof (char *));
	for (ix = 0; ix < count; ix += 1) {
		char *val = va_arg (va, char *);
		if (!is_valid_symbol (val))
			fatal ("Invalid symbol in '%s' constraint list!", name);
		((char **) options[pos].constraints)[ix] = xstrdup (val);
		if (!strcasecmp (val, value))
			options[pos].value.str = ((char **) options[pos].constraints)[ix];
	}
	if (!options[pos].value.str)
		fatal ("Invalid default value symbol in '%s'!", name);
	va_end (va);
}

/* Add a list option to the options table. This is intended to be used at
 * initialization to make a table of valid options and their default values. */
static void add_list (const char *name, const char *value, options_t_check *check, const int count, ...)
{
	int ix, pos;
	va_list va;

	pos = init_option (name, OPTION_LIST);
	options[pos].value.list = lists_strs_new (8);
	if (value)
		lists_strs_split (options[pos].value.list, value, ":");
	options[pos].check = check;
	options[pos].count = count;
	if (count > 0) {
		va_start (va, count);
		if (check == check_length) {
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
void options_set_int (const char *name, const int value)
{
	int i = find_option (name, OPTION_INT);

	if (i == -1)
		fatal ("Tried to set wrong option '%s'!", name);
	options[i].value.num = value;
}

/* Set a boolean option to the value. */
void options_set_bool (const char *name, const bool value)
{
	int i = find_option (name, OPTION_BOOL);

	if (i == -1)
		fatal ("Tried to set wrong option '%s'!", name);
	options[i].value.boolean = value;
}

/* Set a symbol option to the value. */
void options_set_symb (const char *name, const char *value)
{
	int opt, ix;

	opt = find_option (name, OPTION_SYMB);
	if (opt == -1)
		fatal ("Tried to set wrong option '%s'!", name);

	options[opt].value.str = NULL;
	for (ix = 0; ix < options[opt].count; ix += 1) {
		if (!strcasecmp(value, (((char **) options[opt].constraints)[ix])))
			options[opt].value.str = ((char **) options[opt].constraints)[ix];
	}
	if (!options[opt].value.str)
		fatal ("Tried to set '%s' to unknown symbol '%s'!", name, value);
}

/* Set a string option to the value. The string is duplicated. */
void options_set_str (const char *name, const char *value)
{
	int opt = find_option (name, OPTION_STR);

	if (opt == -1)
		fatal ("Tried to set wrong option '%s'!", name);

	if (options[opt].value.str)
		free (options[opt].value.str);
	options[opt].value.str = xstrdup (value);
}

/* Set list option values to the colon separated value. */
void options_set_list (const char *name, const char *value, bool append)
{
	int opt;

	opt = find_option (name, OPTION_LIST);
	if (opt == -1)
		fatal ("Tried to set wrong option '%s'!", name);

	if (!append && !lists_strs_empty (options[opt].value.list))
		lists_strs_clear (options[opt].value.list);
	lists_strs_split (options[opt].value.list, value, ":");
}

/* Given a type, a name and a value, set that option's value.
 * Return false on error. */
bool options_set_pair (const char *name, const char *value, bool append)
{
	int num;
	char *end;
	bool val;

	switch (options_get_type (name)) {

		case OPTION_INT:
			num = strtol (value, &end, 10);
			if (*end)
				return false;
			if (!options_check_int (name, num))
				return false;
			options_set_int (name, num);
			break;

		case OPTION_BOOL:
			if (!strcasecmp (value, "yes"))
				val = true;
			else if (!strcasecmp (value, "no"))
				val = false;
			else
				return false;
			options_set_bool (name, val);
			break;

		case OPTION_STR:
			if (!options_check_str (name, value))
				return false;
			options_set_str (name, value);
			break;

		case OPTION_SYMB:
			if (!options_check_symb (name, value))
				return false;
			options_set_symb (name, value);
			break;

		case OPTION_LIST:
			if (!options_check_list (name, value))
				return false;
			options_set_list (name, value, append);
			break;

		case OPTION_FREE:
		case OPTION_ANY:
			return false;
	}

	return true;
}

void options_ignore_config (const char *name)
{
	int opt = find_option (name, OPTION_ANY);

	if (opt == -1)
		fatal ("Tried to set wrong option '%s'!", name);

	options[opt].ignore_in_config = 1;
}

#define CHECK_DISCRETE(c)   check_discrete, (c)
#define CHECK_RANGE(c)      check_range, (2 * (c))
#define CHECK_LENGTH(c)     check_length, (2 * (c))
#define CHECK_SYMBOL(c)     (c)
#define CHECK_FUNCTION      check_function, 0
#define CHECK_NONE          check_true, 0

/* Make a table of options and its default values. */
void options_init ()
{
	memset (options, 0, sizeof(options));

	add_bool ("ReadTags", true);
	add_str  ("MusicDir", NULL, CHECK_NONE);
	add_bool ("StartInMusicDir", false);
	add_int  ("CircularLogSize", 0, CHECK_RANGE(1), 0, INT_MAX);
	add_symb ("Sort", "FileName", CHECK_SYMBOL(1), "FileName");
	add_bool ("ShowStreamErrors", false);
	add_bool ("MP3IgnoreCRCErrors", true);
	add_bool ("Repeat", false);
	add_bool ("Shuffle", false);
	add_bool ("AutoNext", true);
	add_str  ("FormatString",
	          "%(n:%n :)%(a:%a - :)%(t:%t:)%(A: \\(%A\\):)", CHECK_NONE);
	add_int  ("InputBuffer", 512, CHECK_RANGE(1), 32, INT_MAX);
	add_int  ("OutputBuffer", 512, CHECK_RANGE(1), 128, INT_MAX);
	add_int  ("Prebuffering", 64, CHECK_RANGE(1), 0, INT_MAX);
	add_str  ("HTTPProxy", NULL, CHECK_NONE);

#ifdef OPENBSD
	add_list ("SoundDriver", "SNDIO:JACK:OSS",
	          CHECK_DISCRETE(5), "SNDIO", "Jack", "ALSA", "OSS", "null");
#else
	add_list ("SoundDriver", "Jack:ALSA:OSS",
	          CHECK_DISCRETE(5), "SNDIO", "Jack", "ALSA", "OSS", "null");
#endif

	add_str  ("JackClientName", "moc", CHECK_NONE);
	add_bool ("JackStartServer", false);
	add_str  ("JackOutLeft", "system:playback_1", CHECK_NONE);
	add_str  ("JackOutRight", "system:playback_2", CHECK_NONE);

	add_str  ("OSSDevice", "/dev/dsp", CHECK_NONE);
	add_str  ("OSSMixerDevice", "/dev/mixer", CHECK_NONE);
	add_symb ("OSSMixerChannel1", "pcm",
	          CHECK_SYMBOL(3), "pcm", "master", "speaker");
	add_symb ("OSSMixerChannel2", "master",
	          CHECK_SYMBOL(3), "pcm", "master", "speaker");

	add_str  ("ALSADevice", "default", CHECK_NONE);
	add_str  ("ALSAMixer1", "PCM", CHECK_NONE);
	add_str  ("ALSAMixer2", "Master", CHECK_NONE);
	add_bool ("ALSAStutterDefeat", false);

	add_bool ("Softmixer_SaveState", true);
	add_bool ("Equalizer_SaveState", true);

	add_bool ("ShowHiddenFiles", false);
	add_bool ("HideFileExtension", false);
	add_bool ("ShowFormat", true);
	add_symb ("ShowTime", "IfAvailable",
	                 CHECK_SYMBOL(3), "yes", "no", "IfAvailable");
	add_bool ("ShowTimePercent", false);

	add_list ("ScreenTerms", "screen:screen-w:vt100", CHECK_NONE);

	add_list ("XTerms", "xterm:"
	                    "xterm-colour:xterm-color:"
	                    "xterm-256colour:xterm-256color:"
	                    "rxvt:rxvt-unicode:"
	                    "rxvt-unicode-256colour:rxvt-unicode-256color:"
	                    "eterm", CHECK_NONE);

	add_str  ("Theme", NULL, CHECK_NONE);
	add_str  ("XTermTheme", NULL, CHECK_NONE);
	add_str  ("ForceTheme", NULL, CHECK_NONE); /* Used when -T is set */
	add_bool ("AutoLoadLyrics", true);
	add_str  ("MOCDir", "~/.moc", CHECK_NONE);
	add_bool ("UseMMap", false);
	add_bool ("UseMimeMagic", false);
	add_str  ("ID3v1TagsEncoding", "WINDOWS-1250", CHECK_NONE);
	add_bool ("UseRCC", true);
	add_bool ("UseRCCForFilesystem", true);
	add_bool ("EnforceTagsEncoding", false);
	add_bool ("FileNamesIconv", false);
	add_bool ("NonUTFXterm", false);
	add_bool ("Precache", true);
	add_bool ("SavePlaylist", true);
	add_bool ("SyncPlaylist", true);
	add_str  ("Keymap", NULL, CHECK_NONE);
	add_bool ("ASCIILines", false);

	add_str  ("FastDir1", NULL, CHECK_NONE);
	add_str  ("FastDir2", NULL, CHECK_NONE);
	add_str  ("FastDir3", NULL, CHECK_NONE);
	add_str  ("FastDir4", NULL, CHECK_NONE);
	add_str  ("FastDir5", NULL, CHECK_NONE);
	add_str  ("FastDir6", NULL, CHECK_NONE);
	add_str  ("FastDir7", NULL, CHECK_NONE);
	add_str  ("FastDir8", NULL, CHECK_NONE);
	add_str  ("FastDir9", NULL, CHECK_NONE);
	add_str  ("FastDir10", NULL, CHECK_NONE);

	add_int  ("SeekTime", 1, CHECK_RANGE(1), 1, INT_MAX);
	add_int  ("SilentSeekTime", 5, CHECK_RANGE(1), 1, INT_MAX);

	add_list ("PreferredDecoders",
	                 "aac(aac,ffmpeg):m4a(ffmpeg):"
	                 "mpc(musepack,*,ffmpeg):mpc8(musepack,*,ffmpeg):"
	                 "sid(sidplay2):mus(sidplay2):"
	                 "wav(sndfile,*,ffmpeg):"
	                 "wv(wavpack,*,ffmpeg):"
	                 "audio/aac(aac):audio/aacp(aac):audio/m4a(ffmpeg):"
	                 "audio/wav(sndfile,*):"
	                 "ogg(vorbis,*,ffmpeg):oga(vorbis,*,ffmpeg):ogv(ffmpeg):"
	                 "application/ogg(vorbis):audio/ogg(vorbis):"
	                 "flac(flac,*,ffmpeg):"
	                 "opus(ffmpeg):"
	                 "spx(speex)",
	                 CHECK_FUNCTION);

	add_symb ("ResampleMethod", "Linear",
	                 CHECK_SYMBOL(5), "SincBestQuality", "SincMediumQuality",
	                                  "SincFastest", "ZeroOrderHold", "Linear");
	add_int  ("ForceSampleRate", 0, CHECK_RANGE(1), 0, 500000);
	add_bool ("Allow24bitOutput", false);
	add_bool ("UseRealtimePriority", false);
	add_int  ("TagsCacheSize", 256, CHECK_RANGE(1), 0, INT_MAX);
	add_bool ("PlaylistNumbering", true);

	add_list ("Layout1", "directory(0,0,50%,100%):playlist(50%,0,FILL,100%)",
	                     CHECK_FUNCTION);
	add_list ("Layout2", "directory(0,0,100%,100%):playlist(0,0,100%,100%)",
	                     CHECK_FUNCTION);
	add_list ("Layout3", NULL, CHECK_FUNCTION);

	add_bool ("FollowPlayedFile", true);
	add_bool ("CanStartInPlaylist", true);
	add_str  ("ExecCommand1", NULL, CHECK_NONE);
	add_str  ("ExecCommand2", NULL, CHECK_NONE);
	add_str  ("ExecCommand3", NULL, CHECK_NONE);
	add_str  ("ExecCommand4", NULL, CHECK_NONE);
	add_str  ("ExecCommand5", NULL, CHECK_NONE);
	add_str  ("ExecCommand6", NULL, CHECK_NONE);
	add_str  ("ExecCommand7", NULL, CHECK_NONE);
	add_str  ("ExecCommand8", NULL, CHECK_NONE);
	add_str  ("ExecCommand9", NULL, CHECK_NONE);
	add_str  ("ExecCommand10", NULL, CHECK_NONE);

	add_bool ("UseCursorSelection", false);
	add_bool ("SetXtermTitle", true);
	add_bool ("SetScreenTitle", true);
	add_bool ("PlaylistFullPaths", true);

	add_str  ("BlockDecorators", "`\"'", CHECK_LENGTH(1), 3, 3);
	add_int  ("MessageLingerTime", 3, CHECK_RANGE(1), 0, INT_MAX);
	add_bool ("PrefixQueuedMessages", true);
	add_str  ("ErrorMessagesQueued", "!", CHECK_NONE);

	add_bool ("ModPlug_Oversampling", true);
	add_bool ("ModPlug_NoiseReduction", true);
	add_bool ("ModPlug_Reverb", false);
	add_bool ("ModPlug_MegaBass", false);
	add_bool ("ModPlug_Surround", false);
	add_symb ("ModPlug_ResamplingMode", "FIR",
	                 CHECK_SYMBOL(4), "FIR", "SPLINE", "LINEAR", "NEAREST");
	add_int  ("ModPlug_Channels", 2, CHECK_DISCRETE(2), 1, 2);
	add_int  ("ModPlug_Bits", 16, CHECK_DISCRETE(3), 8, 16, 32);
	add_int  ("ModPlug_Frequency", 44100,
	                 CHECK_DISCRETE(4), 11025, 22050, 44100, 48000);
	add_int  ("ModPlug_ReverbDepth", 0, CHECK_RANGE(1), 0, 100);
	add_int  ("ModPlug_ReverbDelay", 0, CHECK_RANGE(1), 0, INT_MAX);
	add_int  ("ModPlug_BassAmount", 0, CHECK_RANGE(1), 0, 100);
	add_int  ("ModPlug_BassRange", 10, CHECK_RANGE(1), 10, 100);
	add_int  ("ModPlug_SurroundDepth", 0, CHECK_RANGE(1), 0, 100);
	add_int  ("ModPlug_SurroundDelay", 0, CHECK_RANGE(1), 0, INT_MAX);
	add_int  ("ModPlug_LoopCount", 0, CHECK_RANGE(1), -1, INT_MAX);

	add_int  ("TiMidity_Rate", 44100, CHECK_RANGE(1), 8000, 48000);
		// not sure about the limits... I like 44100
	add_int  ("TiMidity_Bits", 16, CHECK_DISCRETE(2), 8, 16);
	add_int  ("TiMidity_Channels", 2, CHECK_DISCRETE(2), 1, 2);
	add_int  ("TiMidity_Volume", 100, CHECK_RANGE(1), 0, 800);
	add_str  ("TiMidity_Config", NULL, CHECK_NONE);

	add_int  ("SidPlay2_DefaultSongLength", 180,
	                 CHECK_RANGE(1), 0, INT_MAX);
	add_int  ("SidPlay2_MinimumSongLength", 0,
	                 CHECK_RANGE(1), 0, INT_MAX);
	add_str  ("SidPlay2_Database", NULL, CHECK_NONE);
	add_int  ("SidPlay2_Frequency", 44100, CHECK_RANGE(1), 4000, 48000);
	add_int  ("SidPlay2_Bits", 16, CHECK_DISCRETE(2), 8, 16);
	add_int  ("SidPlay2_Optimisation", 0, CHECK_RANGE(1), 0, 2);
	add_symb ("SidPlay2_PlayMode", "M",
	                 CHECK_SYMBOL(4), "M", "S", "L", "R");
	add_bool ("SidPlay2_StartAtStart", true);
	add_bool ("SidPlay2_PlaySubTunes", true);

	add_str  ("OnSongChange", NULL, CHECK_NONE);
	add_bool ("RepeatSongChange", false);
	add_str  ("OnStop", NULL, CHECK_NONE);

	add_bool ("QueueNextSongReturn", false);
}

/* Return 1 if a parameter to an integer option is valid. */
int options_check_int (const char *name, const int val)
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
int options_check_bool (const char *name, const bool val)
{
	int opt, result = 0;

	opt = find_option (name, OPTION_BOOL);
	if (opt == -1)
		return 0;
	if (val == true || val == false)
		result = 1;
	return result;
}

/* Return 1 if a parameter to a string option is valid. */
int options_check_str (const char *name, const char *val)
{
	int opt;

	opt = find_option (name, OPTION_STR);
	if (opt == -1)
		return 0;
	return options[opt].check (opt, val);
}

/* Return 1 if a parameter to a symbol option is valid. */
int options_check_symb (const char *name, const char *val)
{
	int opt;

	opt = find_option (name, OPTION_SYMB);
	if (opt == -1)
		return 0;
	return check_discrete (opt, val);
}

/* Return 1 if a parameter to a list option is valid. */
int options_check_list (const char *name, const char *val)
{
	int opt, size, ix, result;
	lists_t_strs *list;

	assert (name);
	assert (val);

	opt = find_option (name, OPTION_LIST);
	if (opt == -1)
		return 0;

	list = lists_strs_new (8);
	size = lists_strs_split (list, val, ":");
	result = 1;
	for (ix = 0; ix < size; ix += 1) {
		if (!options[opt].check (opt, lists_strs_at (list, ix))) {
			result = 0;
			break;
		}
	}

	lists_strs_free (list);

	return result;
}

/* Return 1 if the named option was defaulted. */
int options_was_defaulted (const char *name)
{
	int opt, result = 0;

	assert (name);

	opt = find_option (name, OPTION_ANY);
	if (opt == -1)
		return 0;

	if (!options[opt].set_in_config && !options[opt].ignore_in_config)
		result = 1;

	return result;
}

/* Find and substitute variables enclosed by '${...}'.  Variables are
 * substituted first from the environment then, if not found, from
 * the configuration options.  Strings of the form '$${' are reduced to
 * '${' and not substituted.  The result is returned as a new string. */
static char *substitute_variable (const char *name_in, const char *value_in)
{
	size_t len;
	char *dollar, *result, *ptr, *name, *value, *dflt, *end;
	static const char accept[] = "abcdefghijklmnopqrstuvwxyz"
	                             "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
	                             "0123456789_";
	lists_t_strs *strs;

	result = xstrdup (value_in);
	ptr = result;
	strs = lists_strs_new (5);
	dollar = strstr (result, "${");
	while (dollar) {

		/* Escape "$${". */
		if (dollar > ptr && dollar[-1] == '$') {
			dollar[-1] = 0x00;
			lists_strs_append (strs, ptr);
			ptr = dollar;
			dollar = strstr (&dollar[2], "${");
			continue;
		}

		/* Copy up to this point verbatim. */
		dollar[0] = 0x00;
		lists_strs_append (strs, ptr);

		/* Find where the substitution variable name ends. */
		name = &dollar[2];
		len = strspn (name, accept);
		if (len == 0)
			fatal ("Error in config file option '%s':\n"
			       "             substitution variable name is missing!",
			       name_in);

		/* Find default substitution or closing brace. */
		dflt = NULL;
		if (name[len] == '}') {
			end = &name[len];
			end[0] = 0x00;
		}
		else if (strncmp (&name[len], ":-", 2) == 0) {
			name[len] = 0x00;
			dflt = &name[len + 2];
			end = strchr (dflt, '}');
			if (end == NULL)
				fatal ("Error in config file option '%s': "
				       "unterminated '${%s:-'!",
				       name_in, name);
			end[0] = 0x00;
		}
		else if (name[len] == 0x00) {
			fatal ("Error in config file option '%s': "
			       "unterminated '${'!",
			       name_in);
		}
		else {
			fatal ("Error in config file option '%s':\n"
			       "             expecting  ':-' or '}' found '%c'!",
			       name_in, name[len]);
		}

		/* Fetch environment variable or configuration option value. */
		value = xstrdup (getenv (name));
		if (value == NULL && find_option (name, OPTION_ANY) != -1) {
			char buf[16];
			lists_t_strs *list;

			switch (options_get_type (name)) {
			case OPTION_INT:
				snprintf (buf, sizeof (buf), "%d", options_get_int (name));
				value = xstrdup (buf);
				break;
			case OPTION_BOOL:
				value = xstrdup (options_get_bool (name) ? "yes" : "no");
				break;
			case OPTION_STR:
				value = xstrdup (options_get_str (name));
				break;
			case OPTION_SYMB:
				value = xstrdup (options_get_symb (name));
				break;
			case OPTION_LIST:
				list = options_get_list (name);
				if (!lists_strs_empty (list)) {
					value = lists_strs_fmt (list, "%s:");
					value[strlen (value) - 1] = 0x00;
				}
				break;
			case OPTION_FREE:
			case OPTION_ANY:
				break;
			}
		}
		if (value && value[0])
			lists_strs_append (strs, value);
		else if (dflt)
			lists_strs_append (strs, dflt);
		else
			fatal ("Error in config file option '%s':\n"
			       "             substitution variable '%s' not set or null!",
		           name_in, &dollar[2]);
		free (value);

		/* Go look for another substitution. */
		ptr = &end[1];
		dollar = strstr (ptr, "${");
	}

	/* If anything changed copy segments to result. */
	if (!lists_strs_empty (strs)) {
		lists_strs_append (strs, ptr);
		free (result);
		result = lists_strs_cat (strs);
	}
	lists_strs_free (strs);

	return result;
}

/* Set an option read from the configuration file. Return false on error. */
static bool set_option (const char *name, const char *value_in, bool append)
{
	int i;
	char *value;

	i = find_option (name, OPTION_ANY);
	if (i == -1) {
		fprintf (stderr, "Wrong option name: '%s'.", name);
		return false;
	}

	if (options[i].ignore_in_config)
		return true;

	if (append && options[i].type != OPTION_LIST) {
		fprintf (stderr,
		         "Only list valued options can be appended to ('%s').",
		         name);
		return false;
	}

	if (!append && options[i].set_in_config) {
		fprintf (stderr, "Tried to set an option that has been already "
		                 "set in the config file ('%s').", name);
		return false;
	}

	options[i].set_in_config = 1;

	/* Substitute environmental variables. */
	value = substitute_variable (name, value_in);

	if (!options_set_pair (name, value, append))
		return false;

	free (value);
	return true;
}

/* Check if values of options make sense. This only checks options that can't
 * be checked without parsing the whole file. */
static void sanity_check ()
{
	if (options_get_int ("Prebuffering") > options_get_int ("InputBuffer"))
		fatal ("Prebuffering is set to a value greater than InputBuffer!");
}

/* Parse the configuration file. */
void options_parse (const char *config_file)
{
	int ch;
	int comm = 0; /* comment? */
	int eq = 0; /* equal character appeared? */
	int quote = 0; /* are we in quotes? */
	int esc = 0;
	bool plus = false; /* plus character appeared? */
	bool append = false; /* += (list append) appeared */
	bool sp = false; /* first post-name space detected */
	char opt_name[30];
	char opt_value[512];
	int line = 1;
	int name_pos = 0;
	int value_pos = 0;
	FILE *file;

	if (!is_secure (config_file))
		fatal ("Configuration file is not secure: %s", config_file);

	if (!(file = fopen(config_file, "r"))) {
		log_errno ("Can't open config file", errno);
		return;
	}

	while ((ch = getc(file)) != EOF) {

		/* Skip comment */
		if (comm && ch != '\n')
			continue;

		/* Check for "+=" (list append) */
		if (ch != '=' && plus)
			fatal ("Error in config file: stray '+' on line %d!", line);

		/* Interpret parameter */
		if (ch == '\n') {
			comm = 0;

			opt_name[name_pos] = 0;
			opt_value[value_pos] = 0;

			if (name_pos) {
				if (value_pos == 0 && strncasecmp (opt_name, "Layout", 6))
					fatal ("Error in config file: "
					       "missing option value on line %d!", line);
				if (!set_option (opt_name, opt_value, append))
					fatal ("Error in config file on line %d!", line);
			}

			name_pos = 0;
			value_pos = 0;
			eq = 0;
			quote = 0;
			esc = 0;
			append = false;
			sp = false;

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

		else if (!esc && !eq && ch == '+')
			plus = true;

		else if (ch == '=' && !quote) {
			if (eq)
				fatal ("Error in config file: stray '=' on line %d!", line);
			if (name_pos == 0)
				fatal ("Error in config file: "
				       "missing option name on line %d!", line);
			append = plus;
			plus = false;
			eq = 1;
		}

		/* Turn on escape */
		else if (ch == '\\' && !esc)
			esc = 1;

		/* Embedded blank detection */
		else if (!eq && name_pos && isblank(ch))
			sp = true;
		else if (!eq && sp && !isblank(ch))
			fatal ("Error in config file: "
			       "embedded blank in option name on line %d!", line);

		/* Add char to parameter value */
		else if ((!isblank(ch) || quote) && eq) {
			if (esc && ch != '"') {
				if (sizeof(opt_value) == value_pos)
					fatal ("Error in config file: "
					       "option value on line %d is too long!", line);
				opt_value[value_pos++] = '\\';
			}

			if (sizeof(opt_value) == value_pos)
				fatal ("Error in config file: "
				       "option value on line %d is too long!", line);
			opt_value[value_pos++] = ch;
			esc = 0;
		}

		/* Add char to parameter name */
		else if (!isblank(ch) || quote) {
			if (sizeof(opt_name) == name_pos)
				fatal ("Error in config file: "
				       "option name on line %d is too long!", line);
			opt_name[name_pos++] = ch;
			esc = 0;
		}
	}

	if (name_pos || value_pos)
		fatal ("Parse error at the end of the config file (need end of "
				"line?)!");

	sanity_check ();

	fclose (file);
}

void options_free ()
{
	int i, ix;

	for (i = 0; i < options_num; i++) {
		if (options[i].type == OPTION_STR && options[i].value.str) {
			free (options[i].value.str);
			options[i].value.str = NULL;
		}
		else if (options[i].type == OPTION_LIST) {
			lists_strs_free (options[i].value.list);
			options[i].value.list = NULL;
			for (ix = 0; ix < options[i].count; ix += 1)
				free (((char **) options[i].constraints)[ix]);
		}
		else if (options[i].type == OPTION_SYMB)
			options[i].value.str = NULL;
		if (options[i].type & (OPTION_STR | OPTION_SYMB)) {
			if (options[i].check != check_length) {
				for (ix = 0; ix < options[i].count; ix += 1)
					free (((char **) options[i].constraints)[ix]);
			}
		}
		options[i].check = check_true;
		options[i].count = 0;
		if (options[i].constraints)
			free (options[i].constraints);
		options[i].constraints = NULL;
	}
}

int options_get_int (const char *name)
{
	int i = find_option (name, OPTION_INT);

	if (i == -1)
		fatal ("Tried to get wrong option '%s'!", name);

	return options[i].value.num;
}

bool options_get_bool (const char *name)
{
	int i = find_option (name, OPTION_BOOL);

	if (i == -1)
		fatal ("Tried to get wrong option '%s'!", name);

	return options[i].value.boolean;
}

char *options_get_str (const char *name)
{
	int i = find_option (name, OPTION_STR);

	if (i == -1)
		fatal ("Tried to get wrong option '%s'!", name);

	return options[i].value.str;
}

char *options_get_symb (const char *name)
{
	int i = find_option (name, OPTION_SYMB);

	if (i == -1)
		fatal ("Tried to get wrong option '%s'!", name);

	return options[i].value.str;
}

lists_t_strs *options_get_list (const char *name)
{
	int i = find_option (name, OPTION_LIST);

	if (i == -1)
		fatal ("Tried to get wrong option '%s'!", name);

	return options[i].value.list;
}

enum option_type options_get_type (const char *name)
{
	int i = find_option (name, OPTION_ANY);

	if (i == -1)
		return OPTION_FREE;

	return options[i].type;
}
