/*
 * MOC - music on console
 * Copyright (C) 2005 Damian Pietras <daper@daper.net>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * Functions based on pseudocode from "Introduction to Algorithms"
 * The only modification is that we avoid to modify fields of the nil value.
 *
 */

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <stdlib.h>
#include <assert.h>

#include "common.h"
#include "rbtree.h"

enum rb_color { RB_RED, RB_BLACK };

struct rb_node
{
	struct rb_node *left;
	struct rb_node *right;
	struct rb_node *parent;
	enum rb_color color;
	const void *data;
};

struct rb_tree
{
	struct rb_node *root;

	/* compare function for two data elements */
	rb_t_compare *cmp_fn;

	/* compare function for data element and a key value */
	rb_t_compare_key *cmp_key_fn;

	/* pointer to additional data passed to compare functions */
	const void *adata;
};

/* item used as a null value */
static struct rb_node rb_null = { NULL, NULL, NULL, RB_BLACK, NULL };

static void rb_left_rotate (struct rb_node **root, struct rb_node *x)
{
	struct rb_node *y = x->right;

	assert (y != &rb_null);

	x->right = y->left;
	if (y->left != &rb_null)
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

	assert (y != &rb_null);

	x->left = y->right;
	if (y->right != &rb_null)
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
	while (z->parent->color == RB_RED)
		if (z->parent == z->parent->parent->left) {
			struct rb_node *y = z->parent->parent->right;

			if (y->color == RB_RED) {
				z->parent->color = RB_BLACK;
				y->color = RB_BLACK;
				z->parent->parent->color = RB_RED;
				z = z->parent->parent;
			}
			else {
				if (z == z->parent->right) {
					z = z->parent;
					rb_left_rotate (root, z);
				}

				z->parent->color = RB_BLACK;
				z->parent->parent->color = RB_RED;
				rb_right_rotate (root, z->parent->parent);
			}
		}
		else {
			struct rb_node *y = z->parent->parent->left;

			if (y->color == RB_RED) {
				z->parent->color = RB_BLACK;
				y->color = RB_BLACK;
				z->parent->parent->color = RB_RED;
				z = z->parent->parent;
			}
			else {
				if (z == z->parent->left) {
					z = z->parent;
					rb_right_rotate (root, z);
				}

				z->parent->color = RB_BLACK;
				z->parent->parent->color = RB_RED;
				rb_left_rotate (root, z->parent->parent);
			}
		}

	(*root)->color = RB_BLACK;
}

static void rb_delete_fixup (struct rb_node **root, struct rb_node *x,
		struct rb_node *parent)
{
	struct rb_node *w;

	while (x != *root && x->color == RB_BLACK) {
		if (x == parent->left) {
			w = parent->right;

			if (w->color == RB_RED) {
				w->color = RB_BLACK;
				parent->color = RB_RED;
				rb_left_rotate (root, parent);
				w = parent->right;
			}

			if (w->left->color == RB_BLACK
					&& w->right->color == RB_BLACK) {
				w->color = RB_RED;
				x = parent;
				parent = x->parent;
			}
			else {
				if (w->right->color == RB_BLACK) {
					w->left->color = RB_BLACK;
					w->color = RB_RED;
					rb_right_rotate (root, w);
					w = parent->right;
				}

				w->color = parent->color;
				parent->color = RB_BLACK;
				w->right->color = RB_BLACK;
				rb_left_rotate (root, parent);
				x = *root;
			}
		}
		else {
			w = parent->left;

			if (w->color == RB_RED) {
				w->color = RB_BLACK;
				parent->color = RB_RED;
				rb_right_rotate (root, parent);
				w = parent->left;
			}

			if (w->right->color == RB_BLACK
					&& w->left->color == RB_BLACK) {
				w->color = RB_RED;
				x = parent;
				parent = x->parent;
			}
			else {
				if (w->left->color == RB_BLACK) {
					w->right->color = RB_BLACK;
					w->color = RB_RED;
					rb_left_rotate (root, w);
					w = parent->left;
				}

				w->color = parent->color;
				parent->color = RB_BLACK;
				w->left->color = RB_BLACK;
				rb_right_rotate (root, parent);
				x = *root;
			}
		}

	}

	x->color = RB_BLACK;
}

