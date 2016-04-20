/*
 * MOC - music on console
 * Copyright (C) 2002 - 2005 Damian Pietras <daper@daper.net>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <limits.h>
#include <inttypes.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <errno.h>
#include <assert.h>
#ifndef HAVE_TREMOR
#include <vorbis/vorbisfile.h>
#include <vorbis/codec.h>
#else
#include <tremor/ivorbisfile.h>
#include <tremor/ivorbiscodec.h>
#endif

#define DEBUG

#include "common.h"
#include "log.h"
#include "decoder.h"
#include "io.h"
#include "audio.h"

/* These merely silence compiler warnings about unused definitions in
 * the Vorbis library header files. */
#if defined(HAVE_VAR_ATTRIBUTE_UNUSED) && !defined(HAVE_TREMOR)
static ov_callbacks *vorbis_unused[] ATTR_UNUSED = {
	&OV_CALLBACKS_DEFAULT,
	&OV_CALLBACKS_NOCLOSE,
	&OV_CALLBACKS_STREAMONLY,
	&OV_CALLBACKS_STREAMONLY_NOCLOSE
};
#endif

/* Tremor defines time as 64-bit integer milliseconds. */
#ifndef HAVE_TREMOR
static const int64_t time_scaler = 1;
#else
static const int64_t time_scaler = 1000;
#endif

struct vorbis_data
{
	struct io_stream *stream;
	OggVorbis_File vf;
	int last_section;
	int bitrate;
	int avg_bitrate;
	int duration;
	struct decoder_error error;
	int ok; /* was this stream successfully opened? */

	int tags_change; /* the tags were changed from the last call of
	                    ogg_current_tags() */
	struct file_tags *tags;
};

static void get_comment_tags (OggVorbis_File *vf, struct file_tags *info)
{
	int i;
	vorbis_comment *comments;

	comments = ov_comment (vf, -1);
	for (i = 0; i < comments->comments; i++) {
		if (!strncasecmp(comments->user_comments[i], "title=",
				 strlen ("title=")))
			info->title = xstrdup(comments->user_comments[i]
					+ strlen ("title="));
		else if (!strncasecmp(comments->user_comments[i],
					"artist=", strlen ("artist=")))
			info->artist = xstrdup (
					comments->user_comments[i]
					+ strlen ("artist="));
		else if (!strncasecmp(comments->user_comments[i],
					"album=", strlen ("album=")))
			info->album = xstrdup (
					comments->user_comments[i]
					+ strlen ("album="));
		else if (!strncasecmp(comments->user_comments[i],
					"tracknumber=",
					strlen ("tracknumber=")))
			info->track = atoi (comments->user_comments[i]
					+ strlen ("tracknumber="));
		else if (!strncasecmp(comments->user_comments[i],
					"track=", strlen ("track=")))
			info->track = atoi (comments->user_comments[i]
					+ strlen ("track="));
	}
}

/* Return a description of an ov_*() error. */
static const char *vorbis_strerror (const int code)
{
	const char *result;

	switch (code) {
		case OV_EREAD:
			result = "read error";
			break;
		case OV_ENOTVORBIS:
			result = "not a vorbis file";
			break;
		case OV_EVERSION:
			result = "vorbis version mismatch";
			break;
		case OV_EBADHEADER:
			result = "invalid Vorbis bitstream header";
			break;
		case OV_EFAULT:
			result = "internal (vorbis) logic fault";
			break;
		default:
			result = "unknown error";
	}

	return result;
}

/* Fill info structure with data from ogg comments */
static void vorbis_tags (const char *file_name, struct file_tags *info,
		const int tags_sel)
{
	OggVorbis_File vf;
	FILE *file;
	int err_code;

	if (!(file = fopen (file_name, "r"))) {
		log_errno ("Can't open an OGG file", errno);
		return;
	}

	/* ov_test() is faster than ov_open(), but we can't read file time
	 * with it. */
	if (tags_sel & TAGS_TIME)
		err_code = ov_open(file, &vf, NULL, 0);
	else
		err_code = ov_test(file, &vf, NULL, 0);

	if (err_code < 0) {
		logit ("Can't open %s: %s", file_name, vorbis_strerror (err_code));
		fclose (file);
		return;
	}

	if (tags_sel & TAGS_COMMENTS)
		get_comment_tags (&vf, info);

	if (tags_sel & TAGS_TIME) {
		int64_t vorbis_time;

		vorbis_time = ov_time_total (&vf, -1);
		if (vorbis_time >= 0)
			info->time = vorbis_time / time_scaler;
	}

	ov_clear (&vf);
}

