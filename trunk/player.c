/*
 * MOC - music on console
 * Copyright (C) 2004-2005 Damian Pietras <daper@daper.net>
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

#include <pthread.h>
#include <string.h>
#include <errno.h>
#include <assert.h>

#define DEBUG

#include "main.h"
#include "log.h"
#include "decoder.h"
#include "audio.h"
#include "out_buf.h"
#include "server.h"
#include "options.h"
#include "player.h"
#include "files.h"
#include "playlist.h"

#define PCM_BUF_SIZE	(32 * 1024)

enum request
{
	REQ_NOTHING,
	REQ_SEEK,
	REQ_STOP
};

struct precache
{
	char *file; /* the file to precache */
	char buf[2 * PCM_BUF_SIZE]; /* PCM buffer with precached data */
	int buf_fill;
	int ok; /* 1 if precache succeed */
	struct sound_params sound_params; /* of the sound in the buffer */
	struct decoder *f; /* decoder functions for precached file */
	void *decoder_data;
	int running; /* if the precache thread is running */
	pthread_t tid; /* tid of the precache thread */
};

struct precache precache;

/* request conditional and mutex */
static pthread_cond_t request_cond = PTHREAD_COND_INITIALIZER;
static pthread_mutex_t request_cond_mutex = PTHREAD_MUTEX_INITIALIZER;

static enum request request = REQ_NOTHING;
static int req_seek;

/* Tags of the currentply played file. */
static struct file_tags *curr_tags = NULL;
static pthread_mutex_t curr_tags_mut = PTHREAD_MUTEX_INITIALIZER;

/* Stream associated with the currently playing decoder. */
static struct io_stream *decoder_stream = NULL;
static pthread_mutex_t decoder_stream_mut = PTHREAD_MUTEX_INITIALIZER;

static void update_time ()
{
	static int last_time = 0;
	int ctime = audio_get_time ();

	if (ctime != last_time) {
		last_time = ctime;
		ctime_change ();
	}
}

static void *precache_thread (void *data)
{
	struct precache *precache = (struct precache *)data;
	int decoded;
	struct sound_params new_sound_params;

	precache->buf_fill = 0;
	precache->sound_params.channels = 0; /* mark that sound_params were not
						yet filled. */
	precache->f = get_decoder (precache->file);
	assert (precache->f != NULL);

	if (!(precache->decoder_data = precache->f->open(precache->file))) {
		logit ("Failed to open the file for precache.");
		return NULL;
	}

	audio_plist_set_time (precache->file,
			precache->f->get_duration(precache->decoder_data));

	/* Stop at PCM_BUF_SIZE, because when we decode too much, there is no
	 * place when we can put the data that don't fit into the buffer. */
	while (precache->buf_fill < PCM_BUF_SIZE) {
		struct decoder_error err;
		
		decoded = precache->f->decode (precache->decoder_data,
				precache->buf + precache->buf_fill,
				PCM_BUF_SIZE, &new_sound_params);

		if (!decoded) {
			
			/* EOF so fast? we can't pass this information
			 * in precache, so give up */
			logit ("EOF when precaching.");
			precache->f->close (precache->decoder_data);
			return NULL;
		}

		precache->f->get_error (precache->decoder_data, &err);
		
		if (err.type == ERROR_FATAL) {
			precache->f->close (precache->decoder_data);
			return NULL;
		}
		
		if (!precache->sound_params.channels)
			precache->sound_params = new_sound_params;
		else if (!sound_params_eq(precache->sound_params,
					new_sound_params)) {

			/* there is no way to store sound with two different
			 * parameters in the buffer, give up with
			 * precacheing. (this should never happen) */
			logit ("Sound parameters has changed when precaching.");
			precache->f->close (precache->decoder_data);
			return NULL;
		}

		precache->buf_fill += decoded;

		if (err.type != ERROR_OK)
			break; /* Don't lose the error message */
	}
	
	precache->ok = 1;
	logit ("Successfully precached file (%d bytes)", precache->buf_fill);
	return NULL;
}

