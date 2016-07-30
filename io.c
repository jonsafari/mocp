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
 * - handle SIGBUS (mmap() read error)
 */

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <strings.h>
#include <assert.h>
#include <pthread.h>
#include <inttypes.h>

#ifdef HAVE_MMAP
# include <sys/mman.h>
#endif

/*#define DEBUG*/

#include "common.h"
#include "log.h"
#include "io.h"
#include "options.h"
#include "files.h"
#ifdef HAVE_CURL
# include "io_curl.h"
#endif

#ifdef HAVE_CURL
# define CURL_ONLY
#else
# define CURL_ONLY ATTR_UNUSED
#endif

#ifdef HAVE_MMAP
static void *io_mmap_file (const struct io_stream *s)
{
	void *result = NULL;

	do {
		if (s->size < 1 || (uint64_t)s->size > SIZE_MAX) {
			logit ("File size unsuitable for mmap()");
			break;
		}

		const size_t sz = (size_t)s->size;

		result = mmap (0, sz, PROT_READ, MAP_SHARED, s->fd, 0);
		if (result == MAP_FAILED) {
			log_errno ("mmap() failed", errno);
			result = NULL;
			break;
		}

		logit ("mmap()ed %zu bytes", sz);
	} while (0);

	return result;
}
#endif

#ifdef HAVE_MMAP
static ssize_t io_read_mmap (struct io_stream *s, const int dont_move,
		void *buf, size_t count)
{
	struct stat file_stat;
	size_t to_read;

	assert (s->mem != NULL);

	if (fstat (s->fd, &file_stat) == -1) {
		log_errno ("fstat() failed", errno);
		return -1;
	}

	if (s->size != file_stat.st_size) {
		logit ("File size has changed");

		if (munmap (s->mem, (size_t)s->size)) {
			log_errno ("munmap() failed", errno);
			return -1;
		}

		s->size = file_stat.st_size;
		s->mem = io_mmap_file (s);
		if (!s->mem)
			return -1;

		if (s->mem_pos > s->size)
			logit ("File shrunk");
	}

	if (s->mem_pos >= s->size)
		return 0;

	to_read = MIN(count, (size_t) (s->size - s->mem_pos));
	memcpy (buf, (char *)s->mem + s->mem_pos, to_read);

	if (!dont_move)
		s->mem_pos += to_read;

	return to_read;
}
#endif

static ssize_t io_read_fd (struct io_stream *s, const int dont_move, void *buf,
		size_t count)
{
	ssize_t res;

	res = read (s->fd, buf, count);

	if (res < 0)
		return -1;

	if (dont_move && lseek(s->fd, -res, SEEK_CUR) < 0)
		return -1;

	return res;
}

/* Read the data from the stream resource.  If dont_move was set, the stream
 * position is unchanged. */
static ssize_t io_internal_read (struct io_stream *s, const int dont_move,
		char *buf, size_t count)
{
	ssize_t res = 0;

	assert (s != NULL);
	assert (buf != NULL);

	switch (s->source) {
	case IO_SOURCE_FD:
		res = io_read_fd (s, dont_move, buf, count);
		break;
#ifdef HAVE_MMAP
	case IO_SOURCE_MMAP:
		res = io_read_mmap (s, dont_move, buf, count);
		break;
#endif
#ifdef HAVE_CURL
	case IO_SOURCE_CURL:
		if (dont_move)
			fatal ("You can't peek data directly from CURL!");
		res = io_curl_read (s, buf, count);
		break;
#endif
	default:
		fatal ("Unknown io_stream->source: %d", s->source);
	}

	return res;
}

#ifdef HAVE_MMAP
static off_t io_seek_mmap (struct io_stream *s, const off_t where)
{
	return (s->mem_pos = where);
}
#endif

static off_t io_seek_fd (struct io_stream *s, const off_t where)
{
	return lseek (s->fd, where, SEEK_SET);
}

static off_t io_seek_buffered (struct io_stream *s, const off_t where)
{
	off_t res = -1;

	assert (s->source != IO_SOURCE_CURL);

	logit ("Seeking...");

	switch (s->source) {
	case IO_SOURCE_FD:
		res = io_seek_fd (s, where);
		break;
#ifdef HAVE_MMAP
	case IO_SOURCE_MMAP:
		res = io_seek_mmap (s, where);
		break;
#endif
	default:
		fatal ("Unknown io_stream->source: %d", s->source);
	}

	LOCK (s->buf_mtx);
	fifo_buf_clear (s->buf);
	pthread_cond_signal (&s->buf_free_cond);
	s->after_seek = 1;
	s->eof = 0;
	UNLOCK (s->buf_mtx);

	return res;
}

