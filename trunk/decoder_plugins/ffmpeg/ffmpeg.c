/*
 * MOC - music on console
 * Copyright (C) 2005, 2006 Damian Pietras <daper@daper.net>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * Based on FFplay Copyright (c) 2003 Fabrice Bellard
 *
 */

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <stdlib.h>
#include <pthread.h>
#include <string.h>
#include <assert.h>
#ifdef HAVE_STDINT_H
# include <stdint.h>
#endif
#ifdef HAVE_INTTYPES_H
# include <inttypes.h>
#endif
#if HAVE_LIBAVFORMAT_AVFORMAT_H
#include <libavformat/avformat.h>
#else
#include <ffmpeg/avformat.h>
#endif

/* FFmpeg also likes common names, without that, our common.h and log.h
 * would not be included. */
#undef COMMON_H
#undef LOG_H

#define DEBUG

#include "common.h"
#include "decoder.h"
#include "log.h"
#include "files.h"

struct ffmpeg_data
{
	AVFormatContext *ic;
	AVStream *stream;
	AVCodecContext *enc;
	AVCodec *codec;

	char *remain_buf;
	int remain_buf_len;

	bool okay; /* was this stream successfully opened? */
	struct decoder_error error;
	int bitrate;
	int avg_bitrate;
};

static void ffmpeg_log_repeats (char *msg)
{
	static int msg_count = 0;
	static char *prev_msg = NULL;
	static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;

	/* We need to gate the decoder and precaching threads. */
	LOCK (mutex);
	if (prev_msg && !msg) {
		if (msg_count > 1)
			logit ("FFmpeg said: Last message repeated %d times", msg_count);
		free (prev_msg);
		prev_msg = NULL;
		msg_count = 0;
	}
	if (prev_msg && msg) {
		if (!strcmp (msg, prev_msg)) {
			free (msg);
			msg = NULL;
			msg_count += 1;
		}
	}
	if (!prev_msg && msg) {
		logit ("FFmpeg said: %s", msg);
		prev_msg = msg;
		msg_count = 1;
	}
	UNLOCK (mutex);
}

static void ffmpeg_log_cb (void *data ATTR_UNUSED, int level,
                           const char *fmt, va_list vl)
{
	int len;
	char *msg;
	va_list vlist;

	assert (fmt);

	if (level > av_log_get_level ())
		return;

#if LIBAVCODEC_VERSION_MAJOR == 52
	/* Drop this message because it is issued repeatedly and is pointless. */
	const char diverting[] =
	           "Diverting av_*_packet function calls to libavcodec.";

	if (!strncmp (diverting, fmt, sizeof (diverting) - 1))
		return;
#endif

	va_copy (vlist, vl);
	len = vsnprintf (NULL, 0, fmt, vlist);
	va_end (vlist);
	msg = xmalloc (len + 1);
	vsnprintf (msg, len + 1, fmt, vl);
	if (len > 0 && msg[len - 1] == '\n')
		msg[len - 1] = 0x00;

	ffmpeg_log_repeats (msg);
}

/* Find the first audio stream and return its index, or nb_streams if
 * none found. */
static unsigned int find_first_audio (AVFormatContext *ic)
{
	unsigned int result;

	assert (ic);

	for (result = 0; result < ic->nb_streams; result += 1) {
#if LIBAVUTIL_VERSION_INT < AV_VERSION_INT(50,15,1)
		if (ic->streams[result]->codec->codec_type == CODEC_TYPE_AUDIO)
#else
		if (ic->streams[result]->codec->codec_type == AVMEDIA_TYPE_AUDIO)
#endif
		{
			break;
		}
	}

	return result;
}

static void ffmpeg_init ()
{
#ifdef DEBUG
	av_log_set_level (AV_LOG_INFO);
#else
	av_log_set_level (AV_LOG_ERROR);
#endif
	av_log_set_callback (ffmpeg_log_cb);
	avcodec_register_all ();
	av_register_all ();
}

static void ffmpeg_destroy ()
{
	av_log_set_level (AV_LOG_QUIET);
	ffmpeg_log_repeats (NULL);
}

