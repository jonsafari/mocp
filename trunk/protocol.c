/*
 * MOC - music on console
 * Copyright (C) 2003,2004,2005 Damian Pietras <daper@daper.net>
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

#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <time.h>
#include <assert.h>

#include "main.h"
#include "log.h"
#include "protocol.h"
#include "playlist.h"

/* Maximal socket name. */
#define UNIX_PATH_MAX	108
#define SOCKET_NAME	"socket2"

/* Create a socket name, return NULL if the name could not be created */
char *socket_name ()
{
	char *socket_name = create_file_name (SOCKET_NAME);

	if (strlen(socket_name) > UNIX_PATH_MAX)
		fatal ("Can't create socket name.");

	return socket_name;
}

/* Get an intiger value from the socket, return == 0 on error. */
int get_int (int sock, int *i)
{
	int res;
	
	res = recv (sock, i, sizeof(int), 0);
	if (res == -1)
		logit ("recv() failed when getting int: %s", strerror(errno));

	return res == sizeof(int) ? 1 : 0;
}

/* Send an integer value to the socket, return == 0 on error */
int send_int (int sock, int i)
{
	int res;
	
	res = send (sock, &i, sizeof(int), 0);
	if (res == -1)
		logit ("send() failed: %s", strerror(errno));

	return res == sizeof(int) ? 1 : 0;
}

/* Get a long value from the socket, return == 0 on error. */
static int get_long (int sock, long *i)
{
	int res;
	
	res = recv (sock, i, sizeof(long), 0);
	if (res == -1)
		logit ("recv() failed when getting int: %s", strerror(errno));

	return res == sizeof(long) ? 1 : 0;
}

#if 0
/* Send a long value to the socket, return == 0 on error */
static int send_long (int sock, long i)
{
	int res;
	
	res = send (sock, &i, sizeof(long), 0);
	if (res == -1)
		logit ("send() failed: %s", strerror(errno));

	return res == sizeof(long) ? 1 : 0;
}
#endif

/* Get the string from socket, return NULL on error. The memory is malloced. */
char *get_str (int sock)
{
	int len;
	int res, nread = 0;
	char *str;
	
	if (!get_int(sock, &len))
		return NULL;

	if (len < 0 || len > MAX_SEND_STRING) {
		logit ("Bad string length.");
		return NULL;
	}

	str = (char *)xmalloc (sizeof(char) * (len + 1));
	while (nread < len) {
		res = recv (sock, str + nread, len - nread, 0);
		if (res == -1) {
			logit ("recv() failed when getting string: %s",
					strerror(errno));
			free (str);
			return NULL;
		}
		if (res == 0) {
			logit ("Unexpected EOF when getting string");
			free (str);
			return NULL;
		}
		nread += res;
	}
	str[len] = 0;

	return str;
}

int send_str (int sock, const char *str)
{
	int len = strlen (str);
	
	if (!send_int(sock, strlen(str)))
		return 0;

	if (send(sock, str, len, 0) != len)
		return 0;
	
	return 1;
}

/* Get a time_t value from the socket, return == 0 on error. */
int get_time (int sock, time_t *i)
{
	int res;
	
	res = recv (sock, i, sizeof(time_t), 0);
	if (res == -1)
		logit ("recv() failed when getting time_t: %s", strerror(errno));

	return res == sizeof(time_t) ? 1 : 0;
}

/* Send a time_t value to the socket, return == 0 on error */
int send_time (int sock, time_t i)
{
	int res;
	
	res = send (sock, &i, sizeof(time_t), 0);
	if (res == -1)
		logit ("send() failed: %s", strerror(errno));

	return res == sizeof(time_t) ? 1 : 0;
}

/* Add a string to the dynamicaly allocated buffer which has aready len bytes
 * data and is allocated big. Returns the pointer to the buffer which may be not
 * the same as buf. */
