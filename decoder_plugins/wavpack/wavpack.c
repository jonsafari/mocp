/*
 * MOC - music on console
 * Copyright (C) 2004 Damian Pietras <daper@daper.net>
 *
 * libwavpack-plugin Copyright (C) 2006 Alexandrov Sergey <splav@unsorted.ru>
 * Enables MOC to play wavpack files (actually just a wrapper around
 * wavpack library).
 *
 * Structure of this plugin is an adaption of the libvorbis-plugin from
 * moc.
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

#include <string.h>
#include <strings.h>
#include <stdint.h>
#include <assert.h>
#include <wavpack/wavpack.h>

#define DEBUG

#include "common.h" /* for xmalloc(), xstrdup() etc. */
#include "log.h" /* for logit() and debug() */
#include "decoder.h" /* required: provides decoder structure definition */
#include "io.h" /* if you use io_*() functions to access files. */
#include "audio.h" /* for sound_params structure */

struct wavpack_data
{
	WavpackContext *wpc;
	int sample_num;
	int sample_rate;
	int avg_bitrate;
	int channels;
	int duration;
	int mode;
	struct decoder_error error;
	int ok; /* was this stream successfully opened? */
};


static void wav_data_init (struct wavpack_data *data)
{
	data->sample_num = WavpackGetNumSamples (data->wpc);
	data->sample_rate = WavpackGetSampleRate (data->wpc);
	data->channels = WavpackGetReducedChannels (data->wpc);
	data->duration = data->sample_num / data->sample_rate;
	data->mode = WavpackGetMode (data->wpc);
	data->avg_bitrate = WavpackGetAverageBitrate (data->wpc, 1) / 1000;

	data->ok = 1;
	debug ("File opened. S_n %d. S_r %d. Time %d. Avg_Bitrate %d.",
		data->sample_num, data->sample_rate,
		data->duration, data->avg_bitrate
		);
}


static void *wav_open (const char *file)
{
	struct wavpack_data *data;
	data = (struct wavpack_data *)xmalloc (sizeof(struct wavpack_data));
	data->ok = 0;
	decoder_error_init (&data->error);

	int o_flags = OPEN_2CH_MAX | OPEN_WVC;

	char wv_error[100];

	if ((data->wpc = WavpackOpenFileInput(file,
				wv_error, o_flags, 0)) == NULL) {
		decoder_error (&data->error, ERROR_FATAL, 0, "%s", wv_error);
		logit ("wv_open error: %s", wv_error);
	}
	else
		wav_data_init (data);

	return data;
}

static void wav_close (void *prv_data)
{
	struct wavpack_data *data = (struct wavpack_data *)prv_data;

	if (data->ok) {
		WavpackCloseFile (data->wpc);
	}

	decoder_error_clear (&data->error);
	free (data);
	logit ("File closed");
}

static int wav_seek (void *prv_data, int sec)
{
	struct wavpack_data *data = (struct wavpack_data *)prv_data;

	assert (sec >= 0);

	if (WavpackSeekSample (data->wpc, sec * data->sample_rate))
		return sec;

	decoder_error (&data->error, ERROR_FATAL, 0, "Fatal seeking error!");
	return -1;
}


static int wav_get_bitrate (void *prv_data)
{
	struct wavpack_data *data = (struct wavpack_data *)prv_data;

	int bitrate;
	bitrate = WavpackGetInstantBitrate (data->wpc) / 1000;

	return (bitrate == 0)? data->avg_bitrate : bitrate;
}

static int wav_get_avg_bitrate (void *prv_data)
{
	struct wavpack_data *data = (struct wavpack_data *)prv_data;

	return data->avg_bitrate;
}

static int wav_get_duration (void *prv_data)
{
	struct wavpack_data *data = (struct wavpack_data *)prv_data;
	return data->duration;
}

static void wav_get_error (void *prv_data, struct decoder_error *error)
{
	struct wavpack_data *data = (struct wavpack_data *)prv_data;
	decoder_error_copy (error, &data->error);
}