void rb_insert (struct rb_tree *t, void *data)
{
	struct rb_node *x, *y, *z;

	assert (t != NULL);
	assert (t->root != NULL);

	x = t->root;
	y = &rb_null;
	z = (struct rb_node *)xmalloc (sizeof(struct rb_node));

	z->data = data;

	while (x != &rb_null) {
		int cmp = t->cmp_fn (z->data, x->data, t->adata);

		y = x;
		if (cmp < 0)
			x = x->left;
		else if (cmp > 0)
			x = x->right;
		else
			abort ();
	}

	z->parent = y;
	if (y == &rb_null)
		t->root = z;
	else {
		if (t->cmp_fn (z->data, y->data, t->adata) < 0)
			y->left = z;
		else
			y->right = z;
	}

	z->left = &rb_null;
	z->right = &rb_null;
	z->color = RB_RED;

	rb_insert_fixup (&t->root, z);
}

struct rb_node *rb_search (struct rb_tree *t, const void *key)
{
	struct rb_node *x;

	assert (t != NULL);
	assert (t->root != NULL);
	assert (key != NULL);

	x = t->root;

	while (x != &rb_null) {
		int cmp = t->cmp_key_fn (key, x->data, t->adata);

		if (cmp < 0)
			x = x->left;
		else if (cmp > 0)
			x = x->right;
		else
			break;
	}

	return x;
}

int rb_is_null (const struct rb_node *n)
{
	return n == &rb_null;
}

const void *rb_get_data (const struct rb_node *n)
{
	return n->data;
}

void rb_set_data (struct rb_node *n, const void *data)
{
	n->data = data;
}

static struct rb_node *rb_min_internal (struct rb_node *n)
{
	if (n == &rb_null)
		return &rb_null;

	while (n->left != &rb_null)
		n = n->left;

	return n;
}

struct rb_node *rb_min (struct rb_tree *t)
{
	assert (t != NULL);
	assert (t->root != NULL);

	return rb_min_internal (t->root);
}

struct rb_node *rb_next (struct rb_node *x)
{
	struct rb_node *y;

	if (x->right != &rb_null)
		return rb_min_internal (x->right);

	y = x->parent;
	while (y != &rb_null && x == y->right) {
		x = y;
		y = y->parent;
	}

	return y;
}

void rb_delete (struct rb_tree *t, const void *key)
{
	struct rb_node *z;

	assert (t != NULL);
	assert (t->root != NULL);
	assert (key != NULL);

	z = rb_search (t, key);

	if (z != &rb_null) {
		struct rb_node *x, *y, *parent;

		if (z->left == &rb_null || z->right == &rb_null)
			y = z;
		else
			y = rb_next (z);

		if (y->left != &rb_null)
			x = y->left;
		else
			x = y->right;

		parent = y->parent;
		if (x != &rb_null)
			x->parent = parent;

		if (y->parent == &rb_null)
			t->root = x;
		else {
			if (y == y->parent->left)
				y->parent->left = x;
			else
				y->parent->right = x;
		}

		if (y != z)
			z->data = y->data;

		if (y->color == RB_BLACK)
			rb_delete_fixup (&t->root, x, parent);

		free (y);
	}
}

struct rb_tree *rb_tree_new (rb_t_compare *cmp_fn,
                             rb_t_compare_key *cmp_key_fn,
                             const void *adata)
{
	struct rb_tree *t;

	assert (cmp_fn != NULL);
	assert (cmp_key_fn != NULL);

	t = xmalloc (sizeof (*t));

	t->root = &rb_null;
	t->cmp_fn = cmp_fn;
	t->cmp_key_fn = cmp_key_fn;
	t->adata = adata;

	return t;
}

static void rb_destroy (struct rb_node *n)
{
	if (n != &rb_null) {
		rb_destroy (n->right);
		rb_destroy (n->left);
		free (n);
	}
}

void rb_tree_clear (struct rb_tree *t)
{
	assert (t != NULL);
	assert (t->root != NULL);

	if (t->root != &rb_null) {
		rb_destroy (t->root->left);
		rb_destroy (t->root->right);
		free (t->root);
		t->root = &rb_null;
	}
}

void rb_tree_free (struct rb_tree *t)
{
	assert (t != NULL);
	assert (t->root != NULL);

	rb_tree_clear (t);
	free (t);
}
