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

/* Defining OUT_TEST causes the raw audio samples to be written
 * to the file 'out_test' in the current directory for debugging. */
/*#define OUT_TEST*/

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <assert.h>
#include <pthread.h>
#include <string.h>
#include <errno.h>

#ifdef OUT_TEST
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#endif

/*#define DEBUG*/

#include "common.h"
#include "audio.h"
#include "log.h"
#include "fifo_buf.h"
#include "out_buf.h"
#include "options.h"

struct out_buf
{
	struct fifo_buf *buf;
	pthread_mutex_t	mutex;
	pthread_t tid;	/* Thread id of the reading thread. */

	/* Signals. */
	pthread_cond_t play_cond;	/* Something was written to the buffer. */
	pthread_cond_t ready_cond;	/* There is some space in the buffer. */

	/* Optional callback called when there is some free space in
	 * the buffer. */
	out_buf_free_callback *free_callback;

	/* State flags of the buffer. */
	int pause;
	int exit;	/* Exit when the buffer is empty. */
	int stop;	/* Don't play anything. */

	int reset_dev;	/* Request to the reading thread to reset the audio
			   device. */

	float time;	/* Time of played sound. */
	int hardware_buf_fill;	/* How the sound card buffer is filled. */

	int read_thread_waiting; /* Is the read thread waiting for data? */
};

/* Don't play more than this value (in seconds) in one audio_play().
 * This prevents locking. */
#define AUDIO_MAX_PLAY		0.1
#define AUDIO_MAX_PLAY_BYTES	32768

#ifdef OUT_TEST
static int fd;
#endif

static void set_realtime_prio ()
{
#ifdef HAVE_SCHED_GET_PRIORITY_MAX
	int rc;

	if (options_get_bool("UseRealtimePriority")) {
		struct sched_param param;

		param.sched_priority = sched_get_priority_max(SCHED_RR);
		rc = pthread_setschedparam (pthread_self (), SCHED_RR, &param);
		if (rc != 0)
			log_errno ("Can't set realtime priority", rc);
	}
#else
	logit ("No sched_get_priority_max() function: "
	                  "realtime priority not used.");
#endif
}

/* Reading thread of the buffer. */
static void *read_thread (void *arg)
{
	struct out_buf *buf = (struct out_buf *)arg;
	int audio_dev_closed = 0;

	logit ("entering output buffer thread");

	set_realtime_prio ();

	LOCK (buf->mutex);

	while (1) {
		int played = 0;
		char play_buf[AUDIO_MAX_PLAY_BYTES];
		int play_buf_fill;
		int play_buf_pos = 0;

		if (buf->reset_dev && !audio_dev_closed) {
			audio_reset ();
			buf->reset_dev = 0;
		}

		if (buf->stop)
			fifo_buf_clear (buf->buf);

		if (buf->free_callback) {
			/* unlock the mutex to make calls to out_buf functions
			 * possible in the callback */
			UNLOCK (buf->mutex);
			buf->free_callback ();
			LOCK (buf->mutex);
		}

		debug ("sending the signal");
		pthread_cond_broadcast (&buf->ready_cond);

		if ((fifo_buf_get_fill(buf->buf) == 0 || buf->pause || buf->stop)
				&& !buf->exit) {
			if (buf->pause && !audio_dev_closed) {
				logit ("Closing the device due to pause");
				audio_close ();
				audio_dev_closed = 1;
			}

			debug ("waiting for something in the buffer");
			buf->read_thread_waiting = 1;
			pthread_cond_wait (&buf->play_cond, &buf->mutex);
			debug ("something appeared in the buffer");
		}

		buf->read_thread_waiting = 0;

		if (audio_dev_closed && !buf->pause) {
			logit ("Opening the device again after pause");
			if (!audio_open(NULL)) {
				logit ("Can't reopen the device! sleeping...");
				xsleep (1, 1); /* there is no way to exit :( */
			}
			else
				audio_dev_closed = 0;
		}

		if (fifo_buf_get_fill(buf->buf) == 0) {
			if (buf->exit) {
				logit ("exit");
				break;
			}

			logit ("buffer empty");
			continue;
		}

		if (buf->pause) {
			logit ("paused");
			continue;
		}

		if (buf->stop) {
			logit ("stopped");
			continue;
		}

		if (!audio_dev_closed) {
			int audio_bpf;
			size_t play_buf_frames;

			audio_bpf = audio_get_bpf();
			play_buf_frames = MIN(audio_get_bps() * AUDIO_MAX_PLAY,
			                      AUDIO_MAX_PLAY_BYTES) / audio_bpf;
			play_buf_fill = fifo_buf_get(buf->buf, play_buf,
			                             play_buf_frames * audio_bpf);
			UNLOCK (buf->mutex);

			debug ("playing %d bytes", play_buf_fill);

			while (play_buf_pos < play_buf_fill) {
				played = audio_send_pcm (
						play_buf + play_buf_pos,
						play_buf_fill - play_buf_pos);

#ifdef OUT_TEST
				write (fd, play_buf + play_buf_pos, played);
#endif

				play_buf_pos += played;
			}

			/*logit ("done sending PCM");*/

			LOCK (buf->mutex);

			/* Update time */
			if (play_buf_fill && audio_get_bps())
				buf->time += play_buf_fill / (float)audio_get_bps();
			buf->hardware_buf_fill = audio_get_buf_fill();
		}
	}

	UNLOCK (buf->mutex);

	logit ("exiting");

	return NULL;
}

