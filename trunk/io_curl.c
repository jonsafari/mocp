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

#include <curl/curl.h>
#include <string.h>
#include <stdlib.h>
#ifdef HAVE_SYS_SELECT_H
# include <sys/select.h>
#endif
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#include <errno.h>
#include <assert.h>

#define DEBUG

#include "main.h"
#include "log.h"
#include "io.h"
#include "io_curl.h"

/* TODO:
 *   - proxy support
 */

void io_curl_init ()
{
	curl_global_init (CURL_GLOBAL_NOTHING);
}

void io_curl_cleanup ()
{
	curl_global_cleanup ();
}

static size_t write_callback (void *data, size_t size, size_t nmemb,
		void *stream)
{
	struct io_stream *s = (struct io_stream *)stream;
	size_t buf_start = s->curl.buf_fill;
	size_t data_size = size * nmemb;

	s->curl.buf_fill += data_size;
	debug ("Got %lu bytes", (unsigned long)data_size);
	s->curl.buf = (char *)xrealloc (s->curl.buf, s->curl.buf_fill);
	memcpy (s->curl.buf + buf_start, data, data_size);

	return s->curl.buf_fill;
}

static size_t header_callback (void *data, size_t size, size_t nmemb,
		void *stream)
{
	struct io_stream *s = (struct io_stream *)stream;
	char *header;
	size_t header_size;

	assert (s != NULL);

	if (size * nmemb <= 2)
		return size * nmemb;
		
	/* we dont need '\r\n', so cut it. */
	header_size = sizeof(char) * (size * nmemb + 1 - 2);

	/* copy the header to char* array*/
	header = (char *)xmalloc (header_size);
	memcpy (header, data, size * nmemb - 2);
	header[header_size-1] = 0;

	if (!strncasecmp(header, "Content-Type:", sizeof("Content-Type:")-1)) {
		if (s->curl.mime_type)
			logit ("Another Content-Type header!");
		else {
			char *value = header + sizeof("Content-Type:") - 1;

			while (isblank(value[0]))
				value++;
			
			s->curl.mime_type = xstrdup (value);
			debug ("Mime type: '%s'", s->curl.mime_type);
		}
	}
	else if (!strncasecmp(header, "icy-name:", sizeof("icy-name:")-1)
			|| !strncasecmp(header, "x-audiocast-name",
				sizeof("x-audiocast-name")-1)) {
		char *value = strchr (header, ':') + 1;
		
		if (s->title)
			free (s->title);

		while (isblank(value[0]))
			value++;

		s->title = xstrdup (value);
	}

	free (header);
	
	return size * nmemb;
}

static int debug_callback (CURL *curl ATTR_UNUSED, curl_infotype i, char *msg,
		size_t size, void *d ATTR_UNUSED)
{
	if (i == CURLINFO_TEXT || i == CURLINFO_HEADER_IN
			|| i == CURLINFO_HEADER_OUT) {
		char *type;
		char *log = (char *)xmalloc (size + 1);

		switch (i) {
			case CURLINFO_TEXT:
				type = "INFO";
				break;
			case CURLINFO_HEADER_IN:
				type = "RECV HEADER";
				break;
			case CURLINFO_HEADER_OUT:
				type = "SEND HEADER";
				break;
			default:
				type = "";
		}
		
		strncpy (log, msg, size);
		if (size > 0 && log[size-1] == '\n')
			log[size-1] = 0;
		else
			log[size] = 0;
		debug ("CURL: [%s] %s", type, log);
		free (log);
	}
	
	return 0;
}

/* Read messages given by curl and set the stream status. Return 0 on error. */
static int check_curl_stream (struct io_stream *s)
{
	CURLMsg *msg;
	int msg_queue_num;
	int res = 1;
	
	while ((msg = curl_multi_info_read(s->curl.multi_handle,
					&msg_queue_num)))
		if (msg->msg == CURLMSG_DONE) {
			s->curl.status = msg->data.result;
			if (s->curl.status != CURLE_OK) {
				debug ("Read error");
				res = 0;
			}
			curl_multi_remove_handle (s->curl.multi_handle,
					s->curl.handle);
			curl_easy_cleanup (s->curl.handle);
			s->curl.handle = NULL;
			debug ("EOF");
			break;
		}

	return res;
}

