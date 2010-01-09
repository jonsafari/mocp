#ifndef BUF_H
#define BUF_H

#include <pthread.h>
#include "fifo_buf.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (out_buf_free_callback)();

struct out_buf
{
	struct fifo_buf buf;
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

	int reset_dev;	/* request to the reading thread to reset the audio
			   device */

	float time;	/* Time of played sound .*/
	int hardware_buf_fill;	/* How the sound card buffer is filled */

	int read_thread_waiting; /* is the read thread waiting for data? */
};

void out_buf_init (struct out_buf *buf, int size);
void out_buf_destroy (struct out_buf *buf);
int out_buf_put (struct out_buf *buf, const char *data, int size);
void out_buf_pause (struct out_buf *buf);
void out_buf_unpause (struct out_buf *buf);
void out_buf_stop (struct out_buf *buf);
void out_buf_reset (struct out_buf *buf);
void out_buf_time_set (struct out_buf *buf, const float time);
int out_buf_time_get (struct out_buf *buf);
void out_buf_set_free_callback (struct out_buf *buf,
		out_buf_free_callback callback);
int out_buf_get_free (struct out_buf *buf);
int out_buf_get_fill (struct out_buf *buf);
void out_buf_wait (struct out_buf *buf);

#ifdef __cplusplus
}
#endif

#endif
