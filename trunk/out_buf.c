/*
 * MOC - music on console
 * Copyright (C) 2004,2005 Damian Pietras <daper@daper.net>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 */

/*#define DEBUG*/

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <assert.h>
#include <pthread.h>
#include <string.h>
#include <errno.h>
#include <signal.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

#include "main.h"
#include "audio.h"
#include "log.h"
#include "fifo_buf.h"
#include "out_buf.h"

/* Don't play more than that value (in seconds) in one audio_play().
 * This prevent locking. */
#define AUDIO_MAX_PLAY		0.1
#define AUDIO_MAX_PLAY_BYTES	32768

/*static int fd;*/

/* Reading thread of the buffer. */
static void *read_thread (void *arg)
{
	struct out_buf *buf = (struct out_buf *)arg;

	logit ("entering output buffer thread");

	while (1) {
		int played = 0;
		char play_buf[AUDIO_MAX_PLAY_BYTES];
		int play_buf_fill;
		int play_buf_pos = 0;
		
		LOCK (buf->mutex);
		
		if (buf->reset_dev) {
			audio_reset ();
			buf->reset_dev = 0;
		}

		if (buf->stop)
			fifo_buf_clear (&buf->buf);

		if (fifo_buf_get_fill(&buf->buf) == 0) {
			debug ("Setting read_thread_waiting flag");
			buf->read_thread_waiting = 1;
		}
		
		debug ("sending the signal");
		pthread_cond_broadcast (&buf->ready_cond);
		if (buf->opt_cond) {
			pthread_mutex_lock (buf->opt_cond_mutex);
			pthread_cond_broadcast (buf->opt_cond);
			pthread_mutex_unlock (buf->opt_cond_mutex);
		}
		
		if ((fifo_buf_get_fill(&buf->buf) == 0 || buf->pause
					|| buf->stop)
				&& !buf->exit) {
			debug ("waiting for someting in the buffer");
			pthread_cond_wait (&buf->play_cond, &buf->mutex);
			debug ("someting appeard in the buffer");
		}

		buf->read_thread_waiting = 0;
		
		if (fifo_buf_get_fill(&buf->buf) == 0) {
			if (buf->exit) {
				logit ("exit");
				UNLOCK (buf->mutex);
				break;
			}

			logit ("buffer empty");
			UNLOCK (buf->mutex);
			continue;
		}

		if (buf->pause) {
			logit ("paused");
			UNLOCK (buf->mutex);
			continue;
		}

		if (buf->stop) {
			logit ("stopped");
			UNLOCK (buf->mutex);
			continue;
		}
				
		play_buf_fill = fifo_buf_get(&buf->buf, play_buf,
				MIN(audio_get_bps() * AUDIO_MAX_PLAY,
					AUDIO_MAX_PLAY_BYTES));
		UNLOCK (buf->mutex);

		debug ("playing %d bytes", play_buf_fill);

		while (play_buf_pos < play_buf_fill) {
			played = audio_send_pcm (play_buf + play_buf_pos,
					play_buf_fill - play_buf_pos);
			assert (played > 0);
			play_buf_pos += played;
		}

		/*logit ("done sending PCM");*/
		/*write (fd, buf->buf + buf->pos, to_play);*/

		LOCK (buf->mutex);
		
		/* Update time */
		if (played && audio_get_bps())
			buf->time += played / (float)audio_get_bps();
		buf->hardware_buf_fill = audio_get_buf_fill();
		
		UNLOCK (buf->mutex);
	}

	logit ("exiting");
	
	return NULL;
}

/* Initialize the buf structure, size is the buffer size. */
void out_buf_init (struct out_buf *buf, int size)
{
	assert (buf != NULL);
	assert (size > 0);
	
	fifo_buf_init (&buf->buf, size);
	buf->exit = 0;
	buf->pause = 0;
	buf->stop = 0;
	buf->time = 0.0;
	buf->reset_dev = 0;
	buf->hardware_buf_fill = 0;
	buf->read_thread_waiting = 0;
	buf->opt_cond = NULL;
	
	pthread_mutex_init (&buf->mutex, NULL);
	pthread_cond_init (&buf->play_cond, NULL);
	pthread_cond_init (&buf->ready_cond, NULL);

	/*fd = open ("out_test", O_CREAT | O_TRUNC | O_WRONLY, 0600);*/

	if (pthread_create(&buf->tid, NULL, read_thread, buf)) {
		logit ("Can't create buffer thread: %s", strerror(errno));
		fatal ("Can't create buffer thread");
	}
}

/* Wait for empty buffer, end playing, free resources allocated for the buf
 * structure. Can be used only if nothing is played */
