/*
 * MOC - music on console
 * Copyright (C) 2004 Damian Pietras <daper@daper.net>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * Author of title building code: Florian Kriener <me@leflo.de>
 *
 * Contributors:
 *  - Florian Kriener <me@leflo.de> - title building code
 *  - Kamil Tarkowski <kamilt@interia.pl> - plist_prev()
 *
 */

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <pthread.h>

#define DEBUG

#include "playlist.h"
#include "main.h"
#include "log.h"
#include "options.h"
#include "files.h"
#include "file_types.h"

/* Initial size of the table */
#define	INIT_SIZE	64

void tags_free (struct file_tags *tags)
{
	assert (tags != NULL);

	if (tags->title)
		free (tags->title);
	if (tags->artist)
		free (tags->artist);
	if (tags->album)
		free (tags->album);

	free (tags);
}

struct file_tags *tags_new ()
{
	struct file_tags *tags;

	tags = (struct file_tags *)xmalloc (sizeof(struct file_tags));
	tags->title = NULL;
	tags->artist = NULL;
	tags->album = NULL;
	tags->track = -1;
	tags->time = -1;
	tags->filled = 0;

	return tags;
}

static struct file_tags *tags_dup (const struct file_tags *tags)
{

	struct file_tags *dtags;

	dtags = (struct file_tags *)xmalloc (sizeof(struct file_tags));
	dtags->title = xstrdup (tags->title);
	dtags->artist = xstrdup (tags->artist);
	dtags->album = xstrdup (tags->album);
	dtags->track = tags->track;
	dtags->time = tags->time;
	dtags->filled = tags->filled;
	
	return dtags;
}

/* Copy missing tags from src to dst. */
static void sync_tags (struct file_tags *src, struct file_tags *dst)
{
	if (src->filled & TAGS_TIME && !(dst->filled & TAGS_TIME)) {
		debug ("Sync time");
		dst->time = src->time;
		dst->filled |= TAGS_TIME;
	}

	if ((src->filled & TAGS_COMMENTS)
			&& !(dst->filled & TAGS_COMMENTS)) {
		debug ("Sync comments");
		dst->title = xstrdup (src->title);
		dst->artist = xstrdup (src->artist);
		dst->album = xstrdup (src->album);
		dst->track = src->track;
		dst->filled |= TAGS_COMMENTS;
	}
}

/* Initialize the playlist. */
void plist_init (struct plist *plist)
{
	plist->num = 0;
	plist->allocated = INIT_SIZE;
	plist->not_deleted = 0;
	plist->items = (struct plist_item *)xmalloc (sizeof(struct plist_item)
			* INIT_SIZE);
	pthread_mutex_init (&plist->mutex, NULL);
}

static int str_hash (const char *file)
{
	long h = 0;
	int i = 0;

	while (file[i]) {
		h ^= i % 2 ? file[i] : file[i] << 8;
		i++;
	}

	return h;
}

/* Create a new playlit item with empty fields. */
struct plist_item *plist_new_item ()
{
	struct plist_item *item;

	item = (struct plist_item *)xmalloc (sizeof(struct plist_item));
	item->file = NULL;
	item->file_hash = -1;
	item->deleted = 0;
	item->title = NULL;
	item->title_file = NULL;
	item->title_tags = NULL;
	item->tags = NULL;
	item->mtime = (time_t)-1;

	return item;
}

/* Add a file to the list. Return the index of the item. */
int plist_add (struct plist *plist, const char *file_name)
{
	LOCK (plist->mutex);
	
	assert (plist != NULL);
	assert (plist->items != NULL);
		
	if (plist->allocated == plist->num) {
		plist->allocated *= 2;
		plist->items = (struct plist_item *)xrealloc (plist->items,
				sizeof(struct plist_item) * plist->allocated);
	}

	plist->items[plist->num].file = xstrdup (file_name);
	plist->items[plist->num].file_hash = file_name
		? str_hash (file_name) : -1;
	plist->items[plist->num].deleted = 0;
	plist->items[plist->num].title = NULL;
	plist->items[plist->num].title_file = NULL;
	plist->items[plist->num].title_tags = NULL;
	plist->items[plist->num].tags = NULL;
	plist->items[plist->num].mtime = (file_name ? get_mtime(file_name)
			: (time_t)-1);
	plist->num++;
	plist->not_deleted++;

	UNLOCK (plist->mutex);

	return plist->num - 1;
}