static void wav_info (const char *file_name, struct file_tags *info,
		const int tags_sel)
{
	char wv_error[100];
	char *tag;
	int tag_len;

	WavpackContext *wpc;

	wpc = WavpackOpenFileInput (file_name, wv_error, OPEN_TAGS, 0);

	if (wpc == NULL) {
		logit ("wv_open error: %s", wv_error);
		return;
	}

	int duration = WavpackGetNumSamples (wpc) / WavpackGetSampleRate (wpc);

	if(tags_sel & TAGS_TIME) {
		info->time = duration;
		info->filled |= TAGS_TIME;
	}

	if(tags_sel & TAGS_COMMENTS) {
		if ((tag_len = WavpackGetTagItem (wpc, "title", NULL, 0)) > 0) {
			info->title = (char *)xmalloc (++tag_len);
			WavpackGetTagItem (wpc, "title", info->title, tag_len);
		}

		if ((tag_len = WavpackGetTagItem (wpc, "artist", NULL, 0)) > 0) {
			info->artist = (char *)xmalloc (++tag_len);
			WavpackGetTagItem (wpc, "artist", info->artist, tag_len);
		}

		if ((tag_len = WavpackGetTagItem (wpc, "album", NULL, 0)) > 0) {
			info->album = (char *)xmalloc (++tag_len);
			WavpackGetTagItem (wpc, "album", info->album, tag_len);
		}

		if ((tag_len = WavpackGetTagItem (wpc, "track", NULL, 0)) > 0) {
			tag = (char *)xmalloc (++tag_len);
			WavpackGetTagItem (wpc, "track", tag, tag_len);
			info->track = atoi (tag);
			free (tag);
		}

		info->filled |= TAGS_COMMENTS;
	}

	WavpackCloseFile (wpc);
}


static int wav_decode (void *prv_data, char *buf, int buf_len,
		struct sound_params *sound_params)
{
	struct wavpack_data *data = (struct wavpack_data *)prv_data;
	int ret, i, s_num, iBps, oBps;

	int8_t *buf8 = (int8_t *)buf;
	int16_t *buf16 = (int16_t *)buf;
	int32_t *buf32 = (int32_t *)buf;

	iBps = data->channels * WavpackGetBytesPerSample (data->wpc);
	oBps = (iBps == 6) ? 8 : iBps;
	s_num = buf_len / oBps;

	decoder_error_clear (&data->error);

	int32_t *dbuf = (int32_t *)xcalloc (s_num, data->channels * 4);

	ret = WavpackUnpackSamples (data->wpc, dbuf, s_num);

	if (ret == 0) {
		free (dbuf);
		return 0;
	}

	if (data->mode & MODE_FLOAT) {
		sound_params->fmt = SFMT_FLOAT;
		memcpy (buf, dbuf, ret * oBps);
	} else	{
		debug ("iBps %d", iBps);
		switch (iBps / data->channels){
		case 4: for (i = 0; i < ret * data->channels; i++)
				buf32[i] = dbuf[i];
			sound_params->fmt = SFMT_S32 | SFMT_NE;
			break;
		case 3: for (i = 0; i < ret * data->channels; i++)
				buf32[i] = dbuf[i] * 256;
			sound_params->fmt = SFMT_S32 | SFMT_NE;
			break;
		case 2: for (i = 0; i < ret * data->channels; i++)
				buf16[i] = dbuf[i];
			sound_params->fmt = SFMT_S16 | SFMT_NE;
			break;
		case 1: for (i = 0; i < ret * data->channels; i++)
				buf8[i] = dbuf[i];
			sound_params->fmt = SFMT_S8 | SFMT_NE;
		}
	}

	sound_params->channels = data->channels;
	sound_params->rate = data->sample_rate;

	free (dbuf);
	return ret * oBps ;
}

static int wav_our_mime (const char *mime ATTR_UNUSED)
{
	/* We don't support internet streams for now. */
#if 0
	return !strcasecmp (mime, "audio/x-wavpack")
		|| !strncasecmp (mime, "audio/x-wavpack;", 16)
#endif

	return 0;
}

static void wav_get_name (const char *unused ATTR_UNUSED, char buf[4])
{
	strcpy (buf, "WV");
}

static int wav_our_format_ext(const char *ext)
{
  return
    !strcasecmp (ext, "WV");
}

static struct decoder wv_decoder = {
        DECODER_API_VERSION,
        NULL,//wav_init
        NULL,//wav_destroy
        wav_open,
        NULL,//wav_open_stream,
        NULL,//wav_can_decode,
        wav_close,
        wav_decode,
        wav_seek,
        wav_info,
        wav_get_bitrate,
        wav_get_duration,
        wav_get_error,
        wav_our_format_ext,
        wav_our_mime,
        wav_get_name,
        NULL,//wav_current_tags,
        NULL,//wav_get_stream
        wav_get_avg_bitrate
};

struct decoder *plugin_init ()
{
        return &wv_decoder;
}
