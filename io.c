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
#include <signal.h>
#ifdef HAVE_MMAP
# include <sys/mman.h>
#endif

/*#define DEBUG*/

#include "common.h"
#include "log.h"
#include "io.h"
#include "options.h"
#ifdef HAVE_CURL
# include "io_curl.h"
#endif
#include "compat.h"

#ifdef HAVE_MMAP
static ssize_t io_read_mmap (struct io_stream *s, const int dont_move,
		void *buf, size_t count)
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

		s->size = file_stat.st_size;
		if ((s->mem = mmap(0, s->size, PROT_READ, MAP_SHARED, s->fd, 0))
					== MAP_FAILED) {
			logit ("mmap() filed: %s", strerror(errno));
			return -1;
		}
		
		logit ("mmap()ed %lu bytes", (unsigned long)s->size);
		if (s->mem_pos > s->size) {
			logit ("File shrinked");
			return 0;
		}
	}

	if (s->mem_pos >= s->size)
		return 0;
	
	to_read = MIN(count, s->size - s->mem_pos);
	memcpy (buf, s->mem + s->mem_pos, to_read);

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

/* Reat the data from the stream resource. If dont_move was set, the stream
 * position is unchanded. */
static ssize_t io_internal_read (struct io_stream *s, const int dont_move,
		char *buf, size_t count)
{
	ssize_t res = 0;
	
	assert (s != NULL);
	assert (buf != NULL);

#ifdef HAVE_MMAP
	if (s->source == IO_SOURCE_MMAP)
		res = io_read_mmap (s, dont_move, buf, count);
	else
#endif
#ifdef HAVE_CURL
	if (s->source == IO_SOURCE_CURL) {
		if (dont_move)
			fatal ("You can't peek data directly from curl");
		res = io_curl_read (s, buf, count);
	}
	else
#endif
	if (s->source == IO_SOURCE_FD)
		res = io_read_fd (s, dont_move, buf, count);
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
	off_t res = -1;

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
	off_t res = -1;
	
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
	assert (s->opened);

	if (s->source == IO_SOURCE_CURL || !io_ok(s))
		return -1;

	LOCK (s->io_mutex);
	switch (whence) {
		case SEEK_SET:
			if (offset >= 0 && (size_t)offset < s->size)
				new_pos = offset;
			break;
		case SEEK_CUR:
			if ((ssize_t)s->pos + offset >= 0
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

	return res;
}

/* Wake up the IO reading thread */
static void io_wake_up (struct io_stream *s ATTR_UNUSED)
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
		LOCK (s->buf_mutex);
		s->stop_read_thread = 1;
		io_wake_up (s);
		pthread_cond_broadcast (&s->buf_fill_cond);
		pthread_cond_broadcast (&s->buf_free_cond);
		UNLOCK (s->buf_mutex);
		logit ("done");
	}
}

