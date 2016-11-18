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
#include <sys/types.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <dirent.h>

#ifdef HAVE_DB_H
# ifndef HAVE_U_INT
typedef unsigned char u_char;
typedef unsigned short u_short;
typedef unsigned int u_int;
typedef unsigned long int u_long;
# endif
#include <db.h>
#endif

#define DEBUG

#define STRERROR_FN bdb_strerror

#include "common.h"
#include "server.h"
#include "playlist.h"
#include "rbtree.h"
#include "files.h"
#include "tags_cache.h"
#include "log.h"
#include "audio.h"

#ifdef HAVE_DB_H
# define DB_ONLY
#else
# define DB_ONLY ATTR_UNUSED
#endif

/* The name of the tags database in the cache directory. */
#define TAGS_DB "tags.db"

/* The name of the version tag file in the cache directory. */
#define MOC_VERSION_TAG "moc_version_tag"

/* The maximum length of the version tag (including trailing NULL). */
#define VERSION_TAG_MAX 64

/* Number used to create cache version tag to detect incompatibilities
 * between cache version stored on the disk and MOC/BerkeleyDB environment.
 *
 * If you modify the DB structure, increase this number.  You can also
 * temporarily set it to zero to disable cache activity during structural
 * changes which require multiple commits.
 */
#define CACHE_DB_FORMAT_VERSION	1

/* How frequently to flush the tags database to disk.  A value of zero
 * disables flushing. */
#define DB_SYNC_COUNT 5

/* Element of a requests queue. */
struct request_queue_node
{
	struct request_queue_node *next;
	char *file; /* file that this request is for (malloc()ed) */
	int tags_sel; /* which tags to read (TAGS_*) */
};

struct request_queue
{
	struct request_queue_node *head;
	struct request_queue_node *tail;
};

struct tags_cache
{
	/* BerkeleyDB's stuff for storing cache. */
#ifdef HAVE_DB_H
	DB_ENV *db_env;
	DB *db;
	u_int32_t locker;
#endif

	int max_items;		/* maximum number of items in the cache. */
	struct request_queue queues[CLIENTS_MAX]; /* requests queues for each
						     client */
	int stop_reader_thread; /* request for stopping read thread (if
				   non-zero) */
	pthread_cond_t request_cond; /* condition for signalizing new
					requests */
	pthread_mutex_t mutex; /* mutex for all above data (except db because
				  it's thread-safe) */
	pthread_t reader_thread; /* tid of the reading thread */
};

struct cache_record
{
	time_t mod_time;		/* last modification time of the file */
	time_t atime;			/* Time of last access. */
	struct file_tags *tags;
};

/* BerkleyDB-provided error code to description function wrapper. */
static inline char *bdb_strerror (int errnum)
{
	char *result;

	if (errnum > 0)
		result = xstrerror (errnum);
	else
		result = xstrdup (db_strerror (errnum));

	return result;
}

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

		if (!strcmp (o->file, file))
			stop = 1;

		free (o->file);
		free (o);
	}

	if (!q->head)
		q->tail = NULL;
}

static void request_queue_add (struct request_queue *q, const char *file,
                                                            int tags_sel)
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

#ifdef HAVE_DB_H
static size_t strlen_null (const char *s)
{
	return s ? strlen (s) : 0;
}
#endif

