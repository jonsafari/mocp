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
#include "buf.h"

/* Don't play more than that value in one audio_play(). This prevent locking. */
#define AUDIO_MAX_PLAY	8 * 1024

#define MIN(a, b) ((a) < (b) ? (a) : (b))

/*static int fd;*/

/* Get the size of continuos filled space in the buffer. */
static int count_fill (const struct buf *buf)
{
	if (buf->pos + buf->fill <= buf->size)
		return buf->fill;
	else
		return buf->size - buf->pos;
}

/* Reading thread of the buffer. */
static void *read_thread (void *arg)
{
	struct buf *buf = (struct buf *)arg;

	logit ("entering output buffer thread");

	while (1) {
		int to_play, played;
		
		LOCK (buf->mutex);
		
		if (buf->reset_dev) {
			audio_reset ();
			buf->reset_dev = 0;
		}

		if (buf->stop)
			buf->fill = 0;

		logit ("sending the signal");
		pthread_cond_broadcast (&buf->ready_cond);
		if (buf->opt_cond) {
			pthread_mutex_lock (buf->opt_cond_mutex);
			pthread_cond_broadcast (buf->opt_cond);
			pthread_mutex_unlock (buf->opt_cond_mutex);
		}
		
		if ((buf->fill == 0 || buf->pause || buf->stop)
				&& !buf->exit) {
			logit ("waiting for someting in the buffer");
			pthread_cond_wait (&buf->play_cond, &buf->mutex);
			logit ("someting appeard in the buffer");
		}
		
		if (buf->exit && buf->fill == 0) {
			logit ("exit");
			UNLOCK (buf->mutex);
			break;
		}

		if (buf->fill == 0) {
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
				
		to_play = MIN (count_fill(buf), AUDIO_MAX_PLAY);
		UNLOCK (buf->mutex);

		/*logit ("READER: playing %d bytes from %d", to_play,
				buf->pos);*/

		/* We don't need mutex here, because we are the only thread
		 * that modify buf->pos, and the buffer part we use is
		 * unchanged. */
		/*logit ("READER: sending PCM");*/
		played = audio_send_pcm (buf->buf + buf->pos, to_play);
		/*logit ("READER: done sending PCM");*/
		/*write (fd, buf->buf + buf->pos, to_play);*/

		LOCK (buf->mutex);
		
		/* Update buffer position and fill */
		buf->pos += played;
		if (buf->pos == buf->size)
			buf->pos = 0;
		buf->fill -= played;

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
void buf_init (struct buf *buf, int size)
{
	assert (buf != NULL);
	assert (size > 0);
	
	buf->buf = (char *)xmalloc (sizeof(char) * size);
	buf->size = size;
	buf->pos = 0;
	buf->exit = 0;
	buf->fill = 0;
	buf->pause = 0;
	buf->stop = 0;
	buf->time = 0.0;
	buf->reset_dev = 0;
	buf->hardware_buf_fill = 0;
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
void buf_destroy (struct buf *buf)
{
	assert (buf != NULL);
	assert (buf->buf != NULL);

	LOCK (buf->mutex);
	buf->exit = 1;
	pthread_cond_signal (&buf->play_cond);
	UNLOCK (buf->mutex);

	pthread_join (buf->tid, NULL);
	
	/* Let know other threads using this buffer that the state of the
	 * buffer is different. */
	LOCK (buf->mutex);
	buf->fill = 0;
	pthread_cond_broadcast (&buf->ready_cond);
	UNLOCK (buf->mutex);

	free (buf->buf);
	buf->size = 0;
	buf->buf = NULL;
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

/* Get the amount of free continuos space in the buffer. */
static int count_free (const struct buf *buf)
{
	if (buf->pos + buf->fill < buf->size)
		return buf->size - (buf->pos + buf->fill);
	else
		return buf->size - buf->fill;
}

/* Return the position of the first free byte in the buffer. */
static int end_pos (const struct buf *buf)
{
	if (buf->pos + buf->fill < buf->size)
		return buf->pos + buf->fill;
	else
		return buf->fill - buf->size + buf->pos;
}

/* Put data at the end of the buffer, return 0 if nothing was put. */
int buf_put (struct buf *buf, const char *data, int size)
{
	int pos = 0;
	
	/*logit ("WRITER: got %d bytes to play", size);*/

	while (size) {
		int to_write;
		
		LOCK (buf->mutex);
		
		if (!count_free(buf) && !buf->stop) {
			/*logit ("WRITER: buffer full, waiting for the signal");*/
			pthread_cond_wait (&buf->ready_cond, &buf->mutex);
			/*logit ("WRITER: buffer ready");*/
		}

		if (buf->stop) {
			logit ("the buffer is stopped, refusing to write to the buffer");
			UNLOCK (buf->mutex);
			return 0;
		}
		
		to_write = MIN (count_free(buf), size);
		if (to_write) {
			memcpy (buf->buf + end_pos(buf), data + pos, to_write);
			/*logit ("WRITER: writing %d bytes from %d to the buffer",
					to_write, end_pos(buf));*/
			buf->fill += to_write;
		
			pthread_cond_signal (&buf->play_cond);
		}

		UNLOCK (buf->mutex);

		size -= to_write;
		pos += to_write;
	}

	return 1;
}

void buf_pause (struct buf *buf)
{
	LOCK (buf->mutex);
	buf->pause = 1;
	buf->reset_dev = 1;
	UNLOCK (buf->mutex);
}

void buf_unpause (struct buf *buf)
{
	LOCK (buf->mutex);
	buf->pause = 0;
	pthread_cond_signal (&buf->play_cond);
	UNLOCK (buf->mutex);
}

/* Stop playing, after that buffer will refuse to play anything and ignore data
 * sent by buf_put(). */
void buf_stop (struct buf *buf)
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
void buf_reset (struct buf *buf)
{
	logit ("resetting the buffer");
	
	LOCK (buf->mutex);
	buf->stop = 0;
	buf->pos = 0;
	buf->fill = 0;
	buf->pause = 0;
	buf->reset_dev = 0;
	buf->hardware_buf_fill = 0;
	
	UNLOCK (buf->mutex);
}

void buf_time_set (struct buf *buf, const float time)
{
	LOCK (buf->mutex);
	buf->time = time;
	UNLOCK (buf->mutex);
}

float buf_time_get (struct buf *buf)
{
	float time;
	int bps = audio_get_bps ();
	
	LOCK (buf->mutex);
	time = buf->time - (bps ? buf->hardware_buf_fill / (float)bps : 0);
	UNLOCK (buf->mutex);

	return time;
}

void buf_set_notify_cond (struct buf *buf, pthread_cond_t *cond,
		pthread_mutex_t *mutex)
{
	assert (buf != NULL);
	
	buf->opt_cond = cond;
	buf->opt_cond_mutex = mutex;
}

int buf_get_free (struct buf *buf)
{
	assert (buf != NULL);

	return buf->size - buf->fill;
}
