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

#include <pthread.h>
#include <string.h>
#include <errno.h>
#include <assert.h>

/*#define DEBUG*/

#include "main.h"
#include "log.h"
#include "file_types.h"
#include "audio.h"
#include "buf.h"
#include "server.h"
#include "options.h"

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
	struct decoder_funcs *f; /* decoder functions for precached file */
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
	precache->f = get_decoder_funcs (precache->file);
	assert (precache->f != NULL);

	if (!(precache->decoder_data = precache->f->open(precache->file))) {
		logit ("Failed to open the file for precache.");
		return NULL;
	}

	/* Stop at PCM_BUF_SIZE, because when we decode too much, there is no
	 * place when we can put the data that don't fit into the buffer. */
	while (precache->buf_fill < PCM_BUF_SIZE) {
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

/* Open a file, decode it and put output into the buffer. At the end, start
 * precaching next_file. */
void player (char *file, char *next_file, struct buf *out_buf)
{
	int eof = 0;
	char buf[PCM_BUF_SIZE];
	void *decoder_data;
	int decoded = 0;
	struct sound_params sound_params = { 0, 0, 0 };
	struct sound_params new_sound_params;
	int sound_params_change = 0;
	struct decoder_funcs *f;
	int bitrate = -1;

	f = get_decoder_funcs (file);
	assert (f != NULL);
	
	buf_set_notify_cond (out_buf, &request_cond, &request_cond_mutex);
	buf_reset (out_buf);
	
	precache_wait (&precache);

	if (precache.ok && !strcmp(precache.file, file)) {
		logit ("Using precached file");

		assert (f == precache.f);
		
		sound_params = precache.sound_params;
		decoder_data = precache.decoder_data;
		set_info_channels (sound_params.channels);
		set_info_rate (sound_params.rate / 1000);
		if (!audio_open(&sound_params))
			return;
		buf_put (out_buf, precache.buf, precache.buf_fill);
	}
	else if (!(decoder_data = f->open(file))) {
		logit ("Can't open file, exiting");
		return;
	}

	precache_reset (&precache);

	while (1) {
		debug ("loop...");
		
		LOCK (request_cond_mutex);
		if (!eof && !decoded) {
			UNLOCK (request_cond_mutex);
			decoded = f->decode (decoder_data, buf, sizeof(buf),
					&new_sound_params);
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
			}
		}

		/* Wait, if there is no space in the buffer to put the decoded
		 * data or EOF occured and there is something in the buffer. */
		else if (decoded > buf_get_free(out_buf)
					|| (eof && out_buf->fill)) {
			debug ("waiting...");
			if (eof && !precache.file && next_file
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
			buf_stop (out_buf);
			
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
				buf_stop (out_buf);
				buf_reset (out_buf);
				buf_time_set (out_buf, decoder_seek);
				eof = 0;
				decoded = 0;
			}

			LOCK (request_cond_mutex);
			if (request == REQ_SEEK)
				request = REQ_NOTHING;
			UNLOCK (request_cond_mutex);

		}
		else if (!eof && decoded <= buf_get_free(out_buf)
				&& !sound_params_change) {
			debug ("putting into the buffer %d bytes", decoded);
			buf_put (out_buf, buf, decoded);
			decoded = 0;
		}
		else if (!eof && sound_params_change && out_buf->fill == 0) {
			logit ("sound parameters has changed.");
			sound_params = new_sound_params;
			sound_params_change = 0;
			set_info_channels (sound_params.channels);
			set_info_rate (sound_params.rate / 1000);
			if (!audio_open(&sound_params))
				break;
		}
		else if (eof && out_buf->fill == 0) {
			logit ("played everything");
			break;
		}
	}

	f->close (decoder_data);
	buf_set_notify_cond (out_buf, NULL, NULL);
	logit ("exiting");
}

void player_cleanup ()
{
	if (pthread_mutex_destroy(&request_cond_mutex))
		logit ("Can't destroy request mutex: %s", strerror(errno));
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
	request = REQ_STOP;
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
