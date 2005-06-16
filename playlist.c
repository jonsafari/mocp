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

#define DEBUG

#include "playlist.h"
#include "main.h"
#include "log.h"
#include "options.h"
#include "files.h"

/* Initial size of the table */
#define	INIT_SIZE	64

enum rb_color { RED, BLACK };

/* structure for Red-Black trees */
struct rb_node
{
	struct rb_node *left;
	struct rb_node *right;
	struct rb_node *parent;
	enum rb_color color;
	int item_num;
};

/* item used as a null value */
static struct rb_node rb_null = { NULL, NULL, NULL, BLACK, -1 };

static void rb_left_rotate (struct rb_node **root, struct rb_node *x)
{
	struct rb_node *y = x->right;

	x->right = y->left;
	y->left->parent = x;
	y->parent = x->parent;

	if (x->parent == &rb_null)
		*root = y;
	else {
		if (x == x->parent->left)
			x->parent->left = y;
		else
			x->parent->right = y;
	}

	y->left = x;
	x->parent = y;
}

static void rb_right_rotate (struct rb_node **root, struct rb_node *x)
{
	struct rb_node *y = x->left;

	x->left = y->right;
	y->right->parent = x;
	y->parent = x->parent;

	if (x->parent == &rb_null)
		*root = y;
	else {
		if (x == x->parent->right)
			x->parent->right = y;
		else
			x->parent->left = y;
	}

	y->right = x;
	x->parent = y;
}

static void rb_insert_fixup (struct rb_node **root, struct rb_node *z)
{
	while (z->parent->color == RED)
		if (z->parent == z->parent->parent->left) {
			struct rb_node *y = z->parent->parent->right;
			
			if (y->color == RED) {
				z->parent->color = BLACK;
				y->color = BLACK;
				z->parent->parent->color = RED;
				z = z->parent->parent;
			}
			else if (z == z->parent->right) {
				z = z->parent;
				rb_left_rotate (root, z);
			}
			else {
				z->parent->color = BLACK;
				z->parent->parent->color = RED;
				rb_right_rotate (root, z->parent->parent);
			}
		}
		else {
			struct rb_node *y = z->parent->parent->left;
			
			if (y->color == RED) {
				z->parent->color = BLACK;
				y->color = BLACK;
				z->parent->parent->color = RED;
				z = z->parent->parent;
			}
			else if (z == z->parent->left) {
				z = z->parent;
				rb_right_rotate (root, z);
			}
			else {
				z->parent->color = BLACK;
				z->parent->parent->color = RED;
				rb_left_rotate (root, z->parent->parent);
			}
		}
	
	(*root)->color = BLACK;
}

static void rb_delete_fixup (struct rb_node **root, struct rb_node *x)
{
	struct rb_node *w;
	
	while (x != &rb_null && x->color == BLACK) {
		if (x == x->parent->left) {
			w = x->parent->right;

			if (w->color == RED) {
				w->color = BLACK;
				x->parent->color = RED;
				rb_left_rotate (root, x->parent);
				w = x->parent->right;
			}

			if (w->left->color == BLACK
					&& w->right->color == BLACK) {
				w->color = RED;
				x = x->parent;
			}
			else if (w->right->color == BLACK) {
				w->left->color = BLACK;
				w->color = RED;
				rb_right_rotate (root, w);
				w = x->parent->right;
			}
			else {
				w->color = x->parent->color;
				x->parent->color = BLACK;
				w->right->color = BLACK;
				rb_left_rotate (root, x->parent);
				x = *root;
			}
		}
		else {
			w = x->parent->left;

			if (w->color == RED) {
				w->color = BLACK;
				x->parent->color = RED;
				rb_right_rotate (root, x->parent);
				w = x->parent->left;
			}

			if (w->right->color == BLACK
					&& w->left->color == BLACK) {
				w->color = RED;
				x = x->parent;
			}
			else if (w->left->color == BLACK) {
				w->right->color = BLACK;
				w->color = RED;
				rb_left_rotate (root, w);
				w = x->parent->left;
			}
			else {
				w->color = x->parent->color;
				x->parent->color = BLACK;
				w->left->color = BLACK;
				rb_right_rotate (root, x->parent);
				x = *root;
			}
		}

		x->color = BLACK;
	}
}

