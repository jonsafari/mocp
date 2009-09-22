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

#include <string.h>
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

/* FFmpeg also likes common names, without that, our common.h and log.h would
 * not be included. */
#undef COMMON_H
#undef LOG_H

/* same for logging... */
#undef LOG_H

#define DEBUG

#include "common.h"
#include "decoder.h"
#include "log.h"
#include "files.h"

struct ffmpeg_data
{
	AVFormatParameters ap;
	AVFormatContext *ic;
	AVCodecContext *enc;
	AVCodec *codec;

	char *remain_buf;
	int remain_buf_len;
	
	int ok; /* was this stream successfully opened? */
	struct decoder_error error;
	int bitrate;
	int avg_bitrate;
};

static void ffmpeg_init ()
{
	avcodec_register_all();
	av_register_all ();
}

/* Fill info structure with data from ffmpeg comments */
static void ffmpeg_info (const char *file_name,
		struct file_tags *info,
		const int tags_sel)
{
	AVFormatParameters ap;
	AVFormatContext *ic;
	int err;
	
	memset (&ap, 0, sizeof(ap));

	if ((err = av_open_input_file(&ic, file_name, NULL, 0, &ap)) < 0) {
		logit ("av_open_input_file() failed (%d)", err);
		return;
	}
	if ((err = av_find_stream_info(ic)) < 0) {
		logit ("av_find_stream_info() failed (%d)", err);
		return;
	}

	if (tags_sel & TAGS_COMMENTS) {
		if (ic->track != 0)
			info->track = ic->track;
		if (ic->title[0] != 0)
			info->title = xstrdup (ic->title);
		if (ic->author[0] != 0)
			info->artist = xstrdup (ic->author);
		if (ic->album[0] != 0)
			info->album = xstrdup (ic->album);
	}

	if (tags_sel & TAGS_TIME)
		info->time = ic->duration >= 0 ? ic->duration / AV_TIME_BASE
			: -1;
}

static void *ffmpeg_open (const char *file)
{
	struct ffmpeg_data *data;
	int err;
	int i;
	int audio_index = -1;

	data = (struct ffmpeg_data *)xmalloc (sizeof(struct ffmpeg_data));
	data->ok = 0;

	decoder_error_init (&data->error);
	memset (&data->ap, 0, sizeof(data->ap));

	err = av_open_input_file (&data->ic, file, NULL, 0, &data->ap);
	if (err < 0) {
		decoder_error (&data->error, ERROR_FATAL, 0, "Can't open file");
		return data;
	}

	err = av_find_stream_info (data->ic);
	if (err < 0) {
		decoder_error (&data->error, ERROR_FATAL, 0,
				"Could not find codec parameters (err %d)",
				err);
		av_close_input_file (data->ic);
		return data;
	}

	av_read_play (data->ic);
	for (i = 0; i < data->ic->nb_streams; i++) {
		data->enc = data->ic->streams[i]->codec;
		if (data->enc->codec_type == CODEC_TYPE_AUDIO) {
			audio_index = i;
			break;
		}
	}
	if (audio_index == -1) {
		decoder_error (&data->error, ERROR_FATAL, 0,
				"No audio stream in file");
		av_close_input_file (data->ic);
		return data;
	}

	/* hack for AC3 */
	if (data->enc->channels > 2)
		data->enc->channels = 2;
	
	data->codec = avcodec_find_decoder (data->enc->codec_id);
	if (!data->codec || avcodec_open(data->enc, data->codec) < 0) {
		decoder_error (&data->error, ERROR_FATAL, 0,
				"No codec for this file.");
		av_close_input_file (data->ic);
		return data;
	}

	data->remain_buf = NULL;
	data->remain_buf_len = 0;

	data->ok = 1;
	data->avg_bitrate = (int) (data->ic->file_size / 
			(data->ic->duration / 1000000) * 8);
	data->bitrate = data->ic->bit_rate / 1000;

	return data;
}

static void ffmpeg_close (void *prv_data)
{
	struct ffmpeg_data *data = (struct ffmpeg_data *)prv_data;

	if (data->ok) {
		avcodec_close (data->enc);
		av_close_input_file (data->ic);

		if (data->remain_buf)
			free (data->remain_buf);
	}

	decoder_error_clear (&data->error);
	free (data);
}

