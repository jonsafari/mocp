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

#define DEBUG

#include <string.h>
#include <sndfile.h>
#include "sndfile_formats.h"
#include "file_types.h"
#include "main.h"
#include "server.h"
#include "playlist.h"
#include "log.h"

/* TODO:
 * - sndfile is not thread-safe: use a mutex?
 * - some tags can be read.
 */

struct sndfile_data
{
	SNDFILE *sndfile;
	SF_INFO snd_info;
};

static void *sndfile_open (const char *file)
{
	struct sndfile_data *data;
	
	data = (struct sndfile_data *)xmalloc (sizeof(struct sndfile_data));

	memset (&data->snd_info, 0, sizeof(data->snd_info));
	
	if (!(data->sndfile = sf_open(file, SFM_READ,
					&data->snd_info))) {

		/* sf_strerror is not thread save with NULL argument */
		error ("Can't open file: %s", sf_strerror(NULL));
		
		free (data);
		return NULL;
	}

	if (data->snd_info.channels > 2) {
		error ("The file has more than 2 channels, this is not "
				"supported.");
		sf_close (data->sndfile);
		free (data);
		return NULL;
	}

	debug ("Opened file %s", file);
	debug ("Channels: %d", data->snd_info.channels);
	debug ("Format: %08X", data->snd_info.format);
	debug ("Sample rate: %d", data->snd_info.samplerate);

	return data;
}

static void sndfile_close (void *void_data)
{
	struct sndfile_data *data = (struct sndfile_data *)void_data;

	sf_close (data->sndfile);
	free (data);
}

static void sndfile_info (const char *file_name, struct file_tags *info,
		const int tags_sel)
{
	if (tags_sel & TAGS_TIME) {
		struct sndfile_data *data;
		
		if ((data = sndfile_open(file_name))) {
			
			/* I don't know why, but this condition is in the
			 * examples. */
			if (data->snd_info.frames <= 0x7FFFFFFF) {
				info->time = data->snd_info.frames
					/ data->snd_info.samplerate;
			}
			sndfile_close (data);
		}
	}
}

static int sndfile_seek (void *void_data, int sec)
{
	struct sndfile_data *data = (struct sndfile_data *)void_data;
	int res;

	res = sf_seek (data->sndfile, data->snd_info.samplerate * sec,
			SEEK_SET);

	if (res < 0)
		return -1;

	return res / data->snd_info.samplerate;
}

static int sndfile_decode (void *void_data, char *buf, int buf_len,
		struct sound_params *sound_params)
{
	struct sndfile_data *data = (struct sndfile_data *)void_data;

	sound_params->channels = data->snd_info.channels;
	sound_params->rate = data->snd_info.samplerate;
	sound_params->format = 2;
	
	return sf_readf_short (data->sndfile, (short *)buf,
			buf_len / 2 / data->snd_info.channels)
		* 2 * data->snd_info.channels;
}

static int sndfile_get_bitrate (void *void_data ATTR_UNUSED)
{
	return -1;
}

static int sndfile_get_duration (void *void_data)
{
	struct sndfile_data *data = (struct sndfile_data *)void_data;

	return	data->snd_info.frames <= 0x7FFFFFFF ?
			data->snd_info.frames / data->snd_info.samplerate
			: -1;
}

static struct decoder_funcs decoder_funcs = {
	sndfile_open,
	sndfile_close,
	sndfile_decode,
	sndfile_seek,
	sndfile_info,
	sndfile_get_bitrate,
	sndfile_get_duration
};

struct decoder_funcs *sndfile_get_funcs ()
{
	return &decoder_funcs;
}
