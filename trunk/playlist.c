/*
 * MOC - music on console
 * Copyright (C) 2004 Damian Pietras <daper@daper.net>
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

#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <pthread.h>

#include "playlist.h"
#include "main.h"
#include "log.h"
#include "options.h"

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

	return tags;
}

struct file_tags *tags_dup (const struct file_tags *tags)
{
	struct file_tags *dtags;

	dtags = (struct file_tags *)xmalloc (sizeof(struct file_tags));
	dtags->title = xstrdup (tags->title);
	dtags->artist = xstrdup (tags->artist);
	dtags->album = xstrdup (tags->album);
	dtags->track = tags->track;

	return dtags;
}

/* Initialize the playlist. */
void plist_init (struct plist *plist)
{
	plist->num = 0;
	plist->allocated = INIT_SIZE;
	plist->items = (struct plist_item *)xmalloc (sizeof(struct plist_item)
			* INIT_SIZE);
	pthread_mutex_init (&plist->mutex, NULL);
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
	plist->items[plist->num].deleted = 0;
	plist->items[plist->num].tags = NULL;
	plist->items[plist->num].title = NULL;
	plist->num++;

	UNLOCK (plist->mutex);

	return plist->num - 1;
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

static void plist_free_item_fields (struct plist_item *item)
{
	if (item->file) {
		free (item->file);
		item->file = NULL;
	}
	if (item->tags) {
		tags_free (item->tags);
		item->tags = NULL;
	}
	if (item->title) {
		free (item->title);
		item->title = NULL;
	}
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
	return strcmp (((struct plist_item *)a)->file,
			((struct plist_item *)b)->file);
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

	assert (plist != NULL);

	LOCK (plist->mutex);
	for (i = 0; i < plist->num; i++)
		if (!plist->items[i].deleted && plist->items[i].file
				&& !strcmp(plist->items[i].file, file)) {
			UNLOCK (plist->mutex);
			return i;
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

	if (!*fmt) {
		dest[0] = 0;
		return;
	}

	dest[0] = 0;

	do {
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
	} while (*++fmt && free > 0);

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

/* Copy the item to the playlist. */
void plist_add_from_item (struct plist *plist, const struct plist_item *item)
{
	int pos = plist_add (plist, item->file);

	assert (!item->deleted);

	plist->items[pos].title = xstrdup (item->title);
	if (item->tags)
		plist->items[pos].tags = tags_dup (item->tags);
}

/* Return a random playlist index of a not deleted item. */
/* Return -1 if there are no items. */
int plist_rand (struct plist *plist)
{
	int rnd = (rand() /(float)RAND_MAX) * (plist->num - 1);
	int i = rnd;
		
	assert (plist != NULL);

	if (!plist->num)
		return -1;

	LOCK (plist->mutex);
	while (plist->items[i].deleted && i < plist->num)
		i++;

	if (i == plist->num) {
		i = 0;
		while (i < rnd && plist->items[i].deleted)
			i++;
	}

	if (i == plist->num)
		i = -1;
	UNLOCK (plist->mutex);

	return i;
}

void plist_delete (struct plist *plist, const int num)
{
	assert (plist != NULL);
	
	LOCK (plist->mutex);
	assert (!plist->items[num].deleted);
	if (num < plist->num) {
		plist->items[num].deleted = 1;
		plist_free_item_fields (&plist->items[num]);
	}
	UNLOCK (plist->mutex);
}

/* Count not deleted items. */
int plist_count (struct plist *plist)
{
	int i;
	int count = 0;
	
	assert (plist != NULL);

	for (i = 0; i < plist->num; i++)
		if (!plist->items[i].deleted)
			count++;

	return count;
}

/* Set title of an intem. */
void plist_set_title (struct plist *plist, const int num, const char *title)
{
	assert (num >=0 && num < plist->num);

	if (plist->items[num].title)
		free (plist->items[num].title);
	plist->items[num].title = xstrdup (title);
}

/* Set file for an item. */
void plist_set_file (struct plist *plist, const int num, const char *file)
{
	assert (num >=0 && num < plist->num);

	if (plist->items[num].file)
		free (plist->items[num].file);
	plist->items[num].file = xstrdup (file);
}

/* Return 1 if an item has 'deleted' flag. */
int plist_deleted (const struct plist *plist, const int num)
{
	assert (num >=0 && num < plist->num);
	return plist->items[num].deleted;
}