void io_curl_open (struct io_stream *s, const char *url)
{
	s->source = IO_SOURCE_CURL;
	s->curl.url = NULL;
	s->curl.http_headers = NULL;
	s->curl.buf = NULL;
	s->curl.buf_fill = 0;
	s->curl.need_perform_loop = 1;

	s->curl.wake_up_pipe[0] = -1;
	s->curl.wake_up_pipe[1] = -1;

	if (!(s->curl.multi_handle = curl_multi_init())) {
		logit ("curl_multi_init() returned NULL");
		return;
	}
	
	if (!(s->curl.handle = curl_easy_init())) {
		logit ("curl_easy_init() returned NULL");
		return;
	}

	s->curl.multi_status = CURLM_OK;
	s->curl.status = CURLE_OK;

	s->curl.url = xstrdup (url);

	s->curl.http200_aliases = curl_slist_append (NULL, "ICY");
	/*s->curl.http_headers = curl_slist_append (NULL, "Icy-MetaData: 1");*/

	curl_easy_setopt (s->curl.handle, CURLOPT_NOPROGRESS, 1);
	curl_easy_setopt (s->curl.handle, CURLOPT_WRITEFUNCTION,
			write_callback);
	curl_easy_setopt (s->curl.handle, CURLOPT_WRITEDATA, s);
	curl_easy_setopt (s->curl.handle, CURLOPT_HEADERFUNCTION,
			header_callback);
	curl_easy_setopt (s->curl.handle, CURLOPT_WRITEHEADER, s);
	curl_easy_setopt (s->curl.handle, CURLOPT_USERAGENT,
			PACKAGE_NAME"/"PACKAGE_VERSION);
	curl_easy_setopt (s->curl.handle, CURLOPT_FOLLOWLOCATION, 1);
	curl_easy_setopt (s->curl.handle, CURLOPT_URL, s->curl.url);
	curl_easy_setopt (s->curl.handle, CURLOPT_FOLLOWLOCATION, 1);
	curl_easy_setopt (s->curl.handle, CURLOPT_FAILONERROR, 1);
	curl_easy_setopt (s->curl.handle, CURLOPT_MAXREDIRS, 15);
	curl_easy_setopt (s->curl.handle, CURLOPT_HTTP200ALIASES,
			s->curl.http200_aliases);
	/*curl_easy_setopt (s->curl.handle, CURLOPT_HTTPHEADER,
			s->curl.http_headers);*/
#ifdef DEBUG
	curl_easy_setopt (s->curl.handle, CURLOPT_VERBOSE, 1);
	curl_easy_setopt (s->curl.handle, CURLOPT_DEBUGFUNCTION,
			debug_callback);
#endif
	

	if ((s->curl.multi_status = curl_multi_add_handle(s->curl.multi_handle,
					s->curl.handle)) != CURLM_OK) {
		logit ("curl_multi_add_handle() failed");
		return;
	}

	if (pipe(s->curl.wake_up_pipe) < 0) {
		logit ("pipe() failed: %s", strerror(errno));
		return;
	}

	s->opened = 1;
}

void io_curl_close (struct io_stream *s)
{
	assert (s != NULL);
	assert (s->source == IO_SOURCE_CURL);

	if (s->curl.url)
		free (s->curl.url);
	if (s->curl.http_headers)
		curl_slist_free_all (s->curl.http_headers);
	if (s->curl.buf)
		free (s->curl.buf);
	if (s->curl.mime_type)
		free (s->curl.mime_type);

	if (s->curl.multi_handle && s->curl.handle)
		curl_multi_remove_handle (s->curl.multi_handle, s->curl.handle);
	if (s->curl.handle)
		curl_easy_cleanup (s->curl.handle);
	if (s->curl.multi_handle)
		curl_multi_cleanup (s->curl.multi_handle);

	if (s->curl.wake_up_pipe[0] != -1) {
		close (s->curl.wake_up_pipe[0]);
		close (s->curl.wake_up_pipe[1]);
	}

	if (s->curl.http200_aliases)
		curl_slist_free_all (s->curl.http200_aliases);
}

/* Get data using curl and put them into the internal buffer.
 * Return 0 on error. */
