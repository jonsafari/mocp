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
#include <errno.h>

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
#include "protocol.h"
#include "options.h"
#include "player.h"

/* FIXME: is 0 a tid that never is valid? */
static pthread_t playing_thread = 0;  /* tid of play thread */

static int curr_playing = -1; /* currently played item */
static pthread_mutex_t curr_playing_mut = PTHREAD_MUTEX_INITIALIZER;

static struct buf out_buf;
static struct hw_funcs hw;

/* Player state */
static int state = STATE_STOP;

/* request for playing thread */
static int stop_playing = 0;

static struct plist playlist;

static void *play_thread (void *unused)
{
	logit ("entering playing thread");

	while (curr_playing != -1) {
		char *file = plist_get_file (&playlist, curr_playing);
		struct decoder_funcs *df;

		if (file) {
			df = get_decoder_funcs (file);
			if (df) {
				logit ("Playing item %d: %s", curr_playing,
						file);
				
				state = STATE_PLAY;
				buf_time_set (&out_buf, 0.0);
				state_change ();
				
				player (file, df, &out_buf);
				
				state = STATE_STOP;
				set_info_time (0);
				set_info_rate (0);
				set_info_bitrate (0);
				set_info_channels (1);
				buf_time_set (&out_buf, 0.0);
			}
			else
				logit ("Unknown file type of item %d: %s",
						curr_playing, file);
			free (file);
		}

		if (stop_playing || !options_get_int("AutoNext")) {
			LOCK (curr_playing_mut);
			curr_playing = -1;
			UNLOCK (curr_playing_mut);
			logit ("stopped");
		}
		else {
			LOCK (curr_playing_mut);
			if (options_get_int("Shuffle"))
				curr_playing = plist_rand (&playlist);
			else {
				curr_playing = plist_next (&playlist,
						curr_playing);
				if (curr_playing == -1
						&& options_get_int("Repeat")) {
					curr_playing = plist_next (&playlist,
							-1);
					logit ("Going back to the first item.");
				}
				else if (curr_playing == -1)
					logit ("End of the list.");
				else
					logit ("Next item.");
			}
			UNLOCK (curr_playing_mut);
		}

		state_change ();
	}

	logit ("exiting");

	return NULL;
}

void audio_reset ()
{
	hw.reset ();
}

void audio_stop ()
{
	if (playing_thread) {
		logit ("audio_stop()");
		stop_playing = 1;
		player_stop ();
		logit ("pthread_join(playing_thread, NULL)");
		if (pthread_join(playing_thread, NULL))
			logit ("pthread_join() failed: %s", strerror(errno));
		playing_thread = 0;
		stop_playing = 0;
		logit ("done stopping");
	}
}

void audio_play (const char *fname)
{
	audio_stop ();
	player_reset ();
	
	LOCK (curr_playing_mut);
	curr_playing = plist_find_fname (&playlist, fname);
	if (curr_playing != -1) {
		if (pthread_create(&playing_thread, NULL, play_thread, NULL))
			error ("can't create thread");
	}
	else
		logit ("Client wanted to play a file not present on the "
				"playlist.");
	UNLOCK (curr_playing_mut);
}

void audio_next ()
{
	player_stop ();
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

/* Return 0 on error. */
int audio_open (struct sound_params *sound_params)
{
	return hw.open (sound_params);
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
	player_cleanup ();
	if (pthread_mutex_destroy(&curr_playing_mut))
		logit ("Can't destroy curr_playing_mut: %s", strerror(errno));
}

void audio_seek (const int sec)
{
	int playing;

	LOCK (curr_playing_mut);
	playing = curr_playing;
	UNLOCK (curr_playing_mut);
	
	if (playing != -1)
		player_seek (sec);
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