/* Copy all fields of item src to dst. */
static void plist_item_copy (struct plist_item *dst,
		const struct plist_item *src)
{
	dst->file = xstrdup (src->file);
	dst->file_hash = src->file_hash != -1
		? src->file_hash : str_hash(src->file);
	dst->title_file = xstrdup (src->title_file);
	dst->title_tags = xstrdup (src->title_tags);
	dst->mtime = src->mtime;
	
	if (src->tags)
		dst->tags = tags_dup (src->tags);
	else
		dst->tags = NULL;

	if (src->title == src->title_file)
		dst->title = dst->title_file;
	else if (src->title == src->title_tags)
		dst->title = dst->title_tags;
	else
		dst->title = NULL;
	
	dst->deleted = src->deleted;
}

/* Get the pointer to the element on the playlist.
 * If the item number is not valid, return NULL.
 * Returned memory is malloced.
 */
char *plist_get_file (struct plist *plist, int i)
{
	char *file = NULL;

	assert (i >= 0);
	assert (plist != NULL);

	LOCK (plist->mutex);
	if (i < plist->num)
		file = xstrdup (plist->items[i].file);
	UNLOCK (plist->mutex);

	return file;
}

/* Get the number of the next item on the list (skipping deleted items).
 * If num == -1, get the first item.
 * Return -1 if there is no items left.
 */
int plist_next (struct plist *plist, int num)
{
	int i = num + 1;
	
	assert (plist != NULL);
	assert (num >= -1);

	LOCK (plist->mutex);
	while (i < plist->num && plist->items[i].deleted)
		i++;
	UNLOCK (plist->mutex);

	return i < plist->num ? i : -1;
}

/* Get the number of the previous item on the list (skipping deleted items). 
 * If num == -1, get the first item.             
 * Return -1 if it is beginning of playlist. 
 */                                                                                                                                              
int plist_prev (struct plist *plist, int num)
{
	int i = num - 1;
	
	assert (plist != NULL);
	assert (num >= -1);

	LOCK (plist->mutex);
	while (i >= 0 && plist->items[i].deleted)
		i--;
	UNLOCK (plist->mutex);

	return i >= 0 ? i : -1;
}

void plist_free_item_fields (struct plist_item *item)
{
	if (item->file) {
		free (item->file);
		item->file = NULL;
	}
	if (item->title_tags) {
		free (item->title_tags);
		item->title_tags = NULL;
	}
	if (item->title_file) {
		free (item->title_file);
		item->title_file = NULL;
	}
	if (item->tags) {
		tags_free (item->tags);
		item->tags = NULL;
	}

	item->title = NULL;
}

/* Clear the list. */
void plist_clear (struct plist *plist)
{
	int i;

	assert (plist != NULL);
	
	LOCK (plist->mutex);
	
	for (i = 0; i < plist->num; i++)
		if (!plist->items[i].deleted)
			plist_free_item_fields (&plist->items[i]);
	
	plist->items = (struct plist_item *)xrealloc (plist->items,
			sizeof(struct plist_item) * INIT_SIZE);
	plist->allocated = INIT_SIZE;
	plist->num = 0;
	plist->not_deleted = 0;
	UNLOCK (plist->mutex);
}

/* Destroy the list freeing memory, the list can't be used after that. */
void plist_free (struct plist *plist)
{
	assert (plist != NULL);
	
	plist_clear (plist);
	free (plist->items);
	plist->allocated = 0;
	plist->items = NULL;
	if (pthread_mutex_destroy(&plist->mutex))
		logit ("Can't destry playlist mutex");
}

static int qsort_func_fname (const void *a, const void *b)
{
	struct plist_item *ap = (struct plist_item *)a;
	struct plist_item *bp = (struct plist_item *)b;

	if (ap->deleted || bp->deleted)
		return ap->deleted - bp->deleted;
	
	return strcmp (ap->file, bp->file);
}

/* Sort the playlist by file names. We don't use mutex here. */
void plist_sort_fname (struct plist *plist)
{
	qsort (plist->items, plist->num, sizeof(struct plist_item),
			qsort_func_fname);
}

/* Find an item on the list. Return the index or -1 if not found. */
int plist_find_fname (struct plist *plist, const char *file)
{
	int i;
	int hash = str_hash (file);

	assert (plist != NULL);

	LOCK (plist->mutex);
	for (i = 0; i < plist->num; i++) {
		assert (plist->items[i].file_hash != -1);
		if (!plist->items[i].deleted && plist->items[i].file
				&& plist->items[i].file_hash == hash
				&& !strcmp(plist->items[i].file, file)) {
			UNLOCK (plist->mutex);
			return i;
		}
	}
	UNLOCK (plist->mutex);

	return -1;
}

#define if_not_empty(str)	((str) && (*str) ? (str) : NULL)