/* Allocate and initialize the buf structure, size is the buffer size. */
struct out_buf *out_buf_new (int size)
{
	int rc;
	struct out_buf *buf;

	assert (size > 0);

	buf = xmalloc (sizeof (struct out_buf));

	buf->buf = fifo_buf_new (size);
	buf->exit = 0;
	buf->pause = 0;
	buf->stop = 0;
	buf->time = 0.0;
	buf->reset_dev = 0;
	buf->hardware_buf_fill = 0;
	buf->read_thread_waiting = 0;
	buf->free_callback = NULL;

	pthread_mutex_init (&buf->mutex, NULL);
	pthread_cond_init (&buf->play_cond, NULL);
	pthread_cond_init (&buf->ready_cond, NULL);

#ifdef OUT_TEST
	fd = open ("out_test", O_CREAT | O_TRUNC | O_WRONLY, 0600);
#endif

	rc = pthread_create (&buf->tid, NULL, read_thread, buf);
	if (rc != 0)
		fatal ("Can't create buffer thread: %s", xstrerror (rc));

	return buf;
}

/* Wait for empty buffer, end playing, free resources allocated for the buf
 * structure.  Can be used only if nothing is played. */
void out_buf_free (struct out_buf *buf)
{
	int rc;

	assert (buf != NULL);

	LOCK (buf->mutex);
	buf->exit = 1;
	pthread_cond_signal (&buf->play_cond);
	UNLOCK (buf->mutex);

	pthread_join (buf->tid, NULL);

	/* Let other threads using this buffer know that the state of the
	 * buffer has changed. */
	LOCK (buf->mutex);
	fifo_buf_clear (buf->buf);
	pthread_cond_broadcast (&buf->ready_cond);
	UNLOCK (buf->mutex);

	fifo_buf_free (buf->buf);
	buf->buf = NULL;
	rc = pthread_mutex_destroy (&buf->mutex);
	if (rc != 0)
		log_errno ("Destroying buffer mutex failed", rc);
	rc = pthread_cond_destroy (&buf->play_cond);
	if (rc != 0)
		log_errno ("Destroying buffer play condition failed", rc);
	rc = pthread_cond_destroy (&buf->ready_cond);
	if (rc != 0)
		log_errno ("Destroying buffer ready condition failed", rc);

	free (buf);

	logit ("buffer destroyed");

#ifdef OUT_TEST
	close (fd);
#endif
}

/* Put data at the end of the buffer, return 0 if nothing was put. */
int out_buf_put (struct out_buf *buf, const char *data, int size)
{
	int pos = 0;

	/*logit ("got %d bytes to play", size);*/

	while (size) {
		int written;

		LOCK (buf->mutex);

		if (fifo_buf_get_space(buf->buf) == 0 && !buf->stop) {
			/*logit ("buffer full, waiting for the signal");*/
			pthread_cond_wait (&buf->ready_cond, &buf->mutex);
			/*logit ("buffer ready");*/
		}

		if (buf->stop) {
			logit ("the buffer is stopped, refusing to write to the buffer");
			UNLOCK (buf->mutex);
			return 0;
		}

		written = fifo_buf_put (buf->buf, data + pos, size);

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
	fifo_buf_clear (buf->buf);
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

/* Return the time in the audio which the user is currently hearing.
 * If unplayed samples still remain in the hardware buffer from the
 * previous audio then the value returned may be negative and it is
 * up to the caller to handle this appropriately in the context of
 * its own processing. */
int out_buf_time_get (struct out_buf *buf)
{
	int time;
	int bps = audio_get_bps ();

	LOCK (buf->mutex);
	time = buf->time - (bps ? buf->hardware_buf_fill / (float)bps : 0);
	UNLOCK (buf->mutex);

	return time;
}

void out_buf_set_free_callback (struct out_buf *buf,
		out_buf_free_callback callback)
{
	assert (buf != NULL);

	LOCK (buf->mutex);
	buf->free_callback = callback;
	UNLOCK (buf->mutex);
}

int out_buf_get_free (struct out_buf *buf)
{
	int space;

	assert (buf != NULL);

	LOCK (buf->mutex);
	space = fifo_buf_get_space (buf->buf);
	UNLOCK (buf->mutex);

	return space;
}

int out_buf_get_fill (struct out_buf *buf)
{
	int fill;

	assert (buf != NULL);

	LOCK (buf->mutex);
	fill = fifo_buf_get_fill (buf->buf);
	UNLOCK (buf->mutex);

	return fill;
}

/* Wait until the read thread will stop and wait for data to come.
 * This makes sure that the audio device isn't used (of course only if you
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