static void start_precache (struct precache *precache, const char *file)
{
	assert (!precache->running);
	assert (file != NULL);

	precache->file = xstrdup (file);
	logit ("Precaching file %s", file);
	precache->ok = 0;
	if (pthread_create(&precache->tid, NULL, precache_thread, precache))
		logit ("Could not run precache thread");
	else
		precache->running = 1;
}

static void precache_wait (struct precache *precache)
{
	if (precache->running) {
		debug ("Waiting for precache thread...");
		if (pthread_join(precache->tid, NULL))
			fatal ("pthread_join() for precache thread failed");
		precache->running = 0;
		debug ("done");
	}
	else
		debug ("Precache thread is not running");
}

static void precache_reset (struct precache *precache)
{
	assert (!precache->running);
	precache->ok = 0;
	if (precache->file) {
		free (precache->file);
		precache->file = NULL;
	}
}

void player_init ()
{
	precache.file = NULL;
	precache.running = 0;
	precache.ok =  0;
}

static void show_tags (const struct file_tags *tags)
{
	
	debug ("TAG[title]: %s", tags->title ? tags->title : "N/A");
	debug ("TAG[album]: %s", tags->album ? tags->album : "N/A");
	debug ("TAG[artist]: %s", tags->artist ? tags->artist : "N/A");
	debug ("TAG[track]: %d", tags->track);
}

/* Decoder loop for already opened and probably running for some time decoder.
 * next_file will be precached at eof. */
static void decode_loop (const struct decoder *f, void *decoder_data,
		const char *next_file, struct out_buf *out_buf,
		struct sound_params sound_params)
{
	int eof = 0;
	char buf[PCM_BUF_SIZE];
	int decoded = 0;
	struct sound_params new_sound_params;
	int bitrate = -1;
	int sound_params_change = 0;

	if (f->current_tags) {
		LOCK (curr_tags_mut);
		curr_tags = tags_new ();
		UNLOCK (curr_tags_mut);
	}
	else
		logit ("No current_tags() function");

	if (f->get_stream) {
		LOCK (decoder_stream_mut);
		decoder_stream = f->get_stream (decoder_data);
		UNLOCK (decoder_stream_mut);
	}
	else
		logit ("No get_stream() function");

	status_msg ("Playing...");

	while (1) {
		debug ("loop...");
		
		LOCK (request_cond_mutex);
		if (!eof && !decoded) {
			struct decoder_error err;
			
			UNLOCK (request_cond_mutex);
			decoded = f->decode (decoder_data, buf, sizeof(buf),
					&new_sound_params);
			
			f->get_error (decoder_data, &err);
			if (err.type != ERROR_OK) {
				if (err.type != ERROR_STREAM
						|| options_get_int(
							"ShowStreamErrors"))
					error ("%s", err.err);
				decoder_error_clear (&err);
			}
			
			if (!decoded) {
				eof = 1;
				logit ("EOF from decoder");
			}
			else {
				int new_bitrate;
				
				debug ("decoded %d bytes", decoded);
				if (!sound_params_eq(new_sound_params,
							sound_params))
					sound_params_change = 1;

				new_bitrate = f->get_bitrate (decoder_data);
				if (bitrate != new_bitrate) {
					bitrate = new_bitrate;
					set_info_bitrate (bitrate);
				}

				LOCK (curr_tags_mut);
				if (f->current_tags && f->current_tags(
							decoder_data,
							curr_tags)) {
					tags_change ();
					debug ("Tags change");
					show_tags (curr_tags);
				}
				UNLOCK (curr_tags_mut);
			}
		}

		/* Wait, if there is no space in the buffer to put the decoded
		 * data or EOF occured and there is something in the buffer. */
		else if (decoded > out_buf_get_free(out_buf)
					|| (eof && out_buf_get_fill(out_buf))) {
			debug ("waiting...");
			if (eof && !precache.file && next_file
					&& file_type(next_file) == F_SOUND
					&& options_get_int("Precache"))
				start_precache (&precache, next_file);
			pthread_cond_wait (&request_cond, &request_cond_mutex);
			UNLOCK (request_cond_mutex);
		}
		else
			UNLOCK (request_cond_mutex);

		update_time ();

		/* When clearing request, we must make sure, that another
		 * request will not arrive at the moment, so we check if
		 * the request has changed. */
		if (request == REQ_STOP) {
			logit ("stop");
			out_buf_stop (out_buf);
			
			LOCK (request_cond_mutex);
			if (request == REQ_STOP)
				request = REQ_NOTHING;
			UNLOCK (request_cond_mutex);
			
			break;
		}
		else if (request == REQ_SEEK) {
			int decoder_seek;
			
			logit ("seeking");
			if ((decoder_seek = f->seek(decoder_data, req_seek))
					== -1)
				logit ("error when seeking");
			else {
				out_buf_stop (out_buf);
				out_buf_reset (out_buf);
				out_buf_time_set (out_buf, decoder_seek);
				eof = 0;
				decoded = 0;
			}

			LOCK (request_cond_mutex);
			if (request == REQ_SEEK)
				request = REQ_NOTHING;
			UNLOCK (request_cond_mutex);

		}
		else if (!eof && decoded <= out_buf_get_free(out_buf)
				&& !sound_params_change) {
			debug ("putting into the buffer %d bytes", decoded);
			audio_send_buf (buf, decoded);
			decoded = 0;
		}
		else if (!eof && sound_params_change
				&& out_buf_get_fill(out_buf) == 0) {
			logit ("sound parameters has changed.");
			sound_params = new_sound_params;
			sound_params_change = 0;
			set_info_channels (sound_params.channels);
			set_info_rate (sound_params.rate / 1000);
			out_buf_wait (out_buf);
			if (!audio_open(&sound_params))
				break;
		}
		else if (eof && out_buf_get_fill(out_buf) == 0) {
			logit ("played everything");
			break;
		}
	}

