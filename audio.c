/*
 * MOC - music on console
 * Copyright (C) 2004-2006 Damian Pietras <daper@daper.net>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * Contributors:
 *  - Kamil Tarkowski <kamilt@interia.pl> - "previous" request
 *
 */

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <stdio.h>
#include <pthread.h>
#include <string.h>
#include <strings.h>
#include <errno.h>
#include <assert.h>

#define DEBUG

#include "common.h"
#include "server.h"
#include "decoder.h"
#include "playlist.h"
#include "log.h"
#include "lists.h"

#ifdef HAVE_OSS
# include "oss.h"
#endif
#ifdef HAVE_SNDIO
# include "sndio_out.h"
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

#include "softmixer.h"
#include "equalizer.h"

#include "out_buf.h"
#include "protocol.h"
#include "options.h"
#include "player.h"
#include "audio.h"
#include "files.h"
#include "io.h"
#include "audio_conversion.h"

static pthread_t playing_thread = 0;  /* tid of play thread */
static int play_thread_running = 0;

/* currently played file */
static int curr_playing = -1;
/* file we played before playing songs from queue */
static char *before_queue_fname = NULL;
static char *curr_playing_fname = NULL;
/* This flag is set 1 if audio_play() was called with nonempty queue,
 * so we know that when the queue is empty, we should play the regular
 * playlist from the beginning. */
static int started_playing_in_queue = 0;
static pthread_mutex_t curr_playing_mtx = PTHREAD_MUTEX_INITIALIZER;

static struct out_buf *out_buf;
static struct hw_funcs hw;
static struct output_driver_caps hw_caps; /* capabilities of the output
					     driver */

/* Player state. */
static int state = STATE_STOP;
static int prev_state = STATE_STOP;

/* requests for playing thread */
static int stop_playing = 0;
static int play_next = 0;
static int play_prev = 0;
static pthread_mutex_t request_mtx = PTHREAD_MUTEX_INITIALIZER;

/* Playlists. */
static struct plist playlist;
static struct plist shuffled_plist;
static struct plist queue;
static struct plist *curr_plist; /* currently used playlist */
static pthread_mutex_t plist_mtx = PTHREAD_MUTEX_INITIALIZER;

/* Is the audio device opened? */
static int audio_opened = 0;

/* Current sound parameters (with which the device is opened). */
static struct sound_params driver_sound_params = { 0, 0, 0 };

/* Sound parameters requested by the decoder. */
static struct sound_params req_sound_params = { 0, 0, 0 };

static struct audio_conversion sound_conv;
static int need_audio_conversion = 0;

/* URL of the last played stream. Used to fake pause/unpause of internet
 * streams. Protected by curr_playing_mtx. */
static char *last_stream_url = NULL;

static int current_mixer = 0;

/* Check if the two sample rates don't differ so much that we can't play. */
#define sample_rate_compat(sound, device) ((device) * 1.05 >= sound \
		&& (device) * 0.95 <= sound)

/* Make a human readable description of the sound sample format(s).
 * Put the description in msg which is of size buf_size.
 * Return msg. */
char *sfmt_str (const long format, char *msg, const size_t buf_size)
{
	assert (sound_format_ok(format));

	assert (buf_size > 0);
	msg[0] = 0;

	if (format & SFMT_S8)
		strncat (msg, ", 8-bit signed", buf_size - strlen(msg) - 1);
	if (format & SFMT_U8)
		strncat (msg, ", 8-bit unsigned", buf_size - strlen(msg) - 1);
	if (format & SFMT_S16)
		strncat (msg, ", 16-bit signed", buf_size - strlen(msg) - 1);
	if (format & SFMT_U16)
		strncat (msg, ", 16-bit unsigned", buf_size - strlen(msg) - 1);
	if (format & SFMT_S32)
		strncat (msg, ", 24-bit signed (as 32-bit samples)",
				buf_size - strlen(msg) - 1);
	if (format & SFMT_U32)
		strncat (msg, ", 24-bit unsigned (as 32-bit samples)",
				buf_size - strlen(msg) - 1);
	if (format & SFMT_FLOAT)
		strncat (msg, ", float",
				buf_size - strlen(msg) - 1);

	if (format & SFMT_LE)
		strncat (msg, " little-endian", buf_size - strlen(msg) - 1);
	else if (format & SFMT_BE)
		strncat (msg, " big-endian", buf_size - strlen(msg) - 1);
	if (format & SFMT_NE)
		strncat (msg, " (native)", buf_size - strlen(msg) - 1);

	/* skip first ", " */
	if (msg[0])
		memmove (msg, msg + 2, strlen(msg) + 1);

	return msg;
}

