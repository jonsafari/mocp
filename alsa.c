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

/* TODO:
 * - When another application changes the mixer settings, they are not visible
 *   here.
 */

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#define DEBUG

#include <stdlib.h>
#include <alsa/asoundlib.h>
#include <assert.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include "server.h"
#include "audio.h"
#include "main.h"
#include "options.h"
#include "log.h"

#define BUFFER_MAX_USEC	300000

static snd_pcm_t *handle = NULL;

static struct
{
	unsigned channels;
	unsigned rate;
	snd_pcm_format_t format;
} params = { 0, 0, SND_PCM_FORMAT_UNKNOWN };

static int chunk_size = -1; /* in frames */
static char alsa_buf[64 * 1024];
static int alsa_buf_fill = 0;
static int bytes_per_frame;

static snd_mixer_t *mixer_handle = NULL;
static snd_mixer_elem_t *mixer_elem = NULL;
static long mixer_min = -1, mixer_max = -1;

#define mixer_percent(vol) ((((vol) - mixer_min)*100)/(mixer_max - mixer_min))

static void alsa_shutdown ()
{
	int err;
	
	if (mixer_handle && (err = snd_mixer_close(mixer_handle)) < 0)
		logit ("Can't close mixer: %s", snd_strerror(err));
}

static void fill_capabilities (struct output_driver_caps *caps)
{
	snd_pcm_hw_params_t *hw_params;
	snd_pcm_format_mask_t *format_mask;
	int err;
	unsigned val;

	if ((err = snd_pcm_open(&handle, options_get_str("AlsaDevice"),
					SND_PCM_STREAM_PLAYBACK,
					SND_PCM_NONBLOCK)) < 0) {
		fatal ("Can't open audio: %s", snd_strerror(err));
	}

	if ((err = snd_pcm_hw_params_malloc(&hw_params)) < 0) {
		fatal ("Can't allocate alsa hardware parameters structure: %s",
				snd_strerror(err));
	}

	if ((err = snd_pcm_hw_params_any (handle, hw_params)) < 0) {
		fatal ("Can't initialize hardware parameters structure: %s",
				snd_strerror(err));
	}

	if ((err = snd_pcm_hw_params_get_channels_min (hw_params, &val)) < 0) {
		fatal ("Can't get the minimum number of channels: %s",
				snd_strerror(err));
	}
	caps->min_channels = val;
	
	if ((err = snd_pcm_hw_params_get_channels_max (hw_params, &val)) < 0) {
		fatal ("Can't get the maximum number of channels: %s",
				snd_strerror(err));
	}
	caps->max_channels = val;

	if ((err = snd_pcm_format_mask_malloc(&format_mask)) < 0) {
		fatal ("Can't allocate format mask: %s", snd_strerror(err));
	}
	snd_pcm_hw_params_get_format_mask (hw_params, format_mask);

	caps->formats = SFMT_LE;
	if (snd_pcm_format_mask_test(format_mask, SND_PCM_FORMAT_S8))
		caps->formats |= SFMT_S8;
	if (snd_pcm_format_mask_test(format_mask, SND_PCM_FORMAT_U8))
		caps->formats |= SFMT_U8;
	if (snd_pcm_format_mask_test(format_mask, SND_PCM_FORMAT_S16))
		caps->formats |= SFMT_S16;
	if (snd_pcm_format_mask_test(format_mask, SND_PCM_FORMAT_U16))
		caps->formats |= SFMT_U16;
#if 0
	if (snd_pcm_format_mask_test(format_mask, SND_PCM_FORMAT_S24))
		caps->formats |= SFMT_S32; /* conversion needed */
#endif
	if (snd_pcm_format_mask_test(format_mask, SND_PCM_FORMAT_S32))
		caps->formats |= SFMT_S32;
	if (snd_pcm_format_mask_test(format_mask, SND_PCM_FORMAT_U32))
		caps->formats |= SFMT_U32;

	snd_pcm_format_mask_free (format_mask);
	snd_pcm_hw_params_free (hw_params);
	snd_pcm_close (handle);
	handle = NULL;
}

