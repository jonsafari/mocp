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
	lists_t_strs *result;

	assert (reserve >= 0);

	result = (lists_t_strs *) xmalloc (sizeof (lists_t_strs));
	result->size = 0;
	result->capacity = (reserve ? reserve : 64);
	result->strs = (char **) xcalloc (sizeof (char *), result->capacity);

	return result;
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

/* Return true iff the list has no members. */
_Bool lists_strs_empty (const lists_t_strs *list)
{
	assert (list);

	return list->size == 0 ? true : false;
}

/* Given an index, return the string at that position in a list. */
char *lists_strs_at (const lists_t_strs *list, int index)
{
	assert (list);
	assert (index >= 0 && index < list->size);

	return list->strs[index];
}

/* Return the concatenation of all the strings in a list, or NULL
 * if the list is empty. */
char *lists_strs_cat (const lists_t_strs *list)
{
	int len, ix;
	char *result;

	assert (list);

	len = 0;
	for (ix = 0; ix < list->size; ix += 1)
		len += strlen (list->strs[ix]);

	result = NULL;
	if (list->size > 0) {
		result = xmalloc (len + 1);
		result[0] = 0x00;
		for (ix = 0; ix < list->size; ix += 1)
			strcat (result, list->strs[ix]);
	}

	return result;
}

/* Sort string list into an order determined by caller's comparitor. */
void lists_strs_sort (lists_t_strs *list, lists_t_compare *compare)
{
	assert (list);
	assert (compare);

	qsort (list->strs, list->size, sizeof (char *), compare);
}

/* Reverse the order of entries in a list. */
void lists_strs_reverse (lists_t_strs *list)
{
	int ix, iy;

	assert (list);

	for (ix = 0, iy = list->size - 1; ix < iy; ix += 1, iy -= 1) {
		char *str;

		str = list->strs[ix];
		list->strs[ix] = list->strs[iy];
		list->strs[iy] = str;
	}
}

/* Take a string and push it onto the end of a list
 * (expanding the list if necessary). */
void lists_strs_push (lists_t_strs *list, char *s)
{
	assert (list);
	assert (s);
	
	if (list->size == list->capacity) {
		list->capacity *= 2;
		list->strs = (char **) xrealloc (list->strs, list->capacity * sizeof (char *));
	}

	list->strs[list->size] = s;
	list->size += 1;
}

/* Remove the last string on the list and return it, or NULL if the list
 * is empty. */
char *lists_strs_pop (lists_t_strs *list)
{
	char *result;

	assert (list);
	
	result = NULL;
	if (list->size > 0) {
		list->size -= 1;
		result = list->strs[list->size];
	}

	return result;
}

/* Replace the nominated string with a new one and return the old one. */
char *lists_strs_swap (lists_t_strs *list, int index, char *s)
{
	char *result;

	assert (list);
	assert (index >= 0 && index < list->size);
	assert (s);

	result = list->strs[index];
	list->strs[index] = s;

	return result;
}

/* Copy a string and append it to the end of a list. */
void lists_strs_append (lists_t_strs *list, char *s)
{
	char *str;

	assert (list);
	assert (s);

	str = xstrdup (s);
	lists_strs_push (list, str);
}

/* Remove a string from the end of the list and free it. */
void lists_strs_remove (lists_t_strs *list)
{
	char *str;

	assert (list);

	str = lists_strs_pop (list);
	if (str)
		free (str);
}

/* Replace the nominated string with a copy of the new one
 * and free the old one. */
void lists_strs_replace (lists_t_strs *list, int index, char *s)
{
	char *str;

	assert (list);
	assert (index >= 0 && index < list->size);

	str = xstrdup (s);
	str = lists_strs_swap (list, index, str);
	free (str);
}
