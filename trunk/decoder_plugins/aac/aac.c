/*
 * MOC - music on console
 * Copyright (C) 2005, 2006 Damian Pietras <daper@daper.net>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This code is based on CMUS aac plugin Copyright 2006 dnk <dnk@bjum.net>
 *
 */

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <stdio.h>
#include <string.h>
#ifdef HAVE_STDINT_H
# include <stdint.h>
#endif
#ifdef HAVE_INTTYPES_H
# include <inttypes.h>
#endif
#include <assert.h>

#include <faad.h>
#include <id3tag.h>

#define DEBUG

#include "common.h"
#include "decoder.h"
#include "log.h"
#include "files.h"

/* FAAD_MIN_STREAMSIZE == 768, 6 == # of channels */
#define BUFFER_SIZE	(FAAD_MIN_STREAMSIZE * 6 * 4)

struct aac_data
{
	struct io_stream *stream;
	char rbuf[BUFFER_SIZE];
	int rbuf_len;
	int rbuf_pos;

	uint8_t channels;
	uint32_t sample_rate;

	char *overflow_buf;
	int overflow_buf_len;

	faacDecHandle decoder;	/* typedef void * */

	int ok; /* was this stream successfully opened? */
	struct decoder_error error;

	int in_bytes_counter;
	int bitrate;
	int avg_bitrate;
	int duration;
};

static int buffer_length (const struct aac_data *data)
{
	return data->rbuf_len - data->rbuf_pos;
}

static void *buffer_data (struct aac_data *data)
{
	return data->rbuf + data->rbuf_pos;
}

static int buffer_fill (struct aac_data *data)
{
	int32_t n;

	if (data->rbuf_pos > 0) {
		data->rbuf_len = buffer_length (data);
		memmove (data->rbuf, data->rbuf + data->rbuf_pos, data->rbuf_len);
		data->rbuf_pos = 0;
	}

	if (data->rbuf_len == BUFFER_SIZE)
		return 1;

	n = io_read (data->stream, data->rbuf + data->rbuf_len, BUFFER_SIZE - data->rbuf_len);
	if (n == -1)
		return -1;
	if (n == 0)
		return 0;

	data->in_bytes_counter += n;
	data->rbuf_len += n;
	return 1;
}

static inline void buffer_consume(struct aac_data *data, int n)
{
	assert (n <= buffer_length(data));

	data->rbuf_pos += n;
}

static int buffer_fill_min (struct aac_data *data, int len)
{
	int rc;

	assert (len < BUFFER_SIZE);

	while (buffer_length(data) < len) {
		rc = buffer_fill (data);
		if (rc <= 0)
			return rc;
	}

	return 1;
}

/* 'data' must point to at least 6 bytes of data */
static int parse_frame (const unsigned char data[6])
{
	int len;

	/* http://www.audiocoding.com/modules/wiki/?page=ADTS */

	/* first 12 bits must be set */
	if (data[0] != 0xFF)
		return 0;
	if ((data[1] & 0xF0) != 0xF0)
		return 0;

	/* layer is always '00' */
	if ((data[1] & 0x06) != 0x00)
		return 0;

	/* frame length is stored in 13 bits */
	len  = data[3] << 11;	/* ..1100000000000 */
	len |= data[4] << 3;	/* ..xx11111111xxx */
	len |= data[5] >> 5;	/* ..xxxxxxxxxx111 */
	len &= 0x1FFF;		/* 13 bits */
	return len;
}

/* scans forward to the next aac frame and makes sure
 * the entire frame is in the buffer.
 */
