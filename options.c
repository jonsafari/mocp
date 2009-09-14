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

/* Return an index on an option in the options hashtable. If there is no such
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

/* Initializes a position on the options table. This is intended to be used at
 * initialization to make a table of valid options and its default values. */
static unsigned int option_init(const char *name, enum option_type type)
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

	options_num++;
	return pos;
}

/* Add an integer option to the options table. This is intended to be used at
 * initialization to make a table of valid options and its default values. */
static void option_add_int (const char *name, const int value)
{
	unsigned int pos=option_init(name,OPTION_INT);

	options[pos].value.num = value;
}

/* Add an string option to the options table. This is intended to be used at
 * initialization to make a table of valid options and its default values. */
static void option_add_str (const char *name, const char *value)
{
	unsigned int pos=option_init(name, OPTION_STR);

	options[pos].value.str = xstrdup (value);
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
	option_add_str ("OSSMixerChannel2", "master");
	option_add_str ("SoundDriver", "Jack, ALSA, OSS");
	option_add_int ("ShowHiddenFiles", 1);
	option_add_str ("AlsaDevice", "default");
	option_add_str ("AlsaMixer", "PCM");
	option_add_str ("AlsaMixer2", "Master");
	option_add_int ("HideFileExtension", 0);
	option_add_int ("ShowFormat", 1);
	option_add_str ("ShowTime", "IfAvailable");
	option_add_str ("Theme", NULL);
	option_add_str ("XTermTheme", NULL);
	option_add_str ("ForceTheme", NULL); /* Used when -T is set */
	option_add_str ("MOCDir", "~/.moc");
	option_add_int ("UseMmap", 0);
	option_add_int ("Precache", 1);
	option_add_int ("SavePlaylist", 1);
	option_add_str ("Keymap", NULL);
	option_add_int ("SyncPlaylist", 1);
	option_add_int ("InputBuffer", 512);
	option_add_int ("Prebuffering", 64);
	option_add_str ("JackOutLeft", "alsa_pcm:playback_1");
	option_add_str ("JackOutRight", "alsa_pcm:playback_2");
	option_add_int ("ASCIILines", 0);
	option_add_str ("FastDir1", NULL);
	option_add_str ("FastDir2", NULL);
	option_add_str ("FastDir3", NULL);
	option_add_str ("FastDir4", NULL);
	option_add_str ("FastDir5", NULL);
	option_add_str ("FastDir6", NULL);
	option_add_str ("FastDir7", NULL);
	option_add_str ("FastDir8", NULL);
	option_add_str ("FastDir9", NULL);
	option_add_str ("FastDir10", NULL);
	option_add_str ("ExecCommand1", NULL);
	option_add_str ("ExecCommand2", NULL);
	option_add_str ("ExecCommand3", NULL);
	option_add_str ("ExecCommand4", NULL);
	option_add_str ("ExecCommand5", NULL);
	option_add_str ("ExecCommand6", NULL);
	option_add_str ("ExecCommand7", NULL);
	option_add_str ("ExecCommand8", NULL);
	option_add_str ("ExecCommand9", NULL);
	option_add_str ("ExecCommand10", NULL);
	option_add_int ("Mp3IgnoreCRCErrors", 1);
	option_add_int ("SeekTime", 1);
	option_add_int ("SilentSeekTime", 5);
	option_add_str ("ResampleMethod", "Linear");
	option_add_int ("ForceSampleRate", 0);
	option_add_str ("HTTPProxy", NULL);
	option_add_int ("UseRealtimePriority", 0);
	option_add_int ("TagsCacheSize", 256);
	option_add_int ("PlaylistNumbering", 1);
	option_add_str ("Layout1",
			"directory:0,0,50%,100% playlist:50%,0,FILL,100%");
	option_add_str ("Layout2",
			"directory:0,0,100%,100% playlist:0,0,100%,100%");
	option_add_str ("Layout3", NULL);
	option_add_int ("FollowPlayedFile", 1);
	option_add_int ("CanStartInPlaylist", 1);
	option_add_int ("UseCursorSelection", 0);
	option_add_str ("ID3v1TagsEncoding", "WINDOWS-1250");
	option_add_int ("UseRCC", 1);
	option_add_int ("UseRCCForFilesystem", 1);
	option_add_int ("EnforceTagsEncoding", 0);
	option_add_int ("FileNamesIconv", 0);
	option_add_int ("NonUTFXterm", 0);
	option_add_int ("SetXtermTitle", 1);
	option_add_int ("SetScreenTitle", 1);
	option_add_int ("PlaylistFullPaths", 1);
	option_add_str ("BlockDecorators", "`\"'");
	option_add_int ("MessageLingerTime", 3);
	option_add_int ("PrefixQueuedMessages", 1);
	option_add_str ("ErrorMessagesQueued", "!");
	option_add_int ("Allow24bitOutput", 0);

	option_add_int ("ModPlug_Channels", 2);
	option_add_int ("ModPlug_Frequency", 44100);
	option_add_int ("ModPlug_Bits", 16);

	option_add_int ("ModPlug_Oversampling", 1);
	option_add_int ("ModPlug_NoiseReduction", 1);
	option_add_int ("ModPlug_Reverb", 0);
	option_add_int ("ModPlug_MegaBass", 0);
	option_add_int ("ModPlug_Surround", 0);

	option_add_str ("ModPlug_ResamplingMode", "FIR");

	option_add_int ("ModPlug_ReverbDepth", 0);
	option_add_int ("ModPlug_ReverbDelay", 0);
	option_add_int ("ModPlug_BassAmount", 0);
	option_add_int ("ModPlug_BassRange", 10);
	option_add_int ("ModPlug_SurroundDepth", 0);
	option_add_int ("ModPlug_SurroundDelay", 0);
	option_add_int ("ModPlug_LoopCount", 0);

	option_add_int ("TiMidity_Volume", 100);
	option_add_int ("TiMidity_Rate", 44100);
	option_add_int ("TiMidity_Bits", 16);
	option_add_int ("TiMidity_Channels", 2);
	option_add_str ("TiMidity_Config", NULL);

	option_add_int ("SidPlay2_DefaultSongLength", 180);
	option_add_int ("SidPlay2_MinimumSongLength", 0);
	option_add_str ("SidPlay2_Database", NULL);
	option_add_int ("SidPlay2_Frequency", 44100);
	option_add_int ("SidPlay2_Bits", 16);
	option_add_str ("SidPlay2_PlayMode", "M");
	option_add_int ("SidPlay2_Optimisation", 0);
	option_add_int ("SidPlay2_StartAtStart", 1);
	option_add_int ("SidPlay2_PlaySubTunes", 1);

	option_add_str ("OnSongChange", NULL);
	option_add_str ("OnStop", NULL);

	option_add_int ("Softmixer_SaveState", 1);

	option_add_int ("Equalizer_SaveState", 1);

	option_add_int ("QueueNextSongReturn", 0);

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
			|| !strcasecmp(name, "SavePlaylist")
			|| !strcasecmp(name, "SyncPlaylist")
			|| !strcasecmp(name, "Mp3IgnoreCRCErrors")
			|| !strcasecmp(name, "PlaylistNumbering")
			|| !strcasecmp(name, "FollowPlayedFile")
			|| !strcasecmp(name, "CanStartInPlaylist")
			|| !strcasecmp(name, "UseCursorSelection")
			|| !strcasecmp(name, "UseRCC")
			|| !strcasecmp(name, "UseRCCForFilesystem")
			|| !strcasecmp(name, "SetXtermTitle")
			|| !strcasecmp(name, "SetScreenTitle")
			|| !strcasecmp(name, "PlaylistFullPaths")
			|| !strcasecmp(name, "PrefixQueuedMessages")
			|| !strcasecmp(name, "Allow24bitOutput")
			|| !strcasecmp(name, "SidPlay2_StartAtStart")
			|| !strcasecmp(name, "SidPlay2_PlaySubTunes")
			|| !strcasecmp(name, "Softmixer_SaveState")
			|| !strcasecmp(name, "Equalizer_SaveState")
			|| !strcasecmp(name, "FileNamesIconv")
			|| !strcasecmp(name, "NonUTFXterm")
			|| !strcasecmp(name, "EnforceTagsEncoding"))
	{
		if (!(val == 1 || val == 0))
			return 0;
	}
	else if (!strcasecmp(name, "OutputBuffer")) {
		if (val < 128)
			return 0;
	}
	else if (!strcasecmp(name, "InputBuffer")) {
		if (val < 32)
			return 0;
	}
	else if (!strcasecmp(name, "Prebuffering")) {
		if (val < 0)
			return 0;
	}
	else if (!strcasecmp(name, "SeekTime")) {
		if (val < 1)
			return 0;
	}
	else if (!strcasecmp(name, "SilentSeekTime")) {
		if (val < 1)
			return 0;
	}
	else if (!strcasecmp(name, "ForceSampleRate")) {
		if (val < 0 || val > 500000)
			return 0;
	}
	else if (!strcasecmp(name, "TagsCacheSize")) {
		if (val < 0)
			return 0;
	}
	else if (!strcasecmp(name, "MessageLingerTime")) {
		if (val < 0)
			return 0;
	}
	else if (!strcasecmp(name, "ModPlug_Oversampling")
			|| !strcasecmp(name, "ModPlug_NoiseReduction")
			|| !strcasecmp(name, "ModPlug_Reverb")
			|| !strcasecmp(name, "ModPlug_MegaBass")
			|| !strcasecmp(name, "ModPlug_Surround"))
	{
		if (!(val == 0 || val == 1))
			return 0;
	}
	else if (!strcasecmp(name, "ModPlug_Channels")) {
		if (!(val == 1 || val == 2 ))
			return 0;
	}
	else if (!strcasecmp(name, "ModPlug_Frequency")) {
		if (!(val == 11025 || val == 22050 || val == 44100 || val == 48000))
			return 0;
	}
	else if (!strcasecmp(name, "ModPlug_Bits")) {
		if (!(val== 8 || val == 16 || val == 32))
			return 0;
	}
	else if (!strcasecmp(name, "ModPlug_ReverbDepth")) {
		if (!(val >= 0 && val <= 100))
			return 0;
	}
	else if (!strcasecmp(name, "ModPlug_ReverbDelay")) {
		if (val < 0)
			return 0;
	}
	else if (!strcasecmp(name, "ModPlug_BassAmount")) {
		if (!(val >= 0 && val <= 100))
			return 0;
	}
	else if (!strcasecmp(name, "ModPlug_BassRange")) {
		if (!(val >= 10 && val <= 100))
			return 0;
	}
	else if (!strcasecmp(name, "ModPlug_SurroundDepth")) {
		if (!(val >= 0 && val <= 100))
			return 0;
	}
	else if (!strcasecmp(name, "ModPlug_SurroundDelay")) {
		if (val < 0)
			return 0;
	}
	else if (!strcasecmp(name, "ModPlug_LoopCount")) {
		if (val < -1)
			return 0;
	}
	else if (!strcasecmp(name, "TiMidity_Channels")) {
		if (!(val == 1 || val == 2)  )
			return 0;
	}
	else if (!strcasecmp(name, "TiMidity_Bits")) {
		if (!(val == 8 || val == 16)  )
			return 0;
	}
	else if (!strcasecmp(name, "TiMidity_Volume")) {
		if (val < 0 || val > 800)
			return 0;
	}
	else if (!strcasecmp(name, "TiMidity_Rate")) {
		// not sure about the limits... I like 44100
		if (val < 8000 || val > 48000)
			return 0;
	}
	else if (!strcasecmp(name, "SidPlay2_DefaultSongLength")) {
		if (val < 0)
			return 0;
	}
	else if (!strcasecmp(name, "SidPlay2_MinimumSongLength")) {
		if (val < 0)
			return 0;
	}
	else if (!strcasecmp(name, "SidPlay2_Frequency")) {
		if (val < 4000 || val > 48000)
			return 0;
	}
	else if (!strcasecmp(name, "SidPlay2_Bits")) {
		if (!(val == 8 || val == 16))
			return 0;
	}
	else if (!strcasecmp(name, "SidPlay2_Optimisation")) {
		if (val < 0 || val > 2)
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
	else if (!strcasecmp(name, "ShowTime")) {
		if (strcasecmp(val, "yes") && strcasecmp(val, "no")
				&& strcasecmp(val, "IfAvailable"))
			return 0;
	}
	else if (!strcmp(name, "ResampleMethod")) {
		if (strcasecmp(val, "SincBestQuality")
				&& strcasecmp(val, "SincMediumQuality")
				&& strcasecmp(val, "SincFastest")
				&& strcasecmp(val, "ZeroOrderHold")
				&& strcasecmp(val, "Linear"))
			return 0;
	}
	else if (!strcasecmp(name, "BlockDecorators")) {
		if (strlen(val) != 3)
			return 0;
	}
	else if (!strcasecmp(name, "ModPlug_ResamplingMode")) {
		if (strcasecmp(val, "FIR")
			&& strcasecmp(val, "SPLINE")
			&& strcasecmp(val, "LINEAR")
			&& strcasecmp(val, "NEAREST"))
			return 0;
	}
	else if (!strcasecmp(name, "SidPlay2_PlayMode")) {
		if (strcasecmp(val, "M")
			&& strcasecmp(val, "S")
			&& strcasecmp(val, "L")
			&& strcasecmp(val, "R"))
			return 0;
	}
	return 1;
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