void out_buf_destroy (struct out_buf *buf)
{
	assert (buf != NULL);

	LOCK (buf->mutex);
	buf->exit = 1;
	pthread_cond_signal (&buf->play_cond);
	UNLOCK (buf->mutex);

	pthread_join (buf->tid, NULL);
	
	/* Let know other threads using this buffer that the state of the
	 * buffer is different. */
	LOCK (buf->mutex);
	fifo_buf_clear (&buf->buf);
	pthread_cond_broadcast (&buf->ready_cond);
	UNLOCK (buf->mutex);

	fifo_buf_destroy (&buf->buf);
	if (pthread_mutex_destroy(&buf->mutex))
		logit ("Destroying buffer mutex failed: %s", strerror(errno));
	if (pthread_cond_destroy(&buf->play_cond))
		logit ("Destroying buffer play condition failed: %s",
				strerror(errno));
	if (pthread_cond_destroy(&buf->ready_cond))
		logit ("Destroying buffer ready condition failed: %s",
				strerror(errno));

	logit ("buffer destroyed");

	/*close (fd);*/
}

/* Put data at the end of the buffer, return 0 if nothing was put. */
int out_buf_put (struct out_buf *buf, const char *data, int size)
{
	int pos = 0;
	
	/*logit ("got %d bytes to play", size);*/

	while (size) {
		int written;
		
		LOCK (buf->mutex);
		
		if (fifo_buf_get_space(&buf->buf) == 0 && !buf->stop) {
			/*logit ("buffer full, waiting for the signal");*/
			pthread_cond_wait (&buf->ready_cond, &buf->mutex);
			/*logit ("buffer ready");*/
		}

		if (buf->stop) {
			logit ("the buffer is stopped, refusing to write to the buffer");
			UNLOCK (buf->mutex);
			return 0;
		}

		written = fifo_buf_put (&buf->buf, data + pos, size);
		
		if (written) {
			pthread_cond_signal (&buf->play_cond);
			size -= written;
			pos += written;
		}

		UNLOCK (buf->mutex);

	}

	return 1;
}

void out_buf_pause (struct out_buf *buf)
{
	LOCK (buf->mutex);
	buf->pause = 1;
	buf->reset_dev = 1;
	UNLOCK (buf->mutex);
}

void out_buf_unpause (struct out_buf *buf)
{
	LOCK (buf->mutex);
	buf->pause = 0;
	pthread_cond_signal (&buf->play_cond);
	UNLOCK (buf->mutex);
}

/* Stop playing, after that buffer will refuse to play anything and ignore data
 * sent by buf_put(). */
void out_buf_stop (struct out_buf *buf)
{
	logit ("stopping the buffer");
	LOCK (buf->mutex);
	buf->stop = 1;
	buf->pause = 0;
	buf->reset_dev = 1;
	logit ("sending signal");
	pthread_cond_signal (&buf->play_cond);
	logit ("waiting for signal");
	pthread_cond_wait (&buf->ready_cond, &buf->mutex);
	logit ("done");
	UNLOCK (buf->mutex);
}

/* Reset the buffer state: this can by called ONLY when the buffer is stopped
 * and buf_put is not used! */
void out_buf_reset (struct out_buf *buf)
{
	logit ("resetting the buffer");
	
	LOCK (buf->mutex);
	fifo_buf_clear (&buf->buf);
	buf->stop = 0;
	buf->pause = 0;
	buf->reset_dev = 0;
	buf->hardware_buf_fill = 0;
	
	UNLOCK (buf->mutex);
}

void out_buf_time_set (struct out_buf *buf, const float time)
{
	LOCK (buf->mutex);
	buf->time = time;
	UNLOCK (buf->mutex);
}

int out_buf_time_get (struct out_buf *buf)
{
	int time;
	int bps = audio_get_bps ();
	
	LOCK (buf->mutex);
	time = buf->time - (bps ? buf->hardware_buf_fill / (float)bps : 0);
	UNLOCK (buf->mutex);

	assert (time >= 0);

	return time;
}

void out_buf_set_notify_cond (struct out_buf *buf, pthread_cond_t *cond,
		pthread_mutex_t *mutex)
{
	assert (buf != NULL);
	
	buf->opt_cond = cond;
	buf->opt_cond_mutex = mutex;
}

int out_buf_get_free (struct out_buf *buf)
{
	int space;
	
	assert (buf != NULL);

	LOCK (buf->mutex);
	space = fifo_buf_get_space (&buf->buf);
	UNLOCK (buf->mutex);

	return space;
}

int out_buf_get_fill (struct out_buf *buf)
{
	int fill;
	
	assert (buf != NULL);

	LOCK (buf->mutex);
	fill = fifo_buf_get_fill (&buf->buf);
	UNLOCK (buf->mutex);

	return fill;
}

/* Wait until the read thread will stop and wait for data to come.
 * This makes sur ethat the audio device isn't used (of course only if you
 * don't put anything in the buffer). */
void out_buf_wait (struct out_buf *buf)
{
	assert (buf != NULL);

	logit ("Waiting for read thread to suspend...");

	LOCK (buf->mutex);
	while (!buf->read_thread_waiting) {
		debug ("waiting....");
		pthread_cond_wait (&buf->ready_cond, &buf->mutex);
	}
	UNLOCK (buf->mutex);

	logit ("done");
}