static int ffmpeg_seek (void *prv_data, int sec)
{
	struct ffmpeg_data *data = (struct ffmpeg_data *)prv_data;
	int err;

	/*if ((err = av_seek_frame(data->ic, -1, sec, 0)) < 0)
		logit ("Seek error %d", err);
	else if (data->remain_buf) {
		free (data->remain_buf);
		data->remain_buf = NULL;
		data->remain_buf_len = 0;
	}

	return err >= 0 ? sec : -1;*/

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

static int ffmpeg_decode (void *prv_data, char *buf, int buf_len,
		struct sound_params *sound_params)
{
	struct ffmpeg_data *data = (struct ffmpeg_data *)prv_data;
	int ret;
	int data_size;
	AVPacket pkt, pkt_tmp;
	uint8_t *pkt_data;
	int pkt_size = 0;
	int filled = 0;

	/* The sample buffer should be 16 byte aligned (because SSE), a segmentation
	 * fault may occur otherwise.
	 * 
	 * See: avcodec.h in ffmpeg
	 */
	char avbuf[(AVCODEC_MAX_AUDIO_FRAME_SIZE * 3) / 2] __attribute__((aligned(16)));

	decoder_error_clear (&data->error);

	sound_params->channels = data->enc->channels;
	sound_params->rate = data->enc->sample_rate;
	sound_params->fmt = SFMT_S16 | SFMT_NE;
	
	if (data->remain_buf) {
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
			free (data->remain_buf);
			data->remain_buf = NULL;
			data->remain_buf_len = 0;
		}

		return to_copy;
	}

	do {
		ret = av_read_frame (data->ic, &pkt);
		if (ret < 0)
			return 0;

		memcpy(&pkt_tmp, &pkt, sizeof(pkt));
		pkt_data = pkt.data;
		pkt_size = pkt.size;
		debug ("Got %dB packet", pkt_size);
		
		while (pkt_size) {
			int len;

#if LIBAVCODEC_VERSION_MAJOR >= 52
			data_size = sizeof (avbuf);
			len = avcodec_decode_audio3 (data->enc, (int16_t *)avbuf,
					&data_size, &pkt);
#elif LIBAVCODEC_VERSION_INT >= ((51<<16)+(50<<8)+0)
			data_size = sizeof (avbuf);
			len = avcodec_decode_audio2 (data->enc, (int16_t *)avbuf,
					&data_size, pkt_data, pkt_size);
#else
			len = avcodec_decode_audio (data->enc, (int16_t *)avbuf,
					&data_size, pkt_data, pkt_size);
#endif

			debug ("Decoded %dB", data_size);

			if (len < 0)  {
				/* skip frame */
				decoder_error (&data->error, ERROR_STREAM, 0,
						"Error in the stream!");
				break;
			}

			pkt_data += len;
			pkt_size -= len;
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
	} while (!filled);
	
	/* 2.0 - 16bit/sample*/
	data->bitrate = pkt.size * 8 / ((filled + data->remain_buf_len) / 2.0 /
			sound_params->channels / sound_params->rate) / 1000;

	av_free_packet (&pkt_tmp);
	
	return filled;
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
	char *ext = ext_pos (file);

	if (!strcasecmp(ext, "ra"))
		strcpy (buf, "RA");
	else if (!strcasecmp(ext, "wma"))
		strcpy (buf, "WMA");
	else if (!strcasecmp(ext, "mp4"))
		strcpy (buf, "MP4");
	else if (!strcasecmp(ext, "m4a"))
		strcpy (buf, "M4A");
}

static int ffmpeg_our_format_ext (const char *ext)
{
	return !strcasecmp(ext, "wma")
		|| !strcasecmp(ext, "ra")
		|| !strcasecmp(ext, "m4a")
		|| !strcasecmp(ext, "mp4");
}

static void ffmpeg_get_error (void *prv_data, struct decoder_error *error)
{
	struct ffmpeg_data *data = (struct ffmpeg_data *)prv_data;

	decoder_error_copy (error, &data->error);
}

static struct decoder ffmpeg_decoder = {
	DECODER_API_VERSION,
	ffmpeg_init,
	NULL,
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