/* Close the stream and free all resources associated with it. */
void io_close (struct io_stream *s)
{
	assert (s != NULL);

	logit ("Closing stream...");


	if (s->opened) {
		
		if (s->buffered) {
			io_abort (s);

			logit ("Waiting for io_read_thread()...");
			pthread_join (s->read_thread, NULL);
			logit ("IO read thread exited");
		}
	
#ifdef HAVE_MMAP
		if (s->source == IO_SOURCE_MMAP) {
			if (s->mem && munmap(s->mem, s->size))
				logit ("munmap() failed: %s", strerror(errno));
			close (s->fd);
		}
#endif
		
#ifdef HAVE_CURL
		if (s->source == IO_SOURCE_CURL)
			io_curl_close (s);
#endif
		if (s->source == IO_SOURCE_FD)
			close (s->fd);

		if (s->buffered) {
			fifo_buf_destroy (&s->buf);
			if (pthread_cond_destroy(&s->buf_free_cond))
				logit ("Destroying buf_free_cond faild: %s",
						strerror(errno));
			if (pthread_cond_destroy(&s->buf_fill_cond))
				logit ("Destroying buf_fill_cond faild: %s",
						strerror(errno));
		}

		if (s->metadata.title)
			free (s->metadata.title);
		if (s->metadata.url)
			free (s->metadata.url);
	}

	if (pthread_mutex_destroy(&s->buf_mutex))
		logit ("Destroying buf_mutex failed: %s", strerror(errno));
	if (pthread_mutex_destroy(&s->io_mutex))
		logit ("Destroying io_mutex failed: %s", strerror(errno));
	if (pthread_mutex_destroy(&s->metadata.mutex))
		logit ("Destroying metadata mutex failed: %s", strerror(errno));

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

		LOCK (s->io_mutex);
		debug ("Reading...");
		
		LOCK (s->buf_mutex);
		s->after_seek = 0;
		UNLOCK (s->buf_mutex);
		
		read_buf_fill = io_internal_read (s, 0, read_buf,
				sizeof(read_buf));
		UNLOCK (s->io_mutex);
		debug ("Read %d bytes", (int)read_buf_fill);
		
		LOCK (s->buf_mutex);

		if (s->stop_read_thread) {
			UNLOCK (s->buf_mutex);
			break;
		}
		
		if (read_buf_fill < 0) {

			s->errno_val = errno;
			s->read_error = 1;
			logit ("Exiting due tu read error.");
			pthread_cond_broadcast (&s->buf_fill_cond);
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
				if (s->buf_fill_callback) {
					UNLOCK (s->buf_mutex);
					s->buf_fill_callback (s,
						fifo_buf_get_fill(&s->buf),
						fifo_buf_get_size(&s->buf),
						s->buf_fill_callback_data);
					LOCK (s->buf_mutex);
				}
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

static void io_open_file (struct io_stream *s, const char *file)
{
	struct stat file_stat;

	if ((s->fd = open(file, O_RDONLY)) == -1)
		s->errno_val = errno;
	else if (fstat(s->fd, &file_stat) == -1)
		s->errno_val = errno;
	else {

		s->size = file_stat.st_size;

#ifdef HAVE_MMAP
		if (options_get_int("UseMmap") && s->size > 0) {
			if ((s->mem = mmap(0, s->size, PROT_READ, MAP_SHARED,
							s->fd, 0))
					== MAP_FAILED) {
				s->mem = NULL;
				logit ("mmap() failed: %s", strerror(errno));
				s->source = IO_SOURCE_FD;
			}
			else {
				logit ("mmap()ed %lu bytes",
						(unsigned long)s->size);
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
	}
}

/* Open the file. Return NULL on error. */
struct io_stream *io_open (const char *file, const int buffered)
{
	struct io_stream *s;

	assert (file != NULL);

	s = xmalloc (sizeof(struct io_stream));
	s->errno_val = 0;
	s->read_error = 0;
	s->strerror = NULL;
	s->opened = 0;
	s->buf_fill_callback = NULL;
	memset (&s->metadata, 0, sizeof(s->metadata));

#ifdef HAVE_CURL
	s->curl.mime_type = NULL;
	if (!strncasecmp(file, "http://", sizeof("http://")-1)
			|| !strncasecmp(file, "ftp://", sizeof("ftp://")-1))
		io_curl_open (s, file);
	else
#endif
	io_open_file (s, file);

	pthread_mutex_init (&s->buf_mutex, NULL);
	pthread_mutex_init (&s->io_mutex, NULL);
	pthread_mutex_init (&s->metadata.mutex, NULL);

	if (!s->opened)
		return s;
				
	s->stop_read_thread = 0;
	s->eof = 0;
	s->after_seek = 0;
	s->buffered = buffered;
	s->pos = 0;

	if (buffered) {
		fifo_buf_init (&s->buf, options_get_int("InputBuffer") * 1024);
		s->prebuffer = options_get_int("Prebuffering") * 1024;
		
		pthread_cond_init (&s->buf_free_cond, NULL);
		pthread_cond_init (&s->buf_fill_cond, NULL);

		if (pthread_create(&s->read_thread, NULL, io_read_thread, s))
			fatal ("Can't create read thread: %s", strerror(errno));
	}

	return s;
}

/* Return != 0 if the there were no errors in the stream. */
static int io_ok_nolock (struct io_stream *s)
{
	return !s->read_error && s->errno_val == 0;
}

/* Return != 0 if the there were no errors in the stream. */
int io_ok (struct io_stream *s)
{
	int res;
	
	LOCK (s->buf_mutex);
	res = io_ok_nolock (s);
	UNLOCK (s->buf_mutex);
	
	return res;
}

/* Read data from the buffer withoud removing them, so stream position is
 * unchanged. You can't peek more data than the buffer size. */
static ssize_t io_peek_internal (struct io_stream *s, void *buf, size_t count)
{
	ssize_t received = 0;

	debug ("Peeking data...");

	LOCK (s->buf_mutex);

	/* Wait until enough data will be available */
	while (io_ok_nolock(s) && !s->stop_read_thread
			&& count > fifo_buf_get_fill(&s->buf)
			&& fifo_buf_get_space (&s->buf)
			&& !s->eof) {
		debug ("waiting...");
		pthread_cond_wait (&s->buf_fill_cond, &s->buf_mutex);
	}

	received = fifo_buf_peek (&s->buf, buf, count);
	debug ("Read %d bytes", (int)received);

	UNLOCK (s->buf_mutex);
	
	return io_ok(s) ? received : -1;
}

/* Wait until there will be s->prebuffer bytes in the buffer or some event
 * occurs causing that the prebuffering is not possible. */
void io_prebuffer (struct io_stream *s, const size_t to_fill)
{
	logit ("prebuffering to %lu bytes...", (unsigned long)to_fill);
	
	LOCK (s->buf_mutex);
	while (io_ok_nolock(s) && !s->stop_read_thread && !s->eof
			&& to_fill > fifo_buf_get_fill(&s->buf)) {
		debug ("waiting (buffer %lu bytes full)",
				(unsigned long)fifo_buf_get_fill(&s->buf));
		pthread_cond_wait (&s->buf_fill_cond, &s->buf_mutex);
	}
	UNLOCK (s->buf_mutex);

	logit ("done");
}

static ssize_t io_read_buffered (struct io_stream *s, void *buf, size_t count)
{
	ssize_t received = 0;

	LOCK (s->buf_mutex);

	while (received < (ssize_t)count && !s->stop_read_thread
			&& ((!s->eof && !s->read_error)
				|| fifo_buf_get_fill(&s->buf))) {
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

/* Read data from the string to the buffer of size count. Return the number
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
	char err[256];
	
	if (s->strerror)
		free (s->strerror);
	
#ifdef HAVE_CURL
	if (s->source == IO_SOURCE_CURL)
		io_curl_strerror (s);
	else
#endif
	if (s->errno_val) {
		strerror_r (s->errno_val, err, sizeof(err));
		s->strerror = xstrdup (err);
	}
	else
		s->strerror = xstrdup ("OK");

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
	eof = (s->eof && (!s->buffered || !fifo_buf_get_fill(&s->buf))) ||
		s->stop_read_thread;
	UNLOCK (s->buf_mutex);

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
char *io_get_mime_type (struct io_stream *s ATTR_UNUSED)
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

	LOCK (s->metadata.mutex);
	t = xstrdup (s->metadata.title);
	UNLOCK (s->metadata.mutex);

	return t;
}

/* Return the malloc()ed stream url (from metadata) if available or NULL. */
char *io_get_metadata_url (struct io_stream *s)
{
	char *t;

	LOCK (s->metadata.mutex);
	t = xstrdup (s->metadata.url);
	UNLOCK (s->metadata.mutex);

	return t;
}

/* Set the metadata title of the stream. */
void io_set_metadata_title (struct io_stream *s, const char *title)
{
	LOCK (s->metadata.mutex);
	if (s->metadata.title)
		free (s->metadata.title);
	s->metadata.title = xstrdup (title);
	UNLOCK (s->metadata.mutex);
}

/* Set the metadata url for the stream. */
void io_set_metadata_url (struct io_stream *s, const char *url)
{
	LOCK (s->metadata.mutex);
	if (s->metadata.url)
		free (s->metadata.url);
	s->metadata.url = xstrdup (url);
	UNLOCK (s->metadata.mutex);
}

/* Set the callback function invokedwhen the fill of the buffer changes.
 * data_ptr is a pointer passed to this function along with the pointer
 * to the stream. */
void io_set_buf_fill_callback (struct io_stream *s,
		buf_fill_callback_t callback, void *data_ptr)
{
	assert (s != NULL);
	assert (callback != NULL);
	
	LOCK (s->buf_mutex);
	s->buf_fill_callback = callback;
	s->buf_fill_callback_data = data_ptr;
	UNLOCK (s->buf_mutex);
}

/* Return a non-zero value if the stream is seekable. */
int io_seekable (const struct io_stream *s)
{
	return s->source == IO_SOURCE_FD || s->source == IO_SOURCE_MMAP;
}

