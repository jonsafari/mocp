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

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>
#include <errno.h>
#include <string.h>
#include <strings.h>
#include <assert.h>
#include <sndfile.h>

#define DEBUG

#include "common.h"
#include "decoder.h"
#include "server.h"
#include "log.h"
#include "files.h"
#include "lists.h"

/* TODO:
 * - sndfile is not thread-safe: use a mutex?
 * - some tags can be read.
 */

struct sndfile_data
{
	SNDFILE *sndfile;
	SF_INFO snd_info;
	struct decoder_error error;
	bool timing_broken;
};

static lists_t_strs *supported_extns = NULL;

static void load_extn_list ()
{
	const int counts[] = {SFC_GET_SIMPLE_FORMAT_COUNT,
	                      SFC_GET_FORMAT_MAJOR_COUNT};
	const int formats[] = {SFC_GET_SIMPLE_FORMAT,
	                       SFC_GET_FORMAT_MAJOR};

	supported_extns = lists_strs_new (16);

	for (size_t ix = 0; ix < ARRAY_SIZE(counts); ix += 1) {
		int limit;
		SF_FORMAT_INFO format_info;

		sf_command (NULL, counts[ix], &limit, sizeof (limit));
		for (int iy = 0 ; iy < limit ; iy += 1) {
			format_info.format = iy ;
			sf_command (NULL, formats[ix], &format_info, sizeof (format_info));
			if (!lists_strs_exists (supported_extns, format_info.extension))
				lists_strs_append (supported_extns, format_info.extension);
		}
	}

	/* These are synonyms of supported extensions. */
	if (lists_strs_exists (supported_extns, "aiff"))
		lists_strs_append (supported_extns, "aif");
	if (lists_strs_exists (supported_extns, "au"))
		lists_strs_append (supported_extns, "snd");
	if (lists_strs_exists (supported_extns, "wav")) {
		lists_strs_append (supported_extns, "nist");
		lists_strs_append (supported_extns, "sph");
	}
	if (lists_strs_exists (supported_extns, "iff"))
		lists_strs_append (supported_extns, "svx");
	if (lists_strs_exists (supported_extns, "oga"))
		lists_strs_append (supported_extns, "ogg");
	if (lists_strs_exists (supported_extns, "sf"))
		lists_strs_append (supported_extns, "ircam");
	if (lists_strs_exists (supported_extns, "mat")) {
		lists_strs_append (supported_extns, "mat4");
		lists_strs_append (supported_extns, "mat5");
	}
}

static void sndfile_init ()
{
	load_extn_list ();
}

static void sndfile_destroy ()
{
	lists_strs_free (supported_extns);
}

/* Return true iff libsndfile's frame count is unknown or miscalculated. */
static bool is_timing_broken (int fd, struct sndfile_data *data)
{
	int rc;
	struct stat buf;
	SF_INFO *info = &data->snd_info;

	if (info->frames == SF_COUNT_MAX)
			return true;

	if (info->frames / info->samplerate > INT32_MAX)
			return true;

	/* The libsndfile code warns of miscalculation for huge files of
	 * specific formats, but it's unclear if others are known to work
	 * or the test is just omitted for them.  We'll assume they work
	 * until it's shown otherwise. */
	switch (info->format & SF_FORMAT_TYPEMASK) {
	case SF_FORMAT_AIFF:
	case SF_FORMAT_AU:
	case SF_FORMAT_SVX:
	case SF_FORMAT_WAV:
		rc = fstat (fd, &buf);
		if (rc == -1) {
			log_errno ("Can't stat file", errno);
			/* We really need to return "unknown" here. */
			return false;
		}

		if (buf.st_size > UINT32_MAX)
			return true;
	}

	return false;
}

static void *sndfile_open (const char *file)
{
	int fd;
	struct sndfile_data *data;

	data = (struct sndfile_data *)xmalloc (sizeof(struct sndfile_data));

	decoder_error_init (&data->error);
	memset (&data->snd_info, 0, sizeof(data->snd_info));
	data->timing_broken = false;

	fd = open (file, O_RDONLY);
	if (fd == -1) {
		char *err = xstrerror (errno);
		decoder_error (&data->error, ERROR_FATAL, 0,
		               "Can't open file: %s", err);
		free (err);
		return data;
	}

	/* sf_open_fd() close()s 'fd' on error and in sf_close(). */
	data->sndfile = sf_open_fd (fd, SFM_READ, &data->snd_info, SF_TRUE);
	if (!data->sndfile) {
		/* FIXME: sf_strerror is not thread safe with NULL argument */
		decoder_error (&data->error, ERROR_FATAL, 0,
				"Can't open file: %s", sf_strerror(NULL));
		return data;
	}

	/* If the timing is broken, sndfile only decodes up to the broken value. */
	data->timing_broken = is_timing_broken (fd, data);
	if (data->timing_broken) {
		decoder_error (&data->error, ERROR_FATAL, 0,
		               "File too large for audio format!");
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
		struct sndfile_data *data;

		data = sndfile_open (file_name);
		if (data->sndfile && !data->timing_broken)
			info->time = data->snd_info.frames / data->snd_info.samplerate;
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

static int sndfile_get_bitrate (void *unused ATTR_UNUSED)
{
	return -1;
}

static int sndfile_get_duration (void *void_data)
{
	int result;
	struct sndfile_data *data = (struct sndfile_data *)void_data;

	result = -1;
	if (!data->timing_broken)
		result = data->snd_info.frames / data->snd_info.samplerate;

	return result;
}

static void sndfile_get_name (const char *file, char buf[4])
{
	char *ext;

	ext = ext_pos (file);
	if (ext) {
		if (!strcasecmp (ext, "snd"))
			strcpy (buf, "AU");
		else if (!strcasecmp (ext, "8svx"))
			strcpy (buf, "SVX");
		else if (!strcasecmp (ext, "oga"))
			strcpy (buf, "OGG");
		else if (!strcasecmp (ext, "sf") || !strcasecmp (ext, "icram"))
			strcpy (buf, "IRC");
		else if (!strcasecmp (ext, "mat4") || !strcasecmp (ext, "mat5"))
			strcpy (buf, "MAT");
	}
}

static int sndfile_our_format_ext (const char *ext)
{
	return lists_strs_exists (supported_extns, ext);
}

static void sndfile_get_error (void *prv_data, struct decoder_error *error)
{
	struct sndfile_data *data = (struct sndfile_data *)prv_data;

	decoder_error_copy (error, &data->error);
}

static struct decoder sndfile_decoder = {
	DECODER_API_VERSION,
	sndfile_init,
	sndfile_destroy,
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
