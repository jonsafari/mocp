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

/* TODO:
 * - prebuffering (for network streams)
 * - handle SIGBUS (mmap() read error)
 */

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#define _XOPEN_SOURCE   600 /* we need the POSIX version of strerror_r() */

#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <assert.h>
#ifdef HAVE_MMAP
# include <sys/mman.h>
#endif

#define DEBUG

#include "main.h"
#include "log.h"
#include "io.h"
#include "options.h"

// DEBUG!
/*static char write_mirror_file[128];
static int write_mirror_num = 0;
static int write_mirror_fd;*/

#ifdef HAVE_MMAP
static ssize_t io_read_mmap (struct io_stream *s, void *buf, size_t count)
{
	struct stat file_stat;
	size_t to_read;

	assert (s->mem != NULL);
	
	if (fstat(s->fd, &file_stat) == -1) {
		logit ("stat() failed: %s", strerror(errno));
		return -1;
	}

	if (s->size != (size_t)file_stat.st_size) {
		logit ("File size has changed");
		
		if (munmap(s->mem, s->size)) {
			logit ("munmap() failed: %s", strerror(errno));
			return -1;
		}

		if ((s->mem = mmap(0, s->size, PROT_READ, MAP_SHARED, s->fd, 0))
					== MAP_FAILED) {
			logit ("mmap() filed: %s", strerror(errno));
			return -1;
		}
		
		logit ("mmap()ed %lu bytes", (unsigned long)file_stat.st_size);
		s->size = file_stat.st_size;
		if (s->mem_pos > s->size) {
			logit ("File shrinked");
			return 0;
		}
	}

	if (s->mem_pos >= s->size)
		return 0;
	
	to_read = MIN(count, s->size - s->mem_pos);
	memcpy (buf, s->mem + s->mem_pos, to_read);
	s->mem_pos += to_read;

	return to_read;
}
#endif

static ssize_t io_read_fd (struct io_stream *s, void *buf, size_t count)
{
	return read (s->fd, buf, count);
}

static ssize_t io_internal_read (struct io_stream *s, void *buf, size_t count)
{
	ssize_t res = 0;
	
	assert (s != NULL);
	assert (buf != NULL);

#ifdef HAVE_MMAP
	if (s->source == IO_SOURCE_MMAP)
		res = io_read_mmap (s, buf, count);
	else
#endif
	if (s->source == IO_SOURCE_FD)
		res = io_read_fd (s, buf, count);
	else
		fatal ("Unknown io_stream->source: %d", s->source);

	return res;
}

#ifdef HAVE_MMAP
static off_t io_seek_mmap (struct io_stream *s, const long where)
{
	assert (where >= 0 && where <= (long)s->size);

	return (s->mem_pos = where);
}
#endif

static off_t io_seek_fd (struct io_stream *s, const int where)
{
	return lseek (s->fd, where, SEEK_SET);
}

static off_t io_seek_buffered (struct io_stream *s, const long where)
{
	off_t res;

	logit ("Seeking...");

#ifdef HAVE_MMAP
	if (s->source == IO_SOURCE_MMAP)
		res = io_seek_mmap (s, where);
	else
#endif
	if (s->source == IO_SOURCE_FD)
		res = io_seek_fd (s, where);
	else
		fatal ("Unknown io_stream->source: %d", s->source);
	
	LOCK (s->buf_mutex);
	fifo_buf_clear (&s->buf);
	pthread_cond_signal (&s->buf_free_cond);
	s->after_seek = 1;
	s->eof = 0;
	UNLOCK (s->buf_mutex);

	return res;
}

static off_t io_seek_unbuffered (struct io_stream *s, const long where)
{
	off_t res;
	
#ifdef HAVE_MMAP
	if (s->source == IO_SOURCE_MMAP)
		res = io_seek_mmap (s, where);
#endif
	if (s->source == IO_SOURCE_FD)
		res = io_seek_fd (s, where);

	return res;
}

off_t io_seek (struct io_stream *s, off_t offset, int whence)
{
	off_t res;
	off_t new_pos = 0;
	
	assert (s != NULL);

	LOCK (s->io_mutex);
	switch (whence) {
		case SEEK_SET:
			if (offset >= 0 && (size_t)offset < s->size)
				new_pos = offset;
			break;
		case SEEK_CUR:
			if ((ssize_t)s->mem_pos + offset >= 0
					&& s->pos + offset < s->size)
				new_pos = s->pos + offset;
			break;
		case SEEK_END:
			new_pos = s->size + offset;
			break;
		default:
			fatal ("Bad whence value: %d", whence);
	}
	
	if (s->buffered)
		res = io_seek_buffered (s, new_pos);
	else
		res = io_seek_unbuffered (s, new_pos);

	if (res != -1)
		s->pos = res;
	UNLOCK (s->io_mutex);
			
	if (res != -1)
		debug ("Seek to: %lu", (unsigned long)res);
	else
		logit ("Seek error");

/*	close (write_mirror_fd);
	sprintf (write_mirror_file, "write_mirror_%d", write_mirror_num++);
	write_mirror_fd = open (write_mirror_file, O_WRONLY | O_CREAT | O_TRUNC,
			0600);
	assert (write_mirror_fd != -1);
	debug ("Write mirror: %s", write_mirror_file);*/

	return res;
}

