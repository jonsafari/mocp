#ifndef FIFO_BUF_H
#define FIFO_BUF_H

#ifdef __cplusplus
extern "C" {
#endif

struct fifo_buf;

struct fifo_buf *fifo_buf_new (const size_t size);
void fifo_buf_free (struct fifo_buf *b);
size_t fifo_buf_put (struct fifo_buf *b, const char *data, size_t size);
size_t fifo_buf_get (struct fifo_buf *b, char *user_buf, size_t user_buf_size);
size_t fifo_buf_peek (struct fifo_buf *b, char *user_buf, size_t user_buf_size);
size_t fifo_buf_get_space (const struct fifo_buf *b);
void fifo_buf_clear (struct fifo_buf *b);
size_t fifo_buf_get_fill (const struct fifo_buf *b);
size_t fifo_buf_get_size (const struct fifo_buf *b);

#ifdef __cplusplus
}
#endif

#endif