static char *add_buf_str (char *buf, int *len, int *allocated, const char *str)
{
	int str_len = strlen(str);
	int needed_space = str_len * sizeof(char) + sizeof(int);
	
	if (*allocated - *len < needed_space) {
		*allocated += needed_space + 256; /* put some more space */
		buf = xrealloc (buf, *allocated);
	}

	memcpy (buf + *len, &str_len, sizeof(int));
	*len += sizeof(int);
	memcpy (buf + *len, str, str_len);
	*len += str_len;

	return buf;
}

/* Add an integerg to the dynamicaly allocated buffer which has aready len bytes
 * data and is allocated big. Returns the pointer to the buffer which may be not
 * the same as buf. */
static char *add_buf_int (char *buf, int *len, int *allocated, const int n)
{
	if (*allocated - *len < (int)sizeof(n)) {
		*allocated *= 2;
		buf = xrealloc (buf, *allocated);
	}

	memcpy (buf + *len, &n, sizeof(n));
	*len += sizeof(n);

	return buf;
}

/* Add a long to the dynamicaly allocated buffer which has aready len bytes
 * data and is allocated big. Returns the pointer to the buffer which may be not
 * the same as buf. */
static char *add_buf_long (char *buf, int *len, int *allocated, const int n)
{
	if (*allocated - *len < (int)sizeof(n)) {
		*allocated *= 2;
		buf = xrealloc (buf, *allocated);
	}

	memcpy (buf + *len, &n, sizeof(n));
	*len += sizeof(n);

	return buf;
}

/* Add a time_t to the dynamicaly allocated buffer which has aready len bytes
 * data and is allocated big. Returns the pointer to the buffer which may be not
 * the same as buf. */
static char *add_buf_time (char *buf, int *len, int *allocated, const time_t t)
{
	if (*allocated - *len < (int)sizeof(t)) {
		*allocated *= 2;
		buf = xrealloc (buf, *allocated);
	}

	memcpy (buf + *len, &t, sizeof(t));
	*len += sizeof(t);

	return buf;
}

/* Make a apcket from the item fields suitable to send() as. The returned
 * memory is malloc()ed and the size of the packed is put into len. */
static char *make_item_packet (const struct plist_item *item, size_t *len)
{
	char *buf;
	size_t allocated;

	allocated = 2048;
	buf = xmalloc (allocated);
	*len = 0;

	buf = add_buf_str (buf, len, &allocated, item->file);
	buf = add_buf_long (buf, len, &allocated, item->file_hash);
	buf = add_buf_str (buf, len, &allocated,
			item->title_tags ? item->title_tags : "");
	
	if (item->tags) {
		buf = add_buf_str (buf, len, &allocated,
				item->tags->title ? item->tags->title : "");
		buf = add_buf_str (buf, len, &allocated,
				item->tags->artist ? item->tags->artist : "");
		buf = add_buf_str (buf, len, &allocated,
				item->tags->album ? item->tags->album : "");
		buf = add_buf_int (buf, len, &allocated, item->tags->track);
		
		buf = add_buf_int (buf, len, &allocated,
				item->tags->filled & TAGS_TIME
				? item->tags->time : -1);
	}
	else {
		
		/* empty tags: */
		buf = add_buf_str (buf, len, &allocated, ""); /* title */
		buf = add_buf_str (buf, len, &allocated, ""); /* artist */
		buf = add_buf_str (buf, len, &allocated, ""); /* album */
		buf = add_buf_int (buf, len, &allocated, -1); /* track */
		buf = add_buf_int (buf, len, &allocated, -1); /* time */
	}

	buf = add_buf_time (buf, len, &allocated, item->mtime);

	return buf;
}

/* Send a playlist item to the socket. If item == NULL, send empty item mark
 * (end of playlist). Return 0 on error. */
int send_item (int sock, const struct plist_item *item)
{
	char *buf;
	ssize_t sent;
	size_t send_pos = 0, pkt_size;
	
	if (!item) {
		if (!send_str(sock, "")) {
			logit ("Error while sending empty item");
			return 0;
		}
		return 1;
	}

	buf = make_item_packet (item, &pkt_size);
	
	while (send_pos < pkt_size) {
		sent = send (sock, buf + send_pos, pkt_size - send_pos, 0);
		if (sent < 0) {
			logit ("Error while sendint item: %s", strerror(errno));
			free (buf);
			return 0;
		}
		send_pos += sent;
	}
		
	free (buf);

	return 1;
}