#ifdef HAVE_DB_H
static char *cache_record_serialize (const struct cache_record *rec, int *len)
{
	char *buf;
	char *p;
	size_t artist_len;
	size_t album_len;
	size_t title_len;

	artist_len = strlen_null (rec->tags->artist);
	album_len = strlen_null (rec->tags->album);
	title_len = strlen_null (rec->tags->title);

	*len = sizeof(rec->mod_time)
		+ sizeof(rec->atime)
		+ sizeof(size_t) * 3 /* lengths of title, artist, time. */
		+ artist_len
		+ album_len
		+ title_len
		+ sizeof(rec->tags->track)
		+ sizeof(rec->tags->time);

	buf = p = (char *)xmalloc (*len);

	memcpy (p, &rec->mod_time, sizeof(rec->mod_time));
	p += sizeof(rec->mod_time);

	memcpy (p, &rec->atime, sizeof(rec->atime));
	p += sizeof(rec->atime);

	memcpy (p, &artist_len, sizeof(artist_len));
	p += sizeof(artist_len);
	if (artist_len) {
		memcpy (p, rec->tags->artist, artist_len);
		p += artist_len;
	}

	memcpy (p, &album_len, sizeof(album_len));
	p += sizeof(album_len);
	if (album_len) {
		memcpy (p, rec->tags->album, album_len);
		p += album_len;
	}

	memcpy (p, &title_len, sizeof(title_len));
	p += sizeof(title_len);
	if (title_len) {
		memcpy (p, rec->tags->title, title_len);
		p += title_len;
	}

	memcpy (p, &rec->tags->track, sizeof(rec->tags->track));
	p += sizeof(rec->tags->track);

	memcpy (p, &rec->tags->time, sizeof(rec->tags->time));
	p += sizeof(rec->tags->time);

	return buf;
}
#endif

#ifdef HAVE_DB_H
static int cache_record_deserialize (struct cache_record *rec,
           const char *serialized, size_t size, int skip_tags)
{
	const char *p = serialized;
	size_t bytes_left = size;
	size_t str_len;

	assert (rec != NULL);
	assert (serialized != NULL);

	if (!skip_tags)
		rec->tags = tags_new ();
	else
		rec->tags = NULL;

#define extract_num(var) \
	do { \
		if (bytes_left < sizeof(var)) \
			goto err; \
		memcpy (&var, p, sizeof(var)); \
		bytes_left -= sizeof(var); \
		p += sizeof(var); \
	} while (0)

#define extract_str(var) \
	do { \
		if (bytes_left < sizeof(str_len)) \
			goto err; \
		memcpy (&str_len, p, sizeof(str_len)); \
		p += sizeof(str_len); \
		if (bytes_left < str_len) \
			goto err; \
		var = xmalloc (str_len + 1); \
		memcpy (var, p, str_len); \
		var[str_len] = '\0'; \
		p += str_len; \
	} while (0)

	extract_num (rec->mod_time);
	extract_num (rec->atime);

	if (!skip_tags) {
		extract_str (rec->tags->artist);
		extract_str (rec->tags->album);
		extract_str (rec->tags->title);
		extract_num (rec->tags->track);
		extract_num (rec->tags->time);

		if (rec->tags->title)
			rec->tags->filled |= TAGS_COMMENTS;
		else {
			if (rec->tags->artist)
				free (rec->tags->artist);
			rec->tags->artist = NULL;

			if (rec->tags->album)
				free (rec->tags->album);
			rec->tags->album = NULL;
		}

		if (rec->tags->time >= 0)
			rec->tags->filled |= TAGS_TIME;
	}

	return 1;

err:
	logit ("Cache record deserialization error at %tdB", p - serialized);
	tags_free (rec->tags);
	rec->tags = NULL;
	return 0;
}
#endif

/* Locked DB function prototype.
 * The function must not acquire or release DB locks. */
#ifdef HAVE_DB_H
typedef void *t_locked_fn (struct tags_cache *, const char *,
                                      int, int, DBT *, DBT *);
#endif

/* This function ensures that a DB function takes place while holding a
 * database record lock.  It also provides an initialised database thang
 * for the key and record. */
#ifdef HAVE_DB_H
static void *with_db_lock (t_locked_fn fn, struct tags_cache *c,
                           const char *file, int tags_sel, int client_id)
{
	int rc;
	void *result;
	DB_LOCK lock;
	DBT key, record;

	assert (c->db_env != NULL);

	memset (&key, 0, sizeof (key));
	key.data = (void *) file;
	key.size = strlen (file);

	memset (&record, 0, sizeof (record));
	record.flags = DB_DBT_MALLOC;

	rc = c->db_env->lock_get (c->db_env, c->locker, 0,
			&key, DB_LOCK_WRITE, &lock);
	if (rc)
		fatal ("Can't get DB lock: %s", db_strerror (rc));

	result = fn (c, file, tags_sel, client_id, &key, &record);

	rc = c->db_env->lock_put (c->db_env, &lock);
	if (rc)
		fatal ("Can't release DB lock: %s", db_strerror (rc));

	if (record.data)
		free (record.data);

	return result;
}
#endif

