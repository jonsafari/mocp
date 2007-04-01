/*
 * MOC - music on console
 * Copyright (C) 2005, 2006 Damian Pietras <daper@daper.net>
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

#include <pthread.h>
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>

#define DEBUG

#include "server.h"
#include "playlist.h"
#include "rbtree.h"
#include "common.h"
#include "files.h"
#include "tags_cache.h"
#include "log.h"
#include "audio.h"

static void request_queue_init (struct request_queue *q)
{
	assert (q != NULL);

	q->head = NULL;
	q->tail = NULL;
}

static void request_queue_clear (struct request_queue *q)
{
	assert (q != NULL);

	while (q->head) {
		struct request_queue_node *o = q->head;

		q->head = q->head->next;

		free (o->file);
		free (o);
	}

	q->tail = NULL;
}

/* Remove items from the queue from the beginning to the specified file. */
static void request_queue_clear_up_to (struct request_queue *q,
		const char *file)
{
	int stop = 0;
	
	assert (q != NULL);

	while (q->head && !stop) {
		struct request_queue_node *o = q->head;

		q->head = q->head->next;

		if (!strcmp(o->file, file))
			stop = 1;

		free (o->file);
		free (o);
	}

	if (!q->head)
		q->tail = NULL;
}

static void request_queue_add (struct request_queue *q, const char *file,
		const int tags_sel)
{
	assert (q != NULL);
	
	if (!q->head) {
		q->head = (struct request_queue_node *)xmalloc (
				sizeof(struct request_queue_node));
		q->tail = q->head;
	}
	else {
		assert (q->tail != NULL);
		assert (q->tail->next == NULL);
		
		q->tail->next = (struct request_queue_node *)xmalloc (
				sizeof(struct request_queue_node));
		q->tail = q->tail->next;
	}
	
	q->tail->file = xstrdup (file);
	q->tail->tags_sel = tags_sel;
	q->tail->next = NULL;
}

static int request_queue_empty (const struct request_queue *q)
{
	assert (q != NULL);
	
	return q->head == NULL;
}

/* Get the file name of the first element in the queue or NULL if the queue is
 * empty. Put tags to be read in *tags_sel. Returned memory is malloc()ed. */ 
static char *request_queue_pop (struct request_queue *q, int *tags_sel)
{
	struct request_queue_node *n;
	char *file;
	
	assert (q != NULL);

	if (q->head == NULL)
		return NULL;

	n = q->head;
	q->head = n->next;
	file = n->file;
	*tags_sel = n->tags_sel;
	free (n);

	if (q->tail == n)
		q->tail = NULL; /* the queue is empty */

	return file;
}

static void cache_list_init (struct cache_list *l)
{
	assert (l != NULL);

	l->head = NULL;
	l->tail = NULL;
}

static int cache_list_empty (const struct cache_list *l)
{
	assert (l != NULL);

	return l->head == NULL;
}

/* Detach the oldest element from the cache list and return a pointer to it
 * or NULL if the list is empty. */
static struct cache_list_node *cache_list_pop (struct cache_list *l)
{
	struct cache_list_node *n;
	
	assert (l != NULL);

	if (l->head == NULL)
		return NULL;

	n = l->head;
	l->head = n->next;

	if (l->tail == n)
		l->tail = NULL; /* the queue is empty */

	return n;
}

/* Remove the oldest element of the cache (if it is not empty). */
static void tags_cache_remove_oldest (struct tags_cache *c)
{
	struct cache_list_node *n;

	if ((n = cache_list_pop(&c->cache))) {
		debug ("Removing from cache: %s", n->file);
		rb_delete (&c->search_tree, n->file);
		c->size -= n->size;
		
		free (n->file);
		tags_free (n->tags);
		free (n);
	}
}


/* Add this tags object for the file to the cache. */
static void tags_cache_add (struct tags_cache *c, const char *file,
		struct file_tags *tags)
{
	assert (c != NULL);
	assert (tags != NULL);
	
	if (!c->cache.head) {
		c->cache.head = (struct cache_list_node *)xmalloc (
				sizeof(struct cache_list_node));
		c->cache.tail = c->cache.head;
	}
	else {
		assert (c->cache.tail != NULL);
		assert (c->cache.tail->next == NULL);
		
		c->cache.tail->next = (struct cache_list_node *)xmalloc (
				sizeof(struct cache_list_node));
		c->cache.tail = c->cache.tail->next;
	}
	
