/*
 * MOC - music on console
 * Copyright (C) 2003-2005 Damian Pietras <daper@daper.net>
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

#include <stdio.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/soundcard.h>
#include <errno.h>
#include <string.h>
#include <strings.h>
#include <pthread.h>
#include <unistd.h>

#include "server.h"
#include "audio.h"
#include "log.h"
#include "options.h"
#include "common.h"

static int mixer_fd = -1;
static int volatile dsp_fd = -1;
static int mixer_channel1 = -1;
static int mixer_channel2 = -1;
static int mixer_channel_current;

static struct sound_params params = { 0, 0, 0 };

static int open_dev ()
{
	if ((dsp_fd = open(options_get_str("OSSDevice"), O_WRONLY)) == -1) {
		error ("Can't open %s, %s", options_get_str("OSSDevice"),
				strerror(errno));
		return 0;
	}

	logit ("Audio device opened");
	
	return 1;
}

/* Fill caps with the device capabilities. Return 0 on error. */
static int set_capabilities (struct output_driver_caps *caps)
{
	int format_mask;

	if (!open_dev()) {
		error ("Can't open the device.");
		return 0;
	}

	if (ioctl(dsp_fd, SNDCTL_DSP_GETFMTS, &format_mask) == -1) {
		error ("Can't get supported audio formats: %s",
				strerror(errno));
		close (dsp_fd);
		return 0;
	}

	caps->formats = 0;
	if (format_mask & AFMT_S8)
		caps->formats |= SFMT_S8;
	if (format_mask & AFMT_U8)
		caps->formats |= SFMT_U8;
	if (format_mask & AFMT_S16_NE)
		caps->formats |= SFMT_S16;
#ifdef AFMT_S32_LE
	if (format_mask & AFMT_S32_LE)
		caps->formats |= SFMT_S32;
#endif
#ifdef AFMT_U32_LE
	if (format_mask & AFMT_U32_LE)
		caps->formats |= SFMT_U32;
#endif

	caps->formats |= SFMT_LE;

	if (!caps->formats) {
		error ("No known format supported by the audio device.");
		close (dsp_fd);
		return 0;
	}

	caps->min_channels = caps->max_channels = 1;
	if (ioctl(dsp_fd, SNDCTL_DSP_CHANNELS, &caps->min_channels)) {
		error ("Can't set number of channels: %s", strerror(errno));
		close (dsp_fd);
		return 0;
	}

	close (dsp_fd);
	if (!open_dev()) {
		error ("Can't open the device.");
		return 0;
	}

	if (caps->min_channels != 1)
		caps->min_channels = 2;
	caps->max_channels = 2;
	if (ioctl(dsp_fd, SNDCTL_DSP_CHANNELS, &caps->max_channels)) {
		error ("Can't set number of channels: %s", strerror(errno));
		close (dsp_fd);
		return 0;
	}

	if (caps->max_channels != 2) {
		if (caps->min_channels == 2) {
			error ("Can't get any supported number of channels.");
			close (dsp_fd);
			return 0;
		}
		caps->max_channels = 1;
	}

	close (dsp_fd);

	return 1;
}

/* Get PCM volume, return -1 on error */
static int oss_read_mixer ()
{
	int vol;
	
	if (mixer_fd != -1 && mixer_channel_current != -1) {
		if (ioctl(mixer_fd, MIXER_READ(mixer_channel_current), &vol)
				== -1)
			error ("Can't read from mixer");
		else {
			/* Average between left and right */
			return ((vol & 0xFF) + ((vol >> 8) & 0xFF)) / 2;
		}
	}

	return -1;
}

static int oss_init (struct output_driver_caps *caps)
{
	/* Open the mixer device */
	mixer_fd = open (options_get_str("OSSMixerDevice"), O_RDWR);
	if (mixer_fd == -1)
		error ("Can't open mixer device %s: %s",
				options_get_str("OSSMixerDevice"),
				strerror(errno));
	else {
		char *channel;
		
		/* first channel */
		channel = options_get_str ("OSSMixerChannel");
		if (!strcasecmp(channel, "pcm"))
			mixer_channel1 = SOUND_MIXER_PCM;
		else if (!strcasecmp(channel, "master"))
			mixer_channel1 = SOUND_MIXER_VOLUME;
		else
			fatal ("Bad first OSS mixer channel!");
		
		/* second channel */
		channel = options_get_str ("OSSMixerChannel2");
		if (!strcasecmp(channel, "pcm"))
			mixer_channel2 = SOUND_MIXER_PCM;
		else if (!strcasecmp(channel, "master"))
			mixer_channel2 = SOUND_MIXER_VOLUME;
		else
			fatal ("Bad second OSS mixer channel!");
		
		/* test mixer channels */
		mixer_channel_current = mixer_channel1;
		if (oss_read_mixer() == -1)
			mixer_channel1 = -1;

		mixer_channel_current = mixer_channel2;
		if (oss_read_mixer() == -1)
			mixer_channel2 = -1;

		if (mixer_channel1 != -1)
			mixer_channel_current = mixer_channel1;
	}

	return set_capabilities (caps);
}

static void oss_shutdown ()
{
	if (mixer_fd != -1) {
		close (mixer_fd);
		mixer_fd = -1;
	}
}

static void oss_close ()
{
	if (dsp_fd != -1) {
		close (dsp_fd);
		dsp_fd = -1;
		logit ("Audio device closed");
	}

	params.channels = 0;
	params.rate = 0;
	params.fmt = 0;
}

