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
#include <db.h>

/* Include dirent for various systems */
#ifdef HAVE_DIRENT_H
# include <dirent.h>
#else
# define dirent direct
# if HAVE_SYS_NDIR_H
#  include <sys/ndir.h>
# endif
#endif

#define DEBUG

#include "server.h"
#include "playlist.h"
#include "rbtree.h"
#include "common.h"
#include "files.h"
#include "tags_cache.h"
#include "log.h"
#include "audio.h"

/* Number used to create cache version tag to detect incompatibilities
 * between cache verswion stored on the disk and MOC/BerkeleyDB environment.
 *
 * If you modify the DB structure, increase this number.
 */
#define CACHE_DB_FORMAT_VERSION	1

struct cache_record
{
	time_t mod_time;		/* last modification time of the file */
	time_t atime;			/* Time of last access. */
	struct file_tags *tags;
};

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

static size_t strlen_null (const char *s)
{
	return s ? strlen(s) : 0;
}

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
		+ sizeof(size_t) * 3 /* lenghts of title, artist, time. */
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

static int cache_record_deserialize (struct cache_record *rec,
		const char *serialized, const size_t size,
		const int skip_tags)
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

#define extract_num(var)			\
	if (bytes_left < sizeof(var))		\
		goto err;			\
	memcpy (&var, p, sizeof(var));		\
	bytes_left -= sizeof(var);		\
	p += sizeof(var);

#define extract_str(var)			\
	if (bytes_left < sizeof(str_len))	\
		goto err;			\
	memcpy (&str_len, p, sizeof(str_len));	\
	p += sizeof(str_len);			\
	if (bytes_left < str_len)		\
		goto err;			\
	var = xmalloc (str_len + 1);		\
	memcpy (var, p, str_len);		\
	var[str_len] = '\0';			\
	p += str_len;

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
	logit ("Cache record deserialization error at %dB", p - serialized);
	tags_free (rec->tags);
	rec->tags = NULL;
	return 0;
}

static void tags_cache_remove_rec (struct tags_cache *c, const char *fname)
{
	DBT key;
	int ret;

	assert (c != NULL);
	assert (c->db != NULL);
	assert (fname != NULL);

	debug ("Removing %s from the cache...", fname);

	memset (&key, 0, sizeof(key));
	key.data = (void *)fname;
	key.size = strlen (fname);

	ret = c->db->del (c->db, NULL, &key, 0);
	if (ret)
		logit ("Can't remove item for %s from the cache: %s", fname,
				db_strerror(ret));
}