	c->cache.tail->file = xstrdup (file);
	c->cache.tail->tags = tags;
	c->cache.tail->mod_time = get_mtime (file);
	c->cache.tail->size = sizeof(struct cache_list_node) +
		strlen(file) + 1 + tags_mem (tags);
	c->cache.tail->next = NULL;

	rb_insert (&c->search_tree, c->cache.tail);
	c->size += c->cache.tail->size;
}

/* Read the selected tags for this file and add it to the cache.
 * If client_id != -1, the server is notified using tags_response().
 * If client_id == -1, copy of file_tag sis returned. */
static struct file_tags *tags_cache_read_add (struct tags_cache *c,
		const int client_id, const char *file, int tags_sel)
{
	struct file_tags *tags;
	struct rb_node *x;
	struct cache_list_node *node = NULL;
		
	assert (c != NULL);
	assert (file != NULL);

	debug ("Getting tags for %s", file);

	LOCK (c->mutex);

	/* If this entry is already presend in the cache, we have 3 options:
	 * we must read different tags (TAGS_*) or the tags are outdated
	 * or this is an immediate tags read (client_id == -1) */
	if (!rb_is_null(x = rb_search(&c->search_tree, file))) {
		node = (struct cache_list_node *)x->data;
			
		if (node->mod_time != get_mtime(file)) {

			/* outdated tags - remove them and reread */
			rb_delete (&c->search_tree, file);
			tags_free (node->tags);
			tags = tags_new ();

			debug ("Tags in the cache are outdated");
		}
		else if ((node->tags->filled & tags_sel) && client_id == -1) {
			debug ("Tags are in the cache.");
			tags = tags_dup (node->tags);
			UNLOCK (c->mutex);

			return tags;
		}
		else {
			tags = node->tags; /* read tags in addition to already
					   present tags */
			debug ("Tags in the cache are not what we want.");
		}
	}
	else
		tags = tags_new ();
	UNLOCK (c->mutex);

	if (tags_sel & TAGS_TIME) {
		int time;

		/* Try to get it from the server's playlist first. */
		time = audio_get_ftime (file);

		if (time != -1) {
			tags->time = time;
			tags->filled |= TAGS_TIME;
			tags_sel &= ~TAGS_TIME;
		}
	}

	tags = read_file_tags (file, tags, tags_sel);

	LOCK (c->mutex);

	if (!node) {
		debug ("Adding to the cache");
		tags_cache_add (c, file, tags);
	}
	else
		debug ("Tags updated");

	if (client_id != -1) {
		tags_response (client_id, file, tags);
		tags = NULL;
	}
	else
		tags = tags_dup (tags);
	
	/* Removed the oldest items from the cache if we exceeded the maximum
	 * cache size */
	while (c->size > c->max_size && !cache_list_empty(&c->cache))
		tags_cache_remove_oldest (c);
	debug ("Cache is %.2fKB big", c->size / 1024.0);
	UNLOCK (c->mutex);

	return tags;
}

static void *reader_thread (void *cache_ptr)
{
	struct tags_cache *c = (struct tags_cache *)cache_ptr;
	int curr_queue = 0; /* index of the queue from where we will get the
			       next request */

	logit ("tags reader thread started");

	LOCK (c->mutex);

	while (!c->stop_reader_thread) {
		int i;
		char *request_file;
		int tags_sel;
		
		/* find the queue with a request waiting, begin searching at
		 * curr_queue: we want to get one request from each queue,
		 * and then move to the next non-empty queue */
		i = curr_queue;
		while (i < CLIENTS_MAX && request_queue_empty(&c->queues[i]))
			i++;
		if (i == CLIENTS_MAX) {
			i = 0;
			while (i < curr_queue
					&& request_queue_empty(&c->queues[i]))
				i++;

			if (i == curr_queue) {
				debug ("all queues empty, waiting");
				pthread_cond_wait (&c->request_cond, &c->mutex);
				continue;
			}
		}
		curr_queue = i;

		request_file = request_queue_pop (&c->queues[curr_queue],
				&tags_sel);
		UNLOCK (c->mutex);
		
		tags_cache_read_add (c, curr_queue, request_file, tags_sel);
		free (request_file);
		
		LOCK (c->mutex);
		if (++curr_queue == CLIENTS_MAX)
			curr_queue = 0;
	}

	UNLOCK (c->mutex);

	logit ("exiting tags reader thread");
	
	return NULL;
}

