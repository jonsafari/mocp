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

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <stdio.h>
#include <pthread.h>
#include <signal.h>
#include <string.h>

#include "server.h"
#include "main.h"
#include "file_types.h"
#include "playlist.h"
#include "log.h"

#ifdef HAVE_OSS
# include "oss.h"
#endif
#ifndef NDEBUG
# include "null_out.h"
#endif

#include "buf.h"
#include "audio.h"
#include "protocol.h"
#include "options.h"

static pthread_t playing_thread;  /* tid of play thread */

static int curr_playing = -1; /* currently played item */
static pthread_mutex_t curr_playing_mut = PTHREAD_MUTEX_INITIALIZER;

static struct buf out_buf;

static struct hw_funcs hw;

/* Player state */
static int state = STATE_STOP;

/* request for playing thread */
static volatile enum play_request request = PR_NOTHING;
static int stop_playing = 0;

static struct plist playlist;

enum play_request get_request ()
{
	int req = request;

	request = PR_NOTHING;
	
	return req;
}

static void make_play_request (const enum play_request req)
{
	request = req;
	buf_abort_put (&out_buf);
}

static void *play_thread (void *unused)
{
	play_func_t play_func;
	int play_num; /* The REAL position of the item on the playlist */

	logit ("entering playing thread");

	LOCK (curr_playing_mut);
	play_num = plist_find (&playlist, curr_playing);
	UNLOCK (curr_playing_mut);
	
	while (play_num != -1) {
		char *file;
		
		file = plist_get_file (&playlist, play_num);
		if (file) {
			play_func = get_play_func (file);
			if (play_func) {
				logit ("Playing item %d: %s", play_num, file);
				
				request = PR_NOTHING;
				state = STATE_PLAY;
				buf_time_set (&out_buf, 0.0);
				state_change ();
				
				play_func (file, &out_buf);
				
				audio_close ();
				state = STATE_STOP;
				set_info_time (0);
				set_info_rate (0);
				set_info_bitrate (0);
				set_info_channels (1);
				buf_time_set (&out_buf, 0.0);
			}
			else
				logit ("Unknown file type of item %d: %s",
						play_num, file);
			free (file);
		}

		if (stop_playing || !options_get_int("AutoNext")) {
			LOCK (curr_playing_mut);
			curr_playing = -1;
			play_num = -1;
			UNLOCK (curr_playing_mut);
			stop_playing = 0;
			logit ("play thread: stopped");
		}
		else {
			LOCK (curr_playing_mut);
			curr_playing = plist_next (&playlist, play_num);
			if (curr_playing == -1 && options_get_int("Repeat"))
				curr_playing = 0;
			play_num = curr_playing != -1 ?
				plist_find (&playlist, curr_playing) : -1;
			UNLOCK (curr_playing_mut);
		}

		state_change ();
	}

	logit ("exiting playing thread");

	return NULL;
}

void audio_reset ()
{
	hw.reset ();
}

void audio_stop ()
{
	int playing;

	LOCK (curr_playing_mut);
	playing = curr_playing;
	UNLOCK (curr_playing_mut);
	
	if (playing != -1) {
		logit ("audio_stop()");
		stop_playing = 1;
		make_play_request (PR_STOP);
		logit ("pthread_join(playing_thread, NULL)");
		pthread_join (playing_thread, NULL);
		logit ("done stopping");
	}
}

void audio_play (int num)
{
	audio_stop ();
	
	LOCK (curr_playing_mut);
	curr_playing = num;
	if (pthread_create(&playing_thread, NULL, play_thread, NULL))
		error ("can't create thread");
	UNLOCK (curr_playing_mut);
}

void audio_next ()
{
	make_play_request (PR_STOP);
}

void audio_pause ()
{
	buf_pause (&out_buf);
	state = STATE_PAUSE;
	state_change ();
}

void audio_unpause ()
{
	buf_unpause (&out_buf);
	state = STATE_PLAY;
	state_change ();
}

int audio_open (const int bits, const int channels, const int rate)
{
	return hw.open (bits, channels, rate);
}

int audio_send_buf (const char *buf, const size_t size)
{
	return buf_put (&out_buf, buf, size);
}

/* Get the current audio format bits per second value. May return 0 if the
 * audio device is closed. */
int audio_get_bps ()
{
	return hw.get_format() * hw.get_rate() * hw.get_channels();
}

int audio_get_buf_fill ()
{
	return hw.get_buff_fill ();
}

int audio_send_pcm (const char *buf, const size_t size)
{
	return hw.play (buf, size);
}

/* Get current time of the song in seconds. */
int audio_get_time ()
{
	return buf_time_get (&out_buf);
}

void audio_close ()
{
	buf_wait_empty (&out_buf);
	buf_reset (&out_buf);
	hw.close ();
}

static void find_hw_funcs (const char *driver, struct hw_funcs *funcs)
{
#ifdef HAVE_OSS
	if (!strcasecmp(driver, "oss")) {
		oss_funcs (funcs);
		return;
	}
#endif

#ifndef NDEBUG
	if (!strcasecmp(driver, "null")) {
		null_funcs (funcs);
		return;
	}
#endif

	fatal ("No valid sound driver");
}

void audio_init (const char *sound_driver)
{
	memset (&hw, 0, sizeof(hw));
	find_hw_funcs (sound_driver, &hw);
	hw.init ();
	buf_init (&out_buf, options_get_int("OutputBuffer") * 1024);
	plist_init (&playlist);
}

void audio_exit ()
{
	audio_stop ();
	hw.shutdown ();
	buf_destroy (&out_buf);
	plist_free (&playlist);
}

void audio_seek (const int sec)
{
	int playing;

	LOCK (curr_playing_mut);
	playing = curr_playing;
	UNLOCK (curr_playing_mut);
	
	if (playing != -1) {
		if (sec == 1)
			make_play_request (PR_SEEK_FORWARD);
		else if (sec == -1)
			make_play_request (PR_SEEK_BACKWARD);
		else
			logit ("Can't seek using this time.");
	}
	else
		logit ("Seeking when nothing is played.");
}

int audio_get_state ()
{
	return state;
}

void audio_plist_add (const char *file)
{
	plist_add (&playlist, file);
}

void audio_plist_clear ()
{
	plist_clear (&playlist);
}

/* Returned memory is malloc()ed. */
char *audio_get_sname ()
{
	char *sname = NULL;
	
	LOCK (curr_playing_mut);
	if (curr_playing != -1)
		sname = xstrdup (playlist.items[curr_playing].file);
	UNLOCK (curr_playing_mut);

	return sname;
}

int audio_get_mixer ()
{
	return hw.read_mixer ();
}

void audio_set_mixer (const int val)
{
	if (val >= 0 && val <= 100)
		hw.set_mixer (val);
	else
		logit ("Tried to set mixer to volume out of range.");
}