static off_t io_seek_unbuffered (struct io_stream *s, const off_t where)
{
	off_t res = -1;

	assert (s->source != IO_SOURCE_CURL);

	switch (s->source) {
#ifdef HAVE_MMAP
	case IO_SOURCE_MMAP:
		res = io_seek_mmap (s, where);
		break;
#endif
	case IO_SOURCE_FD:
		res = io_seek_fd (s, where);
		break;
	default:
		fatal ("Unknown io_stream->source: %d", s->source);
	}

	return res;
}

off_t io_seek (struct io_stream *s, off_t offset, int whence)
{
	off_t res, new_pos = 0;

	assert (s != NULL);
	assert (s->opened);

	if (s->source == IO_SOURCE_CURL || !io_ok(s))
		return -1;

	LOCK (s->io_mtx);
	switch (whence) {
	case SEEK_SET:
		new_pos = offset;
		break;
	case SEEK_CUR:
		new_pos = s->pos + offset;
		break;
	case SEEK_END:
		new_pos = s->size + offset;
		break;
	default:
		fatal ("Bad whence value: %d", whence);
	}

	new_pos = CLAMP(0, new_pos, s->size);

	if (s->buffered)
		res = io_seek_buffered (s, new_pos);
	else
		res = io_seek_unbuffered (s, new_pos);

	if (res != -1)
		s->pos = res;
	UNLOCK (s->io_mtx);

	if (res != -1)
		debug ("Seek to: %"PRId64, res);
	else
		logit ("Seek error");

	return res;
}

/* Wake up the IO reading thread. */
static void io_wake_up (struct io_stream *s CURL_ONLY)
{
#ifdef HAVE_CURL
	if (s->source == IO_SOURCE_CURL)
		io_curl_wake_up (s);
#endif
}

/* Abort an IO operation from another thread. */
void io_abort (struct io_stream *s)
{
	assert (s != NULL);

	if (s->buffered && !s->stop_read_thread) {
		logit ("Aborting...");
		LOCK (s->buf_mtx);
		s->stop_read_thread = 1;
		io_wake_up (s);
		pthread_cond_broadcast (&s->buf_fill_cond);
		pthread_cond_broadcast (&s->buf_free_cond);
		UNLOCK (s->buf_mtx);
		logit ("done");
	}
}