static size_t read_cb (void *ptr, size_t size, size_t nmemb, void *datasource)
{
	ssize_t res;

	res = io_read (datasource, ptr, size * nmemb);

	/* libvorbisfile expects the read callback to return >= 0 with errno
	 * set to non zero on error. */
	if (res < 0) {
		logit ("Read error");
		if (errno == 0)
			errno = 0xffff;
		res = 0;
	}
	else
		res /= size;

	return res;
}

static int seek_cb (void *datasource, ogg_int64_t offset, int whence)
{
	debug ("Seek request to %"PRId64" (%s)", offset,
			whence == SEEK_SET ? "SEEK_SET"
			: (whence == SEEK_CUR ? "SEEK_CUR" : "SEEK_END"));
	return io_seek (datasource, offset, whence) == -1 ? -1 : 0;
}

static int close_cb (void *unused ATTR_UNUSED)
{
	return 0;
}

static long tell_cb (void *datasource)
{
	return (long)io_tell (datasource);
}

static void vorbis_open_stream_internal (struct vorbis_data *data)
{
	int res;
	ov_callbacks callbacks = {
		read_cb,
		seek_cb,
		close_cb,
		tell_cb
	};

	data->tags = tags_new ();

	res = ov_open_callbacks (data->stream, &data->vf, NULL, 0, callbacks);
	if (res < 0) {
		const char *vorbis_err = vorbis_strerror (res);

		decoder_error (&data->error, ERROR_FATAL, 0, "%s", vorbis_err);
		debug ("ov_open error: %s", vorbis_err);
	}
	else {
		int64_t duration;

		data->last_section = -1;
		data->avg_bitrate = ov_bitrate (&data->vf, -1) / 1000;
		data->bitrate = data->avg_bitrate;
		data->duration = -1;
		duration = ov_time_total (&data->vf, -1);
		if (duration >= 0)
			data->duration = duration / time_scaler;
		data->ok = 1;
		get_comment_tags (&data->vf, data->tags);
	}
}

static void *vorbis_open (const char *file)
{
	struct vorbis_data *data;

	data = (struct vorbis_data *)xmalloc (sizeof(struct vorbis_data));
	data->ok = 0;

	decoder_error_init (&data->error);
	data->tags_change = 0;
	data->tags = NULL;

	data->stream = io_open (file, 1);
	if (!io_ok(data->stream)) {
		decoder_error (&data->error, ERROR_FATAL, 0,
		               "Can't load OGG: %s", io_strerror(data->stream));
		return data;
	}

	/* This a restriction placed on us by the vorbisfile API. */
#if INT64_MAX > LONG_MAX
	if (io_file_size (data->stream) > LONG_MAX) {
		decoder_error (&data->error, ERROR_FATAL, 0, "File too large!");
		return data;
	}
#endif

	vorbis_open_stream_internal (data);

	return data;
}

static int vorbis_can_decode (struct io_stream *stream)
{
	char buf[35];

	if (io_peek (stream, buf, 35) == 35 && !memcmp (buf, "OggS", 4)
			&& !memcmp (buf + 28, "\01vorbis", 7))
		return 1;

	return 0;
}

static void *vorbis_open_stream (struct io_stream *stream)
{
	struct vorbis_data *data;

	data = (struct vorbis_data *)xmalloc (sizeof(struct vorbis_data));
	data->ok = 0;

	decoder_error_init (&data->error);
	data->stream = stream;
	vorbis_open_stream_internal (data);

	return data;
}

static void vorbis_close (void *prv_data)
{
	struct vorbis_data *data = (struct vorbis_data *)prv_data;

	if (data->ok) {
		ov_clear (&data->vf);
	}

	io_close (data->stream);
	decoder_error_clear (&data->error);
	if (data->tags)
		tags_free (data->tags);
	free (data);
}

static int vorbis_seek (void *prv_data, int sec)
{
	struct vorbis_data *data = (struct vorbis_data *)prv_data;

	assert (sec >= 0);

	return ov_time_seek (&data->vf, sec * time_scaler) ? -1 : sec;
}