static void alsa_init (struct output_driver_caps *caps)
{
	int err;
	snd_mixer_selem_id_t *mixer_sid;

	snd_mixer_selem_id_alloca (&mixer_sid);
	snd_mixer_selem_id_set_index (mixer_sid, 0);
	snd_mixer_selem_id_set_name (mixer_sid, options_get_str("AlsaMixer"));

	if ((err = snd_mixer_open(&mixer_handle, 0)) < 0) {
		error ("Can't open ALSA mixer: %s", snd_strerror(err));
		mixer_handle = NULL;
	}
	else if ((err = snd_mixer_attach(mixer_handle,
					options_get_str("AlsaDevice"))) < 0) {
		snd_mixer_close (mixer_handle);
		mixer_handle = NULL;
		error ("Can't attach mixer: %s", snd_strerror(err));
	}
	else if ((err = snd_mixer_selem_register(mixer_handle, NULL, NULL))
			< 0) {
		snd_mixer_close (mixer_handle);
		mixer_handle = NULL;
		error ("Can't register mixer: %s", snd_strerror(err));
	}
	else if ((err = snd_mixer_load(mixer_handle)) < 0) {
		snd_mixer_close (mixer_handle);
		mixer_handle = NULL;
		error ("Can't load mixer: %s", snd_strerror(err));
	}
	else if (!(mixer_elem = snd_mixer_find_selem(mixer_handle,
					mixer_sid))) {
		snd_mixer_close (mixer_handle);
		mixer_handle = NULL;
		error ("Can't find mixer: %s", snd_strerror(err));
	}
	else if (!snd_mixer_selem_has_playback_volume(mixer_elem)) {
		snd_mixer_close (mixer_handle);
		mixer_handle = NULL;
		error ("Mixer device has no playback volume.");
	}
	else {
		snd_mixer_selem_get_playback_volume_range (mixer_elem,
				&mixer_min, &mixer_max);
		logit ("Opened mixer, volume range: %ld-%ld", mixer_min,
				mixer_max);
	}

	fill_capabilities (caps);
}

static int alsa_open (struct sound_params *sound_params)
{
	snd_pcm_hw_params_t *hw_params;
	int err;
	unsigned int period_time;
	unsigned int buffer_time;
	snd_pcm_uframes_t chunk_frames;
	snd_pcm_uframes_t buffer_frames;
	char fmt_name[128];

	switch (sound_params->fmt & SFMT_MASK_FORMAT) {
		case SFMT_S8:
			params.format = SND_PCM_FORMAT_S8;
			break;
		case SFMT_U8:
			params.format = SND_PCM_FORMAT_U8;
			break;
		case SFMT_S16:
			params.format = SND_PCM_FORMAT_S16;
			break;
		case SFMT_U16:
			params.format = SND_PCM_FORMAT_U16;
			break;
		case SFMT_S32:
			params.format = SND_PCM_FORMAT_S32;
			break;
		case SFMT_U32:
			params.format = SND_PCM_FORMAT_U32;
			break;
		default:
			error ("Unknown sample format: %s",
					sfmt_str(sound_params->fmt, fmt_name,
						sizeof(fmt_name)));
			params.format = SND_PCM_FORMAT_UNKNOWN;
			return 0;
	}

	if ((err = snd_pcm_open(&handle, options_get_str("AlsaDevice"),
					SND_PCM_STREAM_PLAYBACK,
					SND_PCM_NONBLOCK)) < 0) {
		error ("Can't open audio: %s", snd_strerror(err));
		return 0;
	}

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
					params.format)) < 0) {
		error ("Can't set sample format: %s", snd_strerror(err));
		snd_pcm_hw_params_free (hw_params);
		return 0;
	}

	params.rate = sound_params->rate;
	if ((err = snd_pcm_hw_params_set_rate_near (handle, hw_params,
					&params.rate, 0)) < 0) {
		error ("Can't set sample rate: %s", snd_strerror(err));
		snd_pcm_hw_params_free (hw_params);
		return 0;
	}

	logit ("Set rate to %d", params.rate);
	
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

	bytes_per_frame = sound_params->channels
		* sfmt_Bps(sound_params->fmt);

	logit ("Buffer time: %ldus", buffer_frames * bytes_per_frame);

	if (chunk_frames == buffer_frames) {
		error ("Can't use period equal to buffer size (%lu == %lu)",
				chunk_frames, buffer_frames);
		snd_pcm_hw_params_free (hw_params);
		return 0;
	}

	chunk_size = chunk_frames * bytes_per_frame;

	debug ("Chunk size: %d", chunk_size);
	
	snd_pcm_hw_params_free (hw_params);
	
	if ((err = snd_pcm_prepare(handle)) < 0) {
		error ("Can't prepare audio interface for use: %s",
				snd_strerror(err));
		return 0;
	}

	debug ("ALSA device initialized");
	
	params.channels = sound_params->channels;
	alsa_buf_fill = 0;
	return 1;
}

