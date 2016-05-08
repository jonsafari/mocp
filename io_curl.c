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
#include <strings.h>
#include <ctype.h>
#include <stdlib.h>
#include <sys/select.h>
#include <unistd.h>
#include <errno.h>
#include <assert.h>
#include <stdint.h>

#define DEBUG

#include "common.h"
#include "log.h"
#include "io.h"
#include "io_curl.h"
#include "options.h"
#include "lists.h"

static char user_agent[] = PACKAGE_NAME"/"PACKAGE_VERSION;

void io_curl_init ()
{
	char *ptr;

	for (ptr = user_agent; *ptr; ptr += 1) {
		if (*ptr == ' ')
			*ptr = '-';
	}

	curl_global_init (CURL_GLOBAL_NOTHING);
}

void io_curl_cleanup ()
{
	curl_global_cleanup ();
}

static size_t write_cb (void *data, size_t size, size_t nmemb,
		void *stream)
{
	struct io_stream *s = (struct io_stream *)stream;
	size_t buf_start = s->curl.buf_fill;
	size_t data_size = size * nmemb;

	s->curl.buf_fill += data_size;
	debug ("Got %zu bytes", data_size);
	s->curl.buf = (char *)xrealloc (s->curl.buf, s->curl.buf_fill);
	memcpy (s->curl.buf + buf_start, data, data_size);

	return data_size;
}

static size_t header_cb (void *data, size_t size, size_t nmemb,
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

	/* copy the header to char* array */
	header = (char *)xmalloc (header_size);
	memcpy (header, data, size * nmemb - 2);
	header[header_size-1] = 0;

	if (!strncasecmp(header, "Location:", sizeof("Location:")-1)) {
		s->curl.got_locn = 1;
	}
	else if (!strncasecmp(header, "Content-Type:", sizeof("Content-Type:")-1)) {
		/* If we got redirected then use the last MIME type. */
		if (s->curl.got_locn && s->curl.mime_type) {
			free (s->curl.mime_type);
			s->curl.mime_type = NULL;
		}
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

		while (isblank(value[0]))
			value++;

		io_set_metadata_title (s, value);
	}
	else if (!strncasecmp(header, "icy-url:", sizeof("icy-url:")-1)) {
		char *value = strchr (header, ':') + 1;

		while (isblank(value[0]))
			value++;

		io_set_metadata_url (s, value);
	}
	else if (!strncasecmp(header, "icy-metaint:",
				sizeof("icy-metaint:")-1)) {
		char *end;
		char *value = strchr (header, ':') + 1;

		while (isblank(value[0]))
			value++;

		s->curl.icy_meta_int = strtol (value, &end, 10);
		if (*end) {
			s->curl.icy_meta_int = 0;
			logit ("Bad icy-metaint value");
		}
		else
			debug ("Icy metadata interval: %zu", s->curl.icy_meta_int);
	}

	free (header);

	return size * nmemb;
}

#if !defined(NDEBUG) && defined(DEBUG)
static int debug_cb (CURL *unused1 ATTR_UNUSED, curl_infotype i,
                     char *msg, size_t size, void *unused2 ATTR_UNUSED)
{
	int ix;
	char *log;
	const char *type;
	lists_t_strs *lines;

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
		return 0;
	}

	log = (char *)xmalloc (size + 1);
	strncpy (log, msg, size);
	log[size] = 0;

	lines = lists_strs_new (8);
	lists_strs_split (lines, log, "\n");
	for (ix = 0; ix < lists_strs_size (lines); ix += 1)
		debug ("CURL: [%s] %s", type, lists_strs_at (lines, ix));
	lists_strs_free (lines);
	free (log);

	return 0;
}
#endif

