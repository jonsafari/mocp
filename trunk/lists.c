/*
 * MOC - music on console
 * Copyright (C) 2009 Damian Pietras <daper@daper.net> and John Fitzgerald
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

#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include "common.h"
#include "lists.h"

struct lists_s_strs {
	int size;          /* Number of strings on the list */
	int capacity;      /* Number of allocated strings */
	char **strs;
};

/* Allocate a new list of strings and return its address. */
lists_t_strs *lists_strs_new (int reserve)
{
	lists_t_strs *this_list;

	assert (reserve >= 0);

	this_list = (lists_t_strs *) xmalloc (sizeof (lists_t_strs));
	this_list->size = 0;
	this_list->capacity = (reserve ? reserve : 64);
	this_list->strs = (char **) xcalloc (sizeof (char *), this_list->capacity);

	return this_list;
}

/* Free all storage associated with a list of strings. */
void lists_strs_free (lists_t_strs *list)
{
	int ix;
	
	assert (list);

	for (ix = 0; ix < list->size; ix += 1)
		free ((void *) list->strs[ix]);
	free (list->strs);
	free (list);
}

/* Return the number of strings in a list. */
int lists_strs_size (const lists_t_strs *list)
{
	assert (list);

	return list->size;
}

/* Append a string to the end of a list (and expand it if necessary). */
void lists_strs_append (lists_t_strs *list, char *str)
{
	assert (list);
	
	if (list->size == list->capacity) {
		list->capacity *= 2;
		list->strs = (char **) xrealloc (list->strs, list->capacity * sizeof (char *));
	}

	list->strs[list->size] = xstrdup (str);
	list->size += 1;
}

/* Given an index, return the string at that position in a list. */
char *lists_strs_at (const lists_t_strs *list, int index)
{
	assert (list);
	assert (index >= 0 && index < list->size);

	return list->strs[index];
}

/* Sort string list into an order determined by caller's comparitor. */
void lists_strs_sort (lists_t_strs *list, lists_t_compare *compare)
{
	assert (list);
	assert (compare);

	qsort (list->strs, list->size, sizeof (char *), compare);
}