/* Remove the one element of the cache based on it's access time. */
static void tags_cache_gc (struct tags_cache *c)
{
	DBC *cur;
	DBT key;
	DBT serialized_cache_rec;
	int ret;
	char *last_referenced = NULL;
	time_t last_referenced_atime = time (NULL);
	int nitems = 0;

	assert (c != NULL);
	if (!c->db)
		return;

	c->db->cursor (c->db, NULL, &cur, 0);

	memset (&key, 0, sizeof(key));
	memset (&serialized_cache_rec, 0, sizeof(serialized_cache_rec));

	key.flags = DB_DBT_MALLOC;
	serialized_cache_rec.flags = DB_DBT_MALLOC;

	while ((ret = cur->c_get(cur, &key, &serialized_cache_rec, DB_NEXT))
			== 0) {
		struct cache_record rec;

		if (cache_record_deserialize(&rec, serialized_cache_rec.data,
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
		logit ("Searching for element to remove failed (coursor): %s",
				db_strerror(ret));

	cur->c_close (cur);

	debug ("Elements in cache: %d (limit %d)", nitems, c->max_items);

	if (last_referenced) {
		if (nitems > c->max_items)
			tags_cache_remove_rec (c, last_referenced);
		free (last_referenced);
	}
	else
		debug ("Cache empty");

}

/* Add this tags object for the file to the cache. */
static void tags_cache_add (struct tags_cache *c, const char *file,
		struct file_tags *tags)
{
	char *serialized_cache_rec;
	int serial_len;
	struct cache_record rec;
	DBT key;
	DBT data;
	int ret;

	assert (c != NULL);
	assert (tags != NULL);
	assert (file != NULL);

	rec.mod_time = get_mtime (file);
	rec.atime = time (NULL);
	rec.tags = tags;

	serialized_cache_rec = cache_record_serialize (&rec, &serial_len);
	if (!serialized_cache_rec)
		return;
	
	memset (&key, 0, sizeof(key));
	memset (&data, 0, sizeof(data));

	key.data = (void *)file;
	key.size = strlen(file);

	data.data = serialized_cache_rec;
	data.size = serial_len;

	tags_cache_gc  (c);

	ret = c->db->put (c->db, NULL, &key, &data, 0);
	if (ret) {
		logit ("DB put error: %s", db_strerror(ret));
	}

	free (serialized_cache_rec);
}

/* Read the selected tags for this file and add it to the cache.
 * If client_id != -1, the server is notified using tags_response().
 * If client_id == -1, copy of file_tag sis returned. */
static struct file_tags *tags_cache_read_add (struct tags_cache *c,
		const int client_id, const char *file, int tags_sel)
{
	struct file_tags *tags;
	DBT key;
	DBT serialized_cache_rec;
	DB_LOCK lock;
	int got_lock = 0;
	int ret;
		
	assert (c != NULL);
	assert (c->db != NULL);
	assert (file != NULL);

	debug ("Getting tags for %s", file);

	memset (&key, 0, sizeof(key));
	memset (&serialized_cache_rec, 0, sizeof(serialized_cache_rec));

	key.data = (void *)file;
	key.size = strlen(file);
	serialized_cache_rec.flags = DB_DBT_MALLOC;

	ret = c->db_env->lock_get (c->db_env, c->locker, 0,
			&key, DB_LOCK_WRITE, &lock);
	if (ret) {
		logit ("Can't get DB lock: %s", db_strerror(ret));
	}
	else {
		got_lock = 1;
		ret = c->db->get (c->db, NULL, &key, &serialized_cache_rec, 0);
		if (ret && ret != DB_NOTFOUND)
			logit ("Cache db get error: %s", db_strerror(ret));
	}

	/* If this entry is already presend in the cache, we have 3 options:
	 * we must read different tags (TAGS_*) or the tags are outdated
	 * or this is an immediate tags read (client_id == -1) */
	if (ret == 0) {
		struct cache_record rec;

		if (cache_record_deserialize(&rec, serialized_cache_rec.data,
				serialized_cache_rec.size, 0)) {
			time_t curr_mtime = get_mtime(file);

			if (rec.mod_time != curr_mtime) {

				/* outdated tags - remove them and reread */
				tags_free (rec.tags);
				rec.tags = tags_new ();
				rec.mod_time = curr_mtime;

				debug ("Tags in the cache are outdated");
			}
			else if ((rec.tags->filled & tags_sel) == tags_sel
					&& client_id == -1) {
				debug ("Tags are in the cache.");
				tags = rec.tags;

				goto end;
			}
			else {
				tags = rec.tags; /* read tags in addition to already
						      present tags */
				debug ("Tags in the cache are not what we want.");
			}
		}
		tags = tags_new ();
	}
	else {
		tags = tags_new ();
	}

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

	debug ("Adding/updating cache obiect");
	tags_cache_add (c, file, tags);

	if (client_id != -1) {
		tags_response (client_id, file, tags);
		tags_free (tags);
		tags = NULL;
	}
	
	/* TODO: Removed the oldest items from the cache if we exceeded the maximum
	 * cache size */

end:
	if (got_lock) {
		ret = c->db_env->lock_put (c->db_env, &lock);
		if (ret)
			logit ("Can't release DB lock: %s", db_strerror(ret));
	}

	if (serialized_cache_rec.data)
		free (serialized_cache_rec.data);

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
		int tags_sel = 0;
		
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

void tags_cache_init (struct tags_cache *c, const size_t max_size)
{
	int i;
	
	assert (c != NULL);

	c->db_env = NULL;
	c->db = NULL;

	for (i = 0; i < CLIENTS_MAX; i++)
		request_queue_init (&c->queues[i]);

	c->max_items = max_size;
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

	if (c->db) {
		c->db->close (c->db, 0);
		c->db = NULL;
	}

	if (c->db_env) {
		c->db_env->lock_id_free (c->db_env, c->locker);

		c->db_env->close (c->db_env, 0);
		c->db_env = NULL;
	}

	if (pthread_join(c->reader_thread, NULL))
		fatal ("pthread_join() on cache reader thread failed.");

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
	DBT serialized_cache_rec;
	DBT key;
	int db_ret;
	int got_lock;
	DB_LOCK lock;
	
	assert (c != NULL);
	assert (file != NULL);
	assert (client_id >= 0 && client_id < CLIENTS_MAX);

	debug ("Request for tags for %s from client %d", file, client_id);
	
	memset (&key, 0, sizeof(key));
	key.data = (void *)file;
	key.size = strlen(file);

	memset (&serialized_cache_rec, 0, sizeof(serialized_cache_rec));
	serialized_cache_rec.flags = DB_DBT_MALLOC;

	db_ret = c->db_env->lock_get (c->db_env, c->locker, 0,
			&key, DB_LOCK_WRITE, &lock);
	if (db_ret) {
		got_lock = 0;
		logit ("Can't get DB lock: %s", db_strerror(db_ret));
	}
	else {
		got_lock = 1;
	}

	if (c->db) {
		db_ret = c->db->get(c->db, NULL, &key, &serialized_cache_rec, 0);

		if (db_ret && db_ret != DB_NOTFOUND)
			error ("Cache DB search error: %s", db_strerror(db_ret));
	}
	else
		db_ret = DB_NOTFOUND;

	if (db_ret == 0) {
		struct cache_record rec;

		if (cache_record_deserialize(&rec, serialized_cache_rec.data,
					serialized_cache_rec.size, 0)) {
			if (rec.mod_time == get_mtime(file)
					&& (rec.tags->filled & tags_sel) == tags_sel) {
				tags_response (client_id, file, rec.tags);
				tags_free (rec.tags);
				debug ("Tags are present in the cache");
				goto end;
			}

			debug ("Found outdated or not complete tags in the cache");
		}
	}

	LOCK (c->mutex);
	request_queue_add (&c->queues[client_id], file, tags_sel);
	pthread_cond_signal (&c->request_cond);
	UNLOCK (c->mutex);

end:
	if (got_lock) {
		db_ret = c->db_env->lock_put (c->db_env, &lock);
		if (db_ret)
			logit ("Can't release DB lock: %s", db_strerror(db_ret));
	}

	if (serialized_cache_rec.data)
		free (serialized_cache_rec.data);
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
	//TODO: to remove

	assert (c != NULL);
	assert (file_name != NULL);
}

/* Purge content of a directory. */
static int purge_directory (const char *dir_path)
{
	DIR *dir;
	struct dirent *d;

	logit ("Purging %s...", dir_path);

	dir = opendir (dir_path);
	if (!dir) {
		logit ("Can't open directory %s: %s", dir_path,
				strerror(errno));
		return 0;
	}

	while ((d = readdir(dir))) {
		struct stat st;
		char *fpath;
		int len;
		
		if (!strcmp(d->d_name, ".") || !strcmp(d->d_name, ".."))
			continue;
		
		len = strlen(dir_path) + strlen(d->d_name) + 2;
		fpath = (char *)xmalloc (len);
		snprintf (fpath, len, "%s/%s", dir_path, d->d_name);

		if (stat(fpath, &st) < 0) {
			logit ("Can't stat %s: %s", fpath, strerror(errno));
			free (fpath);
			closedir (dir);
			return 0;
		}

		if (S_ISDIR(st.st_mode)) {
			if (!purge_directory(fpath)) {
				free (fpath);
				closedir (dir);
				return 0;
			}

			logit ("Removing directory %s...", fpath);
			if (rmdir(fpath) < 0) {
				logit ("Can't remove %s: %s", fpath, strerror(errno));
				free (fpath);
				closedir (dir);
				return 0;
			}
		}
		else {
			logit ("Removing file %s...", fpath);

			if (unlink(fpath) < 0) {
				logit ("Can't remove %s: %s", fpath, strerror(errno));
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

/* Create a MOC/db version string.
 *
 * @param buf Output buffer (at least 64 chars long)
 */
static const char *create_version_tag (char *buf)
{
	int db_major;
	int db_minor;

	db_version (&db_major, &db_minor, NULL);

	snprintf (buf, 64, "%d %d %d", CACHE_DB_FORMAT_VERSION, db_major,
			db_minor);

	return buf;
}

/* Chcech version of the cache directory. If it was created
 * using format not handled by this version of MOCE, return 0.
 */
static int cache_version_matches (const char *cache_dir)
{
	char *fname = NULL;
	char disk_version_tag[65];
	ssize_t rres;
	FILE *f;
	int compare_result = 0;

	fname = (char *)xmalloc (strlen(cache_dir) + sizeof("/moc_version_tag"));
	sprintf (fname, "%s/moc_version_tag", cache_dir);

	f = fopen (fname, "r");
	if (!f) {
		logit ("No moc_version_tag in cache directory");
		free (fname);
		return 0;
	}

	rres = fread (disk_version_tag, 1, sizeof(disk_version_tag) - 1, f);
	if (rres == sizeof(disk_version_tag) - 1) {
		logit ("Too long on-disk version tag");
	}
	else {
		char cur_version_tag[64];
		disk_version_tag[rres] = '\0';

		create_version_tag (cur_version_tag);
		compare_result = !strcmp (disk_version_tag, cur_version_tag);
	}


	fclose (f);
	free (fname);

	return compare_result;
}

static void write_cache_version (const char *cache_dir)
{
	char cur_version_tag[64];
	char *fname = NULL;
	FILE *f;

	fname = (char *)xmalloc (strlen(cache_dir) + sizeof("/moc_version_tag"));
	sprintf (fname, "%s/moc_version_tag", cache_dir);

	f = fopen (fname, "w");
	if (!f) {
		logit ("Error writing cache version tag: %s",
				strerror(errno));
		free (fname);
		return;
	}

	create_version_tag (cur_version_tag);
	fwrite (cur_version_tag, 1, strlen(cur_version_tag), f);

	free (fname);
	fclose (f);
}

/* Make sure that the cache directory exist and clear it if necessary.
 */
static int prepare_cache_dir (const char *file_name)
{
	if (mkdir(file_name, 0700) == 0)
		return 1;

	if (errno != EEXIST) {
		error ("Failed to create directory for tags cache: %s",
				strerror(errno));
		return 0;
	}

	if (!cache_version_matches(file_name)) {
		logit ("Tags cache directory is in wrong version, purging....");

		if (!purge_directory(file_name))
			return 0;
		write_cache_version (file_name);
	}

	return 1;
}

void tags_cache_load (struct tags_cache *c, const char *file_name)
{
	int ret;

	if (!prepare_cache_dir(file_name))
		return;

	ret = db_env_create (&c->db_env, 0);
	if (ret) {
		error ("Can't create DB environment: %s", db_strerror(ret));
		return;
	}

	ret = c->db_env->open (c->db_env, file_name,
			DB_CREATE  | DB_INIT_MPOOL | DB_THREAD | DB_INIT_LOCK,
			0);
	if (ret) {
		logit ("Can't open DB environment (%s): %s",
				file_name, db_strerror(ret));
		c->db_env->close (c->db_env, 0);
		c->db_env = NULL;
		return;
	}


	ret = c->db_env->lock_id (c->db_env, &c->locker);
	if (ret) {
		error ("Failed to get DB locker: %s", db_strerror(ret));
		c->db = NULL;

		c->db_env->close (c->db_env, 0);
		c->db_env = NULL;

		return;
	}

	ret = db_create (&c->db, c->db_env, 0);
	if (ret) {
		error ("Failed to create cache db: %s", db_strerror(ret));
		c->db = NULL;

		c->db_env->close (c->db_env, 0);
		c->db_env = NULL;

		return;
	}

	ret = c->db->open (c->db, NULL, "tags.db", NULL, DB_BTREE,
			DB_CREATE, 0);
	if (ret) {
		error ("Failed to open (or create) tags cache db: %s",
				db_strerror(ret));
		abort ();
		c->db->close (c->db, 0);
		c->db = NULL;

		c->db_env->close (c->db_env, 0);
		c->db_env = NULL;

		return;
	}
}

/* Immediatelly read tags for a file bypassing the request queue. */
struct file_tags *tags_cache_get_immediate (struct tags_cache *c,
		const char *file, const int tags_sel)
{
	struct file_tags *tags;

	assert (c != NULL);
	assert (file != NULL);

	debug ("Immediate tags read for %s", file);
	if (!is_url(file))
		tags = tags_cache_read_add (c, -1, file, tags_sel);
	else
		tags = tags_new ();

	return tags;
}