/* Return != 0 if fmt1 and fmt2 have the same sample width. */
int sfmt_same_bps (const long fmt1, const long fmt2)
{
	if (fmt1 & (SFMT_S8 | SFMT_U8)
			&& fmt2 & (SFMT_S8 | SFMT_U8))
		return 1;
	if (fmt1 & (SFMT_S16 | SFMT_U16)
			&& fmt2 & (SFMT_S16 | SFMT_U16))
		return 1;
	if (fmt1 & (SFMT_S32 | SFMT_U32)
			&& fmt2 & (SFMT_S32 | SFMT_U32))
		return 1;
	if (fmt1 & fmt2 & SFMT_FLOAT)
		return 1;

	return 0;
}

/* Return the best matching sample format for the requested format and
 * available format mask. */
static long sfmt_best_matching (const long formats_with_endian,
		const long req_with_endian)
{
	long formats = formats_with_endian & SFMT_MASK_FORMAT;
	long req = req_with_endian & SFMT_MASK_FORMAT;
	long best = 0;

	char fmt_name1[SFMT_STR_MAX] DEBUG_ONLY;
	char fmt_name2[SFMT_STR_MAX] DEBUG_ONLY;

	if (formats & req)
		best = req;
	else if (req == SFMT_S8 || req == SFMT_U8) {
		if (formats & SFMT_S8)
			best = SFMT_S8;
		else if (formats & SFMT_U8)
			best = SFMT_U8;
		else if (formats & SFMT_S16)
			best = SFMT_S16;
		else if (formats & SFMT_U16)
			best = SFMT_U16;
		else if (formats & SFMT_S32)
			best = SFMT_S32;
		else if (formats & SFMT_U32)
			best = SFMT_U32;
		else if (formats & SFMT_FLOAT)
			best = SFMT_FLOAT;
	}
	else if (req == SFMT_S16 || req == SFMT_U16) {
		if (formats & SFMT_S16)
			best = SFMT_S16;
		else if (formats & SFMT_U16)
			best = SFMT_U16;
		else if (formats & SFMT_S32)
			best = SFMT_S32;
		else if (formats & SFMT_U32)
			best = SFMT_U32;
		else if (formats & SFMT_FLOAT)
			best = SFMT_FLOAT;
		else if (formats & SFMT_S8)
			best = SFMT_S8;
		else if (formats & SFMT_U8)
			best = SFMT_U8;
	}
	else if (req == SFMT_S32 || req == SFMT_U32 || req == SFMT_FLOAT) {
		if (formats & SFMT_S32)
			best = SFMT_S32;
		else if (formats & SFMT_U32)
			best = SFMT_U32;
		else if (formats & SFMT_S16)
			best = SFMT_S16;
		else if (formats & SFMT_U16)
			best = SFMT_U16;
		else if (formats & SFMT_FLOAT)
			best = SFMT_FLOAT;
		else if (formats & SFMT_S8)
			best = SFMT_S8;
		else if (formats & SFMT_U8)
			best = SFMT_U8;
	}

	assert (best != 0);

	if (!(best & (SFMT_S8 | SFMT_U8))) {
		if ((formats_with_endian & SFMT_LE)
				&& (formats_with_endian & SFMT_BE))
			best |= SFMT_NE;
		else
			best |= formats_with_endian & SFMT_MASK_ENDIANNESS;
	}

	debug ("Chose %s as the best matching %s",
			sfmt_str(best, fmt_name1, sizeof(fmt_name1)),
			sfmt_str(req_with_endian, fmt_name2, sizeof(fmt_name2)));

	return best;
}