#ifdef HAVE_DB_H
static void tags_cache_remove_rec (struct tags_cache *c, const char *fname)
{
	DBT key;
	int ret;

	assert (fname != NULL);

	debug ("Removing %s from the cache...", fname);

	memset (&key, 0, sizeof(key));
	key.data = (void *)fname;
	key.size = strlen (fname);

	ret = c->db->del (c->db, NULL, &key, 0);
	if (ret)
		logit ("Can't remove item for %s from the cache: %s",
				fname, db_strerror (ret));
}
#endif

/* Remove the one element of the cache based on it's access time. */
#ifdef HAVE_DB_H
static void tags_cache_gc (struct tags_cache *c)
{
	DBC *cur;
	DBT key;
	DBT serialized_cache_rec;
	int ret;
	char *last_referenced = NULL;
	time_t last_referenced_atime = time (NULL) + 1;
	int nitems = 0;

	c->db->cursor (c->db, NULL, &cur, 0);

	memset (&key, 0, sizeof(key));
	memset (&serialized_cache_rec, 0, sizeof(serialized_cache_rec));

	key.flags = DB_DBT_MALLOC;
	serialized_cache_rec.flags = DB_DBT_MALLOC;

	while (true) {
		struct cache_record rec;

#if DB_VERSION_MAJOR == 4 && DB_VERSION_MINOR < 6
		ret = cur->c_get (cur, &key, &serialized_cache_rec, DB_NEXT);
#else
		ret = cur->get (cur, &key, &serialized_cache_rec, DB_NEXT);
#endif

		if (ret != 0)
			break;

		if (cache_record_deserialize (&rec, serialized_cache_rec.data,
					serialized_cache_rec.size, 1)
				&& rec.atime < last_referenced_atime) {
			last_referenced_atime = rec.atime;

			if (last_referenced)
				free (last_referenced);
			last_referenced = (char *)xmalloc (key.size + 1);
			memcpy (last_referenced, key.data, key.size);
			last_referenced[key.size] = '\0';
		}

		// TODO: remove objects with serialization error.

		nitems++;

		free (key.data);
		free (serialized_cache_rec.data);
	}

	if (ret != DB_NOTFOUND)
		log_errno ("Searching for element to remove failed (cursor)", ret);

#if DB_VERSION_MAJOR == 4 && DB_VERSION_MINOR < 6
	cur->c_close (cur);
#else
	cur->close (cur);
#endif

	debug ("Elements in cache: %d (limit %d)", nitems, c->max_items);

	if (last_referenced) {
		if (nitems >= c->max_items)
			tags_cache_remove_rec (c, last_referenced);
		free (last_referenced);
	}
	else
		debug ("Cache empty");
}
#endif

/* Synchronize cache every DB_SYNC_COUNT updates. */
#ifdef HAVE_DB_H
static void tags_cache_sync (struct tags_cache *c)
{
	static int sync_count = 0;

	if (DB_SYNC_COUNT == 0)
		return;

	sync_count += 1;
	if (sync_count >= DB_SYNC_COUNT) {
		sync_count = 0;
		c->db->sync (c->db, 0);
	}
}
#endif

