/*
 * MOC - music on console
 * Copyright (C) 2004 Damian Pietras <daper@daper.net>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * Contributors:
 *  - Kamil Tarkowski <kamilt@interia.pl> - "porevious" request
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
#include <assert.h>

/*#define DEBUG*/

#include "server.h"
#include "main.h"
#include "file_types.h"
#include "playlist.h"
#include "log.h"

#ifdef HAVE_OSS
# include "oss.h"
#endif
#ifdef HAVE_ALSA
# include "alsa.h"
#endif
#ifndef NDEBUG
# include "null_out.h"
#endif

#include "buf.h"
#include "protocol.h"
#include "options.h"
#include "player.h"
#include "audio.h"

static pthread_t playing_thread = 0;  /* tid of play thread */
static int play_thread_running = 0;

/* currentlu played file */
static int curr_playing = -1;
static pthread_mutex_t curr_playing_mut = PTHREAD_MUTEX_INITIALIZER;

static struct buf out_buf;
static struct hw_funcs hw;

/* Player state */
static int state = STATE_STOP;

/* requests for playing thread */
static int stop_playing = 0;
static int play_next = 0;
static int play_prev = 0;

/* Playlists. */
static struct plist playlist;
static struct plist shuffled_plist;
static struct plist *curr_plist; /* currently used playlist */

/* Is the audio deice opened? */
static int audio_opened = 0;

/* Current sound parameters. */
static struct sound_params sound_params;

/* Move to the next file depending on set options and the user request. */
static void go_to_another_file ()
{
	int shuffle = options_get_int("Shuffle");
	
	LOCK (curr_playing_mut);
	
	if (shuffle && plist_count(&playlist)
			&& !plist_count(&shuffled_plist)) {
		plist_cat (&shuffled_plist, &playlist);
		plist_shuffle (&shuffled_plist);
		if (curr_playing != -1)
			plist_swap_first_fname (&shuffled_plist,
					plist_get_file(curr_plist,
						curr_playing));
	}


	/* If Shuffle was switched while playing, we must correct curr_playing
	 * by searching for the current item on the list where we are
	 * switching. */
	if (shuffle && curr_plist != &shuffled_plist) {
		curr_playing = plist_find_fname (&shuffled_plist,
				plist_get_file(&playlist, curr_playing));
		assert (curr_playing != -1);
	}
	else if (!shuffle && curr_plist != &playlist) {
		curr_playing = plist_find_fname (&playlist,
				plist_get_file(&shuffled_plist, curr_playing));
		assert (curr_playing != -1);
	}
	
	if (shuffle)
		curr_plist = &shuffled_plist;
	else
		curr_plist = &playlist;
	
	if (play_prev == 1) { 
		curr_playing = plist_prev (curr_plist, curr_playing);
		if (curr_playing == -1)
			logit ("Beginning of the list.");
		else 
			logit ("Previous item.");
	}
	else {
		curr_playing = plist_next (curr_plist, curr_playing);
		if (curr_playing == -1 && options_get_int("Repeat")) {
			if (shuffle)
				plist_shuffle (&shuffled_plist);
			curr_playing = plist_next (curr_plist, -1);
			logit ("Going back to the first item.");
		}
		else if (curr_playing == -1)
			logit ("End of the list.");
		else
			logit ("Next item.");
	}
	UNLOCK (curr_playing_mut);
}