/* Play from alsa_buf as many chunks as possible. Move the remaining data
 * to the beginning of the buffer. Return the number of bytes written
 * or -1 on error. */
static int play_buf_chunks ()
{
	int written = 0;
	
	while (alsa_buf_fill >= chunk_size) {
		int err;
		
		err = snd_pcm_writei (handle, alsa_buf + written,
				chunk_size / bytes_per_frame);
		if (err == -EAGAIN) {
			if (snd_pcm_wait(handle, 5000) < 0)
				logit ("snd_pcm_wait() failed");
		}
		else if (err == -EPIPE) {
			logit ("underrun!");
			if ((err = snd_pcm_prepare(handle)) < 0) {
				error ("Can't recover after underrun: %s",
						snd_strerror(err));
				/* TODO: reopen the device */
				return -1;
			}
		}
		else if (err == -ESTRPIPE) {
			logit ("Suspend, trying to resume");
			while ((err = snd_pcm_resume(handle))
					== -EAGAIN)
				sleep (1);
			if (err < 0) {
				logit ("Failed, restarting");
				if ((err = snd_pcm_prepare(handle))
						< 0) {
					error ("Failed to restart "
							"device: %s.",
							snd_strerror(err));
					return -1;
				}
			}
		}
		else if (err < 0) {
			error ("Can't play: %s", snd_strerror(err));
			return -1;
		}
		else {
			int written_bytes = err * bytes_per_frame;

			written += written_bytes;
			alsa_buf_fill -= written_bytes;

			debug ("Played %d bytes", written_bytes);
		}
	}

	debug ("%d bytes remain in alsa_buf", alsa_buf_fill);
	memmove (alsa_buf, alsa_buf + written, alsa_buf_fill);

	return written * bytes_per_frame;
}

static void alsa_close ()
{

	assert (handle != NULL);

	/* play what remained in the buffer */
	if (alsa_buf_fill) {
		assert (alsa_buf_fill < chunk_size);

		/* FIXME: why the last argument is multiplied by number of
		 * channels? */
		snd_pcm_format_set_silence (params.format,
				alsa_buf + alsa_buf_fill,
				(chunk_size - alsa_buf_fill) / bytes_per_frame
				* params.channels);
		play_buf_chunks ();
	}
	
	params.format = 0;
	params.rate = 0;
	params.channels = 0;
	snd_pcm_close (handle);
	logit ("ALSA device closed");
	handle = NULL;
}

static int alsa_play (const char *buff, const size_t size)
{
	int to_write = size;
	int buf_pos = 0;

	assert (chunk_size > 0);

	debug ("Got %d bytes to play", (int)size);

	while (to_write) {
		int to_copy = MIN((size_t)to_write,
				sizeof(alsa_buf) - (size_t)alsa_buf_fill);
		
		memcpy (alsa_buf + alsa_buf_fill, buff + buf_pos, to_copy);
		to_write -= to_copy;
		buf_pos += to_copy;
		alsa_buf_fill += to_copy;

		debug ("Copied %d bytes to alsa_buf (now is filled with %d "
				"bytes)", to_copy, alsa_buf_fill);
		
		if (play_buf_chunks() < 0)
			return -1;
	}

	debug ("Played everything");

	return size;
}