/* Add this tags object for the file to the cache. */
#ifdef HAVE_DB_H
static void tags_cache_add (struct tags_cache *c, const char *file,
                                  DBT *key, struct file_tags *tags)
{
	char *serialized_cache_rec;
	int serial_len;
	struct cache_record rec;
	DBT data;
	int ret;

	assert (tags != NULL);

	debug ("Adding/updating cache object");

	rec.mod_time = get_mtime (file);
	rec.atime = time (NULL);
	rec.tags = tags;

	serialized_cache_rec = cache_record_serialize (&rec, &serial_len);
	if (!serialized_cache_rec)
		return;

	memset (&data, 0, sizeof(data));
	data.data = serialized_cache_rec;
	data.size = serial_len;

	tags_cache_gc (c);

	ret = c->db->put (c->db, NULL, key, &data, 0);
	if (ret)
		error_errno ("DB put error", ret);

	tags_cache_sync (c);

	free (serialized_cache_rec);
}
#endif

/* Read time tags for a file into tags structure (or create it if NULL). */
struct file_tags *read_missing_tags (const char *file,
                 struct file_tags *tags, int tags_sel)
{
	if (tags == NULL)
		tags = tags_new ();

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

	return tags;
}

/* Read the selected tags for this file and add it to the cache. */
#ifdef HAVE_DB_H
static void *locked_read_add (struct tags_cache *c, const char *file,
                              const int tags_sel, const int client_id,
                              DBT *key, DBT *serialized_cache_rec)
{
	int ret;
	struct file_tags *tags = NULL;

	assert (c->db != NULL);

	ret = c->db->get (c->db, NULL, key, serialized_cache_rec, 0);
	if (ret && ret != DB_NOTFOUND)
		log_errno ("Cache DB get error", ret);

	/* If this entry is already present in the cache, we have 3 options:
	 * we must read different tags (TAGS_*) or the tags are outdated
	 * or this is an immediate tags read (client_id == -1) */
	if (ret == 0) {
		struct cache_record rec;

		if (cache_record_deserialize (&rec, serialized_cache_rec->data,
		                              serialized_cache_rec->size, 0)) {
			time_t curr_mtime = get_mtime (file);

			if (rec.mod_time != curr_mtime) {
				debug ("Tags in the cache are outdated");
				tags_free (rec.tags);  /* remove them and reread tags */
			}
			else if ((rec.tags->filled & tags_sel) == tags_sel
					&& client_id == -1) {
				debug ("Tags are in the cache.");
				return rec.tags;
			}
			else {
				debug ("Tags in the cache are not what we want");
				tags = rec.tags;  /* read additional tags */
			}
		}
	}

	tags = read_missing_tags (file, tags, tags_sel);
	tags_cache_add (c, file, key, tags);

	return tags;
}
#endif

/* Read the selected tags for this file and add it to the cache.
 * If client_id != -1, the server is notified using tags_response().
 * If client_id == -1, copy of file_tags is returned. */
static struct file_tags *tags_cache_read_add (struct tags_cache *c DB_ONLY,
                     const char *file, int tags_sel, int client_id)
{
	struct file_tags *tags = NULL;

	assert (file != NULL);

	debug ("Getting tags for %s", file);

#ifdef HAVE_DB_H
	if (c->max_items)
		tags = (struct file_tags *)with_db_lock (locked_read_add, c, file,
		                                         tags_sel, client_id);
	else
#endif
		tags = read_missing_tags (file, tags, tags_sel);

	if (client_id != -1) {
		tags_response (client_id, file, tags);
		tags_free (tags);
		tags = NULL;
	}

	/* TODO: Remove the oldest items from the cache if we exceeded the maximum
	 * cache size */

	return tags;
}

