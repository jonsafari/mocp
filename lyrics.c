/*
 * MOC - music on console
 * Copyright (C) 2008-2009 Geraud Le Falher and John Fitzgerald
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
#include <errno.h>
#include <string.h>

#include "common.h"
#include "files.h"
#include "log.h"
#include "options.h"
#include "lists.h"
#include "lyrics.h"

static lists_t_strs *raw_lyrics = NULL;
static const char *lyrics_message = NULL;

/* Return the list of lyrics lines, or NULL if none are loaded. */
lists_t_strs *lyrics_lines_get (void)
{
	return raw_lyrics;
}

/* Store new lyrics lines as supplied. */
void lyrics_lines_set (lists_t_strs *lines)
{
	assert (!raw_lyrics);
	assert (lines);

	raw_lyrics = lines;
	lyrics_message = NULL;
}

/* Return a list of lyrics lines loaded from a file, or NULL on error. */
lists_t_strs *lyrics_load_file (const char *filename)
{
	int text_plain;
	FILE *lyrics_file = NULL;
	char *mime, *line;
	lists_t_strs *result;

	assert (filename);

	lyrics_message = "[No lyrics file!]";
	if (!file_exists (filename))
		return NULL;
	mime = file_mime_type (filename);
	text_plain = mime ? !strncmp (mime, "text/plain", 10) : 0;
	free (mime);
	if (!text_plain)
		return NULL;

	lyrics_file = fopen (filename, "r");
	if (lyrics_file == NULL) {
		char *err = xstrerror (errno);
		logit ("Error reading '%s': %s", filename, err);
		free (err);
		lyrics_message = "[Lyrics file cannot be read!]";
		return NULL;
	}

	result = lists_strs_new (0);
	while ((line = read_line (lyrics_file)) != NULL)
		lists_strs_push (result, line);
	fclose (lyrics_file);

	lyrics_message = NULL;
	return result;
}

/* Given an audio's file name, load lyrics from the default lyrics file name. */
void lyrics_autoload (const char *filename)
{
	char *lyrics_filename, *extn;

	assert (!raw_lyrics);
	assert (lyrics_message);

	if (filename == NULL) {
		lyrics_message = "[No file playing!]";
		return;
	}

	if (!options_get_bool ("AutoLoadLyrics")) {
		lyrics_message = "[Lyrics not autoloaded!]";
		return;
	}

	if (is_url (filename)) {
		lyrics_message = "[Lyrics from URL is not supported!]";
		return;
	}

	lyrics_filename = xstrdup (filename);
	extn = ext_pos (lyrics_filename);
	if (extn) {
		*--extn = '\0';
		raw_lyrics = lyrics_load_file (lyrics_filename);
	}
	else
		lyrics_message = "[No lyrics file!]";

	free (lyrics_filename);
}

/* Given a line, return a centred copy of it. */
static char *centre_line (const char* line, int max)
{
	char *result;
	int len;

	len = strlen (line);
	if (len < (max - 1)) {
		int space;

		space = (max - len) / 2;
		result = (char *) xmalloc (space + len + 2);
		memset (result, ' ', space);
		strcpy (&result[space], line);
		len += space;
	}
	else {
		result = (char *) xmalloc (max + 2);
		strncpy (result, line, max);
		len = max;
	}
	strcpy (&result[len], "\n");

	return result;
}

/* Centre all the lines in the lyrics. */
static lists_t_strs *centre_style (lists_t_strs *lines, int unused1 ATTR_UNUSED,
                                   int width, void *unused2 ATTR_UNUSED)
{
	lists_t_strs *result;
	int ix, size;

	size = lists_strs_size (lines);
	result = lists_strs_new (size);
	for (ix = 0; ix < size; ix += 1) {
		char *old_line, *new_line;

		old_line = lists_strs_at (lines, ix);
		new_line = centre_line (old_line, width);
		lists_strs_push (result, new_line);
	}

	return result;
}

/* Formatting function information. */
static lyrics_t_formatter *lyrics_formatter = centre_style;
static lyrics_t_reaper *formatter_reaper = NULL;
static void *formatter_data = NULL;

/* Register a new function to be used for formatting.  A NULL formatter
 * resets formatting to the default centred style. */
void lyrics_use_formatter (lyrics_t_formatter formatter,
                           lyrics_t_reaper reaper, void *data)
{
	if (formatter_reaper)
		formatter_reaper (formatter_data);

	if (formatter) {
		lyrics_formatter = formatter;
		formatter_reaper = reaper;
		formatter_data = data;
	}
	else {
		lyrics_formatter = centre_style;
		formatter_reaper = NULL;
		formatter_data = NULL;
	}
}

/* Return a list of either the formatted lyrics if any are loaded or
 * a centred message. */
lists_t_strs *lyrics_format (int height, int width)
{
	int ix;
	lists_t_strs *result;

	assert (raw_lyrics || lyrics_message);

	result = NULL;

	if (raw_lyrics) {
		result = lyrics_formatter (raw_lyrics, height, width - 1,
			                                                 formatter_data);
		if (!result)
			lyrics_message = "[Error formatting lyrics!]";
	}

	if (!result) {
		char *line;

		result = lists_strs_new (1);
		line = centre_line (lyrics_message, width - 1);
		lists_strs_push (result, line);
	}

	for (ix = 0; ix < lists_strs_size (result); ix += 1) {
		int len;
		char *this_line;

		this_line = lists_strs_at (result, ix);
		len = strlen (this_line);
		if (len > width - 1)
			strcpy (&this_line[width - 1], "\n");
		else if (this_line[len - 1] != '\n') {
			char *new_line;

			new_line = xmalloc (len + 2);
			strcpy (new_line, this_line);
			strcat (new_line, "\n");
			lists_strs_swap (result, ix, new_line);
			free (this_line);
		}
	}

	return result;
}

/* Dispose of raw lyrics lines. */
void lyrics_cleanup (void)
{
	if (raw_lyrics) {
		lists_strs_free (raw_lyrics);
		raw_lyrics = NULL;
	}

	lyrics_message = "[No lyrics loaded!]";
}
