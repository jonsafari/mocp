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
#include "files.h"

#ifdef HAVE_MAD
# include "mp3.h"
#endif

#ifdef HAVE_VORBIS
# include "ogg.h"
#endif

#include "wav.h"

struct file_type_data {
	char *ext;			/* file extension */
	struct decoder_funcs *funcs;
	char name[4];			/* short format name */
};

static struct file_type_data types[8];
static int types_num = 0;

/* Find the index in table types for the given file. Return -1 if not found. */
static int find_type (char *file)
{
	char *ext = ext_pos (file);
	int i;

	if (ext)
		for (i = 0; i < types_num; i++)
			if (!strcasecmp(ext, types[i].ext))
				return i;
	return -1;
}

int is_sound_file (char *name)
{
	return find_type(name) != -1 ? 1 : 0;
}

/* Return short format name for the given file or NULL if not found. */
char *format_name (char *file)
{
	int i;
	
	if ((i = find_type(file)) != -1)
		return types[i].name;

	return NULL;

}

struct decoder_funcs *get_decoder_funcs (char *file)
{
	int i;
	
	if ((i = find_type(file)) != -1)
		return types[i].funcs;

	return NULL;
}

void file_types_init ()
{
	types[types_num].ext = "wav";
	strcpy (types[types_num].name, "WAV");
	types[types_num++].funcs = wav_get_funcs ();

#ifdef HAVE_MAD
	types[types_num].ext = "mp3";
	strcpy (types[types_num].name, "MP3");
	types[types_num++].funcs = mp3_get_funcs ();
#endif

#ifdef HAVE_VORBIS
	types[types_num].ext = "ogg";
	strcpy (types[types_num].name, "OGG");
	types[types_num++].funcs = ogg_get_funcs ();
#endif
}