	status_msg ("");

	LOCK (decoder_stream_mut);
	decoder_stream = NULL;
	f->close (decoder_data);
	UNLOCK (decoder_stream_mut);

	LOCK (curr_tags_mut);
	if (curr_tags) {
		tags_free (curr_tags);
		curr_tags = NULL;
	}
	UNLOCK (curr_tags_mut);

	out_buf_wait (out_buf);
}

/* Play a file (disk file) using the given decoder. next_file is precached. */
static void play_file (const char *file, const struct decoder *f,
		const char *next_file, struct out_buf *out_buf)
{
	void *decoder_data;
	struct sound_params sound_params = { 0, 0, 0 };
	
	out_buf_set_notify_cond (out_buf, &request_cond, &request_cond_mutex);
	out_buf_reset (out_buf);
	
	precache_wait (&precache);

	if (precache.ok && strcmp(precache.file, file)) {
		logit ("The precached file is not the file we want.");
		precache.f->close (precache.decoder_data);
	}

	if (precache.ok && !strcmp(precache.file, file)) {
		struct decoder_error err;

		logit ("Using precached file");

		assert (f == precache.f);
		
		sound_params = precache.sound_params;
		decoder_data = precache.decoder_data;
		set_info_channels (sound_params.channels);
		set_info_rate (sound_params.rate / 1000);
		if (!audio_open(&sound_params))
			return;
		out_buf_put (out_buf, precache.buf, precache.buf_fill);

		precache.f->get_error (precache.decoder_data, &err);
		if (err.type != ERROR_OK) {
			if (err.type != ERROR_STREAM
					|| options_get_int(
						"ShowStreamErrors"))
				error ("%s", err.err);
			decoder_error_clear (&err);
		}
	}
	else {
		struct decoder_error err;

		status_msg ("Opening...");
		decoder_data = f->open(file);
		f->get_error (decoder_data, &err);
		if (err.type != ERROR_OK) {
			f->close (decoder_data);
			error ("%s", err.err);
			decoder_error_clear (&err);
			logit ("Can't open file, exiting");
			return;
		}
	}

	audio_plist_set_time (file, f->get_duration(decoder_data));
	audio_state_started_playing ();
	precache_reset (&precache);

	decode_loop (f, decoder_data, next_file, out_buf, sound_params);

	out_buf_set_notify_cond (out_buf, NULL, NULL);
}

