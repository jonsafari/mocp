/*
 * MOC - music on console
 * Copyright (C) 2004,2005 Damian Pietras <daper@daper.net>
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
#include <stdint.h>
#include <assert.h>

#define DEBUG

#include "common.h"
#include "playlist.h"
#include "log.h"
#include "options.h"
#include "files.h"
#include "rbtree.h"
#include "utf8.h"
#include "rcc.h"

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

void tags_clear (struct file_tags *tags)
{
	assert (tags != NULL);

	if (tags->title)
		free (tags->title);
	if (tags->artist)
		free (tags->artist);
	if (tags->album)
		free (tags->album);

	tags->title = NULL;
	tags->artist = NULL;
	tags->album = NULL;
	tags->track = -1;
	tags->time = -1;
}

/* Copy the tags data from src to dst freeing old fields if necessary. */
void tags_copy (struct file_tags *dst, const struct file_tags *src)
{
	if (dst->title)
		free (dst->title);
	dst->title = xstrdup (src->title);

	if (dst->artist)
		free (dst->artist);
	dst->artist = xstrdup (src->artist);

	if (dst->album)
		free (dst->album);
	dst->album = xstrdup (src->album);

	dst->track = src->track;
	dst->time = src->time;
	dst->filled = src->filled;
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

struct file_tags *tags_dup (const struct file_tags *tags)
{
	struct file_tags *dtags;

	assert (tags != NULL);

	dtags = tags_new();
	tags_copy (dtags, tags);

	return dtags;
}

static int rb_compare (const void *a, const void *b, const void *adata)
{
	struct plist *plist = (struct plist *)adata;
	int pos_a = (intptr_t)a;
	int pos_b = (intptr_t)b;

	return strcoll (plist->items[pos_a].file, plist->items[pos_b].file);
}

static int rb_fname_compare (const void *key, const void *data,
                             const void *adata)
{
	struct plist *plist = (struct plist *)adata;
	const char *fname = (const char *)key;
	const int pos = (intptr_t)data;

	return strcoll (fname, plist->items[pos].file);
}

/* Return 1 if an item has 'deleted' flag. */
inline int plist_deleted (const struct plist *plist, const int num)
{
	assert (LIMIT(num, plist->num));

	return plist->items[num].deleted;
}

/* Initialize the playlist. */
void plist_init (struct plist *plist)
{
	plist->num = 0;
	plist->allocated = INIT_SIZE;
	plist->not_deleted = 0;
	plist->items = (struct plist_item *)xmalloc (sizeof(struct plist_item)
			* INIT_SIZE);
	plist->serial = -1;
	plist->search_tree = rb_tree_new (rb_compare, rb_fname_compare, plist);
	plist->total_time = 0;
	plist->items_with_time = 0;
}

/* Create a new playlist item with empty fields. */
struct plist_item *plist_new_item ()
{
	struct plist_item *item;

	item = (struct plist_item *)xmalloc (sizeof(struct plist_item));
	item->file = NULL;
	item->type = F_OTHER;
	item->deleted = 0;
	item->title_file = NULL;
	item->title_tags = NULL;
	item->tags = NULL;
	item->mtime = (time_t)-1;
	item->queue_pos = 0;

	return item;
}

/* Add a file to the list. Return the index of the item. */
int plist_add (struct plist *plist, const char *file_name)
{
	assert (plist != NULL);
	assert (plist->items != NULL);

	if (plist->allocated == plist->num) {
		plist->allocated *= 2;
		plist->items = (struct plist_item *)xrealloc (plist->items,
				sizeof(struct plist_item) * plist->allocated);
	}

	plist->items[plist->num].file = xstrdup (file_name);
	plist->items[plist->num].type = file_name ? file_type (file_name)
		: F_OTHER;
	plist->items[plist->num].deleted = 0;
	plist->items[plist->num].title_file = NULL;
	plist->items[plist->num].title_tags = NULL;
	plist->items[plist->num].tags = NULL;
	plist->items[plist->num].mtime = (file_name ? get_mtime(file_name)
			: (time_t)-1);
	plist->items[plist->num].queue_pos = 0;

	if (file_name) {
		rb_delete (plist->search_tree, file_name);
		rb_insert (plist->search_tree, (void *)(intptr_t)plist->num);
	}

	plist->num++;
	plist->not_deleted++;

	return plist->num - 1;
}

/* Copy all fields of item src to dst. */
void plist_item_copy (struct plist_item *dst, const struct plist_item *src)
{
	if (dst->file)
		free (dst->file);
	dst->file = xstrdup (src->file);
	dst->type = src->type;
	dst->title_file = xstrdup (src->title_file);
	dst->title_tags = xstrdup (src->title_tags);
	dst->mtime = src->mtime;
	dst->queue_pos = src->queue_pos;

	if (src->tags)
		dst->tags = tags_dup (src->tags);
	else
		dst->tags = NULL;

	dst->deleted = src->deleted;
}

/* Get the pointer to the element on the playlist.
 * If the item number is not valid, return NULL.
 * Returned memory is malloced.
 */
char *plist_get_file (const struct plist *plist, int i)
{
	char *file = NULL;

	assert (i >= 0);
	assert (plist != NULL);

	if (i < plist->num)
		file = xstrdup (plist->items[i].file);

	return file;
}

/* Get the number of the next item on the list (skipping deleted items).
 * If num == -1, get the first item.
 * Return -1 if there are no items left.
 */
int plist_next (struct plist *plist, int num)
{
	int i = num + 1;

	assert (plist != NULL);
	assert (num >= -1);

	while (i < plist->num && plist->items[i].deleted)
		i++;

	return i < plist->num ? i : -1;
}

/* Get the number of the previous item on the list (skipping deleted items).
 * If num == -1, get the first item.
 * Return -1 if it is the beginning of the playlist.
 */
int plist_prev (struct plist *plist, int num)
{
	int i = num - 1;

	assert (plist != NULL);
	assert (num >= -1);

	while (i >= 0 && plist->items[i].deleted)
		i--;

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
}

/* Clear the list. */
void plist_clear (struct plist *plist)
{
	int i;

	assert (plist != NULL);

	for (i = 0; i < plist->num; i++)
		plist_free_item_fields (&plist->items[i]);

	plist->items = (struct plist_item *)xrealloc (plist->items,
			sizeof(struct plist_item) * INIT_SIZE);
	plist->allocated = INIT_SIZE;
	plist->num = 0;
	plist->not_deleted = 0;
	rb_tree_clear (plist->search_tree);
	plist->total_time = 0;
	plist->items_with_time = 0;
}

/* Destroy the list freeing memory; the list can't be used after that. */
void plist_free (struct plist *plist)
{
	assert (plist != NULL);

	plist_clear (plist);
	free (plist->items);
	plist->allocated = 0;
	plist->items = NULL;
	rb_tree_free (plist->search_tree);
}

/* Sort the playlist by file names. */
void plist_sort_fname (struct plist *plist)
{
	struct plist_item *sorted;
	struct rb_node *x;
	int n;

	if (plist_count(plist) == 0)
		return;

	sorted = (struct plist_item *)xmalloc (plist_count(plist) *
			sizeof(struct plist_item));

	x = rb_min (plist->search_tree);
	assert (!rb_is_null(x));

	while (plist_deleted(plist, (intptr_t)rb_get_data (x)))
		x = rb_next (x);

	sorted[0] = plist->items[(intptr_t)rb_get_data (x)];
	rb_set_data (x, NULL);

	n = 1;
	while (!rb_is_null(x = rb_next(x))) {
		if (!plist_deleted(plist, (intptr_t)rb_get_data (x))) {
			sorted[n] = plist->items[(intptr_t)rb_get_data (x)];
			rb_set_data (x, (void *)(intptr_t)n++);
		}
	}

	plist->num = n;
	plist->not_deleted = n;

	memcpy (plist->items, sorted, sizeof(struct plist_item) * n);
	free (sorted);
}

/* Find an item in the list.  Return the index or -1 if not found. */
int plist_find_fname (struct plist *plist, const char *file)
{
	struct rb_node *x;

	assert (plist != NULL);

	x = rb_search (plist->search_tree, file);

	if (rb_is_null(x))
		return -1;

	return !plist_deleted(plist, (intptr_t)rb_get_data (x)) ?
                                 (intptr_t)rb_get_data (x) : -1;
}

/* Find an item in the list; also find deleted items.  If there is more than
 * one item for this file, return the non-deleted one or, if all are deleted,
 * return the last of them.  Return the index or -1 if not found. */
int plist_find_del_fname (const struct plist *plist, const char *file)
{
	int i;
	int item = -1;

	assert (plist != NULL);

	for (i = 0; i < plist->num; i++) {
		if (plist->items[i].file
				&& !strcmp(plist->items[i].file, file)) {
			if (item == -1 || plist_deleted(plist, item))
				item = i;
		}
	}

	return item;
}

/* Returns the next filename that is a dead entry, or NULL if there are none
 * left.
 *
 * It will set the index on success.
 */
const char *plist_get_next_dead_entry (const struct plist *plist,
                                       int *last_index)
{
	int i;

	assert (last_index != NULL);
	assert (plist != NULL);

	for (i = *last_index; i < plist->num; i++) {
		if (plist->items[i].file
			  && ! plist_deleted(plist, i)
			  && ! can_read_file(plist->items[i].file)) {
			*last_index = i + 1;
			return plist->items[i].file;
		}
	}

	return NULL;
}

#define if_not_empty(str)	(tags && (str) && (*str) ? (str) : NULL)

static char *title_expn_subs(char fmt, const struct file_tags *tags)
{
	static char track[16];

	switch (fmt) {
		case 'n':
			if (!tags || tags->track == -1)
				break;
			snprintf (track, sizeof(track), "%d", tags->track);
			return track;
		case 'a':
			return if_not_empty (tags->artist);
		case 'A':
			return if_not_empty (tags->album);
		case 't':
			return if_not_empty (tags->title);
		default:
			fatal ("Error parsing format string!");
	}

	return NULL;
}

static inline void check_zero (const char *x)
{
	if (*x == '\0')
		fatal ("Unexpected end of title expression!");
}

/* Generate a title from fmt. */
static void do_title_expn (char *dest, int size, const char *fmt,
		const struct file_tags *tags)
{
	const char *h;
	int free = --size;
	short escape = 0;

	dest[0] = 0;

	while (free > 0 && *fmt) {
		if (*fmt == '%' && !escape) {
			check_zero(++fmt);

			/* do ternary expansion
			 * format: %(x:true:false)
			 */
			if (*fmt == '(') {
				char separator, expr[256];
				int expr_pos = 0;

				check_zero(++fmt);
				h = title_expn_subs(*fmt, tags);

				check_zero(++fmt);
				separator = *fmt;

				check_zero(++fmt);

				if(h) { /* true */

					/* copy the expression */
					while (escape || *fmt != separator) {
						if (expr_pos == sizeof(expr)-2)
							fatal ("Nested ternary expression too long!");
						expr[expr_pos++] = *fmt;
						if (*fmt == '\\')
							escape = 1;
						else
							escape = 0;
						check_zero(++fmt);
					}
					expr[expr_pos] = '\0';

					/* eat the rest */
					while (escape || *fmt != ')') {
						if (escape)
							escape = 0;
						else if (*fmt == '\\')
							escape = 1;
						check_zero(++fmt);
					}
				}
				else { /* false */

					/* eat the truth :-) */
					while (escape || *fmt != separator) {
						if (escape)
							escape = 0;
						else if (*fmt == '\\')
							escape = 1;
						check_zero(++fmt);
					}

					check_zero(++fmt);

					/* copy the expression */
					while (escape || *fmt != ')') {
						if (expr_pos == sizeof(expr)-2)
							fatal ("Ternary expression too long!");
						expr[expr_pos++] = *fmt;
						if (*fmt == '\\')
							escape = 1;
						else
							escape = 0;
						check_zero(++fmt);
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

	free = MAX(free, 0); /* Possible integer overflow? */
	dest[size - free] = '\0';
}

/* Build file title from struct file_tags. Returned memory is malloc()ed. */
char *build_title_with_format (const struct file_tags *tags, const char *fmt)
{
	char title[512];

	do_title_expn (title, sizeof(title), fmt, tags);
	return xstrdup (title);
}

/* Build file title from struct file_tags. Returned memory is malloc()ed. */
char *build_title (const struct file_tags *tags)
{
	return build_title_with_format (tags, options_get_str ("FormatString"));
}

/* Copy the item to the playlist. Return the index of the added item. */
int plist_add_from_item (struct plist *plist, const struct plist_item *item)
{
	int pos = plist_add (plist, item->file);

	plist_item_copy (&plist->items[pos], item);

	if (item->tags && item->tags->time != -1) {
		plist->total_time += item->tags->time;
		plist->items_with_time++;
	}

	return pos;
}

void plist_delete (struct plist *plist, const int num)
{
	assert (plist != NULL);
	assert (!plist->items[num].deleted);
	assert (plist->not_deleted > 0);

	if (num < plist->num) {

		/* Free every field except the file, it is needed in deleted
		 * items. */
		char *file = plist->items[num].file;

		plist->items[num].file = NULL;

		if (plist->items[num].tags
				&& plist->items[num].tags->time != -1) {
			plist->total_time -= plist->items[num].tags->time;
			plist->items_with_time--;
		}

		plist_free_item_fields (&plist->items[num]);
		plist->items[num].file = file;

		plist->items[num].deleted = 1;

		plist->not_deleted--;
	}
}

/* Count non-deleted items. */
int plist_count (const struct plist *plist)
{
	assert (plist != NULL);

	return plist->not_deleted;
}

/* Set tags title of an item. */
void plist_set_title_tags (struct plist *plist, const int num,
		const char *title)
{
	assert (LIMIT(num, plist->num));

	if (plist->items[num].title_tags)
		free (plist->items[num].title_tags);
	plist->items[num].title_tags = xstrdup (title);
}

/* Set file title of an item. */
void plist_set_title_file (struct plist *plist, const int num,
		const char *title)
{
	assert (LIMIT(num, plist->num));

	if (plist->items[num].title_file)
		free (plist->items[num].title_file);

#ifdef  HAVE_RCC
	if (options_get_bool ("UseRCCForFilesystem")) {
		char *t_str = xstrdup (title);
		plist->items[num].title_file = rcc_reencode (t_str);
		return;
	}
#endif

	plist->items[num].title_file = xstrdup (title);
}

/* Set file for an item. */
void plist_set_file (struct plist *plist, const int num, const char *file)
{
	assert (LIMIT(num, plist->num));
	assert (file != NULL);

	if (plist->items[num].file) {
		rb_delete (plist->search_tree, file);
		free (plist->items[num].file);
	}

	plist->items[num].file = xstrdup (file);
	plist->items[num].type = file_type (file);
	plist->items[num].mtime = get_mtime (file);
	rb_insert (plist->search_tree, (void *)(intptr_t)num);
}

/* Add the content of playlist b to a by copying items. */
void plist_cat (struct plist *a, struct plist *b)
{
	int i;

	assert (a != NULL);
	assert (b != NULL);

	for (i = 0; i < b->num; i++) {
		assert (b->items[i].file != NULL);

		if (!plist_deleted (b, i) &&
		    plist_find_fname (a, b->items[i].file) == -1)
			plist_add_from_item (a, &b->items[i]);
	}
}

/* Set the time tags field for the item. */
void plist_set_item_time (struct plist *plist, const int num, const int time)
{
	int old_time;

	assert (plist != NULL);
	assert (LIMIT(num, plist->num));

	if (!plist->items[num].tags) {
		plist->items[num].tags = tags_new ();
		old_time = -1;
	}
	else if (plist->items[num].tags->time != -1)
		old_time = plist->items[num].tags->time;
	else
		old_time = -1;

	if (old_time != -1) {
		plist->total_time -= old_time;
		plist->items_with_time--;
	}

	if (time != -1) {
		plist->total_time += time;
		plist->items_with_time++;
	}

	plist->items[num].tags->time = time;
	plist->items[num].tags->filled |= TAGS_TIME;
}

int get_item_time (const struct plist *plist, const int i)
{
	assert (plist != NULL);

	if (plist->items[i].tags)
		return plist->items[i].tags->time;

	return -1;
}

/* Return the total time of all files on the playlist having the time tag.
 * If the time information is missing for any file, all_files is set to 0,
 * otherwise 1.
 * Returned value is that counted by plist_count_time(), so may be not
 * up-to-date. */
int plist_total_time (const struct plist *plist, int *all_files)
{
	*all_files = plist->not_deleted == plist->items_with_time;

	return plist->total_time;
}

/* Swap two items on the playlist. */
static void plist_swap (struct plist *plist, const int a, const int b)
{
	assert (plist != NULL);
	assert (LIMIT(a, plist->num));
	assert (LIMIT(b, plist->num));

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

	for (i = 0; i < plist->num; i += 1)
		plist_swap (plist, i, (rand () / (float)RAND_MAX) * (plist->num - 1));

	rb_tree_clear (plist->search_tree);

	for (i = 0; i < plist->num; i++)
		rb_insert (plist->search_tree, (void *)(intptr_t)i);
}

/* Swap the first item on the playlist with the item with file fname. */
void plist_swap_first_fname (struct plist *plist, const char *fname)
{
	int i;

	assert (plist != NULL);
	assert (fname != NULL);

	i = plist_find_fname (plist, fname);

	if (i != -1 && i != 0) {
		rb_delete (plist->search_tree, fname);
		rb_delete (plist->search_tree, plist->items[0].file);
		plist_swap (plist, 0, i);
		rb_insert (plist->search_tree, NULL);
		rb_insert (plist->search_tree, (void *)(intptr_t)i);
	}
}

void plist_set_serial (struct plist *plist, const int serial)
{
	plist->serial = serial;
}

int plist_get_serial (const struct plist *plist)
{
	return plist->serial;
}

/* Return the index of the last non-deleted item from the playlist.
 * Return -1 if there are no items. */
int plist_last (const struct plist *plist)
{
	int i;

	i = plist->num - 1;

	while (i > 0 && plist_deleted(plist, i))
		i--;

	return i;
}

enum file_type plist_file_type (const struct plist *plist, const int num)
{
	assert (plist != NULL);
	assert (num < plist->num);

	return plist->items[num].type;
}

/* Remove items from playlist 'a' that are also present on playlist 'b'. */
void plist_remove_common_items (struct plist *a, struct plist *b)
{
	int i;

	assert (a != NULL);
	assert (b != NULL);

	for (i = 0; i < a->num; i += 1) {
		if (plist_find_fname(b, a->items[i].file) != -1)
			plist_delete (a, i);
	}
}

void plist_discard_tags (struct plist *plist)
{
	int i;

	assert (plist != NULL);

	for (i = 0; i < plist->num; i++)
		if (!plist_deleted(plist, i) && plist->items[i].tags) {
			tags_free (plist->items[i].tags);
			plist->items[i].tags = NULL;
		}

	plist->items_with_time = 0;
	plist->total_time = 0;
}

void plist_set_tags (struct plist *plist, const int num,
		const struct file_tags *tags)
{
	int old_time;

	assert (plist != NULL);
	assert (LIMIT(num, plist->num));
	assert (tags != NULL);

	if (plist->items[num].tags && plist->items[num].tags->time != -1)
		old_time = plist->items[num].tags->time;
	else
		old_time = -1;

	if (plist->items[num].tags)
		tags_free (plist->items[num].tags);
	plist->items[num].tags = tags_dup (tags);

	if (old_time != -1) {
		plist->total_time -= old_time;
		plist->items_with_time--;
	}

	if (tags->time != -1) {
		plist->total_time += tags->time;
		plist->items_with_time++;
	}
}

struct file_tags *plist_get_tags (const struct plist *plist, const int num)
{
	assert (plist != NULL);
	assert (LIMIT(num, plist->num));

	if (plist->items[num].tags)
		return tags_dup (plist->items[num].tags);

	return NULL;
}

/* Swap two files on the playlist. */
void plist_swap_files (struct plist *plist, const char *file1,
		const char *file2)
{
	struct rb_node *x1, *x2;

	assert (plist != NULL);
	assert (file1 != NULL);
	assert (file2 != NULL);

	x1 = rb_search (plist->search_tree, file1);
	x2 = rb_search (plist->search_tree, file2);

	if (!rb_is_null(x1) && !rb_is_null(x2)) {
		const void *t;

		plist_swap (plist, (intptr_t)rb_get_data (x1),
		                   (intptr_t)rb_get_data (x2));

		t = rb_get_data (x1);
		rb_set_data (x1, rb_get_data (x2));
		rb_set_data (x2, t);
	}
}

/* Return the position of a file in the list, starting with 1. */
int plist_get_position (const struct plist *plist, int num)
{
	int i, pos = 1;

	assert (LIMIT(num, plist->num));

	for (i = 0; i < num; i++) {
		if(!plist->items[i].deleted)
			pos++;
	}

	return pos;
}
