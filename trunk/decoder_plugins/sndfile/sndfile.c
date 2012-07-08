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

#include <string.h>
#include <assert.h>
#include <sndfile.h>

#define DEBUG

#include "common.h"
#include "decoder.h"
#include "server.h"
#include "log.h"
#include "files.h"

/* TODO:
 * - sndfile is not thread-safe: use a mutex?
 * - some tags can be read.
 */

struct sndfile_data
{
	SNDFILE *sndfile;
	SF_INFO snd_info;
	struct decoder_error error;
};

static void *sndfile_open (const char *file)
{
	struct sndfile_data *data;

	data = (struct sndfile_data *)xmalloc (sizeof(struct sndfile_data));

	decoder_error_init (&data->error);
	memset (&data->snd_info, 0, sizeof(data->snd_info));

	if (!(data->sndfile = sf_open(file, SFM_READ,
					&data->snd_info))) {

		/* FIXME: sf_strerror is not thread save with NULL argument */
		decoder_error (&data->error, ERROR_FATAL, 0,
				"Can't open file: %s", sf_strerror(NULL));
		return data;
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

	if (data->sndfile)
		sf_close (data->sndfile);

	decoder_error_clear (&data->error);
	free (data);
}

static void sndfile_info (const char *file_name, struct file_tags *info,
		const int tags_sel)
{
	if (tags_sel & TAGS_TIME) {
		struct sndfile_data *data = sndfile_open (file_name);

		if (data->sndfile) {

			/* I don't know why, but this condition is in the
			 * examples. */
			if (data->snd_info.frames <= 0x7FFFFFFF) {
				info->time = data->snd_info.frames
					/ data->snd_info.samplerate;
			}
		}

		sndfile_close (data);
	}
}

static int sndfile_seek (void *void_data, int sec)
{
	struct sndfile_data *data = (struct sndfile_data *)void_data;
	int res;

	assert (sec >= 0);

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
	sound_params->fmt = SFMT_FLOAT;

	return sf_readf_float (data->sndfile, (float *)buf,
			buf_len / sizeof(float) / data->snd_info.channels)
		* sizeof(float) * data->snd_info.channels;
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

static void sndfile_get_name (const char *file, char buf[4])
{
	char *ext;

	ext = ext_pos (file);
	if (!strcasecmp (ext, "au") || !strcasecmp (ext, "snd"))
		strcpy (buf, "AU");
	else if (!strcasecmp (ext, "wav"))
		strcpy (buf, "WAV");
	else if (!strcasecmp (ext, "aif") || !strcasecmp (ext, "aiff"))
		strcpy (buf, "AIF");
	else if (!strcasecmp (ext, "8svx"))
		strcpy (buf, "SVX");
	else if (!strcasecmp (ext, "sph"))
		strcpy (buf, "SPH");
	else if (!strcasecmp (ext, "sf"))
		strcpy (buf, "IRC");
	else if (!strcasecmp (ext, "voc"))
		strcpy (buf, "VOC");
}

static int sndfile_our_format_ext (const char *ext)
{
	return !strcasecmp (ext, "au")
		|| !strcasecmp (ext, "snd")
		|| !strcasecmp (ext, "wav")
		|| !strcasecmp (ext, "aif")
		|| !strcasecmp (ext, "aiff")
		|| !strcasecmp (ext, "8svx")
		|| !strcasecmp (ext, "sph")
		|| !strcasecmp (ext, "sf")
		|| !strcasecmp (ext, "voc");
}

static void sndfile_get_error (void *prv_data, struct decoder_error *error)
{
	struct sndfile_data *data = (struct sndfile_data *)prv_data;

	decoder_error_copy (error, &data->error);
}

static struct decoder sndfile_decoder = {
	DECODER_API_VERSION,
	NULL,
	NULL,
	sndfile_open,
	NULL,
	NULL,
	sndfile_close,
	sndfile_decode,
	sndfile_seek,
	sndfile_info,
	sndfile_get_bitrate,
	sndfile_get_duration,
	sndfile_get_error,
	sndfile_our_format_ext,
	NULL,
	sndfile_get_name,
	NULL,
	NULL,
	NULL
};

struct decoder *plugin_init ()
{
	return &sndfile_decoder;
}