/* Compare function for RB trees */
static int compare_cache_items (const void *a, const void *b,
		void *adata ATTR_UNUSED)
{
	struct cache_list_node *na = (struct cache_list_node *)a;
	struct cache_list_node *nb = (struct cache_list_node *)b;

	return strcmp (na->file, nb->file);
}

/* Compare function for RB trees */
static int compare_file_cache_item (const void *key, const void *data,
		void *adata ATTR_UNUSED)
{
	struct cache_list_node *node = (struct cache_list_node *)data;
	const char *file = (const char *)key;

	return strcmp (file, node->file);
}

void tags_cache_init (struct tags_cache *c, const size_t max_size)
{
	int i;
	
	assert (c != NULL);

	rb_init_tree (&c->search_tree, compare_cache_items,
			compare_file_cache_item, NULL);
	cache_list_init (&c->cache);

	for (i = 0; i < CLIENTS_MAX; i++)
		request_queue_init (&c->queues[i]);

	c->size = 0;
	c->max_size = max_size;
	c->stop_reader_thread = 0;
	pthread_mutex_init (&c->mutex, NULL);
	
	if (pthread_cond_init(&c->request_cond, NULL))
		fatal ("Can't create request_cond");
	
	if (pthread_create(&c->reader_thread, NULL, reader_thread, c))
		fatal ("Can't create tags cache thread.");
}

void tags_cache_destroy (struct tags_cache *c)
{
	int i;
	
	assert (c != NULL);
	
	LOCK (c->mutex);
	c->stop_reader_thread = 1;
	pthread_cond_signal (&c->request_cond);
	UNLOCK (c->mutex);

	if (pthread_join(c->reader_thread, NULL))
		fatal ("pthread_join() on cache reader thread failed.");

	while (!cache_list_empty(&c->cache))
		tags_cache_remove_oldest (c);

	for (i = 0; i < CLIENTS_MAX; i++)
		request_queue_clear (&c->queues[i]);

	if (pthread_mutex_destroy(&c->mutex))
		logit ("Can't destroy mutex");
	if (pthread_cond_destroy(&c->request_cond))
		logit ("Can't destroy request_cond");
}

void tags_cache_add_request (struct tags_cache *c, const char *file,
		const int tags_sel, const int client_id)
{
	struct rb_node *x;
	
	assert (c != NULL);
	assert (file != NULL);
	assert (client_id >= 0 && client_id < CLIENTS_MAX);

	debug ("Request for tags for %s from client %d", file, client_id);
	
	LOCK (c->mutex);
	if (!rb_is_null(x = rb_search(&c->search_tree, file))) {
		struct cache_list_node *n = (struct cache_list_node *)x->data;

		if (n->mod_time == get_mtime(file)
				&& (n->tags->filled & tags_sel) == tags_sel) {
			tags_response (client_id, file, n->tags);
			debug ("Tags are present in the cache");
			UNLOCK (c->mutex);
			return;
		}

		debug ("Found outdated or not complete tags in the cache");
	}
	request_queue_add (&c->queues[client_id], file, tags_sel);
	pthread_cond_signal (&c->request_cond);
	UNLOCK (c->mutex);
}

void tags_cache_clear_queue (struct tags_cache *c, const int client_id)
{
	assert (c != NULL);
	assert (client_id >= 0 && client_id < CLIENTS_MAX);

	LOCK (c->mutex);
	request_queue_clear (&c->queues[client_id]);
	debug ("Cleared requests queue for client %d", client_id);
	UNLOCK (c->mutex);
}

/* Remove all pending requests from the queue for the given client upo to
 * the request associated with the given file. */
void tags_cache_clear_up_to (struct tags_cache *c, const char *file,
		const int client_id)
{
	assert (c != NULL);
	assert (client_id >= 0 && client_id < CLIENTS_MAX);
	assert (file != NULL);
	
	LOCK (c->mutex);
	request_queue_clear_up_to (&c->queues[client_id], file);
	debug ("Removing requests for client %d up to file %s", client_id,
			file);
	UNLOCK (c->mutex);
}

