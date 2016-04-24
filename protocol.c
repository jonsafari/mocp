/*
 * MOC - music on console
 * Copyright (C) 2003 - 2005 Damian Pietras <daper@daper.net>
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
#include <unistd.h>
#include <fcntl.h>

#include "common.h"
#include "log.h"
#include "protocol.h"
#include "playlist.h"
#include "files.h"

/* Maximal socket name. */
#define UNIX_PATH_MAX	108
#define SOCKET_NAME	"socket2"

#define nonblocking(fn, result, sock, buf, len) \
	do { \
		long flags = fcntl (sock, F_GETFL); \
		if (flags == -1) \
			fatal ("Getting flags for socket failed: %s", \
			        xstrerror (errno)); \
		flags |= O_NONBLOCK; \
		if (fcntl (sock, F_SETFL, O_NONBLOCK) == -1) \
			fatal ("Setting O_NONBLOCK for the socket failed: %s", \
			        xstrerror (errno)); \
		result = fn (sock, buf, len, 0); \
		flags &= ~O_NONBLOCK; \
		if (fcntl (sock, F_SETFL, flags) == -1) \
			fatal ("Restoring flags for socket failed: %s", \
			        xstrerror (errno)); \
	} while (0)

/* Buffer used to send data in one bigger chunk instead of sending sigle
 * integer, string etc. values. */
struct packet_buf
{
	char *buf;
	size_t allocated;
	size_t len;
};

/* Create a socket name, return NULL if the name could not be created. */
char *socket_name ()
{
	char *socket_name = create_file_name (SOCKET_NAME);

	if (strlen(socket_name) > UNIX_PATH_MAX)
		fatal ("Can't create socket name!");

	return socket_name;
}

/* Get an integer value from the socket, return == 0 on error. */
int get_int (int sock, int *i)
{
	ssize_t res;

	res = recv (sock, i, sizeof(int), 0);
	if (res == -1)
		log_errno ("recv() failed when getting int", errno);

	return res == ssizeof(int) ? 1 : 0;
}

/* Get an integer value from the socket without blocking. */
enum noblock_io_status get_int_noblock (int sock, int *i)
{
	ssize_t res;
	char *err;

	nonblocking (recv, res, sock, i, sizeof (int));

