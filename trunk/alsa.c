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

/* Based on aplay copyright (c) by Jaroslav Kysela <perex@suse.cz> */

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <stdlib.h>
#include <alsa/asoundlib.h>
#include <assert.h>

/* DEBUG */
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include "server.h"
#include "audio.h"
#include "main.h"
#include "log.h"

#define DEBUG

#define DEVICE		"default"
#define BUFFER_MAX_USEC	300000

/* TODO:
 * nonblock?
 * configurable buffer
 */

static snd_pcm_t *handle = NULL;
static struct sound_params params = { 0, 0, 0 };
static int chunk_size = -1;
static char alsa_buf[16384];
static int alsa_buf_fill = 0;

static int alsa_open (struct sound_params *sound_params)
{
	snd_pcm_hw_params_t *hw_params;
	int err;
	int rate;
	int period_time;
	int buffer_time;
	snd_pcm_uframes_t chunk_frames;
	snd_pcm_uframes_t buffer_frames;

	if ((err = snd_pcm_open(&handle, DEVICE, SND_PCM_STREAM_PLAYBACK,
					0)))
		error ("Can't open audio: %s", snd_strerror(err));

	if ((err = snd_pcm_hw_params_malloc(&hw_params)) < 0) {
		error ("Can't allocate alsa hardware parameters structure: %s",
				snd_strerror(err));
		return 0;
	}

	if ((err = snd_pcm_hw_params_any (handle, hw_params)) < 0) {
		error ("Can't initialize hardware parameters structure: %s",
				snd_strerror(err));
		snd_pcm_hw_params_free (hw_params);
		return 0;
	}

	if ((err = snd_pcm_hw_params_set_access (handle, hw_params, SND_PCM_ACCESS_RW_INTERLEAVED)) < 0) {
		error ("Can't set alsa access type: %s", snd_strerror(err));
		snd_pcm_hw_params_free (hw_params);
		return 0;
	}

	if ((err = snd_pcm_hw_params_set_format (handle, hw_params,
					sound_params->format == 1 ?
					SND_PCM_FORMAT_S8
					: SND_PCM_FORMAT_S16_LE)) < 0) {
		error ("Can't set sample format: %s", snd_strerror(err));
		snd_pcm_hw_params_free (hw_params);
		return 0;
	}

	rate = sound_params->rate;
	if ((err = snd_pcm_hw_params_set_rate_near (handle, hw_params,
					&rate, 0)) < 0) {
		error ("Can't set sample rate: %s", snd_strerror(err));
		snd_pcm_hw_params_free (hw_params);
		return 0;
	}

	if ((float)rate * 1.05 < sound_params->rate
			|| (float)rate * 0.95 > sound_params->rate) {
		error ("Can't set acurative rate: %dHz vs %dHz",
				sound_params->rate, rate);
		snd_pcm_hw_params_free (hw_params);
		return 0;
	}

#ifdef DEBUG
	logit ("Set rate to %d", rate);
#endif
	
	if ((err = snd_pcm_hw_params_set_channels (handle, hw_params,
					sound_params->channels)) < 0) {
		error ("Can't set number of channels: %s", snd_strerror(err));
		snd_pcm_hw_params_free (hw_params);
		return 0;
	}

	if ((err = snd_pcm_hw_params_get_buffer_time_max(hw_params,
					&buffer_time, 0)) < 0) {
		error ("Can't get maximum buffer time: %s", snd_strerror(err));
		snd_pcm_hw_params_free (hw_params);
		return 0;
	}

	if (buffer_time > BUFFER_MAX_USEC)
		buffer_time = BUFFER_MAX_USEC;
	period_time = buffer_time / 4;

	if ((err = snd_pcm_hw_params_set_period_time_near(handle, hw_params, 
					&period_time, 0)) < 0) {
		error ("Can't set period time: %s", snd_strerror(err));
		snd_pcm_hw_params_free (hw_params);
		return 0;
	}

	if ((err = snd_pcm_hw_params_set_buffer_time_near(handle, hw_params, 
					&buffer_time, 0)) < 0) {
		error ("Can't set buffer time: %s", snd_strerror(err));
		snd_pcm_hw_params_free (hw_params);
		return 0;
	}

	if ((err = snd_pcm_hw_params (handle, hw_params)) < 0) {
		error ("Can't set audio parameters: %s", snd_strerror(err));
		snd_pcm_hw_params_free (hw_params);
		return 0;
	}

	snd_pcm_hw_params_get_period_size (hw_params, &chunk_frames, 0);
	snd_pcm_hw_params_get_buffer_size (hw_params, &buffer_frames);

#ifdef DEBUG
	logit ("Buffer time: %ldus", buffer_frames * sound_params->channels
			* sound_params->format);
#endif


	if (chunk_frames == buffer_frames) {
		error ("Can't use period equal to buffer size (%lu == %lu)",
				chunk_frames, buffer_frames);
		snd_pcm_hw_params_free (hw_params);
		return 0;
	}

	chunk_size = chunk_frames * sound_params->format
		* sound_params->channels;

#ifdef DEBUG
	logit ("Chunk size: %d", chunk_size);
#endif
	
	snd_pcm_hw_params_free (hw_params);
	
	if ((err = snd_pcm_prepare(handle)) < 0) {
		error ("Can't prepare audio interface for use: %s",
				snd_strerror(err));
		return 0;
	}

#ifdef DEBUG
	logit ("ALSA device initialized");
#endif
	
	params.format = sound_params->format;
	params.channels = sound_params->channels;
	params.rate = rate;
	alsa_buf_fill = 0;
	return 1;
}