/* Abort an IO operation from another thread. */
void io_abort (struct io_stream *s)
{
	assert (s != NULL);

	logit ("Aborting...");
	
	LOCK (s->buf_mutex);
	s->stop_read_thread = 1;
	pthread_cond_broadcast (&s->buf_fill_cond);
	pthread_cond_broadcast (&s->buf_free_cond);
	UNLOCK (s->buf_mutex);

	logit ("done");
}

/* Close the stream and free all resources associated with it. */
void io_close (struct io_stream *s)
{
	assert (s != NULL);

	logit ("Closing stream...");

	if (s->buffered) {
		io_abort (s);

		logit ("Waiting for io_read_thread()...");
		pthread_join (s->read_thread, NULL);
		logit ("IO read thread exited");
	}
	
	if (s->opened) {
#ifdef HAVE_MMAP
		if (s->source == IO_SOURCE_MMAP && s->mem
				&& munmap(s->mem, s->size))
			logit ("munmap() failed: %s", strerror(errno));
#endif
		close (s->fd);
	}

	if (s->buffered) {
		fifo_buf_destroy (&s->buf);
		if (pthread_mutex_destroy(&s->buf_mutex))
			logit ("Destroying buf_mutex failed: %s",
					strerror(errno));
		if (pthread_mutex_destroy(&s->io_mutex))
			logit ("Destroying io_mutex failed: %s",
					strerror(errno));
		if (pthread_cond_destroy(&s->buf_free_cond))
			logit ("Destroying buf_free_cond faild: %s",
					strerror(errno));
		if (pthread_cond_destroy(&s->buf_fill_cond))
			logit ("Destroying buf_fill_cond faild: %s",
					strerror(errno));
	}

	if (s->strerror)
		free (s->strerror);
	free (s);

	// DEBUG !!!
/*	close (write_mirror_fd);*/

	logit ("done");
}

static void *io_read_thread (void *data)
{
	struct io_stream *s = (struct io_stream *)data;

	logit ("IO read thread created");

	while (!s->stop_read_thread) {
		char read_buf[8096];
		int read_buf_fill = 0;
		int read_buf_pos = 0;

		LOCK (s->io_mutex);
		debug ("Reading...");
		
		LOCK (s->buf_mutex);
		s->after_seek = 0;
		UNLOCK (s->buf_mutex);
		
		read_buf_fill = io_internal_read (s, read_buf,
				sizeof(read_buf));
		UNLOCK (s->io_mutex);
		debug ("Read %d bytes", (int)read_buf_fill);
		
		LOCK (s->buf_mutex);
		
		if (read_buf_fill < 0) {

			s->errno_val = errno;
			logit ("Exiting due tu read error.");
			UNLOCK (s->buf_mutex);
			break;
		}

		if (read_buf_fill == 0) {
			s->eof = 1;
			debug ("EOF, waiting");
			pthread_cond_broadcast (&s->buf_fill_cond);
			pthread_cond_wait (&s->buf_free_cond, &s->buf_mutex);
			debug ("Got signal");
			UNLOCK (s->buf_mutex);
			continue;
		}

		s->eof = 0;

		while (read_buf_pos < read_buf_fill && !s->after_seek) {
			int put;

			debug ("Buffer fill: %lu", (unsigned long)
					fifo_buf_get_fill(&s->buf));
			
			put = fifo_buf_put (&s->buf,
					read_buf + read_buf_pos,
					read_buf_fill - read_buf_pos);

			if (s->stop_read_thread)
				break;

			if (put > 0) {
				debug ("Put %d bytes into the buffer", put);
				pthread_cond_broadcast (&s->buf_fill_cond);
				read_buf_pos += put;
			}
			else {
				debug ("The buffer is full, waiting.");
				pthread_cond_wait (&s->buf_free_cond,
						&s->buf_mutex);
				debug ("Some space in the buffer was freed");
			}
		}

		UNLOCK (s->buf_mutex);
	}

	if (s->stop_read_thread)
		logit ("Stop request");
	
	logit ("Exiting IO read thread");

	return NULL;
}

/* Open the file. Return NULL on error. */
struct io_stream *io_open (const char *file, const int buffered)
{
	struct io_stream *s;
	struct stat file_stat;

	assert (file != NULL);

	s = xmalloc (sizeof(struct io_stream));
	s->errno_val = 0;
	s->strerror = NULL;
	