	if (res == ssizeof (int))
		return NB_IO_OK;
	if (res < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
		return NB_IO_BLOCK;

	err = xstrerror (errno);
	logit ("recv() failed when getting int (res %zd): %s", res, err);
	free (err);

	return NB_IO_ERR;
}

/* Send an integer value to the socket, return == 0 on error */
int send_int (int sock, int i)
{
	ssize_t res;

	res = send (sock, &i, sizeof(int), 0);
	if (res == -1)
		log_errno ("send() failed", errno);

	return res == ssizeof(int) ? 1 : 0;
}

#if 0
/* Get a long value from the socket, return == 0 on error. */
static int get_long (int sock, long *i)
{
	ssize_t res;

	res = recv (sock, i, sizeof(long), 0);
	if (res == -1)
		log_errno ("recv() failed when getting int", errno);

	return res == ssizeof(long) ? 1 : 0;
}
#endif

#if 0
/* Send a long value to the socket, return == 0 on error */
static int send_long (int sock, long i)
{
	ssize_t res;

	res = send (sock, &i, sizeof(long), 0);
	if (res == -1)
		log_errno ("send() failed", errno);

	return res == ssizeof(long) ? 1 : 0;
}
#endif

/* Get the string from socket, return NULL on error. The memory is malloced. */
char *get_str (int sock)
{
	int len, nread = 0;
	char *str;

	if (!get_int(sock, &len))
		return NULL;

	if (!RANGE(0, len, MAX_SEND_STRING)) {
		logit ("Bad string length.");
		return NULL;
	}

	str = (char *)xmalloc (sizeof(char) * (len + 1));
	while (nread < len) {
		ssize_t res;

		res = recv (sock, str + nread, len - nread, 0);
		if (res == -1) {
			log_errno ("recv() failed when getting string", errno);
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
	int len;

	len = strlen (str);
	if (!send_int (sock, len))
		return 0;

	if (send (sock, str, len, 0) != len)
		return 0;

	return 1;
}

/* Get a time_t value from the socket, return == 0 on error. */
int get_time (int sock, time_t *i)
{
	ssize_t res;

	res = recv (sock, i, sizeof(time_t), 0);
	if (res == -1)
		log_errno ("recv() failed when getting time_t", errno);

	return res == ssizeof(time_t) ? 1 : 0;
}

/* Send a time_t value to the socket, return == 0 on error */
int send_time (int sock, time_t i)
{
	ssize_t res;

	res = send (sock, &i, sizeof(time_t), 0);
	if (res == -1)
		log_errno ("send() failed", errno);

	return res == ssizeof(time_t) ? 1 : 0;
}

static struct packet_buf *packet_buf_new ()
{
	struct packet_buf *b;

	b = (struct packet_buf *)xmalloc (sizeof(struct packet_buf));
	b->buf = (char *)xmalloc (1024);
	b->allocated = 1024;
	b->len = 0;

	return b;
}

static void packet_buf_free (struct packet_buf *b)
{
	assert (b != NULL);

	free (b->buf);
	free (b);
}

/* Make sure that there is at least len bytes free. */
static void packet_buf_add_space (struct packet_buf *b, const size_t len)
{
	assert (b != NULL);

	if (b->allocated < b->len + len) {
		b->allocated += len + 256; /* put some more space */
		b->buf = (char *)xrealloc (b->buf, b->allocated);
	}
}

/* Add an integer value to the buffer */
static void packet_buf_add_int (struct packet_buf *b, const int n)
{
	assert (b != NULL);

	packet_buf_add_space (b, sizeof(n));
	memcpy (b->buf + b->len, &n, sizeof(n));
	b->len += sizeof(n);
}

/* Add a string value to the buffer. */
static void packet_buf_add_str (struct packet_buf *b, const char *str)
{
	int str_len;

	assert (b != NULL);
	assert (str != NULL);

	str_len = strlen (str);

	packet_buf_add_int (b, str_len);
	packet_buf_add_space (b, str_len * sizeof(char));
	memcpy (b->buf + b->len, str, str_len * sizeof(char));
	b->len += str_len * sizeof(char);
}

/* Add a time_t value to the buffer. */
static void packet_buf_add_time (struct packet_buf *b, const time_t n)
{
	assert (b != NULL);

	packet_buf_add_space (b, sizeof(n));
	memcpy (b->buf + b->len, &n, sizeof(n));
	b->len += sizeof(n);
}

/* Add tags to the buffer. If tags == NULL, add empty tags. */
void packet_buf_add_tags (struct packet_buf *b, const struct file_tags *tags)
{
	assert (b != NULL);

	if (tags) {
		packet_buf_add_str (b, tags->title ? tags->title : "");
		packet_buf_add_str (b, tags->artist ? tags->artist : "");
		packet_buf_add_str (b, tags->album ? tags->album : "");
		packet_buf_add_int (b, tags->track);
		packet_buf_add_int (b, tags->filled & TAGS_TIME ? tags->time : -1);
		packet_buf_add_int (b, tags->filled);
	}
	else {

		/* empty tags: */
		packet_buf_add_str (b, ""); /* title */
		packet_buf_add_str (b, ""); /* artist */
		packet_buf_add_str (b, ""); /* album */
		packet_buf_add_int (b, -1); /* track */
		packet_buf_add_int (b, -1); /* time */
		packet_buf_add_int (b, 0); /* filled */
	}
}

/* Add an item to the buffer. */
void packet_buf_add_item (struct packet_buf *b, const struct plist_item *item)
{
	packet_buf_add_str (b, item->file);
	packet_buf_add_str (b, item->title_tags ? item->title_tags : "");
	packet_buf_add_tags (b, item->tags);
	packet_buf_add_time (b, item->mtime);
}

/* Send data to the socket. Return 0 on error. */
static int send_all (int sock, const char *buf, const size_t size)
{
	ssize_t sent;
	size_t send_pos = 0;

	while (send_pos < size) {
		sent = send (sock, buf + send_pos, size - send_pos, 0);
		if (sent < 0) {
			log_errno ("Error while sending data", errno);
			return 0;
		}
		send_pos += sent;
	}

	return 1;
}

/* Send a playlist item to the socket. If item == NULL, send empty item mark
 * (end of playlist). Return 0 on error. */
int send_item (int sock, const struct plist_item *item)
{
	int res = 1;
	struct packet_buf *b;

	if (!item) {
		if (!send_str(sock, "")) {
			logit ("Error while sending empty item");
			return 0;
		}
		return 1;
	}

	b = packet_buf_new ();
	packet_buf_add_item (b, item);
	if (!send_all(sock, b->buf, b->len)) {
		logit ("Error when sending item");
		res = 0;
	}

	packet_buf_free (b);
	return res;
}

struct file_tags *recv_tags (int sock)
{
	struct file_tags *tags = tags_new ();

	if (!(tags->title = get_str(sock))) {
		logit ("Error while receiving title");
		tags_free (tags);
		return NULL;
	}

	if (!(tags->artist = get_str(sock))) {
		logit ("Error while receiving artist");
		tags_free (tags);
		return NULL;
	}

	if (!(tags->album = get_str(sock))) {
		logit ("Error while receiving album");
		tags_free (tags);
		return NULL;
	}

	if (!get_int(sock, &tags->track)) {
		logit ("Error while receiving track");
		tags_free (tags);
		return NULL;
	}

	if (!get_int(sock, &tags->time)) {
		logit ("Error while receiving time");
		tags_free (tags);
		return NULL;
	}

	if (!get_int(sock, &tags->filled)) {
		logit ("Error while receiving 'filled'");
		tags_free (tags);
		return NULL;
	}

	/* Set NULL instead of empty tags. */
	if (!tags->title[0]) {
		free (tags->title);
		tags->title = NULL;
	}
	if (!tags->artist[0]) {
		free (tags->artist);
		tags->artist = NULL;
	}
	if (!tags->album[0]) {
		free (tags->album);
		tags->album = NULL;
	}

	return tags;
}

/* Send tags. If tags == NULL, send empty tags. Return 0 on error. */
int send_tags (int sock, const struct file_tags *tags)
{
	int res = 1;
	struct packet_buf *b;

	b = packet_buf_new ();
	packet_buf_add_tags (b, tags);

	if (!send_all(sock, b->buf, b->len))
		res = 0;

	packet_buf_free (b);
	return res;
}

/* Get a playlist item from the server.
 * The end of the playlist is indicated by item->file being an empty string.
 * The memory is malloc()ed.  Returns NULL on error. */
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
		if (!(item->title_tags = get_str(sock))) {
			logit ("Error while receiving tags title");
			free (item->file);
			free (item);
			return NULL;
		}

		item->type = file_type (item->file);

		if (!item->title_tags[0]) {
			free (item->title_tags);
			item->title_tags = NULL;
		}

		if (!(item->tags = recv_tags(sock))) {
			logit ("Error while receiving tags");
			free (item->file);
			if (item->title_tags)
				free (item->title_tags);
			free (item);
			return NULL;
		}

		if (!get_time(sock, &item->mtime)) {
			logit ("Error while receiving mtime");
			if (item->title_tags)
				free (item->title_tags);
			free (item->file);
			tags_free (item->tags);
			free (item);
			return NULL;
		}
	}

	return item;
}

struct move_ev_data *recv_move_ev_data (int sock)
{
	struct move_ev_data *d;