static int alsa_read_mixer_raw ()
{
	if (mixer_handle) {
		long volume = 0;
		int nchannels = 0;
		int i;

		for (i = 0; i < SND_MIXER_SCHN_LAST; i++)
			if (snd_mixer_selem_has_playback_channel(mixer_elem,
						1 << i)) {
				int err;
				long vol;
				
				nchannels++;
				if ((err = snd_mixer_selem_get_playback_volume(
								mixer_elem,
								1 << i,
								&vol)) < 0) {
					error ("Can't read mixer: %s",
							snd_strerror(err));
					return -1;
						
				}
				/*logit ("Vol %d: %ld", i, vol);*/
				volume += vol;
			}

		volume /= nchannels;

		/*logit ("Max: %ld, Min: %ld", mixer_max, mixer_min);*/
		return volume;

	}
	else
		return -1;
}

static int alsa_read_mixer ()
{
	int vol = alsa_read_mixer_raw ();

	if (vol != -1)
		return mixer_percent(vol);
	return -1;
}

static void alsa_set_mixer (int vol)
{
	if (mixer_handle) {
		int err;
		long curr_vol = alsa_read_mixer_raw ();
		long vol_alsa;

		if (vol < 0)
			vol = 0;
		else if (vol > 100)
			vol = 100;

		debug ("Setting vol to %d%%", vol);

		vol_alsa = mixer_min + (mixer_max - mixer_min) * vol/100.0;
		
		if (vol_alsa == curr_vol) {

			/* Problem with ALSA mixer resolution: it could be
			 * worse than 1% */
			if (vol > mixer_percent(curr_vol))
				vol_alsa++;
			else if (vol < mixer_percent(curr_vol))
				vol_alsa--;
		}

		if (vol_alsa > mixer_max)
			vol_alsa = mixer_max;
		else if (vol_alsa < mixer_min)
			vol_alsa = mixer_min;
		
		if ((err = snd_mixer_selem_set_playback_volume_all(
						mixer_elem, vol_alsa)) < 0)
			error ("Can't set mixer: %s", snd_strerror(err));
	}
}

static int alsa_get_buff_fill ()
{
	if (handle) {
		int err;
		snd_pcm_sframes_t delay;
		
		if ((err = snd_pcm_delay(handle, &delay)) < 0) {
			logit ("snd_pcm_delay() failed: %s", snd_strerror(err));
			return 0;
		}

		/* delay can be negative when underrun occur */
		return delay >= 0 ? delay * bytes_per_frame : 0;
	}
	return 0;
}

static int alsa_reset ()
{
	if (handle) {
		int err;
		
		if ((err = snd_pcm_drop(handle)) < 0) {
			error ("Can't reset the device: %s",
					snd_strerror(err));
			return 0;
		}
		if ((err = snd_pcm_prepare(handle)) < 0) {
			error ("Can't prepare anfter reset: %s",
					snd_strerror(err));
			return 0;
		}

		alsa_buf_fill = 0;
	}
	else
		logit ("alsa_reset() when the device is not opened.");
	return 1;
}

static int alsa_get_rate ()
{
	return params.rate;
}

void alsa_funcs (struct hw_funcs *funcs)
{
	funcs->init = alsa_init;
	funcs->shutdown = alsa_shutdown;
	funcs->open = alsa_open;
	funcs->close = alsa_close;
	funcs->play = alsa_play;
	funcs->read_mixer = alsa_read_mixer;
	funcs->set_mixer = alsa_set_mixer;
	funcs->get_buff_fill = alsa_get_buff_fill;
	funcs->reset = alsa_reset;
	funcs->get_rate = alsa_get_rate;
}
