#ifndef BUF_H
#define BUF_H

#include <pthread.h>

struct buf
{
	int size;	/* Size of the buffer. */
	int pos;	/* Current position. */
	int fill;	/* Current fill. */
	pthread_mutex_t	mutex;
	pthread_t tid;	/* Thread id of the reading thread. */

	/* Signals. */
	pthread_cond_t play_cond;	/* Something was written to the buffer. */
	pthread_cond_t ready_cond;	/* There is some space in the buffer. */

	/* State flags of the buffer. */
	int pause;
	int exit;	/* Exit when the buffer is empty. */
	int stop;	/* Don't play anything. */

	int reset_dev;	/* request to the reading thread to reset the audio
			   device */

	float time;	/* Time of played sound .*/
	int hardware_buf_fill;	/* How the sound card buffer is filled */

	char *buf;	/* The buffer. */
};

void buf_init (struct buf *buf, int size);
void buf_destroy (struct buf *buf);
int buf_put (struct buf *buf, const char *data, int size);
void buf_pause (struct buf *buf);
void buf_unpause (struct buf *buf);
void buf_wait_empty (struct buf *buf);
void buf_stop (struct buf *buf);
void buf_reset (struct buf *buf);
void buf_abort_put (struct buf *buf);
void buf_time_set (struct buf *buf, const float time);
float buf_time_get (struct buf *buf);

#endif