	d = (struct move_ev_data *)xmalloc (sizeof(struct move_ev_data));

	if (!(d->from = get_str(sock))) {
		logit ("Error while receiving 'from' data");
		free (d);
		return NULL;
	}

	if (!(d->to = get_str(sock))) {
		logit ("Error while receiving 'to' data");
		free (d->from);
		free (d);
		return NULL;
	}

	return d;
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

void free_tag_ev_data (struct tag_ev_response *d)
{
	assert (d != NULL);

	free (d->file);
	tags_free (d->tags);
	free (d);
}

void free_move_ev_data (struct move_ev_data *m)
{
	assert (m != NULL);
	assert (m->from != NULL);
	assert (m->to != NULL);

	free (m->to);
	free (m->from);
	free (m);
}

struct move_ev_data *move_ev_data_dup (const struct move_ev_data *m)
{
	struct move_ev_data *new;

	assert (m != NULL);
	assert (m->from != NULL);
	assert (m->to != NULL);

	new = (struct move_ev_data *)xmalloc (sizeof(struct move_ev_data));
	new->from = xstrdup (m->from);
	new->to = xstrdup (m->to);

	return new;
}

/* Free data associated with the event if any. */
void free_event_data (const int type, void *data)
{
	if (type == EV_PLIST_ADD || type == EV_QUEUE_ADD) {
		plist_free_item_fields ((struct plist_item *)data);
		free (data);
	}
	else if (type == EV_FILE_TAGS)
		free_tag_ev_data ((struct tag_ev_response *)data);
	else if (type == EV_PLIST_DEL || type == EV_STATUS_MSG
			|| type == EV_SRV_ERROR || type == EV_QUEUE_DEL)
		free (data);
	else if (type == EV_PLIST_MOVE || type == EV_QUEUE_MOVE)
		free_move_ev_data ((struct move_ev_data *)data);
	else if (data)
		abort (); /* BUG */
}

/* Free event queue content without the queue structure. */
void event_queue_free (struct event_queue *q)
{
	struct event *e;

	assert (q != NULL);

	while ((e = event_get_first(q))) {
		free_event_data (e->type, e->data);
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

/* Make a packet buffer filled with the event (with data). */
static struct packet_buf *make_event_packet (const struct event *e)
{
	struct packet_buf *b;

	assert (e != NULL);

	b = packet_buf_new ();

	packet_buf_add_int (b, e->type);

	if (e->type == EV_PLIST_DEL
			|| e->type == EV_QUEUE_DEL
			|| e->type == EV_SRV_ERROR
			|| e->type == EV_STATUS_MSG) {
		assert (e->data != NULL);
		packet_buf_add_str (b, e->data);
	}
	else if (e->type == EV_PLIST_ADD || e->type == EV_QUEUE_ADD) {
		assert (e->data != NULL);
		packet_buf_add_item (b, e->data);
	}
	else if (e->type == EV_FILE_TAGS) {
		struct tag_ev_response *r;

		assert (e->data != NULL);
		r = e->data;

		packet_buf_add_str (b, r->file);
		packet_buf_add_tags (b, r->tags);
	}
	else if (e->type == EV_PLIST_MOVE || e->type == EV_QUEUE_MOVE) {
		struct move_ev_data *m;

		assert (e->data != NULL);

		m = (struct move_ev_data *)e->data;
		packet_buf_add_str (b, m->from);
		packet_buf_add_str (b, m->to);
	}
	else if (e->data)
		abort (); /* BUG */

	return b;
}

/* Send the first event from the queue and remove it on success.  If the
 * operation would block return NB_IO_BLOCK.  Return NB_IO_ERR on error
 * or NB_IO_OK on success. */
enum noblock_io_status event_send_noblock (int sock, struct event_queue *q)
{
	ssize_t res;
	char *err;
	struct packet_buf *b;
	enum noblock_io_status result;

	assert (q != NULL);
	assert (!event_queue_empty(q));

	b = make_event_packet (event_get_first(q));

	/* We must do it in one send() call to be able to handle blocking. */
	nonblocking (send, res, sock, b->buf, b->len);

	if (res == (ssize_t)b->len) {
		struct event *e;

		e = event_get_first (q);
		free_event_data (e->type, e->data);
		event_pop (q);

		result = NB_IO_OK;
		goto exit;
	}

	if (res < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
		logit ("Sending event would block");
		result = NB_IO_BLOCK;
		goto exit;
	}

	err = xstrerror (errno);
	logit ("send()ing event failed (%zd): %s", res, err);
	free (err);
	result = NB_IO_ERR;

exit:
	packet_buf_free (b);
	return result;
}