/* Insert the playlist position into the reb-black tree. */
static void rb_insert (struct rb_node **root, const struct plist *plist,
		const int num)
{
	struct rb_node *z;
	struct rb_node *y = &rb_null;
	struct rb_node *x = *root;

	assert (root != NULL);
	assert (num >= 0);
	assert (plist->items[num].file != NULL);

	z = (struct rb_node *)xmalloc (sizeof(struct rb_node));

	z->item_num = num;
	
	while (x != &rb_null) {
		y = x;
		if (strcmp(plist->items[z->item_num].file,
					plist->items[x->item_num].file) < 0)
			x = x->left;
		else
			x = x->right;
	}

	z->parent = y;
	if (y == &rb_null)
		*root = z;
	else {
		if (strcmp(plist->items[z->item_num].file,
					plist->items[y->item_num].file)	< 0)
			y->left = z;
		else
			y->right = z;
	}
	
	z->left = &rb_null;
	z->right = &rb_null;
	z->color = RED;

	rb_insert_fixup (root, z);
}

static void rb_destroy (struct rb_node *root)
{
	assert (root != NULL);

	if (root != &rb_null) {	
		rb_destroy (root->left);
		rb_destroy (root->right);
		free (root);
	}
}

static struct rb_node *rb_search_internal (struct rb_node *root,
		const struct plist *plist, const char *file)
{
	struct rb_node *x = root;

	while (x != &rb_null) {
		int cmp = strcmp (file, plist->items[x->item_num].file);
		
		if (cmp < 0)
			x = x->left;
		else if (cmp > 0)
			x = x->right;
		else
			return x;
	}

	return NULL;
}

/* Search in the red-black tree for the item with file name file.
 * Return the item position on the playlist or -1 if not found. */
static int rb_search (struct rb_node *root, const struct plist *plist,
		const char *file)
{
	const struct rb_node *x = rb_search_internal (root, plist, file);
	
	return x ? x->item_num : -1;
}

static struct rb_node *rb_min (struct rb_node *root)
{
	struct rb_node *x = root;

	if (root == &rb_null)
		return NULL;

	while (x->left != &rb_null)
		x = x->left;

	return x;
}

#if 0
static struct rb_node *rb_max (struct rb_node *root)
{
	struct rb_node *x = root;

	if (root == &rb_null)
		return NULL;

	while (x->right != &rb_null)
		x = x->right;

	return x;
}
#endif

static struct rb_node *rb_next (struct rb_node *x)
{
	struct rb_node *y;
	
	if (x->right != &rb_null)
		return rb_min (x->right);
	
	y = x->parent;
	while (y != &rb_null && x == y->right) {
		x = y;
		y = y->parent;
	}

	return y;
}

static void rb_delete (struct rb_node **root, const struct plist *plist,
		const char *file)
{
	struct rb_node *z = rb_search_internal (*root, plist, file);

	if (z) {
		struct rb_node *x, *y;
		
		if (z->left == &rb_null || z->right == &rb_null) 
			y = z;
		else
			y = rb_next (z);

		if (y->left != &rb_null)
			x = y->left;
		else
			x = y->right;

		x->parent = y->parent;
		
		if (y->parent == &rb_null)
			*root = x;
		else if (y == y->parent->left)
			y->parent->left = x;
		else
			y->parent->right = x;

		if (y != z)
			*z = *y;

		if (y->color == BLACK)
			rb_delete_fixup (root, x);
	}
}

/* Update playlist position for item with the given file name. Return 0 if
 * there is no such item. */
static int rb_update_position (struct rb_node *root, const struct plist *plist,
		const char *file, const int new_pos)
{
	struct rb_node *x = rb_search_internal (root, plist, file);

	if (x) {
		x->item_num = new_pos;
		return 1;
	}

	return 0;
}

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

/* Return 1 if an item has 'deleted' flag. */
inline int plist_deleted (const struct plist *plist, const int num)
{
	assert (num >=0 && num < plist->num);
	return plist->items[num].deleted;
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
	plist->serial = -1;
	plist->search_tree = &rb_null;
	plist->total_time = 0;
	plist->time_for_all_files = 0;
}

/* Create a new playlit item with empty fields. */
struct plist_item *plist_new_item ()
{
	struct plist_item *item;

	item = (struct plist_item *)xmalloc (sizeof(struct plist_item));
	item->file = NULL;
	item->type = F_OTHER;
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
	assert (plist != NULL);
	assert (plist->items != NULL);
		
	if (plist->allocated == plist->num) {
		plist->allocated *= 2;
		plist->items = (struct plist_item *)xrealloc (plist->items,
				sizeof(struct plist_item) * plist->allocated);
	}

	plist->items[plist->num].file = xstrdup (file_name);
	plist->items[plist->num].type = F_OTHER;
	plist->items[plist->num].deleted = 0;
	plist->items[plist->num].title = NULL;
	plist->items[plist->num].title_file = NULL;
	plist->items[plist->num].title_tags = NULL;
	plist->items[plist->num].tags = NULL;
	plist->items[plist->num].mtime = (file_name ? get_mtime(file_name)
			: (time_t)-1);