/* Fill info structure with data from ffmpeg comments. */
static void ffmpeg_info (const char *file_name,
		struct file_tags *info,
		const int tags_sel)
{
	int err;
	AVFormatContext *ic = NULL;

#ifdef HAVE_AVFORMAT_OPEN_INPUT
	err = avformat_open_input (&ic, file_name, NULL, NULL);
	if (err < 0) {
		ffmpeg_log_repeats (NULL);
		logit ("avformat_open_input() failed (%d)", err);
		return;
	}
#else
	err = av_open_input_file (&ic, file_name, NULL, 0, NULL);
	if (err < 0) {
		ffmpeg_log_repeats (NULL);
		logit ("av_open_input_file() failed (%d)", err);
		return;
	}
#endif

	err = av_find_stream_info (ic);
	if (err < 0) {
		ffmpeg_log_repeats (NULL);
		logit ("av_find_stream_info() failed (%d)", err);
		goto end;
	}

	if (tags_sel & TAGS_COMMENTS) {

#if defined(HAVE_AV_DICT_GET)

		AVDictionaryEntry *entry;

		entry = av_dict_get (ic->metadata, "track", NULL, 0);
		if (entry && entry->value && entry->value[0])
			info->track = atoi (entry->value);
		entry = av_dict_get (ic->metadata, "title", NULL, 0);
		if (entry && entry->value && entry->value[0])
			info->title = xstrdup (entry->value);
		entry = av_dict_get (ic->metadata, "artist", NULL, 0);
		if (entry && entry->value && entry->value[0])
			info->artist = xstrdup (entry->value);
		entry = av_dict_get (ic->metadata, "album", NULL, 0);
		if (entry && entry->value && entry->value[0])
			info->album = xstrdup (entry->value);

#elif defined(HAVE_AV_METADATA_GET)

		AVMetadataTag *tag;
 
		av_metadata_conv (ic, NULL, ic->iformat->metadata_conv);

		tag = av_metadata_get (ic->metadata, "track", NULL, 0);
		if (tag && tag->value && tag->value[0])
			info->track = atoi (tag->value);
		tag = av_metadata_get (ic->metadata, "title", NULL, 0);
		if (tag && tag->value && tag->value[0])
			info->title = xstrdup (tag->value);
		if (avformat_version () < AV_VERSION_INT(52,50,0))
			tag = av_metadata_get (ic->metadata, "author", NULL, 0);
		else
			tag = av_metadata_get (ic->metadata, "artist", NULL, 0);
		if (tag && tag->value && tag->value[0])
			info->artist = xstrdup (tag->value);
		tag = av_metadata_get (ic->metadata, "album", NULL, 0);
		if (tag && tag->value && tag->value[0])
			info->album = xstrdup (tag->value);

#else

		if (ic->track != 0)
			info->track = ic->track;
		if (ic->title[0] != 0)
			info->title = xstrdup (ic->title);
		if (ic->author[0] != 0)
			info->artist = xstrdup (ic->author);
		if (ic->album[0] != 0)
			info->album = xstrdup (ic->album);

#endif
	}

	if (tags_sel & TAGS_TIME)
		info->time = ic->duration >= 0 ? ic->duration / AV_TIME_BASE
			: -1;

end:
	av_close_input_file (ic);
	ffmpeg_log_repeats (NULL);
}

static void *ffmpeg_open (const char *file)
{
	struct ffmpeg_data *data;
	int err;
	unsigned int audio_ix;

	data = (struct ffmpeg_data *)xmalloc (sizeof (struct ffmpeg_data));
	data->okay = false;
	data->ic = NULL;
	data->stream = NULL;
	data->enc = NULL;
	data->codec = NULL;
	data->bitrate = 0;
	data->avg_bitrate = 0;
	data->remain_buf = NULL;
	data->remain_buf_len = 0;

	decoder_error_init (&data->error);

#ifdef HAVE_AVFORMAT_OPEN_INPUT
	err = avformat_open_input (&data->ic, file, NULL, NULL);
#else
	err = av_open_input_file (&data->ic, file, NULL, 0, NULL);
#endif
	if (err < 0) {
		ffmpeg_log_repeats (NULL);
		decoder_error (&data->error, ERROR_FATAL, 0, "Can't open file");
		return data;
	}

	err = av_find_stream_info (data->ic);
	if (err < 0) {
		decoder_error (&data->error, ERROR_FATAL, 0,
				"Could not find codec parameters (err %d)",
				err);
		goto end;
	}

	audio_ix = find_first_audio (data->ic);
	if (audio_ix == data->ic->nb_streams) {
		decoder_error (&data->error, ERROR_FATAL, 0,
				"No audio stream in file");
		goto end;
	}
	data->stream = data->ic->streams[audio_ix];
	data->enc = data->stream->codec;

	/* hack for AC3 */
	if (data->enc->channels > 2)
		data->enc->channels = 2;

	data->codec = avcodec_find_decoder (data->enc->codec_id);
	if (!data->codec || avcodec_open (data->enc, data->codec) < 0) {
		decoder_error (&data->error, ERROR_FATAL, 0,
				"No codec for this file");
		goto end;
	}

	data->okay = true;

	data->avg_bitrate = (int) (data->ic->file_size /
			(data->ic->duration / 1000000) * 8);
	data->bitrate = data->ic->bit_rate / 1000;

	return data;

end:
	av_close_input_file (data->ic);
	ffmpeg_log_repeats (NULL);
	return data;
}

