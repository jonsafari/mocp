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

#include "wav.h"

struct file_type {
	char *ext;
	struct decoder_funcs *funcs;
};

static struct file_type types[8];
static int types_num = 0;

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
	int i;

	if (ext)
		for (i = 0; i < types_num; i++)
			if (!strcasecmp(ext, types[i].ext))
				return 1;
	return 0;
}

struct decoder_funcs *get_decoder_funcs (char *file)
{
	char *ext = ext_pos (file);
	int i;

	if (ext)
		for (i = 0; i < types_num; i++)
			if (!strcasecmp(ext, types[i].ext))
				return types[i].funcs;
	return NULL;
}

void file_types_init ()
{
	types[types_num].ext = "wav";
	types[types_num++].funcs = wav_get_funcs ();

#ifdef HAVE_MAD
	types[types_num].ext = "mp3";
	types[types_num++].funcs = mp3_get_funcs ();
#endif

#ifdef HAVE_VORBIS
	types[types_num].ext = "ogg";
	types[types_num++].funcs = ogg_get_funcs ();
#endif
}