/* Return the number of bytes per sample for the given format. */
int sfmt_Bps (const long format)
{
	int Bps = -1;

	switch (format & SFMT_MASK_FORMAT) {
		case SFMT_S8:
		case SFMT_U8:
			Bps = 1;
			break;
		case SFMT_S16:
		case SFMT_U16:
			Bps = 2;
			break;
		case SFMT_S32:
		case SFMT_U32:
			Bps = 4;
			break;
		case SFMT_FLOAT:
			Bps = sizeof (float);
			break;
	}

	assert (Bps > 0);

	return Bps;
}

/* Move to the next file depending on the options set, the user
 * request and whether or not there are files in the queue. */
static void go_to_another_file ()
{
	bool shuffle = options_get_bool ("Shuffle");
	bool go_next = (play_next || options_get_bool("AutoNext"));
	int curr_playing_curr_pos;
	/* XXX: Shouldn't play_next be protected by mutex? */

	LOCK (curr_playing_mtx);
	LOCK (plist_mtx);

	/* If we move forward in the playlist and there are some songs in
	 * the queue, then play them. */
	if (plist_count(&queue) && go_next) {
		logit ("Playing file from queue");

		if (!before_queue_fname && curr_playing_fname)
			before_queue_fname = xstrdup (curr_playing_fname);

		curr_plist = &queue;
		curr_playing = plist_next (&queue, -1);

		server_queue_pop (queue.items[curr_playing].file);
		plist_delete (&queue, curr_playing);
	}
	else {
		/* If we just finished playing files from the queue and the
		 * appropriate option is set, continue with the file played
		 * before playing the queue. */
		if (before_queue_fname && options_get_bool ("QueueNextSongReturn")) {
			free (curr_playing_fname);
			curr_playing_fname = before_queue_fname;
			before_queue_fname = NULL;
		}

		if (shuffle) {
			curr_plist = &shuffled_plist;

			if (plist_count(&playlist)
					&& !plist_count(&shuffled_plist)) {
				plist_cat (&shuffled_plist, &playlist);
				plist_shuffle (&shuffled_plist);

				if (curr_playing_fname)
					plist_swap_first_fname (&shuffled_plist,
							curr_playing_fname);
			}
		}
		else
			curr_plist = &playlist;

		curr_playing_curr_pos = plist_find_fname (curr_plist,
				curr_playing_fname);

		/* If we came from the queue and the last file in
		 * queue wasn't in the playlist, we try to revert to
		 * the QueueNextSongReturn == true behaviour. */
		if (curr_playing_curr_pos == -1 && before_queue_fname) {
			curr_playing_curr_pos = plist_find_fname (curr_plist,
					before_queue_fname);
		}

		if (play_prev && plist_count(curr_plist)) {
			logit ("Playing previous...");

			if (curr_playing_curr_pos == -1
					|| started_playing_in_queue) {
				curr_playing = plist_prev (curr_plist, -1);
				started_playing_in_queue = 0;
			}
			else
				curr_playing = plist_prev (curr_plist,
						curr_playing_curr_pos);

			if (curr_playing == -1) {
				if (options_get_bool("Repeat"))
					curr_playing = plist_last (curr_plist);
				logit ("Beginning of the list.");
			}
			else
				logit ("Previous item.");
		}
		else if (go_next && plist_count(curr_plist)) {
			logit ("Playing next...");

			if (curr_playing_curr_pos == -1
					|| started_playing_in_queue) {
				curr_playing = plist_next (curr_plist, -1);
				started_playing_in_queue = 0;
			}
			else
				curr_playing = plist_next (curr_plist,
						curr_playing_curr_pos);

			if (curr_playing == -1 && options_get_bool("Repeat")) {
				if (shuffle) {
					plist_clear (&shuffled_plist);
					plist_cat (&shuffled_plist, &playlist);
					plist_shuffle (&shuffled_plist);
				}
				curr_playing = plist_next (curr_plist, -1);
				logit ("Going back to the first item.");
			}
			else if (curr_playing == -1)
				logit ("End of the list");
			else
				logit ("Next item");

		}
		else if (!options_get_bool("Repeat")) {
			curr_playing = -1;
		}
		else
			debug ("Repeating file");

		if (before_queue_fname)
			free (before_queue_fname);
		before_queue_fname = NULL;
	}

	UNLOCK (plist_mtx);
	UNLOCK (curr_playing_mtx);
}