/* Close the stream and free all resources associated with it. */
void io_close (struct io_stream *s)
{
	int rc;

	assert (s != NULL);

	logit ("Closing stream...");

	if (s->opened) {
		if (s->buffered) {
			io_abort (s);

			logit ("Waiting for io_read_thread()...");
			pthread_join (s->read_thread, NULL);
			logit ("IO read thread exited");
		}

		switch (s->source) {
		case IO_SOURCE_FD:
			close (s->fd);
			break;
#ifdef HAVE_MMAP
		case IO_SOURCE_MMAP:
			if (s->mem && munmap (s->mem, (size_t)s->size))
				log_errno ("munmap() failed", errno);
			close (s->fd);
			break;
#endif
#ifdef HAVE_CURL
		case IO_SOURCE_CURL:
			io_curl_close (s);
			break;
#endif
		default:
			fatal ("Unknown io_stream->source: %d", s->source);
		}

		s->opened = 0;

		if (s->buffered) {
			fifo_buf_free (s->buf);
			s->buf = NULL;
			rc = pthread_cond_destroy (&s->buf_free_cond);
			if (rc != 0)
				log_errno ("Destroying buf_free_cond failed", rc);
			rc = pthread_cond_destroy (&s->buf_fill_cond);
			if (rc != 0)
				log_errno ("Destroying buf_fill_cond failed", rc);
		}

		if (s->metadata.title)
			free (s->metadata.title);
		if (s->metadata.url)
			free (s->metadata.url);
	}

	rc = pthread_mutex_destroy (&s->buf_mtx);
	if (rc != 0)
		log_errno ("Destroying buf_mtx failed", rc);
	rc = pthread_mutex_destroy (&s->io_mtx);
	if (rc != 0)
		log_errno ("Destroying io_mtx failed", rc);
	rc = pthread_mutex_destroy (&s->metadata.mtx);
	if (rc != 0)
		log_errno ("Destroying metadata.mtx failed", rc);

	if (s->strerror)
		free (s->strerror);
	free (s);

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

		LOCK (s->io_mtx);
		debug ("Reading...");

		LOCK (s->buf_mtx);
		s->after_seek = 0;
		UNLOCK (s->buf_mtx);

		read_buf_fill = io_internal_read (s, 0, read_buf, sizeof(read_buf));
		UNLOCK (s->io_mtx);
		if (read_buf_fill > 0)
			debug ("Read %d bytes", read_buf_fill);

		LOCK (s->buf_mtx);

		if (s->stop_read_thread) {
			UNLOCK (s->buf_mtx);
			break;
		}

		if (read_buf_fill < 0) {
			s->errno_val = errno;
			s->read_error = 1;
			logit ("Exiting due to read error.");
			pthread_cond_broadcast (&s->buf_fill_cond);
			UNLOCK (s->buf_mtx);
			break;
		}

		if (read_buf_fill == 0) {
			s->eof = 1;
			debug ("EOF, waiting");
			pthread_cond_broadcast (&s->buf_fill_cond);
			pthread_cond_wait (&s->buf_free_cond, &s->buf_mtx);
			debug ("Got signal");
			UNLOCK (s->buf_mtx);
			continue;
		}

		s->eof = 0;

		while (read_buf_pos < read_buf_fill && !s->after_seek) {
			size_t put;

			debug ("Buffer fill: %zu", fifo_buf_get_fill (s->buf));

			put = fifo_buf_put (s->buf,
					read_buf + read_buf_pos,
					read_buf_fill - read_buf_pos);

			if (s->stop_read_thread)
				break;

			if (put > 0) {
				debug ("Put %zu bytes into the buffer", put);
				if (s->buf_fill_callback) {
					UNLOCK (s->buf_mtx);
					s->buf_fill_callback (s,
						fifo_buf_get_fill (s->buf),
						fifo_buf_get_size (s->buf),
						s->buf_fill_callback_data);
					LOCK (s->buf_mtx);
				}
				pthread_cond_broadcast (&s->buf_fill_cond);
				read_buf_pos += put;
				continue;
			}

			debug ("The buffer is full, waiting.");
			pthread_cond_wait (&s->buf_free_cond, &s->buf_mtx);
			debug ("Some space in the buffer was freed");
		}

		UNLOCK (s->buf_mtx);
	}

	if (s->stop_read_thread)
		logit ("Stop request");

	logit ("Exiting IO read thread");

	return NULL;
}

static void io_open_file (struct io_stream *s, const char *file)
{
	struct stat file_stat;

	s->source = IO_SOURCE_FD;

	do {
		s->fd = open (file, O_RDONLY);
		if (s->fd == -1) {
			s->errno_val = errno;
			break;
		}

		if (fstat (s->fd, &file_stat) == -1) {
			s->errno_val = errno;
			close (s->fd);
			break;
		}

		s->size = file_stat.st_size;
		s->opened = 1;

#ifdef HAVE_MMAP
		if (!options_get_bool ("UseMMap")) {
			logit ("Not using mmap()");
			s->mem = NULL;
			break;
		}

		s->mem = io_mmap_file (s);
		if (!s->mem)
			break;

		s->source = IO_SOURCE_MMAP;
		s->mem_pos = 0;
#endif
	} while (0);
}

/* Open the file. */
struct io_stream *io_open (const char *file, const int buffered)
{
	int rc;
	struct io_stream *s;

	assert (file != NULL);

	s = xmalloc (sizeof(struct io_stream));
	s->errno_val = 0;
	s->read_error = 0;
	s->strerror = NULL;
	s->opened = 0;
	s->size = -1;
	s->buf_fill_callback = NULL;
	memset (&s->metadata, 0, sizeof(s->metadata));

#ifdef HAVE_CURL
	s->curl.mime_type = NULL;
	if (is_url (file))
		io_curl_open (s, file);
	else
#endif
	io_open_file (s, file);

	pthread_mutex_init (&s->buf_mtx, NULL);
	pthread_mutex_init (&s->io_mtx, NULL);
	pthread_mutex_init (&s->metadata.mtx, NULL);

	if (!s->opened)
		return s;