static void alsa_close ()
{
	/* FIXME: play buf_remain */
	params.format = 0;
	params.rate = 0;
	params.channels = 0;
	snd_pcm_close (handle);
#ifdef DEBUG
	logit ("ALSA device closed");
#endif
	handle = NULL;
}

static int alsa_play (const char *buff, const size_t size)
{
	int to_write = size;
	int buf_pos = 0;

	assert (chunk_size > 0);

#ifdef DEBUG
	logit ("Got %d bytes to play", (int)size);
#endif

	while (to_write) {
		int written = 0;
		int to_copy = MIN((size_t)to_write,
				sizeof(alsa_buf) - (size_t)alsa_buf_fill);
		
		memcpy (alsa_buf + alsa_buf_fill, buff + buf_pos, to_copy);
		to_write -= to_copy;
		buf_pos += to_copy;
		alsa_buf_fill += to_copy;

#ifdef DEBUG
		logit ("Copied %d bytes to alsa_buf (now is filled with %d "
				"bytes)", to_copy, alsa_buf_fill);
#endif
		
		while (alsa_buf_fill >= chunk_size) {
			int err;
			
			err = snd_pcm_writei (handle, alsa_buf + written,
					chunk_size /
					(params.format * params.channels));
			if (err == -EPIPE) {
				logit ("underrun!");
				if ((err = snd_pcm_prepare(handle)) < 0) {
					error ("Can't recover after underrun: %s",
							snd_strerror(err));
					/* TODO: reopen the device */
					return -1;
				}
			}
			/* TODO: handle -ESTRPIPE */
			else if (err < 0) {
				error ("Can't play: %s", snd_strerror(err));
				return -1;
			}
			else {
				int written_bytes = err *
					(params.format * params.channels);

				written += written_bytes;
				alsa_buf_fill -= written_bytes;

#ifdef DEBUG
				logit ("Played %d bytes", written_bytes);
#endif
			}
		}

		memmove (alsa_buf, alsa_buf + written, alsa_buf_fill);

#ifdef DEBUG
		logit ("%d bytes remain in alsa_buf", alsa_buf_fill);
#endif
	}

#ifdef DEBUG
	logit ("Played everything");
#endif

	return size;
}

static int alsa_read_mixer ()
{
	return 100;
}

static void alsa_set_mixer (int vol)
{
}

static int alsa_get_buff_fill ()
{
	return 0;
}

static int alsa_reset ()
{
	if (handle)
		snd_pcm_drop (handle);
	else
		logit ("alsa_reset() when the device is not opened.");
	return 1;
}

static int alsa_get_format ()
{
	return params.format;
}

static int alsa_get_rate ()
{
	return params.rate;
}

static int alsa_get_channels ()
{
	return params.channels;
}

void alsa_funcs (struct hw_funcs *funcs)
{
	funcs->open = alsa_open;
	funcs->close = alsa_close;
	funcs->play = alsa_play;
	funcs->read_mixer = alsa_read_mixer;
	funcs->set_mixer = alsa_set_mixer;
	funcs->get_buff_fill = alsa_get_buff_fill;
	funcs->reset = alsa_reset;
	funcs->get_format = alsa_get_format;
	funcs->get_rate = alsa_get_rate;
	funcs->get_channels = alsa_get_channels;
}