static int curl_read_internal (struct io_stream *s)
{
	int running = 1;

	if (s->curl.need_perform_loop) {
		debug ("Starting curl...");

		do {
			s->curl.multi_status = curl_multi_perform (
					s->curl.multi_handle, &running);
		} while (s->curl.multi_status == CURLM_CALL_MULTI_PERFORM);
		
		if (!check_curl_stream(s))
			return 0;

		s->curl.need_perform_loop = 0;
	}

	while (s->opened && !s->curl.buf_fill && running && s->curl.handle
			&& (s->curl.multi_status == CURLM_CALL_MULTI_PERFORM
				|| s->curl.multi_status == CURLM_OK)) {
		if (s->curl.multi_status != CURLM_CALL_MULTI_PERFORM) {
			fd_set read_fds, write_fds, exc_fds;
			int max_fd;
			int ret;

			logit ("Doing select()...");

			FD_ZERO (&read_fds);
			FD_ZERO (&write_fds);
			FD_ZERO (&exc_fds);

			s->curl.multi_status == curl_multi_fdset (
					s->curl.multi_handle,
					&read_fds, &write_fds, &exc_fds,
					&max_fd);
			if (s->curl.multi_status != CURLM_OK)
				logit ("curl_multi_fdset() failed");

			FD_SET (s->curl.wake_up_pipe[0], &read_fds);
			if (s->curl.wake_up_pipe[0] > max_fd)
				max_fd = s->curl.wake_up_pipe[0];

			ret = select (max_fd + 1, &read_fds, &write_fds,
					&exc_fds, NULL);

			if (ret < 0 && errno == EINTR) {
				logit ("Interrupted");
				return 0;
			}
			
			if (ret < 0) {
				s->errno_val == errno;
				logit ("select() failed");
				return 0;
			}

			if (FD_ISSET(s->curl.wake_up_pipe[0], &read_fds)) {
				logit ("Got wake up - exiting");
				return 0;
			}

			if (s->stop_read_thread)
				return 0;
		}

		s->curl.multi_status = curl_multi_perform (s->curl.multi_handle,
			&running);
		
		if (!check_curl_stream(s))
			return 0;
	}

	return 1;
}

/* Read data from the internal buffer to buf. Return the number of bytes read.
 */
static size_t read_from_buffer (struct io_stream *s, char *buf, size_t count)
{
	if (s->curl.buf_fill) {
		long to_copy = MIN ((long)count, s->curl.buf_fill);

		debug ("Copying %ld bytes", to_copy);

		memcpy (buf, s->curl.buf, to_copy);
		s->curl.buf_fill -= to_copy;

		if (s->curl.buf_fill) {
			memmove (s->curl.buf, s->curl.buf + to_copy,
					s->curl.buf_fill);
			s->curl.buf = (char *)xrealloc (s->curl.buf,
					s->curl.buf_fill);
		}
		else {
			free (s->curl.buf);
			s->curl.buf = NULL;
		}

		return to_copy;
	}
	
	return 0;
}

ssize_t io_curl_read (struct io_stream *s, char *buf, size_t count)
{
	size_t nread = 0;
	
	assert (s != NULL);
	assert (s->source == IO_SOURCE_CURL);
	assert (s->curl.multi_handle != NULL);

	do {
		nread += read_from_buffer (s, buf + nread, count - nread);

		if (nread < count && !curl_read_internal(s))
			return -1;
	} while (nread < count && s->curl.handle); /* s->curl.handle == NULL
						      on EOF */

	return nread;
}

/* Set the error string for the stream. */
void io_curl_strerror (struct io_stream *s)
{
	const char *err = "OK";
	
	assert (s != NULL);
	assert (s->source == IO_SOURCE_CURL);

	if (s->curl.multi_status != CURLM_OK)
		err = curl_multi_strerror(s->curl.multi_status);
	else if (s->curl.status != CURLE_OK)
		err = curl_easy_strerror(s->curl.status);

	s->strerror = xstrdup (err);
}

void io_curl_wake_up (struct io_stream *s)
{
	int w = 1;

	if (write(s->curl.wake_up_pipe[1], &w, sizeof(w)) < 0)
		logit ("Can't wake up curl thread: write() failed: %s",
				strerror(errno));
}