/* Get a playlist item from the server. If empty item->file is an empty string,
 * end of playlist arrived (empty item). The memory is malloc()ed. Return NULL
 * on error. */
struct plist_item *recv_item (int sock)
{
	struct plist_item *item = plist_new_item ();

	/* get the file name */
	if (!(item->file = get_str(sock))) {
		logit ("Error while receiving file name");
		free (item);
		return NULL;
	}

	if (item->file[0]) {
		char *title, *artist, *album;
		int track, time;
		
		if (!(get_long(sock, &item->file_hash))) {
			logit ("Error while receiving file hash");
			free (item->file);
			free (item);
		}

		if (!(item->title_tags = get_str(sock))) {
			logit ("Error while receiving tags title");
			free (item->file);
			free (item);
		}

		if (!item->title_tags[0]) {
			free (item->title_tags);
			item->title_tags = NULL;
		}
		
		if (!(title = get_str(sock))) {
			logit ("Error while receiving titile");
			if (item->title_tags)
				free (item->title_tags);
			free (item);
			return NULL;
		}
		
		if (!(artist = get_str(sock))) {
			logit ("Error while receiving artist");
			if (item->title_tags)
				free (item->title_tags);
			free (title);
			free (item);
			return NULL;
		}
		
		if (!(album = get_str(sock))) {
			logit ("Error while receiving album");
			if (item->title_tags)
				free (item->title_tags);
			free (title);
			free (artist);
			free (item);
			return NULL;
		}
			
		if (!get_int(sock, &track)) {
			logit ("Error while receiving ");
			if (item->title_tags)
				free (item->title_tags);
			free (title);
			free (artist);
			free (item);
			return NULL;
		}
		
		if (!get_int(sock, &time)) {
			logit ("Error while receiving time");
			if (item->title_tags)
				free (item->title_tags);
			free (title);
			free (artist);
			free (item);
			return NULL;
		}
		
		if (!get_time(sock, &item->mtime)) {
			logit ("Error while receiving mtime");
			if (item->title_tags)
				free (item->title_tags);
			free (title);
			free (artist);
			free (item);
			return NULL;
		}

		if (*title || *artist || *album || track != -1 || time != -1) {
			item->tags = tags_new ();

			if (*title || *artist || *album) {
				item->tags->filled |= TAGS_COMMENTS;

				if (*title)
					item->tags->title = title;
				else
					free (title);
				
				if (*artist)
					item->tags->artist = artist;
				else
					free (artist);
				
				if (*album)
					item->tags->album = album;
				else
					free (album);
			}
			else {
				free (title);
				free (artist);
				free (album);
			}
			
			if (time != -1) {
				item->tags->filled |= TAGS_TIME;
				item->tags->time = time;
			}

			item->tags->track = track;
		}
		else {
			free (title);
			free (artist);
			free (album);
		}
	}
	
	return item;
}

/* Push an event on the queue if it's not already there. */
void event_push (struct event_queue *q, const int event, void *data)
{
	assert (q != NULL);
	
	if (!q->head) {
		q->head = (struct event *)xmalloc (sizeof(struct event));
		q->head->next = NULL;
		q->head->type = event;
		q->head->data = data;
		q->tail = q->head;
	}
	else {
		assert (q->head != NULL);
		assert (q->tail != NULL);
		assert (q->tail->next == NULL);
		
		q->tail->next = (struct event *)xmalloc (
				sizeof(struct event));
		q->tail = q->tail->next;
		q->tail->next = NULL;
		q->tail->type = event;
		q->tail->data = data;
	}
}

/* Remove the first event from the queue (don't free the data field). */
void event_pop (struct event_queue *q)
{
	struct event *e;
	
	assert (q != NULL);
	assert (q->head != NULL);
	assert (q->tail != NULL);

	e = q->head;
	q->head = e->next;
	free (e);
	
	if (q->tail == e)
		q->tail = NULL; /* the queue is empty */
}