static void *play_thread (void *unused ATTR_UNUSED)
{
	logit ("Entering playing thread");

	while (curr_playing != -1) {
		char *file;

		LOCK (plist_mtx);
		file = plist_get_file (curr_plist, curr_playing);
		UNLOCK (plist_mtx);

		play_next = 0;
		play_prev = 0;

		if (file) {
			int next;
			char *next_file;

			LOCK (curr_playing_mtx);
			LOCK (plist_mtx);
			logit ("Playing item %d: %s", curr_playing, file);

			if (curr_playing_fname)
				free (curr_playing_fname);
			curr_playing_fname = xstrdup (file);

			out_buf_time_set (out_buf, 0.0);

			next = plist_next (curr_plist, curr_playing);
			next_file = next != -1 ? plist_get_file (curr_plist, next) : NULL;
			UNLOCK (plist_mtx);
			UNLOCK (curr_playing_mtx);

			player (file, next_file, out_buf);
			if (next_file)
				free (next_file);

			set_info_rate (0);
			set_info_bitrate (0);
			set_info_channels (1);
			out_buf_time_set (out_buf, 0.0);
			free (file);
		}

		LOCK (curr_playing_mtx);
		if (last_stream_url) {
			free (last_stream_url);
			last_stream_url = NULL;
		}
		UNLOCK (curr_playing_mtx);

		if (stop_playing) {
			LOCK (curr_playing_mtx);
			curr_playing = -1;
			UNLOCK (curr_playing_mtx);
			logit ("stopped");
		}
		else
			go_to_another_file ();
	}

	prev_state = state;
	state = STATE_STOP;
	state_change ();

	if (curr_playing_fname) {
		free (curr_playing_fname);
		curr_playing_fname = NULL;
	}

	audio_close ();
	logit ("Exiting");

	return NULL;
}

void audio_reset ()
{
	if (hw.reset)
		hw.reset ();
}

void audio_stop ()
{
	int rc;

	if (play_thread_running) {
		logit ("audio_stop()");
		LOCK (request_mtx);
		stop_playing = 1;
		UNLOCK (request_mtx);
		player_stop ();
		logit ("pthread_join (playing_thread, NULL)");
		rc = pthread_join (playing_thread, NULL);
		if (rc != 0)
			log_errno ("pthread_join() failed", rc);
		playing_thread = 0;
		play_thread_running = 0;
		stop_playing = 0;
		logit ("done stopping");
	}
	else if (state == STATE_PAUSE) {

		/* Paused internet stream - we are in fact stopped already. */
		if (curr_playing_fname) {
			free (curr_playing_fname);
			curr_playing_fname = NULL;
		}

		prev_state = state;
		state = STATE_STOP;
		state_change ();
	}
}

/* Start playing from the file fname. If fname is an empty string,
 * start playing from the first file on the list. */