static void *play_thread (void *unused ATTR_UNUSED)
{
	logit ("entering playing thread");

	while (curr_playing != -1) {
		char *file = plist_get_file (curr_plist, curr_playing);
		struct decoder_funcs *df;

		play_next = 0;
		play_prev = 0;

		if (file) {
			df = get_decoder_funcs (file);
			if (df) {
				int next;
				char *next_file;
				
				logit ("Playing item %d: %s", curr_playing,
						file);
				
				buf_time_set (&out_buf, 0.0);
				
				next = plist_next (curr_plist, curr_playing);
				next_file = next != -1 ?
					plist_get_file(curr_plist, next)
					: NULL;
				player (file, next_file, &out_buf);
				if (next_file)
					free (next_file);

				state = STATE_STOP;
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

		if (stop_playing || (!options_get_int("AutoNext")
					&& !play_next && !play_prev)) {
			LOCK (curr_playing_mut);
			curr_playing = -1;
			UNLOCK (curr_playing_mut);
			logit ("stopped");
		}
		else
			go_to_another_file ();

		state_change ();
	}

	audio_close ();
	logit ("exiting");

	return NULL;
}

void audio_reset ()
{
	if (hw.reset)
		hw.reset ();
}

void audio_stop ()
{
	if (play_thread_running) {
		logit ("audio_stop()");
		stop_playing = 1;
		player_stop ();
		logit ("pthread_join(playing_thread, NULL)");
		if (pthread_join(playing_thread, NULL))
			logit ("pthread_join() failed: %s", strerror(errno));
		playing_thread = 0;
		play_thread_running = 0;
		stop_playing = 0;
		logit ("done stopping");
	}
}

/* Start playing from the file fname. If fname is an empty string,
 * start playing from the first file on the list. */
void audio_play (const char *fname)
{
	audio_stop ();
	player_reset ();
	
	LOCK (curr_playing_mut);
	if (options_get_int("Shuffle")) {
		if (plist_count(&playlist) && !plist_count(&shuffled_plist)) {
			plist_cat (&shuffled_plist, &playlist);
			plist_shuffle (&shuffled_plist);
			plist_swap_first_fname (&shuffled_plist, fname);
		}

		curr_plist = &shuffled_plist;

		if (*fname)
			curr_playing = plist_find_fname (curr_plist, fname);
		else if (plist_count(curr_plist)) {
			curr_playing = plist_next (curr_plist, -1);
		}
		else 
			curr_playing = -1;
	}
	else {
		curr_plist = &playlist;
		
		if (*fname)
			curr_playing = plist_find_fname (curr_plist, fname);
		else if (plist_count(curr_plist))
			curr_playing = plist_next (curr_plist, -1);
		else 
			curr_playing = -1;
	}
	
	if (curr_playing != -1) {
		if (pthread_create(&playing_thread, NULL, play_thread, NULL))
			error ("can't create thread");
		play_thread_running = 1;
	}
	else
		logit ("Client wanted to play a file not present on the "
				"playlist.");
	UNLOCK (curr_playing_mut);
}

void audio_next ()
{
	if (play_thread_running) {
		play_next = 1;
		player_stop ();
	}
}

void audio_prev ()
{
	if (play_thread_running) {
		play_prev = 1;
		player_stop ();
	}
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
int audio_open (struct sound_params *req_sound_params)
{
	int res;
	
	if (audio_opened && sound_params_eq(*req_sound_params, sound_params)) {
		if (audio_get_bps() < 88200) {
			logit ("Reopening device due to low bps.");
			
			/* Not closing the device would cause that much
			 * sound from the previuous file stays in the buffer
			 * and the user will see old data, so close it. */
			hw.close ();
			res = hw.open (req_sound_params);
			if (res)
				audio_opened = 1;
			return res;
		}
		else
			logit ("Audio device already opened with such "
					"parameters.");

		return 1;
	}
	else if (audio_opened)
		hw.close ();

	sound_params = *req_sound_params;
	res = hw.open (req_sound_params);
	if (res)
		audio_opened = 1;
	return res;
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
	return state != STATE_STOP ? buf_time_get (&out_buf) : 0;
}

void audio_close ()
{
	if (audio_opened) {
		hw.close ();
		audio_opened = 0;
	}
}

static void find_hw_funcs (const char *driver, struct hw_funcs *funcs)
{
	memset (funcs, 0, sizeof(funcs));

#ifdef HAVE_OSS
	if (!strcasecmp(driver, "oss")) {
		oss_funcs (funcs);
		return;
	}
#endif

#ifdef HAVE_ALSA
	if (!strcasecmp(driver, "alsa")) {
		alsa_funcs (funcs);
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

void audio_init ()
{
	memset (&hw, 0, sizeof(hw));
	find_hw_funcs (options_get_str("SoundDriver"), &hw);
	if (hw.init)
		hw.init ();
	buf_init (&out_buf, options_get_int("OutputBuffer") * 1024);
	plist_init (&playlist);
	plist_init (&shuffled_plist);
	player_init ();
}

void audio_exit ()
{
	audio_stop ();
	if (hw.shutdown)
		hw.shutdown ();
	buf_destroy (&out_buf);
	plist_free (&playlist);
	plist_free (&shuffled_plist);
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
	
	if (playing != -1 && state == STATE_PLAY)
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
	plist_clear (&shuffled_plist);
	plist_add (&playlist, file);
}

void audio_plist_clear ()
{
	plist_clear (&shuffled_plist);
	plist_clear (&playlist);
}

/* Returned memory is malloc()ed. */
char *audio_get_sname ()
{
	char *sname;

	LOCK (curr_playing_mut);
	if (curr_playing != -1)
		sname = plist_get_file (curr_plist, curr_playing);
	else
		sname = NULL;
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

void audio_plist_delete (const char *file)
{
	int num = plist_find_fname (&playlist, file);

	if (num != -1) {
		plist_clear (&shuffled_plist);
		plist_delete (&playlist, num);
	}
}

/* Get the time of a file if it is on the playlist and the time is avilable. */
int audio_get_ftime (const char *file)
{
	int i;
	int time;

	/* TODO: also check the mtime. */

	if ((i = plist_find_fname(&playlist, file)) != -1
			&& (time = get_item_time(&playlist, i)) != -1) {
		debug ("Found time for %s", file);
		return time;
	}

	return -1;
}

/* Set the time for a file on the playlist. */
void audio_plist_set_time (const char *file, const int time)
{
	int i;

	if ((i = plist_find_fname(&playlist, file)) != -1) {
		update_item_time (&playlist.items[i], time);
		/* TODO: also update the mtime */
		debug ("Setting time for %s", file);
	}
	else
		logit ("Request for updating time for a file not present on the"
				" playlist!");
}

/* Notify about changing the state (unsed by the player). */
void audio_state_started_playing ()
{
	state = STATE_PLAY;
	state_change ();
}