static void *reader_thread (void *cache_ptr)
{
	struct tags_cache *c;
	int curr_queue = 0; /* index of the queue from where
	                       we will get the next request */

	logit ("Tags reader thread started");

	assert (cache_ptr != NULL);

	c = (struct tags_cache *)cache_ptr;

	LOCK (c->mutex);

	while (!c->stop_reader_thread) {
		int i;
		char *request_file;
		int tags_sel = 0;

		/* Find the queue with a request waiting.  Begin searching at
		 * curr_queue: we want to get one request from each queue,
		 * and then move to the next non-empty queue. */
		i = curr_queue;
		while (i < CLIENTS_MAX && request_queue_empty (&c->queues[i]))
			i++;
		if (i == CLIENTS_MAX) {
			i = 0;
			while (i < curr_queue && request_queue_empty (&c->queues[i]))
				i++;

			if (i == curr_queue) {
				debug ("All queues empty, waiting");
				pthread_cond_wait (&c->request_cond, &c->mutex);
				continue;
			}
		}
		curr_queue = i;

		request_file = request_queue_pop (&c->queues[curr_queue], &tags_sel);
		UNLOCK (c->mutex);

		tags_cache_read_add (c, request_file, tags_sel, curr_queue);
		free (request_file);

		LOCK (c->mutex);
		curr_queue = (curr_queue + 1) % CLIENTS_MAX;
	}

	UNLOCK (c->mutex);

	logit ("Exiting tags reader thread");

	return NULL;
}

struct tags_cache *tags_cache_new (size_t max_size)
{
	int i, rc;
	struct tags_cache *result;

	result = (struct tags_cache *)xmalloc (sizeof (struct tags_cache));

#ifdef HAVE_DB_H
	result->db_env = NULL;
	result->db = NULL;
#endif

	for (i = 0; i < CLIENTS_MAX; i++)
		request_queue_init (&result->queues[i]);

#if CACHE_DB_FORMAT_VERSION
	result->max_items = max_size;
#else
	result->max_items = 0;
#endif
	result->stop_reader_thread = 0;
	pthread_mutex_init (&result->mutex, NULL);

	rc = pthread_cond_init (&result->request_cond, NULL);
	if (rc != 0)
		fatal ("Can't create request_cond: %s", xstrerror (rc));

	rc = pthread_create (&result->reader_thread, NULL, reader_thread, result);
	if (rc != 0)
		fatal ("Can't create tags cache thread: %s", xstrerror (rc));

	return result;
}

void tags_cache_free (struct tags_cache *c)
{
	int i, rc;

	assert (c != NULL);

	LOCK (c->mutex);
	c->stop_reader_thread = 1;
	pthread_cond_signal (&c->request_cond);
	UNLOCK (c->mutex);

#ifdef HAVE_DB_H
	if (c->db) {
#ifndef NDEBUG
		c->db->set_errcall (c->db, NULL);
		c->db->set_msgcall (c->db, NULL);
		c->db->set_paniccall (c->db, NULL);
#endif
		c->db->close (c->db, 0);
		c->db = NULL;
	}
#endif

#ifdef HAVE_DB_H
	if (c->db_env) {
		c->db_env->lock_id_free (c->db_env, c->locker);
#ifndef NDEBUG
		c->db_env->set_errcall (c->db_env, NULL);
		c->db_env->set_msgcall (c->db_env, NULL);
		c->db_env->set_paniccall (c->db_env, NULL);
#endif
		c->db_env->close (c->db_env, 0);
		c->db_env = NULL;
	}
#endif

	rc = pthread_join (c->reader_thread, NULL);
	if (rc != 0)
		fatal ("pthread_join() on cache reader thread failed: %s",
		        xstrerror (rc));

	for (i = 0; i < CLIENTS_MAX; i++)
		request_queue_clear (&c->queues[i]);

	rc = pthread_mutex_destroy (&c->mutex);
	if (rc != 0)
		log_errno ("Can't destroy mutex", rc);
	rc = pthread_cond_destroy (&c->request_cond);
	if (rc != 0)
		log_errno ("Can't destroy request_cond", rc);

	free (c);
}

#ifdef HAVE_DB_H
static void *locked_add_request (struct tags_cache *c, const char *file,
                                 int tags_sel, int client_id,
                                 DBT *key, DBT *serialized_cache_rec)
{
	int db_ret;
	struct cache_record rec;

	assert (c->db);

	db_ret = c->db->get (c->db, NULL, key, serialized_cache_rec, 0);

	if (db_ret == DB_NOTFOUND)
		return NULL;

	if (db_ret) {
		error_errno ("Cache DB search error", db_ret);
		return NULL;
	}

	if (cache_record_deserialize (&rec, serialized_cache_rec->data,
				serialized_cache_rec->size, 0)) {
		if (rec.mod_time == get_mtime (file)
				&& (rec.tags->filled & tags_sel) == tags_sel) {
			tags_response (client_id, file, rec.tags);
			tags_free (rec.tags);
			debug ("Tags are present in the cache");
			return (void *)1;
		}

		tags_free (rec.tags);
		debug ("Found outdated or incomplete tags in the cache");
	}

	return NULL;
}
#endif