	s->stop_read_thread = 0;
	s->eof = 0;
	s->after_seek = 0;
	s->buffered = buffered;
	s->pos = 0;

	if (buffered) {
		s->buf = fifo_buf_new (options_get_int("InputBuffer") * 1024);
		s->prebuffer = options_get_int("Prebuffering") * 1024;

		pthread_cond_init (&s->buf_free_cond, NULL);
		pthread_cond_init (&s->buf_fill_cond, NULL);

		rc = pthread_create (&s->read_thread, NULL, io_read_thread, s);
		if (rc != 0)
			fatal ("Can't create read thread: %s", xstrerror (errno));
	}

	return s;
}

/* Return non-zero if the stream was free of errors. */
static int io_ok_nolock (struct io_stream *s)
{
	return !s->read_error && s->errno_val == 0;
}

/* Return non-zero if the stream was free of errors. */
int io_ok (struct io_stream *s)
{
	int res;

	LOCK (s->buf_mtx);
	res = io_ok_nolock (s);
	UNLOCK (s->buf_mtx);

	return res;
}

/* Read data from the buffer without removing them, so stream position is
 * unchanged. You can't peek more data than the buffer size. */
static ssize_t io_peek_internal (struct io_stream *s, void *buf, size_t count)
{
	ssize_t received = 0;

	debug ("Peeking data...");

	LOCK (s->buf_mtx);

	/* Wait until enough data will be available */
	while (io_ok_nolock(s) && !s->stop_read_thread
			&& count > fifo_buf_get_fill (s->buf)
			&& fifo_buf_get_space (s->buf)
			&& !s->eof) {
		debug ("waiting...");
		pthread_cond_wait (&s->buf_fill_cond, &s->buf_mtx);
	}

	received = fifo_buf_peek (s->buf, buf, count);
	debug ("Read %zd bytes", received);

	UNLOCK (s->buf_mtx);

	return io_ok(s) ? received : -1;
}

/* Wait until there are s->prebuffer bytes in the buffer or some event
 * occurs which prevents prebuffering. */
void io_prebuffer (struct io_stream *s, const size_t to_fill)
{
	logit ("prebuffering to %zu bytes...", to_fill);

	LOCK (s->buf_mtx);
	while (io_ok_nolock(s) && !s->stop_read_thread && !s->eof
	                       && to_fill > fifo_buf_get_fill(s->buf)) {
		debug ("waiting (buffer %zu bytes full)", fifo_buf_get_fill (s->buf));
		pthread_cond_wait (&s->buf_fill_cond, &s->buf_mtx);
	}
	UNLOCK (s->buf_mtx);

	logit ("done");
}

static ssize_t io_read_buffered (struct io_stream *s, void *buf, size_t count)
{
	ssize_t received = 0;

	LOCK (s->buf_mtx);

	while (received < (ssize_t)count && !s->stop_read_thread
			&& ((!s->eof && !s->read_error)
				|| fifo_buf_get_fill(s->buf))) {
		if (fifo_buf_get_fill(s->buf)) {
			received += fifo_buf_get (s->buf, (char *)buf + received,
					count - received);
			debug ("Read %zd bytes so far", received);
			pthread_cond_signal (&s->buf_free_cond);
			continue;
		}

		debug ("Buffer empty, waiting...");
		pthread_cond_wait (&s->buf_fill_cond, &s->buf_mtx);
	}

	debug ("done");
	s->pos += received;

	UNLOCK (s->buf_mtx);

	return received ? received : (s->read_error ? -1 : 0);
}

/* Read data from the stream without buffering. If dont_move was set, the
 * stream position is unchanged. */
static ssize_t io_read_unbuffered (struct io_stream *s, const int dont_move,
		void *buf, size_t count)
{
	ssize_t res;

	assert (!s->eof);

	res = io_internal_read (s, dont_move, buf, count);

	if (!dont_move) {
		s->pos += res;
		if (res == 0)
			s->eof = 1;
	}

	return res;
}

/* Read data from the stream to the buffer of size count.  Return the number
 * of bytes read, 0 on EOF, < 0 on error. */
ssize_t io_read (struct io_stream *s, void *buf, size_t count)
{
	ssize_t received;

	assert (s != NULL);
	assert (buf != NULL);
	assert (s->opened);

	debug ("Reading...");

	if (s->buffered)
		received = io_read_buffered (s, buf, count);
	else if (s->eof)
		received = 0;
	else
		received = io_read_unbuffered (s, 0, buf, count);

	return received;
}