static char *title_expn_subs(char fmt, const struct file_tags *tags)
{
	static char track[10];
	
	switch (fmt) {
		case 'n':
			if (tags->track != -1) {
				sprintf (track, "%d", tags->track);
				return track;
			}
			return NULL;
		case 'a':
			return if_not_empty (tags->artist);
		case 'A':
			return if_not_empty (tags->album);
		case 't':
			return if_not_empty (tags->title);
		default:
			fatal ("Error parsing format string.");
	}
	return NULL; /* To avoid gcc warning */
}

/* generate a title from fmt */
#define check_zero(x) if((x) == '\0') \
		fatal ("Unexpected end of title expression")

static void do_title_expn (char *dest, int size, char *fmt,
		const struct file_tags *tags)
{
	char *h;
	int free = --size;
	short escape = 0;

	dest[0] = 0;

	while (free > 0 && *fmt) {
		if (*fmt == '%' && !escape) {
			check_zero(*++fmt);
			
			/* do ternary expansion
			 * format: %(x:true:false)
			 */
			if (*fmt == '(') {
				char separator, expr[100];
				int expr_pos = 0;

				check_zero(*++fmt);
				h = title_expn_subs(*fmt, tags);

				check_zero(*++fmt);
				separator = *fmt;

				check_zero(*++fmt);

				if(h) { /* true */

					/* Copy the expression */
					while (escape || *fmt != separator) {
						if (expr_pos == sizeof(expr)-2)
							fatal ("nasted trenary expression too long");
						expr[expr_pos++] = *fmt;
						if (*fmt == '\\')
							escape = 1;
						else
							escape = 0;
						check_zero(*++fmt);
					}
					expr[expr_pos] = '\0';

                                        /* eat the rest */
					while (escape || *fmt != ')') { 
						if (escape)
							escape = 0;
						else if (*fmt == '\\')
							escape = 1;
						check_zero(*++fmt);
					}
				}
				else { /* false */

					/* eat the truth :-) */
					while (escape || *fmt != separator) {
						if (escape)
							escape = 0;
						else if (*fmt == '\\')
							escape = 1;
						check_zero(*++fmt);
					}

					check_zero(*++fmt);

					/* Copy the expression */
					while (escape || *fmt != ')') {
						if (expr_pos == sizeof(expr)-2)
							fatal ("trenary expression too long");
						expr[expr_pos++] = *fmt;
						if (*fmt == '\\')
							escape = 1;
						else
							escape = 0;
						check_zero(*++fmt);
					}
					expr[expr_pos] = '\0';
				}

				do_title_expn((dest + size - free), 
					      free, expr, tags);
				free -= strlen(dest + size - free);
			}
			else {
				h = title_expn_subs(*fmt, tags);

				if (h) {
					strncat(dest, h, free-1);
					free -= strlen (h);
				}
			}
		}
		else if (*fmt == '\\' && !escape)
			escape = 1;
		else {
			dest[size - free] = *fmt;
			dest[size - free + 1] = 0;
			--free;
			escape = 0;
		}
		fmt++;
	}

	free = free < 0 ? 0 : free; /* Possible integer overflow? */
	dest[size - free] = '\0';
}

/* Build file title from struct file_tags. Returned memory is malloc()ed. */
char *build_title (const struct file_tags *tags)
{
	char title[100];

	do_title_expn (title, sizeof(title), options_get_str("FormatString"),
			tags);
	return xstrdup (title);
}

/* Copy the item to the playlist. Return the index of the added item. */
int plist_add_from_item (struct plist *plist, const struct plist_item *item)
{
	int pos = plist_add (plist, NULL);

	assert (!item->deleted);
	plist_item_copy (&plist->items[pos], item);

	return pos;
}

void plist_delete (struct plist *plist, const int num)
{
	assert (plist != NULL);
	
	LOCK (plist->mutex);
	assert (!plist->items[num].deleted);
	assert (plist->not_deleted > 0);
	if (num < plist->num) {
		plist->items[num].deleted = 1;
		plist_free_item_fields (&plist->items[num]);
	}
	plist->not_deleted--;
	UNLOCK (plist->mutex);
}

/* Count not deleted items. */
int plist_count (struct plist *plist)
{
	assert (plist != NULL);
	
	return plist->not_deleted;
}

/* Set tags title of an item. */
void plist_set_title_tags (struct plist *plist, const int num,
		const char *title)
{
	assert (num >= 0 && num < plist->num);

	if (plist->items[num].title_tags)
		free (plist->items[num].title_tags);
	plist->items[num].title_tags = xstrdup (title);
}

/* Set file title of an item. */
void plist_set_title_file (struct plist *plist, const int num,
		const char *title)
{
	assert (num >= 0 && num < plist->num);

	if (plist->items[num].title_file)
		free (plist->items[num].title_file);
	plist->items[num].title_file = xstrdup (title);
}

