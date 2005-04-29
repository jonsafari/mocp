/*
 * MOC - music on console
 * Copyright (C) 2004-2005 Damian Pietras <daper@daper.net>
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

#define DEBUG

#include "server.h"
#include "main.h"
#include "decoder.h"
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
#ifdef HAVE_JACK
# include "jack.h"
#endif

#include "out_buf.h"
#include "protocol.h"
#include "options.h"
#include "player.h"
#include "audio.h"
#include "files.h"
#include "io.h"
#include "audio_convertion.h"

static pthread_t playing_thread = 0;  /* tid of play thread */
static int play_thread_running = 0;

/* currentlu played file */
static int curr_playing = -1;
static pthread_mutex_t curr_playing_mut = PTHREAD_MUTEX_INITIALIZER;

static struct out_buf out_buf;
static struct hw_funcs hw;
static struct output_driver_caps hw_caps; /* capabilities of the output
					     driver */

/* Player state */
static int state = STATE_STOP;

/* requests for playing thread */
static int stop_playing = 0;
static int play_next = 0;
static int play_prev = 0;
static pthread_mutex_t request_mut = PTHREAD_MUTEX_INITIALIZER;

/* Playlists. */
static struct plist playlist;
static struct plist shuffled_plist;
static struct plist *curr_plist; /* currently used playlist */
static pthread_mutex_t plist_mut = PTHREAD_MUTEX_INITIALIZER;

/* Is the audio deice opened? */
static int audio_opened = 0;

/* Current sound parameters (which the device is opened with). */
static struct sound_params driver_sound_params;

/* Sound parameters requestet by the decoder. */
static struct sound_params req_sound_params = { 0, 0, 0 };

static struct audio_convertion sound_conv;
static int need_audio_convertion = 0;

/* Check if the two sample rates don't differ as much that we can't play. */
#define sample_rate_compat(sound, device) ((device) * 1.05 >= sound \
		&& (device) * 0.95 <= sound)

/* Move to the next file depending on set options and the user request. */
static void go_to_another_file ()
{
	int shuffle = options_get_int("Shuffle");
	
	LOCK (curr_playing_mut);
	LOCK (plist_mut);
	
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
		curr_playing = plist_find_del_fname (&shuffled_plist,
				plist_get_file(&playlist, curr_playing));
		assert (curr_playing != -1);
	}
	else if (!shuffle && curr_plist != &playlist) {
		curr_playing = plist_find_del_fname (&playlist,
				plist_get_file(&shuffled_plist, curr_playing));
		assert (curr_playing != -1);
	}
	
	if (shuffle)
		curr_plist = &shuffled_plist;
	else
		curr_plist = &playlist;
	
	if (play_prev == 1) { 
		logit ("Playing previous...");
		curr_playing = plist_prev (curr_plist, curr_playing);
		if (curr_playing == -1) {
			if (options_get_int("Repeat"))
				curr_playing = plist_last (curr_plist);
			logit ("Beginning of the list.");
		}
		else 
			logit ("Previous item.");
	}
	else if (options_get_int("AutoNext") || play_next) {
		curr_playing = plist_next (curr_plist, curr_playing);
		if (curr_playing == -1 && options_get_int("Repeat")) {
			if (shuffle) {
				plist_clear (&shuffled_plist);
				plist_cat (&shuffled_plist, &playlist);
				plist_shuffle (&shuffled_plist);
			}
			curr_playing = plist_next (curr_plist, -1);
			logit ("Going back to the first item.");
		}
		else if (curr_playing == -1)
			logit ("End of the list.");
		else
			logit ("Next item.");
	}
	else if (!options_get_int("Repeat"))
		curr_playing = -1;
	else
		debug ("Repeating file");

	UNLOCK (plist_mut);
	UNLOCK (curr_playing_mut);
}