/* Read data from the stream to the buffer of size count. The data are not
 * removed from the stream. Return the number of bytes read, 0 on EOF, < 0
 * on error. */
ssize_t io_peek (struct io_stream *s, void *buf, size_t count)
{
	ssize_t received;

	assert (s != NULL);
	assert (buf != NULL);

	debug ("Reading...");

	if (s->buffered)
		received = io_peek_internal (s, buf, count);
	else
		received = io_read_unbuffered (s, 1, buf, count);

	return io_ok(s) ? received : -1;
}

/* Get the string describing the error associated with the stream. */
char *io_strerror (struct io_stream *s)
{
	if (s->strerror)
		free (s->strerror);

#ifdef HAVE_CURL
	if (s->source == IO_SOURCE_CURL)
		io_curl_strerror (s);
	else
#endif
	if (s->errno_val)
		s->strerror = xstrerror (s->errno_val);
	else
		s->strerror = xstrdup ("OK");

	return s->strerror;
}

/* Get the file size if available or -1. */
off_t io_file_size (const struct io_stream *s)
{
	assert (s != NULL);

	return s->size;
}

/* Return the stream position. */
off_t io_tell (struct io_stream *s)
{
	off_t res = -1;

	assert (s != NULL);

	if (s->buffered) {
		LOCK (s->buf_mtx);
		res = s->pos;
		UNLOCK (s->buf_mtx);
	}
	else
		res = s->pos;

	debug ("We are at byte %"PRId64, res);

	return res;
}

/* Return != 0 if we are at the end of the stream. */
int io_eof (struct io_stream *s)
{
	int eof;

	assert (s != NULL);

	LOCK (s->buf_mtx);
	eof = (s->eof && (!s->buffered || !fifo_buf_get_fill(s->buf))) ||
		s->stop_read_thread;
	UNLOCK (s->buf_mtx);

	return eof;
}

void io_init ()
{
#ifdef HAVE_CURL
	io_curl_init ();
#endif
}

void io_cleanup ()
{
#ifdef HAVE_CURL
	io_curl_cleanup ();
#endif
}

/* Return the mime type if available or NULL.
 * The mime type is read by curl only after the first read (or peek), until
 * then it's NULL. */
char *io_get_mime_type (struct io_stream *s CURL_ONLY)
{
#ifdef HAVE_CURL
	return s->curl.mime_type;
#else
	return NULL;
#endif
}

/* Return the malloc()ed stream title if available or NULL. */
char *io_get_metadata_title (struct io_stream *s)
{
	char *t;

	LOCK (s->metadata.mtx);
	t = xstrdup (s->metadata.title);
	UNLOCK (s->metadata.mtx);

	return t;
}

/* Return the malloc()ed stream url (from metadata) if available or NULL. */
char *io_get_metadata_url (struct io_stream *s)
{
	char *t;

	LOCK (s->metadata.mtx);
	t = xstrdup (s->metadata.url);
	UNLOCK (s->metadata.mtx);

	return t;
}

/* Set the metadata title of the stream. */
void io_set_metadata_title (struct io_stream *s, const char *title)
{
	LOCK (s->metadata.mtx);
	if (s->metadata.title)
		free (s->metadata.title);
	s->metadata.title = xstrdup (title);
	UNLOCK (s->metadata.mtx);
}

/* Set the metadata url for the stream. */
void io_set_metadata_url (struct io_stream *s, const char *url)
{
	LOCK (s->metadata.mtx);
	if (s->metadata.url)
		free (s->metadata.url);
	s->metadata.url = xstrdup (url);
	UNLOCK (s->metadata.mtx);
}

/* Set the callback function to be invoked when the fill of the buffer
 * changes.  data_ptr is a pointer passed to this function along with
 * the pointer to the stream. */
void io_set_buf_fill_callback (struct io_stream *s,
		buf_fill_callback_t callback, void *data_ptr)
{
	assert (s != NULL);
	assert (callback != NULL);

	LOCK (s->buf_mtx);
	s->buf_fill_callback = callback;
	s->buf_fill_callback_data = data_ptr;
	UNLOCK (s->buf_mtx);
}

/* Return a non-zero value if the stream is seekable. */
int io_seekable (const struct io_stream *s)
{
	return s->source == IO_SOURCE_FD || s->source == IO_SOURCE_MMAP;
}