/* Get the pointer to the first item in the queue or NULL if the queue is
 * empty. */
struct event *event_get_first (struct event_queue *q)
{
	assert (q != NULL);
	
	return q->head;
}

/* Free event queue content without the queue structure. */
void event_queue_free (struct event_queue *q)
{
	struct event *e;
	
	assert (q != NULL);

	while ((e = event_get_first(q))) {
		if (e->data)
			logit ("Event with data that I can't free() found in the"
					" queue while free()ing it!");
		event_pop (q);
	}
}

void event_queue_init (struct event_queue *q)
{
	assert (q != NULL);
	
	q->head = NULL;
	q->tail = NULL;
}

#if 0
/* Search for an event of this type and return pointer to it or NULL if there
 * was no such event. */
struct event *event_search (struct event_queue *q, const int event)
{
	struct event *e;
	
	assert (q != NULL);

	while ((e = q->head)) {
		if (e->type == event)
			return e;
		e = e->next;
	}

	return NULL;
}
#endif

/* Return != 0 if the queue is empty. */
int event_queue_empty (const struct event_queue *q)
{
	assert (q != NULL);
	
	return q->head == NULL ? 1 : 0;
}

/* Fill the buffer with all event data. Insert the size on the buffer in
 * len. Returned memory is malloc()ed. */
static char *make_event_packet (struct event *e, size_t *len)
{
	char *buf;
	
	assert (e != NULL);

	*len = sizeof(e->type);

	if (e->type == EV_PLIST_DEL) {
		int str_len;

		assert (e->data != NULL);

		/* Add the size of the length of the string and the size of
		 * the string. */
		str_len = strlen(e->data);
		*len += sizeof(int) + str_len * sizeof(char);

		buf = xmalloc (*len);
		memcpy (buf, &e->type, sizeof(e->type));
		memcpy (buf + sizeof(e->type), &str_len, sizeof(str_len));
		memcpy (buf + sizeof(e->type) + sizeof(str_len), e->data,
				str_len * sizeof(char));
	}
	else if (e->type == EV_PLIST_ADD) {
		size_t item_packet_len;
		char *item_packet;
		
		item_packet = make_item_packet (e->data, &item_packet_len);
		*len += item_packet_len;
		
		buf = xmalloc(*len);
		memcpy (buf, &e->type, sizeof(e->type));
		memcpy (buf + sizeof(e->type), item_packet, item_packet_len);
		free (item_packet);
	}
	else {
		if (e->data)
			logit ("Unhandled event data!");
		buf = xmalloc(*len);
		memcpy (buf, &e->type, sizeof(e->type));
	}

	return buf;
}

/* Send the first event from the queue an remove it on success. If the
 * operation woulb block return NB_IO_BLOCK. Return NB_IO_ERR on error
 * or NB_IO_OK on success. */
enum noblock_io_status event_send_noblock (int sock, struct event_queue *q)
{
	char *buf;
	size_t buf_len;
	ssize_t res;
	
	assert (q != NULL);
	assert (!event_queue_empty(q));

	/* We must do it in one send() call to be able to handle blocking. */
	buf = make_event_packet (event_get_first(q), &buf_len);
	res = send (sock, buf, buf_len, MSG_DONTWAIT);
	free (buf);

	if (res > 0) {
		struct event *e;

		e = event_get_first (q);

		if (e->type == EV_PLIST_ADD) {
			plist_free_item_fields (e->data);
			free (e->data);
		}
		else if (e->type == EV_PLIST_DEL)
			free (e->data);
		else if (e->data)
			logit ("Unhandled event data!");

		event_pop (q);
		return NB_IO_OK;
	}
	else if (errno == EAGAIN) {
		logit ("Sending event would block");
		return NB_IO_BLOCK;
	}
	
	/* Error */
	logit ("Error when sending event: %s", strerror(errno));
	return NB_IO_ERR;
}
