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

	LOCK (plist->mutex);
	if (i < plist->num)
		file = xstrdup (plist->items[i].file);
	UNLOCK (plist->mutex);

	return file;
}

/* Find the real number of the item on the list taking into account deleted
 * items. Return -1 if there is no such item. */
int plist_find (struct plist *plist, int num)
{
	int i = 0;

	assert (num >= 0);
	
	LOCK (plist->mutex);
	while (i < plist->num && num > 0) {
		if (!plist->items[i].deleted)
			num--;
		i++;
	}
	UNLOCK (plist->mutex);

	return i < plist->num && !plist->items[i].deleted ? i : -1;
}

/* Get the number of the next item on the list (skipping deleted items).
 * The real numbed can be retreived by using plist_find().
 * Return -1 if there is no items left.
 */
int plist_next (struct plist *plist, int num)
{
	assert (num >= 0);
	return  num + 1 < plist->num ? num + 1 : -1;
}

/* Clear the list. */
void plist_clear (struct plist *plist)
{
	int i;
	
	LOCK (plist->mutex);
	
	for (i = 0; i < plist->num; i++) {
		free (plist->items[i].file);
		if (plist->items[i].tags)
			tags_free (plist->items[i].tags);
		if (plist->items[i].title)
			free (plist->items[i].title);
	}
	
	plist->items = (struct plist_item *)xrealloc (plist->items,
			sizeof(struct plist_item) * INIT_SIZE);
	plist->allocated = INIT_SIZE;
	plist->num = 0;
	UNLOCK (plist->mutex);
}

/* Destroy the list freeing memory, the list can't be used after that. */
void plist_free (struct plist *plist)
{
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

/* Find an item oon the list. Return the index or -1 if not found. */
int plist_find_fname (const struct plist *plist, const char *file)
{
	int i;

	assert (plist != NULL);

	for (i = 0; i < plist->num; i++)
		if (!strcmp(plist->items[i].file, file))
				return i;
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