/* Read messages given by curl and set the stream status. Return 0 on error. */
static int check_curl_stream (struct io_stream *s)
{
	CURLMsg *msg;
	int msg_queue_num;
	int res = 1;

	while ((msg = curl_multi_info_read (s->curl.multi_handle,
	                                    &msg_queue_num))) {
		if (msg->msg == CURLMSG_DONE) {
			s->curl.status = msg->data.result;
			if (s->curl.status != CURLE_OK) {
				debug ("Read error");
				res = 0;
			}
			curl_multi_remove_handle (s->curl.multi_handle, s->curl.handle);
			curl_easy_cleanup (s->curl.handle);
			s->curl.handle = NULL;
			debug ("EOF");
			break;
		}
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
	s->curl.got_locn = 0;

	s->curl.wake_up_pipe[0] = -1;
	s->curl.wake_up_pipe[1] = -1;

	if (!(s->curl.multi_handle = curl_multi_init())) {
		logit ("curl_multi_init() returned NULL");
		s->errno_val = EINVAL;
		return;
	}

	if (!(s->curl.handle = curl_easy_init())) {
		logit ("curl_easy_init() returned NULL");
		s->errno_val = EINVAL;
		return;
	}

	s->curl.multi_status = CURLM_OK;
	s->curl.status = CURLE_OK;

	s->curl.url = xstrdup (url);
	s->curl.icy_meta_int = 0;
	s->curl.icy_meta_count = 0;

	s->curl.http200_aliases = curl_slist_append (NULL, "ICY");
	s->curl.http_headers = curl_slist_append (NULL, "Icy-MetaData: 1");

	curl_easy_setopt (s->curl.handle, CURLOPT_NOPROGRESS, 1);
	curl_easy_setopt (s->curl.handle, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_0);
	curl_easy_setopt (s->curl.handle, CURLOPT_WRITEFUNCTION, write_cb);
	curl_easy_setopt (s->curl.handle, CURLOPT_WRITEDATA, s);
	curl_easy_setopt (s->curl.handle, CURLOPT_HEADERFUNCTION, header_cb);
	curl_easy_setopt (s->curl.handle, CURLOPT_WRITEHEADER, s);
	curl_easy_setopt (s->curl.handle, CURLOPT_USERAGENT, user_agent);
	curl_easy_setopt (s->curl.handle, CURLOPT_URL, s->curl.url);
	curl_easy_setopt (s->curl.handle, CURLOPT_FOLLOWLOCATION, 1);
	curl_easy_setopt (s->curl.handle, CURLOPT_FAILONERROR, 1);
	curl_easy_setopt (s->curl.handle, CURLOPT_MAXREDIRS, 15);
	curl_easy_setopt (s->curl.handle, CURLOPT_HTTP200ALIASES,
			s->curl.http200_aliases);
	curl_easy_setopt (s->curl.handle, CURLOPT_HTTPHEADER,
			s->curl.http_headers);
	if (options_get_str("HTTPProxy"))
		curl_easy_setopt (s->curl.handle, CURLOPT_PROXY,
				options_get_str("HTTPProxy"));
#if !defined(NDEBUG) && defined(DEBUG)
	curl_easy_setopt (s->curl.handle, CURLOPT_VERBOSE, 1);
	curl_easy_setopt (s->curl.handle, CURLOPT_DEBUGFUNCTION, debug_cb);
#endif

	if ((s->curl.multi_status = curl_multi_add_handle(s->curl.multi_handle,
					s->curl.handle)) != CURLM_OK) {
		logit ("curl_multi_add_handle() failed");
		s->errno_val = EINVAL;
		return;
	}

	if (pipe(s->curl.wake_up_pipe) < 0) {
		log_errno ("pipe() failed", errno);
		s->errno_val = EINVAL;
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
 * Even if the internal buffer is not empty, more data will be read.
 * Return 0 on error. */
static int curl_read_internal (struct io_stream *s)
{
	int running = 1;
	long buf_fill_before = s->curl.buf_fill;

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

	while (s->opened && running && buf_fill_before == s->curl.buf_fill
			&& s->curl.handle
			&& (s->curl.multi_status == CURLM_CALL_MULTI_PERFORM
				|| s->curl.multi_status == CURLM_OK)) {
		if (s->curl.multi_status != CURLM_CALL_MULTI_PERFORM) {
			fd_set read_fds, write_fds, exc_fds;
			int max_fd, ret;
			long milliseconds;
			struct timespec timeout;

			logit ("Doing pselect()...");

			FD_ZERO (&read_fds);
			FD_ZERO (&write_fds);
			FD_ZERO (&exc_fds);

			s->curl.multi_status = curl_multi_fdset (
					s->curl.multi_handle,
					&read_fds, &write_fds, &exc_fds,
					&max_fd);
			if (s->curl.multi_status != CURLM_OK)
				logit ("curl_multi_fdset() failed");

			FD_SET (s->curl.wake_up_pipe[0], &read_fds);
			if (s->curl.wake_up_pipe[0] > max_fd)
				max_fd = s->curl.wake_up_pipe[0];

			curl_multi_timeout (s->curl.multi_handle, &milliseconds);
			if (milliseconds <= 0)
				milliseconds = 1000;
			timeout.tv_sec = milliseconds / 1000;
			timeout.tv_nsec = (milliseconds % 1000L) * 1000000L;

			ret = pselect (max_fd + 1, &read_fds, &write_fds, &exc_fds,
			              &timeout, NULL);

			if (ret < 0 && errno == EINTR) {
				logit ("Interrupted");
				return 0;
			}

			if (ret < 0) {
				s->errno_val = errno;
				logit ("pselect() failed");
				return 0;
			}

			if (s->stop_read_thread)
				return 1;

			if (FD_ISSET(s->curl.wake_up_pipe[0], &read_fds)) {
				logit ("Got wake up - exiting");
				return 1;
			}

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

		/*debug ("Copying %ld bytes", to_copy);*/

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

/* Parse icy string in form: StreamTitle='my music';StreamUrl='www.x.com' */
static void parse_icy_string (struct io_stream *s, const char *str)
{
	const char *c = str;

	debug ("Got metadata string: %s", str);

	while (*c) {
		char name[64];
		char value[256];
		const char *t;

		/* get the name */
		t = c;
		while (*c && *c != '=')
			c++;
		if (*c != '=' || c - t >= ssizeof(name)) {
			logit ("malformed metadata");
			return;
		}
		strncpy (name, t, c - t);
		name[c - t] = 0;

		/* move to a char after ' */
		c++;
		if (*c != '\'') {
			logit ("malformed metadata");
			return;
		}
		c++;

		/* read the value  - it can contain a quotation mark so we
		 * recognize if it ends the value by checking if there is a
		 * semicolon after it or if it's the end of the string */
		t = c;
		while (*c && (*c != '\'' || (*(c+1) != ';' && *(c+1))))
			c++;

		if (!*c) {
			logit ("malformed metadata");
			return;
		}

		strncpy (value, t, MIN(c - t, ssizeof(value) - 1));
		value[MIN(c - t, ssizeof(value) - 1)] = 0;

		/* eat ' */
		c++;

		/* eat semicolon */
		if (*c == ';')
			c++;

		debug ("METADATA name: '%s' value: '%s'", name, value);

		if (!strcasecmp(name, "StreamTitle"))
			io_set_metadata_title (s, value);
		else if (!strcasecmp(name, "StreamUrl"))
			io_set_metadata_url (s, value);
		else
			logit ("Unknown metadata element '%s'", name);
	}
}

/* Parse the IceCast metadata packet. */
static void parse_icy_metadata (struct io_stream *s, const char *packet,
		const int size)
{
	const char *c = packet;

	while (c - packet < size) {
		const char *p = c;

		while (*c && c - packet < size)
			c++;
		if (c - packet < size && !*c)
			parse_icy_string (s, p);

		/* pass the padding */
		while (c - packet < size && !*c)
			c++;
	}
}

/* Read icy metadata from the curl stream. The stream should be at the
 * beginning of the metadata. Return 0 on error. */
static int read_icy_metadata (struct io_stream *s)
{
	uint8_t size_packet;
	int size;
	char *packet;

	/* read the packet size */
	if (s->curl.buf_fill == 0 && !curl_read_internal(s))
		return 0;
	if (read_from_buffer(s, (char *)&size_packet, sizeof(size_packet))
			== 0) {
		debug ("Got empty metadata packet");
		return 1;
	}

	if (size_packet == 0) {
		debug ("Got empty metadata packet");
		return 1;
	}

	size = size_packet * 16;

	/* make sure that the whole packet is in the buffer */
	while (s->curl.buf_fill < size && s->curl.handle
			&& !s->stop_read_thread)
		if (!curl_read_internal(s))
			return 0;

	if (s->curl.buf_fill < size) {
		logit ("Icy metadata packet broken");
		return 0;
	}

	packet = (char *)xmalloc (size);
	read_from_buffer (s, packet, size);
	debug ("Received metadata packet %d bytes long", size);
	parse_icy_metadata (s, packet, size);
	free (packet);

	return 1;
}

ssize_t io_curl_read (struct io_stream *s, char *buf, size_t count)
{
	size_t nread = 0;

	assert (s != NULL);
	assert (s->source == IO_SOURCE_CURL);
	assert (s->curl.multi_handle != NULL);

	do {
		size_t to_read;
		size_t res;

		if (s->curl.icy_meta_int && s->curl.icy_meta_count
				== s->curl.icy_meta_int) {
			s->curl.icy_meta_count = 0;
			if (!read_icy_metadata(s))
				return -1;
		}

		if (s->curl.icy_meta_int)
			to_read = MIN (count - nread, s->curl.icy_meta_int -
					s->curl.icy_meta_count);
		else
			to_read = count - nread;

		res = read_from_buffer (s, buf + nread, to_read);
		if (s->curl.icy_meta_int)
			s->curl.icy_meta_count += res;
		nread += res;
		debug ("Read %zu bytes from the buffer (%zu bytes full)", res, nread);

		if (nread < count && !curl_read_internal(s))
			return -1;
	} while (nread < count && !s->stop_read_thread
			&& s->curl.handle); /* s->curl.handle == NULL on EOF */

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
		log_errno ("Can't wake up curl thread: write() failed", errno);
}