void tags_cache_add_request (struct tags_cache *c, const char *file,
                                        int tags_sel, int client_id)
{
	void *rc = NULL;

	assert (c != NULL);
	assert (file != NULL);
	assert (LIMIT(client_id, CLIENTS_MAX));

	debug ("Request for tags for '%s' from client %d", file, client_id);

#ifdef HAVE_DB_H
	if (c->max_items)
		rc = with_db_lock (locked_add_request, c, file, tags_sel, client_id);
#endif

	if (!rc) {
		LOCK (c->mutex);
		request_queue_add (&c->queues[client_id], file, tags_sel);
		pthread_cond_signal (&c->request_cond);
		UNLOCK (c->mutex);
	}
}

void tags_cache_clear_queue (struct tags_cache *c, int client_id)
{
	assert (c != NULL);
	assert (LIMIT(client_id, CLIENTS_MAX));

	LOCK (c->mutex);
	request_queue_clear (&c->queues[client_id]);
	debug ("Cleared requests queue for client %d", client_id);
	UNLOCK (c->mutex);
}

/* Remove all pending requests from the queue for the given client up to
 * the request associated with the given file. */
void tags_cache_clear_up_to (struct tags_cache *c, const char *file,
                                                      int client_id)
{
	assert (c != NULL);
	assert (LIMIT(client_id, CLIENTS_MAX));
	assert (file != NULL);

	LOCK (c->mutex);
	debug ("Removing requests for client %d up to file %s", client_id,
			file);
	request_queue_clear_up_to (&c->queues[client_id], file);
	UNLOCK (c->mutex);
}

#if defined(HAVE_DB_H) && !defined(NDEBUG)
static void db_err_cb (const DB_ENV *unused ATTR_UNUSED, const char *errpfx,
                                                         const char *msg)
{
	assert (msg);

	if (errpfx && errpfx[0])
		logit ("BDB said: %s: %s", errpfx, msg);
	else
		logit ("BDB said: %s", msg);
}
#endif

#if defined(HAVE_DB_H) && !defined(NDEBUG)
static void db_msg_cb (const DB_ENV *unused ATTR_UNUSED, const char *msg)
{
	assert (msg);

	logit ("BDB said: %s", msg);
}
#endif

#if defined(HAVE_DB_H) && !defined(NDEBUG)
static void db_panic_cb (DB_ENV *unused ATTR_UNUSED, int errval)
{
	log_errno ("BDB said", errval);
}
#endif

/* Purge content of a directory. */
#ifdef HAVE_DB_H
static int purge_directory (const char *dir_path)
{
	DIR *dir;
	struct dirent *d;

	logit ("Purging %s...", dir_path);

	dir = opendir (dir_path);
	if (!dir) {
		char *err = xstrerror (errno);
		logit ("Can't open directory %s: %s", dir_path, err);
		free (err);
		return 0;
	}

	while ((d = readdir (dir))) {
		struct stat st;
		char *fpath;
		int len;

		if (!strcmp (d->d_name, ".") || !strcmp (d->d_name, ".."))
			continue;

		len = strlen (dir_path) + strlen (d->d_name) + 2;
		fpath = (char *)xmalloc (len);
		snprintf (fpath, len, "%s/%s", dir_path, d->d_name);

		if (stat (fpath, &st) < 0) {
			char *err = xstrerror (errno);
			logit ("Can't stat %s: %s", fpath, err);
			free (err);
			free (fpath);
			closedir (dir);
			return 0;
		}

		if (S_ISDIR(st.st_mode)) {
			if (!purge_directory (fpath)) {
				free (fpath);
				closedir (dir);
				return 0;
			}

			logit ("Removing directory %s...", fpath);
			if (rmdir (fpath) < 0) {
				char *err = xstrerror (errno);
				logit ("Can't remove %s: %s", fpath, err);
				free (err);
				free (fpath);
				closedir (dir);
				return 0;
			}
		}
		else {
			logit ("Removing file %s...", fpath);

			if (unlink (fpath) < 0) {
				char *err = xstrerror (errno);
				logit ("Can't remove %s: %s", fpath, err);
				free (err);
				free (fpath);
				closedir (dir);
				return 0;
			}
		}

		free (fpath);
	}

	closedir (dir);
	return 1;
}
#endif