static void *play_thread (void *unused ATTR_UNUSED)
{
	logit ("entering playing thread");

	while (curr_playing != -1) {
		char *file;

		LOCK (plist_mut);
		file = plist_get_file (curr_plist, curr_playing);
		UNLOCK (plist_mut);

		play_next = 0;
		play_prev = 0;

		if (file) {
			int next;
			char *next_file;
				
			LOCK (curr_playing_mut);
			LOCK (plist_mut);
			logit ("Playing item %d: %s", curr_playing, file);
				
			out_buf_time_set (&out_buf, 0.0);
				
			next = plist_next (curr_plist, curr_playing);
			next_file = next != -1
				? plist_get_file(curr_plist, next) : NULL;
			UNLOCK (plist_mut);
			UNLOCK (curr_playing_mut);
				
			player (file, next_file, &out_buf);
			if (next_file)
				free (next_file);

			state = STATE_STOP;
			set_info_rate (0);
			set_info_bitrate (0);
			set_info_channels (1);
			out_buf_time_set (&out_buf, 0.0);
			free (file);
		}

		if (stop_playing) {
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
		LOCK (request_mut);
		stop_playing = 1;
		UNLOCK (request_mut);
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
	LOCK (plist_mut);
	if (options_get_int("Shuffle")) {
		plist_cat (&shuffled_plist, &playlist);
		plist_shuffle (&shuffled_plist);
		plist_swap_first_fname (&shuffled_plist, fname);

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
	UNLOCK (plist_mut);
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
	LOCK (curr_playing_mut);
	LOCK (plist_mut);
	
	if (curr_playing != -1) {
		char *sname = plist_get_file (curr_plist, curr_playing);
		
		if (file_type(sname) != F_URL) {
			out_buf_pause (&out_buf);
			state = STATE_PAUSE;
			state_change ();
		}
		
		free (sname);
	}

	UNLOCK (plist_mut);
	UNLOCK (curr_playing_mut);
}

void audio_unpause ()
{
	out_buf_unpause (&out_buf);
	state = STATE_PLAY;
	state_change ();
}

static void reset_sound_params (struct sound_params *params)
{
	params->rate = 0;
	params->channels = 0;
	params->format = 0;
}

/* Return 0 on error. */
int audio_open (struct sound_params *sound_params)
{
	int res;
	int actual_rate;

	if (audio_opened && sound_params_eq(req_sound_params, *sound_params)) {
		if (audio_get_bps() < 88200) {
			logit ("Reopening device due to low bps.");
			
			/* Not closing the device would cause that much
			 * sound from the previuous file stays in the buffer
			 * and the user will see old data, so close it. */
			hw.close ();
			res = hw.open (&driver_sound_params);
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
		audio_close ();

	req_sound_params = driver_sound_params = *sound_params;

	/* Set driver_sound_params to parameters supported by the driver that
	 * are nearly the requested parameters */
	
	if (options_get_int("ForceSampleRate"))
		driver_sound_params.rate = options_get_int("ForceSampleRate");
	
	if (driver_sound_params.format > hw_caps.max.format)
		driver_sound_params.format = hw_caps.max.format;
	else if (driver_sound_params.format < hw_caps.min.format)
		driver_sound_params.format = hw_caps.min.format;
	
	if (driver_sound_params.channels > hw_caps.max.channels)
		driver_sound_params.channels = hw_caps.max.channels;
	else if (driver_sound_params.channels < hw_caps.min.channels)
		driver_sound_params.channels = hw_caps.min.channels;


	res = hw.open (&driver_sound_params);
	actual_rate = hw.get_rate ();
	if (res) {
		if (driver_sound_params.format != req_sound_params.format
				|| driver_sound_params.channels
				!= req_sound_params.channels
				|| (!sample_rate_compat(
						req_sound_params.rate,
						actual_rate))) {
			logit ("Convertion of the sound is needed.");
			driver_sound_params.rate = actual_rate;
			if (!audio_conv_new (&sound_conv, &req_sound_params,
					&driver_sound_params)) {
				hw.close ();
				reset_sound_params (&req_sound_params);
				return 0;
			}
			need_audio_convertion = 1;
		}
		else
			driver_sound_params.rate = actual_rate;
		audio_opened = 1;

		logit ("Requestet sound parameters: %dbps, %d channels, %dHz",
				req_sound_params.format * 8,
				req_sound_params.channels,
				req_sound_params.rate);
		logit ("Driver sound parameters: %dbps, %d channels, %dHz",
				driver_sound_params.format * 8,
				driver_sound_params.channels,
				driver_sound_params.rate);
	}
	
	return res;
}

int audio_send_buf (const char *buf, const size_t size)
{
	size_t out_data_len = size;
	int res;
	char *converted = NULL;
	
	if (need_audio_convertion)
		converted = audio_conv (&sound_conv, buf, size, &out_data_len);
	
	if (need_audio_convertion && converted)
		res = out_buf_put (&out_buf, converted,	out_data_len);
	else if (!need_audio_convertion)
		res = out_buf_put (&out_buf, buf, size);
	else
		res = 0;

	if (converted)
		free (converted);

	return res;
}

/* Get the current audio format bits per second value. May return 0 if the
 * audio device is closed. */
int audio_get_bps ()
{
	return driver_sound_params.rate * driver_sound_params.channels
		* driver_sound_params.format;
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
	return state != STATE_STOP ? out_buf_time_get (&out_buf) : 0;
}

void audio_close ()
{
	if (audio_opened) {
		reset_sound_params (&req_sound_params);
		hw.close ();
		if (need_audio_convertion) {
			audio_conv_destroy (&sound_conv);
			need_audio_convertion = 0;
		}
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

#ifdef HAVE_JACK
	if (!strcasecmp(driver, "jack")) {
		moc_jack_funcs (funcs);
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

static void print_output_capabilities (const struct output_driver_caps *caps)
{
	logit ("Sound driver capabilities: channels %d - %d, "
			"sample rate %d - %d, bps: %d - %d",
			caps->min.channels, caps->max.channels,
			caps->min.rate, caps->max.rate,
			caps->min.format * 8, caps->max.format * 8);
}

void audio_init ()
{
	memset (&hw, 0, sizeof(hw));
	find_hw_funcs (options_get_str("SoundDriver"), &hw);
	hw.init (&hw_caps);

	assert (hw_caps.max.channels >= hw_caps.min.channels);
	assert (hw_caps.max.rate >= hw_caps.min.rate);
	assert (hw_caps.max.format >= hw_caps.min.format);

	print_output_capabilities (&hw_caps);
	
	out_buf_init (&out_buf, options_get_int("OutputBuffer") * 1024);
	plist_init (&playlist);
	plist_init (&shuffled_plist);
	player_init ();
}

void audio_exit ()
{
	audio_stop ();
	if (hw.shutdown)
		hw.shutdown ();
	out_buf_destroy (&out_buf);
	plist_free (&playlist);
	plist_free (&shuffled_plist);
	player_cleanup ();
	if (pthread_mutex_destroy(&curr_playing_mut))
		logit ("Can't destroy curr_playing_mut: %s", strerror(errno));
	if (pthread_mutex_destroy(&plist_mut))
		logit ("Can't destroy plist_mut: %s", strerror(errno));
	if (pthread_mutex_destroy(&request_mut))
		logit ("Can't destroy request_mut: %s", strerror(errno));
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
	LOCK (plist_mut);
	plist_clear (&shuffled_plist);
	plist_add (&playlist, file);
	UNLOCK (plist_mut);
}

void audio_plist_clear ()
{
	/* We must stop before clearing the playlist, because w playing thread
	 * is accessing the playlist items. */
	audio_stop ();
	
	LOCK (plist_mut);
	plist_clear (&shuffled_plist);
	plist_clear (&playlist);
	UNLOCK (plist_mut);
}

/* Returned memory is malloc()ed. */
char *audio_get_sname ()
{
	char *sname;

	LOCK (curr_playing_mut);
	LOCK (plist_mut);
	if (curr_playing != -1)
		sname = plist_get_file (curr_plist, curr_playing);
	else
		sname = NULL;
	UNLOCK (plist_mut);
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
	int num;
	
	LOCK (plist_mut);
	num = plist_find_fname (&playlist, file);
	if (num != -1)
		plist_delete (&playlist, num);
	
	num = plist_find_fname (&shuffled_plist, file);
	if (num != -1)
		plist_delete (&shuffled_plist, num);
	UNLOCK (plist_mut);
}

/* Get the time of a file if it is on the playlist and the time is avilable. */
int audio_get_ftime (const char *file)
{
	int i;
	int time;
	time_t mtime = get_mtime (file);

	LOCK (plist_mut);
	if ((i = plist_find_fname(&playlist, file)) != -1
			&& (time = get_item_time(&playlist, i)) != -1) {
		if (playlist.items[i].mtime == mtime) {
			debug ("Found time for %s", file);
			UNLOCK (plist_mut);
			return time;
		}
		else
			logit ("mtime for %s has changed", file);
	}
	UNLOCK (plist_mut);

	return -1;
}

/* Set the time for a file on the playlist. */
void audio_plist_set_time (const char *file, const int time)
{
	int i;

	LOCK (plist_mut);
	if ((i = plist_find_fname(&playlist, file)) != -1) {
		update_item_time (&playlist.items[i], time);
		playlist.items[i].mtime = get_mtime (file);
		debug ("Setting time for %s", file);
	}
	else
		logit ("Request for updating time for a file not present on the"
				" playlist!");
	UNLOCK (plist_mut);
}

/* Notify about changing the state (unsed by the player). */
void audio_state_started_playing ()
{
	state = STATE_PLAY;
	state_change ();
}

int audio_plist_get_serial ()
{
	int serial;

	LOCK (plist_mut);
	serial = plist_get_serial (&playlist);
	UNLOCK (plist_mut);
	
	return serial;
}

void audio_plist_set_serial (const int serial)
{
	LOCK (plist_mut);
	plist_set_serial (&playlist, serial);
	UNLOCK (plist_mut);
}

struct file_tags *audio_get_curr_tags ()
{
	return player_get_curr_tags ();
}