void audio_play (const char *fname)
{
	int rc;

	audio_stop ();
	player_reset ();

	LOCK (curr_playing_mtx);
	LOCK (plist_mtx);

	/* If we have songs in the queue and fname is empty string, start
	 * playing file from the queue. */
	if (plist_count(&queue) && !(*fname)) {
		curr_plist = &queue;
		curr_playing = plist_next (&queue, -1);

		/* remove the file from queue */
		server_queue_pop (queue.items[curr_playing].file);
		plist_delete (curr_plist, curr_playing);

		started_playing_in_queue = 1;
	}
	else if (options_get_bool("Shuffle")) {
		plist_clear (&shuffled_plist);
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

	rc = pthread_create (&playing_thread, NULL, play_thread, NULL);
	if (rc != 0)
		error_errno ("Can't create thread", rc);
	play_thread_running = 1;

	UNLOCK (plist_mtx);
	UNLOCK (curr_playing_mtx);
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
	LOCK (curr_playing_mtx);
	LOCK (plist_mtx);

	if (curr_playing != -1) {
		char *sname = plist_get_file (curr_plist, curr_playing);

		if (file_type(sname) == F_URL) {
			UNLOCK (curr_playing_mtx);
			UNLOCK (plist_mtx);
			audio_stop ();
			LOCK (curr_playing_mtx);
			LOCK (plist_mtx);

			if (last_stream_url)
				free (last_stream_url);
			last_stream_url = xstrdup (sname);

			/* Pretend that we are paused on this. */
			curr_playing_fname = xstrdup (sname);
		}
		else
			out_buf_pause (out_buf);

		prev_state = state;
		state = STATE_PAUSE;
		state_change ();

		free (sname);
	}

	UNLOCK (plist_mtx);
	UNLOCK (curr_playing_mtx);
}

void audio_unpause ()
{
	LOCK (curr_playing_mtx);
	if (last_stream_url && file_type(last_stream_url) == F_URL) {
		char *url = xstrdup (last_stream_url);

		UNLOCK (curr_playing_mtx);
		audio_play (url);
		free (url);
	}
	else if (curr_playing != -1) {
		out_buf_unpause (out_buf);
		prev_state = state;
		state = STATE_PLAY;
		UNLOCK (curr_playing_mtx);
		state_change ();
	}
	else
		UNLOCK (curr_playing_mtx);
}

static void reset_sound_params (struct sound_params *params)
{
	params->rate = 0;
	params->channels = 0;
	params->fmt = 0;
}

/* Return 0 on error. If sound params == NULL, open the device using
 * the previous parameters. */
int audio_open (struct sound_params *sound_params)
{
	int res;
	static struct sound_params last_params = { 0, 0, 0 };

	if (!sound_params)
		sound_params = &last_params;
	else
		last_params = *sound_params;

	assert (sound_format_ok(sound_params->fmt));

	if (audio_opened) {
		if (sound_params_eq(req_sound_params, *sound_params)) {
			if (audio_get_bps() >= 88200) {
				logit ("Audio device already opened with such parameters.");
				return 1;
			}

			/* Not closing the device would cause that much
			 * sound from the previous file to stay in the buffer
			 * and the user will hear old data, so close it. */
			logit ("Reopening device due to low bps.");
		}

		audio_close ();
	}

	req_sound_params = *sound_params;

	/* Set driver_sound_params to parameters supported by the driver that
	 * are nearly the requested parameters. */

	if (options_get_int("ForceSampleRate")) {
		driver_sound_params.rate = options_get_int("ForceSampleRate");
		logit ("Setting forced driver sample rate to %dHz",
				driver_sound_params.rate);
	}
	else
		driver_sound_params.rate = req_sound_params.rate;

	driver_sound_params.fmt = sfmt_best_matching (hw_caps.formats,
			req_sound_params.fmt);

	/* number of channels */
	driver_sound_params.channels = CLAMP(hw_caps.min_channels,
	                                     req_sound_params.channels,
	                                     hw_caps.max_channels);

	res = hw.open (&driver_sound_params);

	if (res) {
		char fmt_name[SFMT_STR_MAX] LOGIT_ONLY;

		driver_sound_params.rate = hw.get_rate ();
		if (driver_sound_params.fmt != req_sound_params.fmt
				|| driver_sound_params.channels
				!= req_sound_params.channels
				|| (!sample_rate_compat(
						req_sound_params.rate,
						driver_sound_params.rate))) {
			logit ("Conversion of the sound is needed.");
			if (!audio_conv_new (&sound_conv, &req_sound_params,
					&driver_sound_params)) {
				hw.close ();
				reset_sound_params (&req_sound_params);
				return 0;
			}
			need_audio_conversion = 1;
		}
		audio_opened = 1;

		logit ("Requested sound parameters: %s, %d channels, %dHz",
				sfmt_str(req_sound_params.fmt, fmt_name, sizeof(fmt_name)),
				req_sound_params.channels,
				req_sound_params.rate);
		logit ("Driver sound parameters: %s, %d channels, %dHz",
				sfmt_str(driver_sound_params.fmt, fmt_name, sizeof(fmt_name)),
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

	if (need_audio_conversion)
		converted = audio_conv (&sound_conv, buf, size, &out_data_len);

	if (need_audio_conversion && converted)
		res = out_buf_put (out_buf, converted, out_data_len);
	else if (!need_audio_conversion)
		res = out_buf_put (out_buf, buf, size);
	else
		res = 0;

	if (converted)
		free (converted);

	return res;
}

/* Get the current audio format bytes per frame value.
 * May return 0 if the audio device is closed. */
int audio_get_bpf ()
{
	return driver_sound_params.channels
		* (driver_sound_params.fmt ? sfmt_Bps(driver_sound_params.fmt)
				: 0);
}

/* Get the current audio format bytes per second value.
 * May return 0 if the audio device is closed. */
int audio_get_bps ()
{
	return driver_sound_params.rate * audio_get_bpf ();
}

int audio_get_buf_fill ()
{
	return hw.get_buff_fill ();
}

int audio_send_pcm (const char *buf, const size_t size)
{
	char *softmixed = NULL;
	char *equalized = NULL;

	if (equalizer_is_active ())
	{
		equalized = xmalloc (size);
		memcpy (equalized, buf, size);

		equalizer_process_buffer (equalized, size, &driver_sound_params);

		buf = equalized;
	}

	if (softmixer_is_active () || softmixer_is_mono ())
	{
		if (equalized)
		{
			softmixed = equalized;
		}
		else
		{
			softmixed = xmalloc (size);
			memcpy (softmixed, buf, size);
		}

		softmixer_process_buffer (softmixed, size, &driver_sound_params);

		buf = softmixed;
	}

	int played;

	played = hw.play (buf, size);

	if (played < 0)
		fatal ("Audio output error!");

	if (softmixed && !equalized)
		free (softmixed);

	if (equalized)
		free (equalized);

	return played;
}

/* Get current time of the song in seconds. */
int audio_get_time ()
{
	return state != STATE_STOP ? out_buf_time_get (out_buf) : 0;
}

void audio_close ()
{
	if (audio_opened) {
		reset_sound_params (&req_sound_params);
		reset_sound_params (&driver_sound_params);
		hw.close ();
		if (need_audio_conversion) {
			audio_conv_destroy (&sound_conv);
			need_audio_conversion = 0;
		}
		audio_opened = 0;
	}
}

/* Try to initialize drivers from the list and fill funcs with
 * those of the first working driver. */
static void find_working_driver (lists_t_strs *drivers, struct hw_funcs *funcs)
{
	int ix;

	memset (funcs, 0, sizeof(*funcs));

	for (ix = 0; ix < lists_strs_size (drivers); ix += 1) {
		const char *name;

		name = lists_strs_at (drivers, ix);

#ifdef HAVE_SNDIO
		if (!strcasecmp(name, "sndio")) {
			sndio_funcs (funcs);
			printf ("Trying SNDIO...\n");
			if (funcs->init(&hw_caps))
				return;
		}
#endif

#ifdef HAVE_OSS
		if (!strcasecmp(name, "oss")) {
			oss_funcs (funcs);
			printf ("Trying OSS...\n");
			if (funcs->init(&hw_caps))
				return;
		}
#endif

#ifdef HAVE_ALSA
		if (!strcasecmp(name, "alsa")) {
			alsa_funcs (funcs);
			printf ("Trying ALSA...\n");
			if (funcs->init(&hw_caps))
				return;
		}
#endif

#ifdef HAVE_JACK
		if (!strcasecmp(name, "jack")) {
			moc_jack_funcs (funcs);
			printf ("Trying JACK...\n");
			if (funcs->init(&hw_caps))
				return;
		}
#endif

#ifndef NDEBUG
		if (!strcasecmp(name, "null")) {
			null_funcs (funcs);
			printf ("Trying NULL...\n");
			if (funcs->init(&hw_caps))
				return;
		}
#endif
	}

	fatal ("No valid sound driver!");
}

static void print_output_capabilities
            (const struct output_driver_caps *caps LOGIT_ONLY)
{
	char fmt_name[SFMT_STR_MAX] LOGIT_ONLY;

	logit ("Sound driver capabilities: channels %d - %d, formats: %s",
			caps->min_channels, caps->max_channels,
			sfmt_str(caps->formats, fmt_name, sizeof(fmt_name)));
}

void audio_initialize ()
{
	find_working_driver (options_get_list ("SoundDriver"), &hw);

	if (hw_caps.max_channels < hw_caps.min_channels)
		fatal ("Error initializing audio device: "
		       "device reports incorrect number of channels.");
	if (!sound_format_ok (hw_caps.formats))
		fatal ("Error initializing audio device: "
		       "device reports no usable formats.");

	print_output_capabilities (&hw_caps);
	if (!options_get_bool ("Allow24bitOutput")
			&& hw_caps.formats & (SFMT_S32 | SFMT_U32)) {
		logit ("Disabling 24bit modes because Allow24bitOutput is set to no.");
		hw_caps.formats &= ~(SFMT_S32 | SFMT_U32);
		if (!sound_format_ok (hw_caps.formats))
			fatal ("No available sound formats after disabling 24bit modes. "
			       "Consider setting Allow24bitOutput to yes.");
	}

	out_buf = out_buf_new (options_get_int("OutputBuffer") * 1024);

	softmixer_init();
	equalizer_init();

	plist_init (&playlist);
	plist_init (&shuffled_plist);
	plist_init (&queue);
	player_init ();
}

void audio_exit ()
{
	int rc;

	audio_stop ();
	if (hw.shutdown)
		hw.shutdown ();
	out_buf_free (out_buf);
	out_buf = NULL;
	plist_free (&playlist);
	plist_free (&shuffled_plist);
	plist_free (&queue);
	player_cleanup ();
	rc = pthread_mutex_destroy (&curr_playing_mtx);
	if (rc != 0)
		log_errno ("Can't destroy curr_playing_mtx", rc);
	rc = pthread_mutex_destroy (&plist_mtx);
	if (rc != 0)
		log_errno ("Can't destroy plist_mtx", rc);
	rc = pthread_mutex_destroy (&request_mtx);
	if (rc != 0)
		log_errno ("Can't destroy request_mtx", rc);

	if (last_stream_url)
		free (last_stream_url);

	softmixer_shutdown();
	equalizer_shutdown();
}

void audio_seek (const int sec)
{
	int playing;

	LOCK (curr_playing_mtx);
	playing = curr_playing;
	UNLOCK (curr_playing_mtx);

	if (playing != -1 && state == STATE_PLAY)
		player_seek (sec);
	else
		logit ("Seeking when nothing is played.");
}

void audio_jump_to (const int sec)
{
	int playing;

	LOCK (curr_playing_mtx);
	playing = curr_playing;
	UNLOCK (curr_playing_mtx);

	if (playing != -1 && state == STATE_PLAY)
		player_jump_to (sec);
	else
		logit ("Jumping when nothing is played.");
}

int audio_get_state ()
{
	return state;
}

int audio_get_prev_state ()
{
	return prev_state;
}

void audio_plist_add (const char *file)
{
	LOCK (plist_mtx);
	plist_clear (&shuffled_plist);
	if (plist_find_fname(&playlist, file) == -1)
		plist_add (&playlist, file);
	else
		logit ("Wanted to add a file already present: %s", file);
	UNLOCK (plist_mtx);
}

void audio_queue_add (const char *file)
{
	LOCK (plist_mtx);
	if (plist_find_fname(&queue, file) == -1)
		plist_add (&queue, file);
	else
		logit ("Wanted to add a file already present: %s", file);
	UNLOCK (plist_mtx);
}

void audio_plist_clear ()
{
	LOCK (plist_mtx);
	plist_clear (&shuffled_plist);
	plist_clear (&playlist);
	UNLOCK (plist_mtx);
}

void audio_queue_clear ()
{
	LOCK (plist_mtx);
	plist_clear (&queue);
	UNLOCK (plist_mtx);
}

/* Returned memory is malloc()ed. */
char *audio_get_sname ()
{
	char *sname;

	LOCK (curr_playing_mtx);
	sname = xstrdup (curr_playing_fname);
	UNLOCK (curr_playing_mtx);

	return sname;
}

int audio_get_mixer ()
{
	if (current_mixer == 2)
		return softmixer_get_value ();

	return hw.read_mixer ();
}

void audio_set_mixer (const int val)
{
	if (!RANGE(0, val, 100)) {
		logit ("Tried to set mixer to volume out of range.");
		return;
	}

	if (current_mixer == 2)
		softmixer_set_value (val);
	else
		hw.set_mixer (val);
}

void audio_plist_delete (const char *file)
{
	int num;

	LOCK (plist_mtx);
	num = plist_find_fname (&playlist, file);
	if (num != -1)
		plist_delete (&playlist, num);

	num = plist_find_fname (&shuffled_plist, file);
	if (num != -1)
		plist_delete (&shuffled_plist, num);
	UNLOCK (plist_mtx);
}

void audio_queue_delete (const char *file)
{
	int num;

	LOCK (plist_mtx);
	num = plist_find_fname (&queue, file);
	if (num != -1)
		plist_delete (&queue, num);
	UNLOCK (plist_mtx);
}

/* Get the time of a file if the file is on the playlist and
 * the time is available. */
int audio_get_ftime (const char *file)
{
	int i;
	int time;
	time_t mtime;

	mtime = get_mtime (file);

	LOCK (plist_mtx);
	i = plist_find_fname (&playlist, file);
	if (i != -1) {
		time = get_item_time (&playlist, i);
		if (time != -1) {
			if (playlist.items[i].mtime == mtime) {
				debug ("Found time for %s", file);
				UNLOCK (plist_mtx);
				return time;
			}
			logit ("mtime for %s has changed", file);
		}
	}
	UNLOCK (plist_mtx);

	return -1;
}

/* Set the time for a file on the playlist. */
void audio_plist_set_time (const char *file, const int time)
{
	int i;

	LOCK (plist_mtx);
	if ((i = plist_find_fname(&playlist, file)) != -1) {
		plist_set_item_time (&playlist, i, time);
		playlist.items[i].mtime = get_mtime (file);
		debug ("Setting time for %s", file);
	}
	else
		logit ("Request for updating time for a file not present on the"
				" playlist!");
	UNLOCK (plist_mtx);
}

/* Notify that the state was changed (used by the player). */
void audio_state_started_playing ()
{
	prev_state = state;
	state = STATE_PLAY;
	state_change ();
}

int audio_plist_get_serial ()
{
	int serial;

	LOCK (plist_mtx);
	serial = plist_get_serial (&playlist);
	UNLOCK (plist_mtx);

	return serial;
}

void audio_plist_set_serial (const int serial)
{
	LOCK (plist_mtx);
	plist_set_serial (&playlist, serial);
	UNLOCK (plist_mtx);
}

/* Swap 2 files on the playlist. */
void audio_plist_move (const char *file1, const char *file2)
{
	LOCK (plist_mtx);
	plist_swap_files (&playlist, file1, file2);
	UNLOCK (plist_mtx);
}

void audio_queue_move (const char *file1, const char *file2)
{
	LOCK (plist_mtx);
	plist_swap_files (&queue, file1, file2);
	UNLOCK (plist_mtx);
}

/* Return a copy of the song queue.  We cannot just return constant
 * pointer, because it will be used in a different thread.
 * It obviously needs to be freed after use. */
struct plist* audio_queue_get_contents ()
{
	struct plist *ret = (struct plist *)xmalloc (sizeof(struct plist));
	plist_init (ret);

	LOCK (plist_mtx);
	plist_cat (ret, &queue);
	UNLOCK (plist_mtx);

	return ret;
}

struct file_tags *audio_get_curr_tags ()
{
	return player_get_curr_tags ();
}

char *audio_get_mixer_channel_name ()
{
	if (current_mixer == 2)
		return softmixer_name ();

	return hw.get_mixer_channel_name ();
}

void audio_toggle_mixer_channel ()
{
	current_mixer = (current_mixer + 1) % 3;
	if (current_mixer < 2)
		hw.toggle_mixer_channel ();
}