	if ((s->fd = open(file, O_RDONLY)) == -1) {
		s->errno_val = errno;
		return s;
	}

	if (fstat(s->fd, &file_stat) == -1) {
		s->errno_val = errno;
		return s;
	}

	s->size = file_stat.st_size;

#ifdef HAVE_MMAP
	if (options_get_int("UseMmap") && s->size > 0) {
		if ((s->mem = mmap(0, s->size, PROT_READ, MAP_SHARED,
						s->fd, 0)) == MAP_FAILED) {
			s->mem = NULL;
			logit ("mmap() failed: %s", strerror(errno));
			s->source = IO_SOURCE_FD;
		}
		else {
			logit ("mmap()ed %lu bytes", (unsigned long)s->size);
			s->source = IO_SOURCE_MMAP;
			s->mem_pos = 0;
		}
	}
	else {
		logit ("Not using mmap()");
		s->source = IO_SOURCE_FD;
	}
#else
	s->source = IO_SOURCE_FD;
#endif

	s->opened = 1;
	s->stop_read_thread = 0;
	s->eof = 0;
	s->after_seek = 0;
	s->buffered = buffered;
	s->pos = 0;

	// DEBUG !!!
/*	sprintf (write_mirror_file, "write_mirror_%d", write_mirror_num++);
	write_mirror_fd = open (write_mirror_file, O_WRONLY | O_CREAT | O_TRUNC,
			0600);
	assert (write_mirror_fd != -1);
	debug ("Write mirror: %s", write_mirror_file);*/

	if (buffered) {
		fifo_buf_init (&s->buf, options_get_int("InputBuffer") * 1024);
		
		pthread_mutex_init (&s->buf_mutex, NULL);
		pthread_mutex_init (&s->io_mutex, NULL);
		pthread_cond_init (&s->buf_free_cond, NULL);
		pthread_cond_init (&s->buf_fill_cond, NULL);

		if (pthread_create(&s->read_thread, NULL, io_read_thread, s))
			fatal ("Can't create read thread: %s", strerror(errno));
	}

	return s;
}

int io_ok (const struct io_stream *s)
{
	return !s->errno_val;
}

static ssize_t io_read_buffered (struct io_stream *s, void *buf, size_t count)
{
	ssize_t received = 0;

	LOCK (s->buf_mutex);

	while (received < (ssize_t)count && io_ok(s) && !s->stop_read_thread
			&& (!s->eof || fifo_buf_get_fill(&s->buf))) {
		if (fifo_buf_get_fill(&s->buf)) {
			received += fifo_buf_get (&s->buf, buf + received,
					count - received);
			debug ("Read %d bytes so far", (int)received);
			pthread_cond_signal (&s->buf_free_cond);
		}
		else {
			debug ("Buffer empty, waiting...");
			pthread_cond_wait (&s->buf_fill_cond, &s->buf_mutex);
		}
	}

	debug ("done");
	s->pos += received;

	UNLOCK (s->buf_mutex);
	
	return io_ok(s) ? received : -1;
}

static ssize_t io_read_unbuffered (struct io_stream *s, void *buf, size_t count)
{
	ssize_t res;
	
	res = io_internal_read (s, buf, count);
	s->pos += res;

	return res;
}

/* Read data from the string to the buffer of size count. Return the number
 * of bytes read, 0 on EOF, < 0 on error. */
ssize_t io_read (struct io_stream *s, void *buf, size_t count)
{
	ssize_t received;
	
	assert (s != NULL);
	assert (buf != NULL);

	debug ("Reading...");

	if (s->buffered)
		received = io_read_buffered (s, buf, count);
	else
		received = io_read_unbuffered (s, buf, count);

/*	write (write_mirror_fd, buf, received);*/

	return received;
}

/* Get the string describing the error associated with the stream. */
char *io_strerror (struct io_stream *s)
{
	char err[256];
	
	if (s->strerror)
		free (s->strerror);
	
	strerror_r (s->errno_val, err, sizeof(err));
	s->strerror = (char *)xmalloc (sizeof(char)*(strlen(err)+1));
	strcpy (s->strerror, err);

	return s->strerror;
}

/* Get the file size if available or -1. */
ssize_t io_file_size (const struct io_stream *s)
{
	assert (s != NULL);
	
	return s->size;
}

/* Return the stream position. */
long io_tell (struct io_stream *s)
{
	long res = -1;
	
	assert (s != NULL);

	if (s->buffered) {
		LOCK (s->buf_mutex);
		res = s->pos;
		UNLOCK (s->buf_mutex);
	}
	else
		res = s->pos;

	debug ("We are at %ld byte", res);

	return res;
}

/* Return != 0 if we are at the end of the stream. */
int io_eof (struct io_stream *s)
{
	int eof;

	assert (s != NULL);
	
	LOCK (s->buf_mutex);
	eof = s->eof;
	UNLOCK (s->buf_mutex);

	return eof;
}