/* Create a MOC/db version string.
 *
 * @param buf Output buffer (at least VERSION_TAG_MAX chars long)
 */
#ifdef HAVE_DB_H
static const char *create_version_tag (char *buf)
{
	int db_major;
	int db_minor;

	db_version (&db_major, &db_minor, NULL);

#ifdef PACKAGE_REVISION
	snprintf (buf, VERSION_TAG_MAX, "%d %d %d r%s",
	          CACHE_DB_FORMAT_VERSION, db_major, db_minor, PACKAGE_REVISION);
#else
	snprintf (buf, VERSION_TAG_MAX, "%d %d %d",
	          CACHE_DB_FORMAT_VERSION, db_major, db_minor);
#endif

	return buf;
}
#endif

/* Check version of the cache directory.  If it was created
 * using format not handled by this version of MOC, return 0. */
#ifdef HAVE_DB_H
static int cache_version_matches (const char *cache_dir)
{
	char *fname = NULL;
	char disk_version_tag[VERSION_TAG_MAX];
	ssize_t rres;
	FILE *f;
	int compare_result = 0;

	fname = (char *)xmalloc (strlen (cache_dir) + sizeof (MOC_VERSION_TAG) + 1);
	sprintf (fname, "%s/%s", cache_dir, MOC_VERSION_TAG);

	f = fopen (fname, "r");
	if (!f) {
		logit ("No %s in cache directory", MOC_VERSION_TAG);
		free (fname);
		return 0;
	}

	rres = fread (disk_version_tag, 1, sizeof (disk_version_tag) - 1, f);
	if (rres == sizeof (disk_version_tag) - 1) {
		logit ("On-disk version tag too long");
	}
	else {
		char *ptr, cur_version_tag[VERSION_TAG_MAX];

		disk_version_tag[rres] = '\0';
		ptr = strrchr (disk_version_tag, '\n');
		if (ptr)
			*ptr = '\0';
		ptr = strrchr (disk_version_tag, ' ');
		if (ptr && ptr[1] == 'r')
			*ptr = '\0';

		create_version_tag (cur_version_tag);
		ptr = strrchr (cur_version_tag, '\n');
		if (ptr)
			*ptr = '\0';
		ptr = strrchr (cur_version_tag, ' ');
		if (ptr && ptr[1] == 'r')
			*ptr = '\0';

		compare_result = !strcmp (disk_version_tag, cur_version_tag);
	}

	fclose (f);
	free (fname);

	return compare_result;
}
#endif

#ifdef HAVE_DB_H
static void write_cache_version (const char *cache_dir)
{
	char cur_version_tag[VERSION_TAG_MAX];
	char *fname = NULL;
	FILE *f;
	int rc;

	fname = (char *)xmalloc (strlen (cache_dir) + sizeof (MOC_VERSION_TAG) + 1);
	sprintf (fname, "%s/%s", cache_dir, MOC_VERSION_TAG);

	f = fopen (fname, "w");
	if (!f) {
		log_errno ("Error opening cache", errno);
		free (fname);
		return;
	}

	create_version_tag (cur_version_tag);
	rc = fwrite (cur_version_tag, strlen (cur_version_tag), 1, f);
	if (rc != 1)
		logit ("Error writing cache version tag: %d", rc);

	free (fname);
	fclose (f);
}
#endif

