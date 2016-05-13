/*
 * MOC - music on console
 * Copyright (C) 2005 Damian Pietras <daper@daper.net>
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

#include <stddef.h>
#include <sys/types.h>
#include <assert.h>
#include <string.h>

#include "common.h"
#include "fifo_buf.h"

struct fifo_buf
{
	int size;                           /* Size of the buffer */
	int pos;                            /* Current position */
	int fill;                           /* Current fill */
	char buf[];                         /* The buffer content */
};

/* Initialize and return a new fifo_buf structure of the size requested. */
struct fifo_buf *fifo_buf_new (const size_t size)
{
	struct fifo_buf *b;

	assert (size > 0);

	b = xmalloc (offsetof (struct fifo_buf, buf) + size);

	b->size = size;
	b->pos = 0;
	b->fill = 0;

	return b;
}

/* Destroy the buffer object. */
void fifo_buf_free (struct fifo_buf *b)
{
	assert (b != NULL);

	free (b);
}

/* Put data into the buffer. Returns number of bytes actually put. */
size_t fifo_buf_put (struct fifo_buf *b, const char *data, size_t size)
{
	size_t written = 0;

	assert (b != NULL);
	assert (b->buf != NULL);

	while (b->fill < b->size && written < size) {
		size_t write_from;
		size_t to_write;

		if (b->pos + b->fill < b->size) {
			write_from = b->pos + b->fill;
			to_write = b->size - (b->pos + b->fill);
		}
		else {
			write_from = b->fill - b->size + b->pos;
			to_write = b->size - b->fill;
		}

		if (to_write > size - written)
			to_write = size - written;

		memcpy (b->buf + write_from, data + written, to_write);
		b->fill += to_write;
		written += to_write;
	}

	return written;
}

/* Copy data from the beginning of the buffer to the user buffer. Returns the
 * number of bytes copied. */
size_t fifo_buf_peek (struct fifo_buf *b, char *user_buf, size_t user_buf_size)
{
	size_t user_buf_pos = 0, written = 0;
	ssize_t left, pos;

	assert (b != NULL);
	assert (b->buf != NULL);

	left = b->fill;
	pos = b->pos;

	while (left && written < user_buf_size) {
		size_t to_copy = pos + left <= b->size
			? left : b->size - pos;

		if (to_copy > user_buf_size - written)
			to_copy = user_buf_size - written;

		memcpy (user_buf + user_buf_pos, b->buf + pos, to_copy);
		user_buf_pos += to_copy;
		written += to_copy;

		left -= to_copy;
		pos += to_copy;
		if (pos == b->size)
			pos = 0;
	}

	return written;
}

size_t fifo_buf_get (struct fifo_buf *b, char *user_buf, size_t user_buf_size)
{
	size_t user_buf_pos = 0, written = 0;

	assert (b != NULL);
	assert (b->buf != NULL);

	while (b->fill && written < user_buf_size) {
		size_t to_copy = b->pos + b->fill <= b->size
			? b->fill : b->size - b->pos;

		if (to_copy > user_buf_size - written)
			to_copy = user_buf_size - written;

		memcpy (user_buf + user_buf_pos, b->buf + b->pos, to_copy);
		user_buf_pos += to_copy;
		written += to_copy;

		b->fill -= to_copy;
		b->pos += to_copy;
		if (b->pos == b->size)
			b->pos = 0;
	}

	return written;
}

/* Get the amount of free space in the buffer. */
size_t fifo_buf_get_space (const struct fifo_buf *b)
{
	assert (b != NULL);
	assert (b->buf != NULL);

	return b->size - b->fill;

}

size_t fifo_buf_get_fill (const struct fifo_buf *b)
{
	assert (b != NULL);
	return b->fill;
}

size_t fifo_buf_get_size (const struct fifo_buf *b)
{
	assert (b != NULL);
	return b->size;
}

void fifo_buf_clear (struct fifo_buf *b)
{
	assert (b != NULL);
	b->fill = 0;
}
