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

#ifdef HAVE_FLAC
# include "flac.h"
#endif

#include "wav.h"

struct file_type_data {
	char **ext;			/* NULL terminated file extension
					   list */
	struct decoder_funcs *funcs;
	char name[4];			/* short format name */
};

static struct file_type_data types[8];
static int types_num = 0;

/* Find the index in table types for the given file. Return -1 if not found. */
static int find_type (char *file)
{
	char *ext = ext_pos (file);
	int i, j;

	if (ext)
		for (i = 0; i < types_num; i++) {
			j = 0;
			while (types[i].ext[j])
				if (!strcasecmp(ext, types[i].ext[j++]))
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

static char **mk_ext_table (const int num)
{
	char **tab = (char **)xmalloc (sizeof(char **) * ((num) + 1));
	
	tab[num] = NULL;
	return tab;
}

void file_types_init ()
{
	types[types_num].ext = mk_ext_table (1);
	types[types_num].ext[0] = "wav";
	strcpy (types[types_num].name, "WAV");
	types[types_num++].funcs = wav_get_funcs ();

#ifdef HAVE_MAD
	types[types_num].ext = mk_ext_table (1);
	types[types_num].ext[0] = "mp3";
	strcpy (types[types_num].name, "MP3");
	types[types_num++].funcs = mp3_get_funcs ();
#endif

#ifdef HAVE_VORBIS
	types[types_num].ext = mk_ext_table (1);	
	types[types_num].ext[0] = "ogg";
	strcpy (types[types_num].name, "OGG");
	types[types_num++].funcs = ogg_get_funcs ();
#endif

#ifdef HAVE_FLAC
	types[types_num].ext = mk_ext_table (2);
	types[types_num].ext[0] = "flac";
	types[types_num].ext[1] = "fla";
	strcpy (types[types_num].name, "FLA");
	types[types_num++].funcs = flac_get_funcs ();
#endif
}