static int buffer_fill_frame(struct aac_data *data)
{
	unsigned char *datap;
	int rc, n, len;
	int max = 32768;

	while (1) {
		/* need at least 6 bytes of data */
		rc = buffer_fill_min(data, 6);
		if (rc <= 0)
			return rc;

		len = buffer_length(data);
		datap = buffer_data(data);

		/* scan for a frame */
		for (n = 0; n < len - 5; n++) {
			/* give up after 32KB */
			if (max-- == 0) {
				logit ("no frame found!\n");
				/* FIXME: set errno? */
				return -1;
			}

			/* see if there's a frame at this location */
			rc = parse_frame(datap + n);
			if (rc == 0)
				continue;

			/* found a frame, consume all data up to the frame */
			buffer_consume (data, n);

			/* rc == frame length */
			rc = buffer_fill_min (data, rc);
			if (rc <= 0)
				return rc;

			return 1;
		}

		/* consume what we used */
		buffer_consume (data, n);
	}

	/* not reached */
	return -1; /* silence the GCC warning */
}

static int aac_count_time (struct aac_data *data)
{
	faacDecFrameInfo frame_info;
	int samples = 0, bytes = 0, frames = 0;
	off_t file_size;
	char *sample_buf;
	long saved_pos;

	file_size = io_file_size (data->stream);
	if (file_size == -1)
		return -1;

	saved_pos = io_tell (data->stream);

	/* guess track length by decoding the first 10 frames */
	while (frames < 10) {
		if (buffer_fill_frame(data) <= 0)
			break;

		sample_buf = faacDecDecode(data->decoder, &frame_info,
			buffer_data(data), buffer_length(data));
		if (frame_info.error == 0 && frame_info.samples > 0) {
			samples += frame_info.samples;
			bytes += frame_info.bytesconsumed;
			frames++;
		}
		if (frame_info.bytesconsumed == 0)
			break;

		buffer_consume (data, frame_info.bytesconsumed);
	}

	if (io_seek(data->stream, saved_pos, SEEK_SET) == (off_t)-1) {
		logit ("Can't seek after couting time");
		return -1;
	}

	if (frames == 0)
		return -1;

	samples /= frames;
	samples /= data->channels;
	bytes /= frames;

	return ((file_size / bytes) * samples) / data->sample_rate;
}

static void *aac_open_internal (struct io_stream *stream, const char *fname)
{
	struct aac_data *data;
	faacDecConfigurationPtr neaac_cfg;
	int n;

	/* init private struct */
	data = (struct aac_data *)xmalloc (sizeof(struct aac_data));
	memset (data, 0, sizeof(struct aac_data));
	data->ok = 0;
	data->decoder = faacDecOpen();

	/* set decoder config */
	neaac_cfg = faacDecGetCurrentConfiguration(data->decoder);
	neaac_cfg->outputFormat = FAAD_FMT_16BIT;	/* force 16 bit audio */
	neaac_cfg->downMatrix = 1;			/* 5.1 -> stereo */
	neaac_cfg->dontUpSampleImplicitSBR = 0;		/* upsample, please! */
	faacDecSetConfiguration(data->decoder, neaac_cfg);

	if (stream)
		data->stream = stream;
	else {
		data->stream = io_open (fname, 1);
		if (!io_ok(data->stream)) {
			decoder_error (&data->error, ERROR_FATAL, 0,
					"Can't open AAC file: %s",
					io_strerror(data->stream));
			return data;
		}
	}

	/* find a frame */
	if (buffer_fill_frame(data) <= 0) {
		decoder_error (&data->error, ERROR_FATAL, 0,
				"Not a valid (or unsupported) AAC file");
		return data;
	}

	/* in case of a bug, make sure there is at least some data
	 * in the buffer for faacDecInit() to work with.
	 */
	if (buffer_fill_min(data, 256) <= 0) {
		logit ("not enough data");
		decoder_error (&data->error, ERROR_FATAL, 0,
				"AAC file/stream too short");
		return data;
	}

	/* init decoder, returns the length of the header (if any) */
	n = faacDecInit (data->decoder, buffer_data(data), buffer_length(data),
		&data->sample_rate, &data->channels);
	if (n < 0) {
		logit ("faacDecInit failed");
		decoder_error (&data->error, ERROR_FATAL, 0,
				"libfaad can't open this stream");
		return data;
	}

	logit ("sample rate %uHz, channels %d\n", (unsigned)data->sample_rate,
			(int)data->channels);
	if (!data->sample_rate || !data->channels) {
		decoder_error (&data->error, ERROR_FATAL, 0,
				"Invalid AAC sound parameters");
		return data;
	}

	/* skip the header */
	logit ("skipping header (%d bytes)\n", n);
	buffer_consume (data, n);

	/*faacDecInitDRM(data->decoder, data->sample_rate, data->channels);*/

	if (fname) {
		data->duration = aac_count_time (data);
		if (data->duration > 0) {
			data->avg_bitrate = io_file_size (data->stream)
				/ data->duration * 8;
		}
		else
			data->avg_bitrate = -1;
	}

	data->ok = 1;
	return data;
}

