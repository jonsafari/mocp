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

#include <stdio.h>
#include <string.h>

#include "main.h"
#include "file_types.h"

#ifdef HAVE_MAD
# include "mp3.h"
#endif

#ifdef HAVE_VORBIS
# include "ogg.h"
#endif

struct file_type {
	char *ext;
	info_func_t info;
	play_func_t play;
};

static struct file_type types[] = {
#ifdef HAVE_MAD
	{ "mp3",	mp3_info,	mp3_play },
#endif
#ifdef HAVE_VORBIS
	{ "ogg",	ogg_info,	ogg_play },
#endif
#if 0
	{ "wav",	wav_info,	wav_play },
#endif

	/* Fake entry to avoid compile errors about lack or unneded comma. */
	{ "",		NULL,		NULL }
};

/* Return the file extension position or NULL if the file has no extension. */
static char *ext_pos (char *file)
{
	char *ext = strrchr (file, '.');
	char *slash = strrchr (file, '/');

	/* don't treat dot in ./file as a dot before extension */
	if (ext && (!slash || slash < ext))
		ext++;
	else
		ext = NULL;

	return ext;
}

int is_sound_file (char *name)
{
	char *ext = ext_pos (name);
	unsigned int i;

	if (ext)
		for (i = 0; i < sizeof(types)/sizeof(types[0]); i++)
			if (!strcasecmp(ext, types[i].ext))
				return 1;
	return 0;
}

info_func_t get_info_func (char *file)
{
	char *ext = ext_pos (file);
	unsigned int i;

	if (ext)
		for (i = 0; i < sizeof(types)/sizeof(types[0]); i++)
			if (!strcasecmp(ext, types[i].ext))
				return types[i].info;
	return NULL;
}

play_func_t get_play_func (char *file)
{
	char *ext = ext_pos (file);
	unsigned int i;

	if (ext)
		for (i = 0; i < sizeof(types)/sizeof(types[0]); i++)
			if (!strcasecmp(ext, types[i].ext))
				return types[i].play;
	return NULL;
}