void tags_cache_save (struct tags_cache *c, const char *file_name)
{
	struct cache_list_node *n;
	FILE *file;

	if (!(file = fopen(file_name, "w"))) {
		logit ("Can't write tags cache to %s: %s", file_name,
				strerror(errno));
		return;
	}
	
	LOCK (c->mutex);
	logit ("Saving tags cache to %s...", file_name);
	n = c->cache.head;

	while (n) {
		fprintf (file, "%s\n", n->file);
		fprintf (file, "%ld\n", (long)n->mod_time);
		fprintf (file, "%s\n", n->tags->title ? n->tags->title : "");
		fprintf (file, "%s\n", n->tags->artist ? n->tags->artist : "");
		fprintf (file, "%s\n", n->tags->album ? n->tags->album : "");
		fprintf (file, "%d\n", n->tags->track);
		fprintf (file, "%d\n", n->tags->time);

		n = n->next;
	}
	
	UNLOCK (c->mutex);

	fclose (file);
}

/* Read a line from a file, convert it to a number. Return 0 on error. */
static int read_num (FILE *file, long *num)
{
	char *tmp;
	
	if ((tmp = read_line(file))) {
		char *num_err;
		
		*num = strtol (tmp, &num_err, 10);
		if (*num_err) {
			free (tmp);
			return 0;
		}

		free (tmp);
		return 1;
	}

	return 0;
}

void tags_cache_load (struct tags_cache *c, const char *file_name)
{
	FILE *file;
	int count = 0;

	if (!(file = fopen(file_name, "r"))) {
		logit ("Can't read tags cache from %s: %s", file_name,
				strerror(errno));
		return;
	}
	
	LOCK (c->mutex);
	logit ("Loading tags cache from %s...", file_name);

	while (!feof(file)) {
		char *node_file_name;
		struct file_tags *tags;
		long mod_time;
		long tmp;

		node_file_name = read_line (file);
		if (!node_file_name)
			break;

		if (!read_num(file, &mod_time)) {
			free (node_file_name);
			logit ("File broken, no modification time");
			break;
		}

		tags = tags_new ();
		
		/* read the title */
		if (!(tags->title = read_line(file))) {
			free (node_file_name);
			tags_free (tags);
			logit ("File broken, no title");
			break;
		}
		if (!tags->title[0]) {
			free (tags->title);
			tags->title = NULL;
		}

		/* read the artist */
		if (!(tags->artist = read_line(file))) {
			free (node_file_name);
			tags_free (tags);
			logit ("File broken, no artist");
			break;
		}
		if (!tags->artist[0]) {
			free (tags->artist);
			tags->artist = NULL;
		}

		/* read the album */
		if (!(tags->album = read_line(file))) {
			free (node_file_name);
			tags_free (tags);
			logit ("File broken, no artist");
			break;
		}
		if (!tags->album[0]) {
			free (tags->album);
			tags->album = NULL;
		}

		if (!read_num(file, &tmp)) {
			free (node_file_name);
			tags_free (tags);
			logit ("File broken, no track");
			break;
		}
		tags->track = tmp;

		if (!read_num(file, &tmp)) {
			free (node_file_name);
			tags_free (tags);
			logit ("File broken, no time");
			break;
		}
		tags->time = tmp;

		if (tags->time < 0) {
			free (node_file_name);
			tags_free (tags);
			logit ("File broken, invalid time");
			break;
		}

		if (tags->title)
			tags->filled |= TAGS_COMMENTS;
		else {
			if (tags->artist) {
				free (tags->artist);
				tags->artist = NULL;
			}
			if (tags->album) {
				free (tags->album);
				tags->album = NULL;
			}
				
		}
		if (tags->time != -1)
			tags->filled |= TAGS_TIME;

		
		/* check if the file was changed */
		if (get_mtime(node_file_name) == mod_time) {
			debug ("Adding file %s", node_file_name);
			tags_cache_add (c, node_file_name, tags);
			count++;
		}
		else
			debug ("File %s was modified", node_file_name);

		free (node_file_name);

		if (c->size > c->max_size) {
			logit ("Maximum tags cache size exceeded");
			break;
		}
	}
	
	UNLOCK (c->mutex);

	logit ("Loaded %d items to the cache", count);
	fclose (file);
}

/* Immediatelly read tags for a file bypassing the request queue. */
struct file_tags *tags_cache_get_immediate (struct tags_cache *c,
		const char *file, const int tags_sel)
{
	struct file_tags *tags;

	assert (c != NULL);
	assert (file != NULL);

	debug ("Immediate tags read for %s", file);
	tags = tags_cache_read_add (c, -1, file, tags_sel);

	return tags;
}
