/*
 * MOC - music on console
 * Copyright (C) 2002-2004 Damian Pietras <daper@daper.net>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 */

/*  Most of this code was taken from:
 *  XMMS - Cross-platform multimedia player
 *  Copyright (C) 1998-2000  Peter Alm, Mikael Alm, Olle Hallnas,
 *  Thomas Nilsson and 4Front Technologies
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include "file_types.h"
#include "server.h"
#include "main.h"
#include "log.h"

#define	WAVE_FORMAT_UNKNOWN		(0x0000)
#define	WAVE_FORMAT_PCM			(0x0001)
#define	WAVE_FORMAT_ADPCM		(0x0002)
#define	WAVE_FORMAT_ALAW		(0x0006)
#define	WAVE_FORMAT_MULAW		(0x0007)
#define	WAVE_FORMAT_OKI_ADPCM		(0x0010)
#define	WAVE_FORMAT_DIGISTD		(0x0015)
#define	WAVE_FORMAT_DIGIFIX		(0x0016)
#define	IBM_FORMAT_MULAW         	(0x0101)
#define	IBM_FORMAT_ALAW			(0x0102)
#define	IBM_FORMAT_ADPCM         	(0x0103)

struct wav_data
{
	FILE *file;
	short channels;
	int rate;
	int format;
	long len;
	long pcm_offset;
	int time;
};

static void *wav_open (const char *file)
{
	char magic[4];
	unsigned long len;
	struct wav_data *data;
	short format_tag;
	short bits_per_sample;
	short avg_bytes_per_sec;
	short block_align;

	data = (struct wav_data *)xmalloc (sizeof(struct wav_data));

	if (!(data->file = fopen(file, "rb"))) {
		error ("Can't open WAV file: %s", strerror(errno));
		free (data);
		return NULL;
	}

	if (fread(magic, 1, 4, data->file) != 4
			|| strncmp(magic, "RIFF", 4)) {
		fclose(data->file);
		error ("Bad wave header.");
		free (data);
		return NULL;
	}
	
	if (fread(&len, 4, 1, data->file) != 1) {
		fclose(data->file);
		error ("Bad wave header.");
		free (data);
		return NULL;
	}
	if (fread(magic, 1, 4, data->file) != 4
			|| strncmp(magic, "WAVE", 4)) {
		fclose(data->file);
		error ("Bad wave header.");
		free (data);
		return NULL;
	}
	
	for (;;)
	{
		if (fread(magic, 1, 4, data->file) != 4
				|| fread(&len, 4, 1, data->file) != 1) {
			fclose(data->file);
			error ("Error in the WAVE file.");
			free (data);
			return NULL;
		}
		if (!strncmp("fmt ", magic, 4))
			break;
		fseek (data->file, len, SEEK_CUR);
	}
	
	if (len < 16) {
		error ("WAV header too short");
		fclose (data->file);
		free (data);
		return NULL;
	}
	
	if (fread(&format_tag, 2, 1, data->file) != 1) {
		error ("WAV header broken");
		fclose (data->file);
		free (data);
		return NULL;
	}
	
	switch (format_tag) {
		case WAVE_FORMAT_UNKNOWN:
		case WAVE_FORMAT_ALAW:
		case WAVE_FORMAT_MULAW:
		case WAVE_FORMAT_ADPCM:
		case WAVE_FORMAT_OKI_ADPCM:
		case WAVE_FORMAT_DIGISTD:
		case WAVE_FORMAT_DIGIFIX:
		case IBM_FORMAT_MULAW:
		case IBM_FORMAT_ALAW:
		case IBM_FORMAT_ADPCM:
			fclose(data->file);
			free (data);
			error ("Unknown WAVE format.");
			return NULL;
	}
	
	if (fread(&data->channels, 2, 1, data->file) != 1
			|| fread(&data->rate, 4, 1, data->file) != 1
			|| fread(&avg_bytes_per_sec, 4, 1, data->file) != 1
			|| fread(&block_align, 2, 1, data->file) != 1
			|| fread(&bits_per_sample, 2, 1, data->file) != 1) {
		fclose(data->file);
		free (data);
		error ("Bad WAVE header.");
		return NULL;
	}
	
	if (bits_per_sample != 8 &&
			bits_per_sample != 16) {
		fclose(data->file);
		free (data);
		error ("Unknown bit per sample value.");
		return NULL;
	}

	data->format = bits_per_sample / 8;
	
	len -= 16;
	if (len)
		fseek(data->file, len, SEEK_CUR);

	for (;;)
	{
		if (fread(magic, 4, 1, data->file) != 1) {
			fclose(data->file);
			free (data);
			error ("Bad WAV header.");
			return NULL;
		}

		if (fread(&len, 4, 1, data->file) != 1) {
			fclose(data->file);
			free (data);
			error ("Bad WAV header.");
			return NULL;
		}
		if (!strncmp("data", magic, 4))
			break;
		fseek (data->file, len, SEEK_CUR);
	}
	
	if ((data->pcm_offset = ftell(data->file)) == -1) {
		fclose(data->file);
		free (data);
		error ("Can't ftell(): %s", strerror(errno));
		return NULL;
	}
	data->len = len;
	data->time = len / (data->rate * data->channels * data->format);
	logit ("PCM: %ld", data->pcm_offset);

	return data;
}

static void wav_close (void *void_data)
{
	struct wav_data *data = (struct wav_data *)void_data;

	fclose (data->file);
	free (data);
}

static void wav_info (const char *file_name, struct file_tags *info)
{
	struct wav_data *data;
	
	data = wav_open (file_name);
	info->time = data->time;
	wav_close (data);

	return;
}

static int wav_seek (void *void_data, int sec)
{
	struct wav_data *data = (struct wav_data *)void_data;
	long to = (data->format * data->channels * data->rate) * sec
		+ data->pcm_offset;
	
	if (to < data->pcm_offset || to > data->len)
		return -1;

	logit ("SEEK to %ld", to);

	return fseek (data->file, to, SEEK_SET) == -1 ? -1 : sec;
}

static int wav_decode (void *void_data, char *buf, int buf_len,
		struct sound_params *sound_params)
{
	struct wav_data *data = (struct wav_data *)void_data;

	sound_params->channels = data->channels;
	sound_params->rate = data->rate;
	sound_params->format = data->format;

	return fread (buf, 1, buf_len, data->file);
}

static struct decoder_funcs decoder_funcs = {
	wav_open,
	wav_close,
	wav_decode,
	wav_seek,
	wav_info
};

struct decoder_funcs *wav_get_funcs ()
{
	return &decoder_funcs;
}
