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

#define _XOPEN_SOURCE	500
#include <unistd.h>

#include "audio.h"

static struct sound_params params = { 0, 0, 0 };

 
static void null_init ()
{
}

static void null_shutdown ()
{
}

static int null_open (struct sound_params *sound_params)
{
	params = *sound_params;
	return 1;
}

static void null_close ()
{
	params.channels = 0;
	params.rate = 0;
	params.format = 0;
}

static int null_play (const char *buff, const size_t size)
{
	usleep (((float)size / (params.channels * params.rate * params.format))
			* 1000000);
	return size;
}

static int null_read_mixer ()
{
	return 100;
}

static void null_set_mixer (int vol)
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

static int null_get_format ()
{
	return params.format;
}

static int null_get_rate ()
{
	return params.rate;
}

static int null_get_channels ()
{
	return params.channels;
}

void null_funcs (struct hw_funcs *funcs)
{
	funcs->init = null_init;
	funcs->shutdown = null_shutdown;
	funcs->open = null_open;
	funcs->close = null_close;
	funcs->play = null_play;
	funcs->read_mixer = null_read_mixer;
	funcs->set_mixer = null_set_mixer;
	funcs->get_buff_fill = null_get_buff_fill;
	funcs->reset = null_reset;
	funcs->get_format = null_get_format;
	funcs->get_rate = null_get_rate;
	funcs->get_channels = null_get_channels;
}

