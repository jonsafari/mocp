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
#include <assert.h>

#include "main.h"
#include "file_types.h"
#include "files.h"

#ifdef HAVE_MAD
# include "mp3.h"
#endif

#ifdef HAVE_VORBIS
# include "ogg.h"
#endif

#ifdef HAVE_FLAC
# include "flac.h"
#endif

#ifdef HAVE_SNDFILE
# include "sndfile_formats.h"
#endif

struct file_type_data {
	char **ext;			/* file extension list */
	int ext_max;
	int ext_num;
	struct decoder_funcs *funcs;
	char name[4];			/* short format name */
};

static struct file_type_data types[20];
static int types_num = 0;

/* Find the index in table types for the given file. Return -1 if not found. */
static int find_type (char *file)
{
	char *ext = ext_pos (file);
	int i, j;

	if (ext)
		for (i = 0; i < types_num; i++) {
			for (j = 0; j < types[i].ext_num; j++)
				if (!strcasecmp(ext, types[i].ext[j]))
					return i;
		}
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

/* Add a file type to the table with ext_num file extensions.
 * Return the index of the type. */
static int add_type (const int ext_num, const char *name,
		struct decoder_funcs *funcs)
{
	types[types_num].ext = (char **)xmalloc (sizeof(char **) * ext_num);
	types[types_num].ext_max = ext_num;
	types[types_num].ext_num = 0;
	strncpy (types[types_num].name, name,
			sizeof(types[types_num].name) - 1);
	types[types_num].funcs = funcs;

	return types_num++;
}

static void add_ext (const int idx, const char *ext)
{
	assert (types[idx].ext_num < types[idx].ext_max);
	types[idx].ext[types[idx].ext_num++] = xstrdup (ext);
}

void file_types_init ()
{
	int i;
	
#ifdef HAVE_MAD
	i = add_type (1, "MP3", mp3_get_funcs());
	add_ext (i, "mp3");
#endif

#ifdef HAVE_VORBIS
	i = add_type (1, "OGG", ogg_get_funcs());
	add_ext (i, "ogg");
#endif

#ifdef HAVE_FLAC
	i = add_type (2, "FLA", flac_get_funcs());
	add_ext (i, "fla");
	add_ext (i, "flac");
#endif

#ifdef HAVE_SNDFILE

	/* Not all file types supported bu libsndfile, but I can test only
	 * them. */
	
	i = add_type (2, "AU", sndfile_get_funcs());
	add_ext (i, "au");
	add_ext (i, "snd");

	i = add_type (1, "WAV", sndfile_get_funcs());
	add_ext (i, "wav");

	i = add_type (1, "AIF", sndfile_get_funcs());
	add_ext (i, "aif");

	i = add_type (1, "SVX", sndfile_get_funcs());
	add_ext (i, "8svx");

	i = add_type (1, "SPH", sndfile_get_funcs());
	add_ext (i, "sph");

	i = add_type (1, "IRC", sndfile_get_funcs());
	add_ext (i, "sf");
	
	i = add_type (1, "VOC", sndfile_get_funcs());
	add_ext (i, "voc");
#endif
}

void file_types_cleanup ()
{
	int i, j;

	for (i = 0; i < types_num; i++)
		for (j = 0; j < types[i].ext_num; j++)
			free (types[i].ext[j]);
}