static void *aac_open (const char *file)
{
	return aac_open_internal (NULL, file);
}

static void *aac_open_stream (struct io_stream *stream)
{
	assert (stream != NULL);

	return aac_open_internal (stream, NULL);
}

static void aac_close (void *prv_data)
{
	struct aac_data *data = (struct aac_data *)prv_data;

	faacDecClose (data->decoder);
	io_close (data->stream);
	decoder_error_clear (&data->error);
	free (data);
}

static char *get_tag (struct id3_tag *tag, const char *what)
{
	struct id3_frame *frame;
	union id3_field *field;
	const id3_ucs4_t *ucs4;
	char *comm = NULL;

	frame = id3_tag_findframe (tag, what, 0);
	if (frame && (field = &frame->fields[1])) {
		ucs4 = id3_field_getstrings (field, 0);
		if (ucs4) {
			comm = (char *)id3_ucs4_utf8duplicate (ucs4);
		}
	}

	return comm;
}

/* Fill info structure with data from aac comments */
static void aac_info (const char *file_name,
		struct file_tags *info,
		const int tags_sel)
{
	if (tags_sel & TAGS_COMMENTS) {
		struct id3_tag *tag;
		struct id3_file *id3file;
		char *track = NULL;
		
		id3file = id3_file_open (file_name, ID3_FILE_MODE_READONLY);
		if (!id3file)
			return;
		tag = id3_file_tag (id3file);
		if (tag) {
			info->artist = get_tag (tag, ID3_FRAME_ARTIST);
			info->title = get_tag (tag, ID3_FRAME_TITLE);
			info->album = get_tag (tag, ID3_FRAME_ALBUM);
			track = get_tag (tag, ID3_FRAME_TRACK);
			
			if (track) {
				char *end;
				
				info->track = strtol (track, &end, 10);
				if (end == track)
					info->track = -1;
				free (track);
			}
		}
		id3_file_close (id3file);
	}

	if (tags_sel & TAGS_TIME) {
		struct aac_data *data;

		data = aac_open_internal (NULL, file_name);
		if (data->ok) {
			info->time = aac_count_time (data);
		}
		aac_close (data);
	}
}

static int aac_seek (void *prv_data ATTR_UNUSED, int sec ATTR_UNUSED)
{
#if 0
	struct aac_data *data = (struct aac_data *)prv_data;

	if ((err = av_seek_frame(data->ic, -1, sec, 0)) < 0)
		logit ("Seek error %d", err);
	else if (data->remain_buf) {
		free (data->remain_buf);
		data->remain_buf = NULL;
		data->remain_buf_len = 0;
	}

	return err >= 0 ? sec : -1;
#endif

	return -1;
}

/* returns -1 on fatal errors
 * returns -2 on non-fatal errors
 * 0 on eof
 * number of bytes put in 'buffer' on success */