static int ffmpeg_seek (void *prv_data ATTR_UNUSED, int sec ATTR_UNUSED)
{
	assert (sec >= 0);

#if 0
	struct ffmpeg_data *data = (struct ffmpeg_data *)prv_data;
	int err;

	if ((err = av_seek_frame(data->ic, -1, sec, 0)) < 0)
		logit ("Seek error %d", err);
	else
		free_remain_buf (data);

	return err >= 0 ? sec : -1;
#endif

	return -1;
}

static void put_in_remain_buf (struct ffmpeg_data *data, const char *buf,
		const int len)
{
	debug ("Remain: %dB", len);

	data->remain_buf_len = len;
	data->remain_buf = (char *)xmalloc (len);
	memcpy (data->remain_buf, buf, len);
}

static void add_to_remain_buf (struct ffmpeg_data *data, const char *buf,
		const int len)
{
	debug ("Adding %dB to remain_buf", len);

	data->remain_buf = (char *)xrealloc (data->remain_buf,
			data->remain_buf_len + len);
	memcpy (data->remain_buf + data->remain_buf_len, buf, len);
	data->remain_buf_len += len;

	debug ("remain_buf is %dB long", data->remain_buf_len);
}

/* Free the remainder buffer. */
static void free_remain_buf (struct ffmpeg_data *data)
{
	free (data->remain_buf);
	data->remain_buf = NULL;
	data->remain_buf_len = 0;
}

/* Satisfy the request from previously decoded samples. */
static int take_from_remain_buf (struct ffmpeg_data *data, char *buf, int buf_len)
{
	int to_copy = MIN (buf_len, data->remain_buf_len);

	debug ("Copying %d bytes from the remain buf", to_copy);

	memcpy (buf, data->remain_buf, to_copy);

	if (to_copy < data->remain_buf_len) {
		memmove (data->remain_buf, data->remain_buf + to_copy,
				data->remain_buf_len - to_copy);
		data->remain_buf_len -= to_copy;
	}
	else {
		debug ("Remain buf is now empty");
		free_remain_buf (data);
	}

	return to_copy;
}

/* Decode samples from stream data. */
static int decode_from_stream (struct ffmpeg_data *data, char *buf,
                               int buf_len, int *bytes_used)
{
	int filled = 0;
	AVPacket pkt;

	/* The sample buffer should be 16 byte aligned (because SSE), a segmentation
	 * fault may occur otherwise.
	 *
	 * See: avcodec.h in ffmpeg
	 */
	char avbuf[(AVCODEC_MAX_AUDIO_FRAME_SIZE * 3) / 2] __attribute__((aligned(16)));

	*bytes_used = 0;

	do {
		int rc;
		uint8_t *saved_pkt_data_ptr;

		rc = av_read_frame (data->ic, &pkt);
		if (rc < 0)
			return 0;

		saved_pkt_data_ptr = pkt.data;
		*bytes_used = pkt.size;
		debug ("Got %dB packet", pkt.size);

		while (pkt.size > 0) {
			int len, data_size;

			data_size = sizeof (avbuf);

#if defined(HAVE_AVCODEC_DECODE_AUDIO3)
			len = avcodec_decode_audio3 (data->enc, (int16_t *)avbuf,
					&data_size, &pkt);
#elif defined(HAVE_AVCODEC_DECODE_AUDIO2)
			len = avcodec_decode_audio2 (data->enc, (int16_t *)avbuf,
					&data_size, pkt.data, pkt.size);
#else
			len = avcodec_decode_audio (data->enc, (int16_t *)avbuf,
					&data_size, pkt.data, pkt.size);
#endif

			debug ("Decoded %dB", data_size);

			if (len < 0)  {
				/* skip frame */
				decoder_error (&data->error, ERROR_STREAM, 0,
						"Error in the stream!");
				break;
			}

			pkt.data += len;
			pkt.size -= len;

			if (buf_len) {
				int to_copy = MIN (data_size, buf_len);

				memcpy (buf, avbuf, to_copy);

				buf += to_copy;
				filled += to_copy;
				buf_len -= to_copy;

				debug ("Copying %dB (%dB filled)", to_copy,
						filled);

				if (to_copy < data_size)
					put_in_remain_buf (data,
							avbuf + to_copy,
							data_size - to_copy);
			}
			else if (data_size)
				add_to_remain_buf (data, avbuf, data_size);

		}

		/* FFmpeg will segfault if the data pointer is not restored. */
		pkt.data = saved_pkt_data_ptr;
		av_free_packet (&pkt);

	} while (!filled);

	return filled;
}

