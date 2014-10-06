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

/* Fake output device - only for testing. */

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <unistd.h>

#include "common.h"
#include "audio.h"

static struct sound_params params = { 0, 0, 0 };

static int null_open (struct sound_params *sound_params)
{
	params = *sound_params;
	return 1;
}

static void null_close ()
{
	params.rate = 0;
}

static int null_play (const char *unused ATTR_UNUSED, const size_t size)
{
	xsleep (size, audio_get_bps ());
	return size;
}

static int null_read_mixer ()
{
	return 100;
}

static void null_set_mixer (int unused ATTR_UNUSED)
{
}

static int null_get_buff_fill ()
{
	return 0;
}

static int null_reset ()
{
	return 1;
}

static int null_init (struct output_driver_caps *caps)
{
	caps->formats = SFMT_S8 | SFMT_S16 | SFMT_LE;
	caps->min_channels = 1;
	caps->max_channels = 2;

	return 1;
}

static int null_get_rate ()
{
	return params.rate;
}

static void null_toggle_mixer_channel ()
{
}

static char *null_get_mixer_channel_name ()
{
	return xstrdup ("FakeMixer");
}

void null_funcs (struct hw_funcs *funcs)
{
	funcs->init = null_init;
	funcs->open = null_open;
	funcs->close = null_close;
	funcs->play = null_play;
	funcs->read_mixer = null_read_mixer;
	funcs->set_mixer = null_set_mixer;
	funcs->get_buff_fill = null_get_buff_fill;
	funcs->reset = null_reset;
	funcs->get_rate = null_get_rate;
	funcs->toggle_mixer_channel = null_toggle_mixer_channel;
	funcs->get_mixer_channel_name = null_get_mixer_channel_name;
}