/* Set file for an item. */
void plist_set_file (struct plist *plist, const int num, const char *file)
{
	assert (num >=0 && num < plist->num);

	if (plist->items[num].file)
		free (plist->items[num].file);
	plist->items[num].file = xstrdup (file);
	plist->items[num].file_hash = str_hash (file);
	plist->items[num].mtime = get_mtime (file);
}

/* Return 1 if an item has 'deleted' flag. */
int plist_deleted (const struct plist *plist, const int num)
{
	assert (num >=0 && num < plist->num);
	return plist->items[num].deleted;
}

/* Add the content of playlist b to a by copying items. */
void plist_cat (struct plist *a, const struct plist *b)
{
	int i;

	for (i = 0; i < b->num; i++)
		if (plist_find_fname(a, b->items[i].file) == -1)
			plist_add_from_item (a, &b->items[i]);
}

/* Copy titles_tags and times from src to dst if the data are available and
 * up-to-date. */
void sync_plists_data (struct plist *dst, struct plist *src)
{
	int i;
	int idx;

	assert (src != NULL);
	assert (dst != NULL);

	debug ("Synchronizing playlists...");

	for (i = 0; i < src->num; i++)
		if (!plist_deleted(src, i)
				&& (idx = plist_find_fname(dst,
						src->items[i].file)) != -1) {
			debug ("Found item for the file %s",
					src->items[i].file);

			if (src->items[i].mtime == dst->items[idx].mtime) {
				
				/* The file was not modified - copy the missing
				 * data */
				if (!dst->items[idx].title_tags) {
					debug ("Filling title_tags");
					dst->items[idx].title_tags = xstrdup (
							src->items[i].title_tags
							);
				}
				
				if (src->items[i].tags) {
					if (dst->items[idx].tags)
						sync_tags (src->items[i].tags,
								dst->items[idx].tags);
					else {
						debug ("copying tags");
						dst->items[idx].tags =
							tags_dup (src->items[i].tags);
					}
				}
			}
			else if (src->items[i].mtime > dst->items[idx].mtime) {
				debug ("Replacing title_tags and tags.");
				
				/* The file was modified - update the data */
				if (dst->items[idx].title_tags)
					free (dst->items[idx].title_tags);

				dst->items[idx].title_tags = xstrdup (
						src->items[i].title_tags);

				dst->items[idx].mtime = src->items[i].mtime;
				
				if (dst->items[idx].tags)
					tags_free (dst->items[idx].tags);

				dst->items[idx].tags = tags_dup (
						src->items[i].tags);
			}
		}
}

/* Set the time tags field for the item. */
void update_item_time (struct plist_item *item, const int time)
{
	if (!item->tags)
		item->tags = tags_new ();

	item->tags->time = time;
	item->tags->filled |= TAGS_TIME;
}

int get_item_time (const struct plist *plist, const int i)
{
	assert (plist != NULL);
	
	if (plist->items[i].tags)
		return plist->items[i].tags->time;

	return -1;
}

/* Return the total time of all files on the playlist that has the time tag.
 * If the time information is missing for any file, all_files is set to 0,
 * otherwise 1. */
int plist_total_time (const struct plist *plist, int *all_files)
{
	int i;
	int total_time = 0;

	*all_files = 1;

	for (i = 0; i < plist->num; i++)
		if (!plist_deleted(plist, i)) {
			if (!plist->items[i].tags
					|| !(plist->items[i].tags->filled
						& TAGS_TIME)
					|| plist->items[i].tags->time == -1)
				*all_files = 0;
			else
				total_time += plist->items[i].tags->time;
		}

	return total_time;
}

/* Swap two items on the playlist. */
static void plist_swap (struct plist *plist, const int a, const int b)
{
	assert (plist != NULL);
	assert (a >= 0 && a < plist->num);
	assert (b >= 0 && b < plist->num);

	if (a != b) {
		struct plist_item t;

		t = plist->items[a];
		plist->items[a] = plist->items[b];
		plist->items[b] = t;
	}
}

/* Shuffle the playlist. */
void plist_shuffle (struct plist *plist)
{
	int i;

	LOCK (plist->mutex);
	for (i = 0; i < plist->num; i++)
		plist_swap (plist, i,
				(rand()/(float)RAND_MAX) * (plist->num - 1));
	UNLOCK (plist->mutex);
}

/* Swap the first item on the playlist with the item with file fname. */
void plist_swap_first_fname (struct plist *plist, const char *fname)
{
	int i;

	assert (plist != NULL);
	assert (fname != NULL);
	
	i = plist_find_fname (plist, fname);

	if (i != -1)
		plist_swap (plist, 0, i);
}