/* Return 0 on error. */
static int oss_set_params ()
{
	int req_format;
	int req_channels;
	char fmt_name[SFMT_STR_MAX];

	/* Set format */
	switch (params.fmt & SFMT_MASK_FORMAT) {
		case SFMT_S8:
			req_format = AFMT_S8;
			break;
		case SFMT_U8:
			req_format = AFMT_U8;
			break;
		case SFMT_S16:
			req_format = AFMT_S16_LE;
			break;
#ifdef AFMT_S32_LE
		case SFMT_S32:
			req_format = AFMT_S32_LE;
			break;
#endif
#ifdef AFMT_U32_LE
		case SFMT_U32:
			req_format = AFMT_U32_LE;
			break;
#endif
		default:
			error ("format %s is not supported by the device",
				sfmt_str(params.fmt, fmt_name,
					sizeof(fmt_name)));
			return 0;
	}
		
	if (ioctl(dsp_fd, SNDCTL_DSP_SETFMT, &req_format) == -1) {
		error ("Can't set audio format: %s", strerror(errno));
		oss_close ();
		return 0;
	}

	/* Set number of channels */
	req_channels = params.channels;
	if (ioctl(dsp_fd, SNDCTL_DSP_CHANNELS, &req_channels) == -1) {
		error ("Can't set number of channels to %d, %s",
				params.channels, strerror(errno));
		oss_close ();
		return 0;
	}
	if (params.channels != req_channels) {
		error ("Can't set number of channels to %d, device doesn't support this value",
				params.channels);
		oss_close ();
		return 0;
	}

	/* Set sample rate */
	if (ioctl(dsp_fd, SNDCTL_DSP_SPEED, &params.rate) == -1) {
		error ("Can't set sampling rate to %d, %s", params.rate,
				strerror(errno));
		oss_close ();
		return 0;
	}

	logit ("Audio parameters set to: %s, %d channels, %dHz",
			sfmt_str(params.fmt, fmt_name, sizeof(fmt_name)),
			params.channels, params.rate);

	return 1;
}

/* Return 0 on fail */
static int oss_open (struct sound_params *sound_params)
{
	params = *sound_params;
	
	if (!open_dev())
		return 0;
	
	if (!oss_set_params()) {
		oss_close ();
		return 0;
	}

	return 1;
}

/* Return -errno on error, number of bytes played when ok */
static int oss_play (const char *buff, const size_t size)
{
	int res;
	if (dsp_fd == -1)
		error ("Can't play, audio device isn't opened!");

	res = write(dsp_fd, buff, size);
	
	if (res == -1)
		error ("Error writing pcm sound: %s", strerror(errno));

	return res == -1 ? -errno : res;
}

/* Set PCM volume */
static void oss_set_mixer (int vol)
{
	if (mixer_fd != -1) {
		if (vol > 100)
			vol = 100;
		else if (vol < 0)
			vol = 0;
		
		vol = vol | (vol << 8);
		if (ioctl(mixer_fd, MIXER_WRITE(mixer_channel_current), &vol)
				== -1)
			error ("Can't set mixer, ioctl failed");
	}
}

/* Return number of bytes in device buffer */
static int oss_get_buff_fill ()
{
	audio_buf_info buff_info;

	if (dsp_fd == -1)
		return 0;

	if (ioctl(dsp_fd, SNDCTL_DSP_GETOSPACE, &buff_info) == -1) {
		error ("SNDCTL_DSP_GETOSPACE failed");
		return 0;
	}
	else
		return (buff_info.fragstotal * buff_info.fragsize)
			- buff_info.bytes;
}

/* Reset device buffer, stop playing immediately, return 0 on error */
static int oss_reset ()
{
	if (dsp_fd == -1) {
		logit ("reset when audio device is not opened");
		return 0;
	}

	logit ("Reseting audio device");

	if (ioctl(dsp_fd, SNDCTL_DSP_RESET, NULL) == -1)
		error ("Reseting audio device failed");
	close (dsp_fd);
	dsp_fd = -1;
	if (!open_dev() || !oss_set_params()) {
		error ("Failed to open audio device after reseting");
		return 0;
	}
	
	return 1;
}

static void oss_toggle_mixer_channel ()
{
	if (mixer_channel_current == mixer_channel1 && mixer_channel2 != -1)
		mixer_channel_current = mixer_channel2;
	else if (mixer_channel1 != -1)
		mixer_channel_current = mixer_channel1;
}

static char *oss_get_mixer_channel_name ()
{
	if (mixer_channel_current == mixer_channel1)
		return xstrdup (options_get_str("OSSMixerChannel"));
	return xstrdup (options_get_str("OSSMixerChannel2"));
}

static int oss_get_rate ()
{
	return params.rate;
}

void oss_funcs (struct hw_funcs *funcs)
{
	funcs->init = oss_init;
	funcs->shutdown = oss_shutdown;
	funcs->open = oss_open;
	funcs->close = oss_close;
	funcs->play = oss_play;
	funcs->read_mixer = oss_read_mixer;
	funcs->set_mixer = oss_set_mixer;
	funcs->get_buff_fill = oss_get_buff_fill;
	funcs->reset = oss_reset;
	funcs->get_rate = oss_get_rate;
	funcs->toggle_mixer_channel = oss_toggle_mixer_channel;
	funcs->get_mixer_channel_name = oss_get_mixer_channel_name;
}