static int ffmpeg_decode (void *prv_data, char *buf, int buf_len,
                          struct sound_params *sound_params)
{
	struct ffmpeg_data *data = (struct ffmpeg_data *)prv_data;
	int bytes_used, bytes_produced;

	decoder_error_clear (&data->error);

	sound_params->channels = data->enc->channels;
	sound_params->rate = data->enc->sample_rate;
	sound_params->fmt = SFMT_S16 | SFMT_NE;

	if (data->remain_buf)
		return take_from_remain_buf (data, buf, buf_len);

	bytes_produced = decode_from_stream (data, buf, buf_len, &bytes_used);

	/* 2.0 - 16bit/sample*/
	data->bitrate = bytes_used * 8 / ((bytes_produced + data->remain_buf_len)
	                                                  / 2.0
	                                                  / sound_params->channels
                                                      / sound_params->rate)
                                   / 1000;

	return bytes_produced;
}

static void ffmpeg_close (void *prv_data)
{
	struct ffmpeg_data *data = (struct ffmpeg_data *)prv_data;

	if (data->okay) {
		avcodec_close (data->enc);
		av_close_input_file (data->ic);
		free_remain_buf (data);
	}

	ffmpeg_log_repeats (NULL);
	decoder_error_clear (&data->error);
	free (data);
}

static int ffmpeg_get_bitrate (void *prv_data)
{
	struct ffmpeg_data *data = (struct ffmpeg_data *)prv_data;

	return data->bitrate;
}

static int ffmpeg_get_avg_bitrate (void *prv_data)
{
	struct ffmpeg_data *data = (struct ffmpeg_data *)prv_data;

	return data->avg_bitrate / 1000;
}

static int ffmpeg_get_duration (void *prv_data)
{
	struct ffmpeg_data *data = (struct ffmpeg_data *)prv_data;

	return (data->ic->duration >= 0) ? data->ic->duration / AV_TIME_BASE
		: -1;
}

static void ffmpeg_get_name (const char *file, char buf[4])
{
	char *ext;

	ext = ext_pos (file);
	if (!strcasecmp (ext, "ra"))
		strcpy (buf, "RA");
	else if (!strcasecmp (ext, "wma"))
		strcpy (buf, "WMA");
	else if (!strcasecmp (ext, "mp4"))
		strcpy (buf, "MP4");
	else if (!strcasecmp (ext, "m4a"))
		strcpy (buf, "M4A");
}

static int ffmpeg_our_format_ext (const char *ext)
{
	return !strcasecmp (ext, "wma")
		|| !strcasecmp (ext, "ra")
		|| !strcasecmp (ext, "m4a")
		|| !strcasecmp (ext, "mp4");
}

static void ffmpeg_get_error (void *prv_data, struct decoder_error *error)
{
	struct ffmpeg_data *data = (struct ffmpeg_data *)prv_data;

	decoder_error_copy (error, &data->error);
}

static struct decoder ffmpeg_decoder = {
	DECODER_API_VERSION,
	ffmpeg_init,
	ffmpeg_destroy,
	ffmpeg_open,
	NULL,
	NULL,
	ffmpeg_close,
	ffmpeg_decode,
	ffmpeg_seek,
	ffmpeg_info,
	ffmpeg_get_bitrate,
	ffmpeg_get_duration,
	ffmpeg_get_error,
	ffmpeg_our_format_ext,
	NULL,
	ffmpeg_get_name,
	NULL,
	NULL,
	ffmpeg_get_avg_bitrate
};

struct decoder *plugin_init ()
{
	return &ffmpeg_decoder;
}