/* Make sure that the cache directory exists and clear it if necessary. */
#ifdef HAVE_DB_H
static int prepare_cache_dir (const char *cache_dir)
{
	if (mkdir (cache_dir, 0700) == 0) {
		write_cache_version (cache_dir);
		return 1;
	}

	if (errno != EEXIST) {
		error_errno ("Failed to create directory for tags cache", errno);
		return 0;
	}

	if (!cache_version_matches (cache_dir)) {
		logit ("Tags cache directory is the wrong version, purging....");

		if (!purge_directory (cache_dir))
			return 0;
		write_cache_version (cache_dir);
	}

	return 1;
}
#endif

void tags_cache_load (struct tags_cache *c DB_ONLY,
                      const char *cache_dir DB_ONLY)
{
	assert (c != NULL);
	assert (cache_dir != NULL);

#ifdef HAVE_DB_H
	int ret;

	if (!c->max_items)
		return;

	if (!prepare_cache_dir (cache_dir)) {
		error ("Can't prepare cache directory!");
		goto err;
	}

	ret = db_env_create (&c->db_env, 0);
	if (ret) {
		error_errno ("Can't create DB environment", ret);
		goto err;
	}

#ifndef NDEBUG
	c->db_env->set_errcall (c->db_env, db_err_cb);
	c->db_env->set_msgcall (c->db_env, db_msg_cb);
	ret = c->db_env->set_paniccall (c->db_env, db_panic_cb);
	if (ret)
		logit ("Could not set DB panic callback");
#endif

	ret = c->db_env->open (c->db_env, cache_dir,
	                       DB_CREATE | DB_PRIVATE | DB_INIT_MPOOL |
	                       DB_THREAD | DB_INIT_LOCK, 0);
	if (ret) {
		error ("Can't open DB environment (%s): %s",
				cache_dir, db_strerror (ret));
		goto err;
	}

	ret = c->db_env->lock_id (c->db_env, &c->locker);
	if (ret) {
		error_errno ("Failed to get DB locker", ret);
		goto err;
	}

	ret = db_create (&c->db, c->db_env, 0);
	if (ret) {
		error_errno ("Failed to create cache db", ret);
		goto err;
	}

#ifndef NDEBUG
	c->db->set_errcall (c->db, db_err_cb);
	c->db->set_msgcall (c->db, db_msg_cb);
	ret = c->db->set_paniccall (c->db, db_panic_cb);
	if (ret)
		logit ("Could not set DB panic callback");
#endif

	ret = c->db->open (c->db, NULL, TAGS_DB, NULL, DB_BTREE,
	                                DB_CREATE | DB_THREAD, 0);
	if (ret) {
		error_errno ("Failed to open (or create) tags cache db", ret);
		goto err;
	}

	return;

err:
	if (c->db) {
#ifndef NDEBUG
		c->db->set_errcall (c->db, NULL);
		c->db->set_msgcall (c->db, NULL);
		c->db->set_paniccall (c->db, NULL);
#endif
		c->db->close (c->db, 0);
		c->db = NULL;
	}
	if (c->db_env) {
#ifndef NDEBUG
		c->db_env->set_errcall (c->db_env, NULL);
		c->db_env->set_msgcall (c->db_env, NULL);
		c->db_env->set_paniccall (c->db_env, NULL);
#endif
		c->db_env->close (c->db_env, 0);
		c->db_env = NULL;
	}
	c->max_items = 0;
	error ("Failed to initialise tags cache: caching disabled");
#endif
}

/* Immediately read tags for a file bypassing the request queue. */
struct file_tags *tags_cache_get_immediate (struct tags_cache *c,
                                  const char *file, int tags_sel)
{
	struct file_tags *tags;

	assert (c != NULL);
	assert (file != NULL);

	debug ("Immediate tags read for %s", file);

	if (!is_url (file))
		tags = tags_cache_read_add (c, file, tags_sel, -1);
	else
		tags = tags_new ();

	return tags;
}
