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

#include "main.h"
#include "log.h"
#include "file_types.h"
#include "audio.h"
#include "buf.h"
#include "server.h"

enum request
{
	REQ_NOTHING,
	REQ_SEEK,
	REQ_STOP
};

/* request conditional and mutex */
static pthread_cond_t request_cond = PTHREAD_COND_INITIALIZER;
static pthread_mutex_t request_cond_mutex = PTHREAD_MUTEX_INITIALIZER;

static enum request request = REQ_NOTHING;
static int req_seek;

/* Does the parameters p1 and p2 are equal? */
#define sound_params_eq(p1, p2) (p1.format == p2.format \
		&& p1.channels == p2.channels && p1.rate == p2.rate)

/* Open a file, decode it and put output to the buffer. */
void player (const char *file, struct decoder_funcs *f, struct buf *out_buf)
{
	int eof = 0;
	char buf[8096];
	void *decoder_data;
	int decoded = 0;
	struct sound_params sound_params = { 0, 0, 0 };
	struct sound_params new_sound_params;
	int sound_params_change = 0;
	int audio_opened = 0;
	
	buf_set_notify_cond (out_buf, &request_cond, &request_cond_mutex);
	buf_reset (out_buf);

	if (!(decoder_data = f->open(file))) {
		logit ("Can't open file, exiting");
		return;
	}

	while (1) {
		logit ("loop...");
		
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
				logit ("decoded %d bytes", decoded);
				if (!sound_params_eq(new_sound_params,
							sound_params))
					sound_params_change = 1;
			}
		}
		else if ((decoded > buf_get_free(out_buf)
				|| (eof && buf_get_free(out_buf)))
				&& out_buf->fill) {
			logit ("waiting...");
			pthread_cond_wait (&request_cond, &request_cond_mutex);
			UNLOCK (request_cond_mutex);
		}
		else
			UNLOCK (request_cond_mutex);

		if (request == REQ_STOP) {
			logit ("stop");
			buf_stop (out_buf);
			request = REQ_NOTHING;
			break;
		}
		else if (request == REQ_SEEK) {
			int sec = req_seek + audio_get_time ();
			int decoder_seek;
			
			logit ("seeking");
			buf_stop (out_buf);
			buf_reset (out_buf);
			if ((decoder_seek = f->seek(decoder_data, sec)) == -1)
				logit ("error when seeking");
			else {
				buf_time_set (out_buf, decoder_seek);
				eof = 0;
			}
			request = REQ_NOTHING;
			decoded = 0;
		}
		else if (!eof && decoded <= buf_get_free(out_buf)
				&& !sound_params_change) {
			buf_put (out_buf, buf, decoded);
			decoded = 0;
		}
		else if (!eof && sound_params_change && out_buf->fill == 0) {
			logit ("sound parameters has changed.");
			if (audio_opened)
				audio_close ();
			sound_params = new_sound_params;
			sound_params_change = 0;
			set_info_channels (sound_params.channels);
			set_info_rate (sound_params.rate / 1000);
			audio_open (&sound_params);
			audio_opened = 1;
		}
		else if (eof && out_buf->fill == 0) {
			logit ("played everything");
			break;
		}
	}

	f->close (decoder_data);
	audio_close ();
	buf_set_notify_cond (out_buf, NULL, NULL);
	logit ("exiting");
}

void player_cleanup ()
{
	if (pthread_mutex_destroy(&request_cond_mutex))
		logit ("Can't destroy mutex: %s", strerror(errno));
	if (pthread_cond_destroy(&request_cond))
		logit ("Can't destroy condition: %s", strerror(errno));
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
	req_seek = sec;
	LOCK (request_cond_mutex);
	pthread_cond_signal (&request_cond);
	UNLOCK (request_cond_mutex);
}