static int vorbis_decode (void *prv_data, char *buf, int buf_len,
		struct sound_params *sound_params)
{
	struct vorbis_data *data = (struct vorbis_data *)prv_data;
	int ret;
	int current_section;
	int bitrate;
	vorbis_info *info;

	decoder_error_clear (&data->error);

	while (1) {
#ifndef HAVE_TREMOR
		ret = ov_read(&data->vf, buf, buf_len,
		              (SFMT_NE == SFMT_LE ? 0 : 1),
		              2, 1, &current_section);
#else
		ret = ov_read(&data->vf, buf, buf_len, &current_section);
#endif
		if (ret == 0)
			return 0;
		if (ret < 0) {
			decoder_error (&data->error, ERROR_STREAM, 0,
			               "Error in the stream!");
			continue;
		}

		if (current_section != data->last_section) {
			logit ("section change or first section");

			data->last_section = current_section;
			data->tags_change = 1;
			tags_free (data->tags);
			data->tags = tags_new ();
			get_comment_tags (&data->vf, data->tags);
		}

		info = ov_info (&data->vf, -1);
		assert (info != NULL);
		sound_params->channels = info->channels;
		sound_params->rate = info->rate;
		sound_params->fmt = SFMT_S16 | SFMT_NE;

		/* Update the bitrate information */
		bitrate = ov_bitrate_instant (&data->vf);
		if (bitrate > 0)
			data->bitrate = bitrate / 1000;

		break;
	}

	return ret;
}

static int vorbis_current_tags (void *prv_data, struct file_tags *tags)
{
	struct vorbis_data *data = (struct vorbis_data *)prv_data;

	tags_copy (tags, data->tags);

	if (data->tags_change) {
		data->tags_change = 0;
		return 1;
	}

	return 0;
}

static int vorbis_get_bitrate (void *prv_data)
{
	struct vorbis_data *data = (struct vorbis_data *)prv_data;

	return data->bitrate;
}

static int vorbis_get_avg_bitrate (void *prv_data)
{
	struct vorbis_data *data = (struct vorbis_data *)prv_data;

	return data->avg_bitrate;
}

static int vorbis_get_duration (void *prv_data)
{
	struct vorbis_data *data = (struct vorbis_data *)prv_data;

	return data->duration;
}

static struct io_stream *vorbis_get_stream (void *prv_data)
{
	struct vorbis_data *data = (struct vorbis_data *)prv_data;

	return data->stream;
}

static void vorbis_get_name (const char *unused ATTR_UNUSED, char buf[4])
{
	strcpy (buf, "OGG");
}

static int vorbis_our_format_ext (const char *ext)
{
	return !strcasecmp (ext, "ogg")
		|| !strcasecmp (ext, "oga");
}

static void vorbis_get_error (void *prv_data, struct decoder_error *error)
{
	struct vorbis_data *data = (struct vorbis_data *)prv_data;

	decoder_error_copy (error, &data->error);
}

static int vorbis_our_mime (const char *mime)
{
	return !strcasecmp (mime, "application/ogg")
		|| !strncasecmp (mime, "application/ogg;", 16)
		|| !strcasecmp (mime, "application/x-ogg")
		|| !strncasecmp (mime, "application/x-ogg;", 18);
}

static struct decoder vorbis_decoder = {
	DECODER_API_VERSION,
	NULL,
	NULL,
	vorbis_open,
	vorbis_open_stream,
	vorbis_can_decode,
	vorbis_close,
	vorbis_decode,
	vorbis_seek,
	vorbis_tags,
	vorbis_get_bitrate,
	vorbis_get_duration,
	vorbis_get_error,
	vorbis_our_format_ext,
	vorbis_our_mime,
	vorbis_get_name,
	vorbis_current_tags,
	vorbis_get_stream,
	vorbis_get_avg_bitrate
};

struct decoder *plugin_init ()
{
	return &vorbis_decoder;
}

/* Defined and true if the Vorbis decoder is using Tremor, otherwise
 * undefined.  This is used by the decoder plugin loader so it can
 * document which library is being used without requiring the decoder
 * and the loader be built with the same HAVE_TREMOR setting. */
#ifdef HAVE_TREMOR
const bool vorbis_has_tremor = true;
#endif
