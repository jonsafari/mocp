#ifndef FIFO_BUF_H
#define FIFO_BUF_H

#include <unistd.h>

#ifdef __cplusplus
extern "C" {
#endif

struct fifo_buf
{
	char *buf;	/* The buffer content. */
	int size;	/* Size of the buffer. */
	int pos;	/* Current position. */
	int fill;	/* Current fill. */
};

void fifo_buf_init (struct fifo_buf *b, const size_t size);
void fifo_buf_destroy (struct fifo_buf *b);
size_t fifo_buf_put (struct fifo_buf *b, const char *data, size_t size);
size_t fifo_buf_get (struct fifo_buf *b, char *user_buf,
		size_t user_buf_size);
size_t fifo_buf_peek (struct fifo_buf *b, char *user_buf,
		size_t user_buf_size);
size_t fifo_buf_get_space (const struct fifo_buf *b);
void fifo_buf_clear (struct fifo_buf *b);
size_t fifo_buf_get_fill (const struct fifo_buf *b);
size_t fifo_buf_get_size (const struct fifo_buf *b);

#ifdef __cplusplus
}
#endif

#endif