/* Play the stream (global decoder_stream) using the given decoder. */
static void play_stream (const struct decoder *f, struct out_buf *out_buf)
{
	void *decoder_data;
	struct sound_params sound_params = { 0, 0, 0 };
	struct decoder_error err;

	out_buf_set_notify_cond (out_buf, &request_cond, &request_cond_mutex);
	out_buf_reset (out_buf);
	
	assert (f->open_stream != NULL);

	decoder_data = f->open_stream (decoder_stream);
	f->get_error (decoder_data, &err);
	if (err.type != ERROR_OK) {
		LOCK (decoder_stream_mut);
		decoder_stream = NULL;
		UNLOCK (decoder_stream_mut);

		f->close (decoder_data);
		error ("%s", err.err);
		decoder_error_clear (&err);
		logit ("Can't open file");
	}
	else {
		audio_state_started_playing ();
		decode_loop (f, decoder_data, NULL, out_buf, sound_params);
		out_buf_set_notify_cond (out_buf, NULL, NULL);
	}
}

/* Open a file, decode it and put output into the buffer. At the end, start
 * precaching next_file. */
void player (const char *file, const char *next_file, struct out_buf *out_buf)
{
	struct decoder *f;

	if (file_type(file) == F_URL) {
		status_msg ("Connecting...");
		
		LOCK (decoder_stream_mut);
		decoder_stream = io_open (file, 1);
		if (!io_ok(decoder_stream)) {
			error ("Could not open URL: %s",
					io_strerror(decoder_stream));
			io_close (decoder_stream);
			decoder_stream = NULL;
			UNLOCK (decoder_stream_mut);
			return;
		}
		UNLOCK (decoder_stream_mut);

		status_msg ("Prebuffering...");
		f = get_decoder_by_content (decoder_stream);
		if (!f) {
			if (request != REQ_STOP)
				error ("Format not supported");
			LOCK (decoder_stream_mut);
			io_close (decoder_stream);
			status_msg ("");
			decoder_stream = NULL;
			UNLOCK (decoder_stream_mut);
			return;
		}

		play_stream (f, out_buf);
	}
	else {
		f = get_decoder (file);
		LOCK (decoder_stream_mut);
		decoder_stream = NULL;
		UNLOCK (decoder_stream_mut);

		if (!f) {
			error ("Can't get decoder for %s", file);
			return;
		}

		play_file (file, f, next_file, out_buf);
	}

	logit ("exiting");
}

void player_cleanup ()
{
	if (pthread_mutex_destroy(&request_cond_mutex))
		logit ("Can't destroy request mutex: %s", strerror(errno));
	if (pthread_mutex_destroy(&curr_tags_mut))
		logit ("Can't destroy tags mutex: %s", strerror(errno));
	if (pthread_mutex_destroy(&decoder_stream_mut))
		logit ("Can't destroy decoder_stream mutex: %s", strerror(errno));
	if (pthread_cond_destroy(&request_cond))
		logit ("Can't destroy request condition: %s", strerror(errno));

	precache_wait (&precache);
	precache_reset (&precache);
}

void player_reset ()
{
	request = REQ_NOTHING;
}

void player_stop ()
{
	logit ("requesting stop");
	request = REQ_STOP;
	
	LOCK (decoder_stream_mut);
	if (decoder_stream) {
		logit ("decoder_stream present, aborting...");
		io_abort (decoder_stream);
	}
	UNLOCK (decoder_stream_mut);

	LOCK (request_cond_mutex);
	pthread_cond_signal (&request_cond);
	UNLOCK (request_cond_mutex);
}

void player_seek (const int sec)
{
	request = REQ_SEEK;
	req_seek = sec + audio_get_time();
	LOCK (request_cond_mutex);
	pthread_cond_signal (&request_cond);
	UNLOCK (request_cond_mutex);
}

/* Return tags for the currently played file or NULL if there are no tags.
 * Tags are duplicated. */
struct file_tags *player_get_curr_tags ()
{
	struct file_tags *tags;

	LOCK (curr_tags_mut);
	if (curr_tags)
		tags = tags_dup (curr_tags);
	else
		tags = NULL;
	UNLOCK (curr_tags_mut);

	return tags;
}