static int decode_one_frame (struct aac_data *data, void *buffer, int count)
{
	unsigned char *aac_data;
	unsigned int aac_data_size;
	faacDecFrameInfo frame_info;
	char *sample_buf;
	int bytes, rc;

	data->in_bytes_counter = 0;

	rc = buffer_fill_frame (data);
	if (rc <= 0)
		return rc;

	aac_data = buffer_data (data);
	aac_data_size = buffer_length (data);

	/* aac data -> raw pcm */
	sample_buf = faacDecDecode (data->decoder, &frame_info, aac_data, aac_data_size);

	buffer_consume (data, frame_info.bytesconsumed);

	if (!sample_buf || frame_info.bytesconsumed <= 0) {
		logit ("fatal error: %s", faacDecGetErrorMessage(frame_info.error));
		return -1;
	}

	if (frame_info.error != 0) {
		logit ("frame error: %s", faacDecGetErrorMessage(frame_info.error));
		return -2;
	}

	if (frame_info.samples <= 0)
		return -2;

	if (frame_info.channels != data->channels || frame_info.samplerate != data->sample_rate) {
		logit ("invalid channel or sample_rate count\n");
		return -2;
	}

	/* 16-bit samples */
	bytes = frame_info.samples * 2;

	if (bytes > count) {
		/* decoded too much, keep overflow */
		data->overflow_buf = sample_buf + count;
		data->overflow_buf_len = bytes - count;
		memcpy (buffer, sample_buf, count);
		return count;
	} else {
		memcpy (buffer, sample_buf, bytes);
	}

	data->bitrate = frame_info.bytesconsumed * 8 / (bytes / 2.0 /
			data->channels / data->sample_rate) / 1000; 

	return bytes;
}

static int aac_decode (void *prv_data, char *buf, int buf_len,
		struct sound_params *sound_params)
{
	struct aac_data *data = (struct aac_data *)prv_data;
	int rc;

	decoder_error_clear (&data->error);

	sound_params->channels = data->channels;
	sound_params->rate = data->sample_rate;
	sound_params->fmt = SFMT_S16 | SFMT_NE;

	/* use overflow from previous call (if any) */
	if (data->overflow_buf_len) {
		int len = data->overflow_buf_len;

		if (len > buf_len)
			len = buf_len;

		memcpy (buf, data->overflow_buf, len);
		data->overflow_buf += len;
		data->overflow_buf_len -= len;
		return len;
	}

	do {
		rc = decode_one_frame (data, buf, buf_len);
	} while (rc == -2);

	return rc > 0 ? rc : 0;
}

static int aac_get_bitrate (void *prv_data)
{
	struct aac_data *data = (struct aac_data *)prv_data;

	return data->bitrate;
}

static int aac_get_avg_bitrate (void *prv_data)
{
	struct aac_data *data = (struct aac_data *)prv_data;

	return data->avg_bitrate / 1000;
}

static int aac_get_duration (void *prv_data)
{
	struct aac_data *data = (struct aac_data *)prv_data;

	return data->duration;

	return -1;
}

static void aac_get_name (const char *file, char buf[4])
{
	char *ext = ext_pos (file);

	if (!strcasecmp(ext, "aac"))
		strcpy (buf, "AAC");
}

static int aac_our_format_ext (const char *ext)
{
	return !strcasecmp(ext, "m4a")
		|| !strcasecmp(ext, "aac");
}

static void aac_get_error (void *prv_data, struct decoder_error *error)
{
	struct aac_data *data = (struct aac_data *)prv_data;

	decoder_error_copy (error, &data->error);
}

static int aac_our_mime (const char *mime)
{
	return !strcmp(mime, "audio/aac")
		|| !strcmp(mime, "audio/aacp");
}

static struct decoder aac_decoder = {
	DECODER_API_VERSION,
	NULL,
	NULL,
	aac_open,
	aac_open_stream,
	NULL,
	aac_close,
	aac_decode,
	aac_seek,
	aac_info,
	aac_get_bitrate,
	aac_get_duration,
	aac_get_error,
	aac_our_format_ext,
	aac_our_mime,
	aac_get_name,
	NULL,
	NULL,
	aac_get_avg_bitrate
};

struct decoder *plugin_init ()
{
	return &aac_decoder;
}
