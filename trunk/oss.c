/*
 * MOC - music on console
 * Copyright (C) 2003,2004 Damian Pietras <daper@daper.net>
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
#include <pthread.h>
#include <unistd.h>

#include "server.h"
#include "audio.h"
#include "log.h"

#define PCM_DEVICE	"/dev/dsp"
#define MIXER_DEVICE	"/dev/mixer"

static int mixer_fd = -1;
static int volatile dsp_fd = -1;

static struct sound_params params = { 0, 0, 0 };

static void oss_init ()
{

	/* Open the mixer device */
	mixer_fd = open (MIXER_DEVICE, O_RDWR);
	if (mixer_fd == -1)
		error ("Can't open mixer device %s: %s", MIXER_DEVICE,
				strerror(errno));
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
	params.format = 0;
}

/* Return 0 on error. */
static int oss_set_params ()
{
	int req_format;
	int req_channels;
	int req_rate;

	/* Set format */
	if (params.format == 1)
		req_format = AFMT_S8;
	else
		req_format = AFMT_S16_LE;
		
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
	req_rate = params.rate;
	if (ioctl(dsp_fd, SNDCTL_DSP_SPEED, &req_rate) == -1) {
		error ("Can't set sampling rate to %d, %s", params.rate,
				strerror(errno));
		oss_close ();
		return 0;
	}
	if (params.rate != req_rate) {
		error ("Can't set sample rate %d, device doesn't support this value",
				params.rate);
		oss_close ();
		return 0;
	}

	logit ("Audio parameters set to: %dbit, %d channels, %dHz",
			params.format * 8, params.channels, params.rate);

	return 1;
}

static int open_dev ()
{
	if ((dsp_fd = open(PCM_DEVICE, O_WRONLY)) == -1) {
		error ("Can't open %s, %s", PCM_DEVICE, strerror(errno));
		return 0;
	}

	logit ("Audio device opened");
	
	return 1;
}

/* Return 0 on fail */
static int oss_open (struct sound_params *sound_params)
{

	params = *sound_params;
	
	if (!open_dev())
		return 0;
	
	if (!oss_set_params())
		return 0;

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

/* Get PCM volume, return -1 on error */
static int oss_read_mixer ()
{
	int vol;
	
	if (mixer_fd != -1) {
		if (ioctl(mixer_fd, MIXER_READ(SOUND_MIXER_PCM), &vol) == -1)
			error ("Can't read from mixer");
		else {
			/* Average between left and right */
			return ((vol & 0xFF) + ((vol >> 8) & 0xFF)) / 2;
		}
	}

	return -1;
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
		if (ioctl(mixer_fd, MIXER_WRITE(SOUND_MIXER_PCM), &vol) == -1)
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

	if (ioctl(dsp_fd, SNDCTL_DSP_RESET) == -1)
		error ("Reseting audio device failed");
	close (dsp_fd);
	dsp_fd = -1;
	if (!open_dev() || !oss_set_params()) {
		error ("Failed to open audio device after reseting");
		return 0;
	}
	
	return 1;
}

/* Return the number of bytes of current format (1 or 2) */
static int oss_get_format ()
{
	return params.format;
}

static int oss_get_rate ()
{
	return params.rate;
}

static int oss_get_channels ()
{
	return params.channels;
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
	funcs->get_format = oss_get_format;
	funcs->get_rate = oss_get_rate;
	funcs->get_channels = oss_get_channels;
}