	if (file_name) {
		if (!rb_update_position(plist->search_tree, plist, file_name,
					plist->num))
			rb_insert (&plist->search_tree, plist, plist->num);
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

	if (i < plist->num)
		file = xstrdup (plist->items[i].file);

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

	while (i < plist->num && plist->items[i].deleted)
		i++;

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

	item->title = NULL;
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
	rb_destroy (plist->search_tree);
	plist->search_tree = &rb_null;
	plist->total_time = 0;
	plist->time_for_all_files = 0;
}

/* Destroy the list freeing memory, the list can't be used after that. */
void plist_free (struct plist *plist)
{
	assert (plist != NULL);
	
	plist_clear (plist);
	free (plist->items);
	plist->allocated = 0;
	plist->items = NULL;
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
	assert (x != NULL);
	
	sorted[0] = plist->items[x->item_num];
	x->item_num = 0;
	
	n = 1;
	while ((x = rb_next(x)) != &rb_null) {
		sorted[n] = plist->items[x->item_num];
		x->item_num = n++;
	}

	plist->num = n;
	plist->not_deleted = n;
	
	memcpy (plist->items, sorted, sizeof(struct plist_item) * n);
	free (sorted);
}

/* Find an item on the list items. Return the index or -1 if not found. */
int plist_find_fname (struct plist *plist, const char *file)
{
	int i;
	
	assert (plist != NULL);

	i = rb_search (plist->search_tree, plist, file);

	return i != -1 && !plist_deleted(plist, i) ? i : -1;
}

/* Find an item on the list, also find deleted items. If there are more than one
 * items for this file, return the not deleted one, or if all are deleted,
 * return the last of them. Return the index or -1 if not found. */
int plist_find_del_fname (struct plist *plist, const char *file)
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
	int pos = plist_add (plist, item->file);

	plist_item_copy (&plist->items[pos], item);

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
		plist_free_item_fields (&plist->items[num]);
		plist->items[num].file = file;

		plist->items[num].deleted = 1;
	}
	plist->not_deleted--;
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
	assert (file != NULL);

	if (plist->items[num].file) {
		rb_delete (&plist->search_tree, plist, file);
		free (plist->items[num].file);
		plist->items[num].type = F_OTHER;
	}
	
	plist->items[num].file = xstrdup (file);
	plist->items[num].mtime = get_mtime (file);
	rb_insert (&plist->search_tree, plist, num);
}

/* Add the content of playlist b to a by copying items. */
void plist_cat (struct plist *a, struct plist *b)
{
	int i;

	assert (a != NULL);
	assert (b != NULL);

	for (i = 0; i < b->num; i++) {
		assert (b->items[i].file != NULL);
		
		if (plist_find_fname(a, b->items[i].file) == -1)
			plist_add_from_item (a, &b->items[i]);
	}
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

/* Count total time of items on the playlist. */
void plist_count_total_time (struct plist *plist)
{
	int i;

	assert (plist != NULL);

	plist->time_for_all_files = 1;
	plist->total_time = 0;

	for (i = 0; i < plist->num; i++)
		if (!plist_deleted(plist, i)) {
			if (!plist->items[i].tags
					|| !(plist->items[i].tags->filled
						& TAGS_TIME)
					|| plist->items[i].tags->time == -1)
				plist->time_for_all_files = 0;
			else
				plist->total_time +=
					plist->items[i].tags->time;
		}
}

/* Return the total time of all files on the playlist that has the time tag.
 * If the time information is missing for any file, all_files is set to 0,
 * otherwise 1.
 * Returned value is that counted by plist_count_time(), so may be not
 * up-to-date. */
int plist_total_time (const struct plist *plist, int *all_files)
{
	*all_files = plist->time_for_all_files;
	
	return plist->total_time;
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

	for (i = 0; i < plist->num; i++)
		plist_swap (plist, i,
				(rand()/(float)RAND_MAX) * (plist->num - 1));
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

void plist_set_serial (struct plist *plist, const int serial)
{
	plist->serial = serial;
}

int plist_get_serial (const struct plist *plist)
{
	return plist->serial;
}

/* Return the index of the last not deleted item from the playlist.
 * Return -1 if there are no items. */
int plist_last (struct plist *plist)
{
	int i;

	i = plist->num - 1;
	
	while (i > 0 && plist_deleted(plist, i))
		i--;

	return i;
}

enum file_type plist_file_type (struct plist *plist, const int num)
{
	assert (plist != NULL);
	assert (num < plist->num);

	if (plist->items[num].type != F_OTHER)
		return plist->items[num].type;

	return (plist->items[num].type = file_type(plist->items[num].file));
}
