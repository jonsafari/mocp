/*
 * MOC - music on console
 * Copyright (C) 2004 - 2006 Damian Pietras <daper@daper.net>
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

#include <unistd.h>
#include <sys/file.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <errno.h>
#include <assert.h>

#define DEBUG

#include "common.h"
#include "playlist.h"
#include "playlist_file.h"
#include "log.h"
#include "files.h"
#include "options.h"
#include "interface.h"
#include "decoder.h"

int is_plist_file (const char *name)
{
	const char *ext = ext_pos (name);

	if (ext && (!strcasecmp(ext, "m3u") || !strcasecmp(ext, "pls")))
		return 1;

	return 0;
}

static void make_path (char *buf, const int buf_size,
		const char *cwd, char *path)
{
	if (file_type(path) == F_URL) {
		strncpy (buf, path, buf_size);
		buf[buf_size-1] = 0;
		return;
	}

	if (path[0] != '/')
		strcpy (buf, cwd);
	else
		strcpy (buf, "/");

	resolve_path (buf, buf_size, path);
}

/* Strip white chars from the end of a string. */
static void strip_string (char *str)
{
	char *c = str;
	char *last_non_white = str;

	while (*c) {
		if (!isblank(*c))
			last_non_white = c;
		c++;
	}

	if (c > last_non_white)
		*(last_non_white + 1) = 0;
}

/* Load M3U file into plist.  Return the number of items read. */
static int plist_load_m3u (struct plist *plist, const char *fname,
		const char *cwd, const int load_serial)
{
	FILE *file;
	char *line = NULL;
	int last_added = -1;
	int after_extinf = 0;
	int added = 0;

	file = fopen (fname, "r");
	if (!file) {
		error_errno ("Can't open playlist file", errno);
		return 0;
	}

	/* Lock gets released by fclose(). */
	if (lockf (fileno (file), F_LOCK, 0) == -1)
		log_errno ("Can't lock the playlist file", errno);

	while ((line = read_line (file))) {
		if (!strncmp (line, "#EXTINF:", sizeof("#EXTINF:") - 1)) {
			char *comma, *num_err;
			char time_text[10] = "";
			int time_sec;

			if (after_extinf) {
				error ("Broken M3U file: double #EXTINF!");
				plist_delete (plist, last_added);
				goto err;
			}

			/* Find the comma */
			comma = strchr (line + (sizeof("#EXTINF:") - 1), ',');
			if (!comma) {
				error ("Broken M3U file: no comma in #EXTINF!");
				goto err;
			}

			/* Get the time string */
			time_text[sizeof(time_text) - 1] = 0;
			strncpy (time_text, line + sizeof("#EXTINF:") - 1,
			         MIN(comma - line - (sizeof("#EXTINF:") - 1),
			         sizeof(time_text)));
			if (time_text[sizeof(time_text) - 1]) {
				error ("Broken M3U file: wrong time!");
				goto err;
			}

			/* Extract the time. */
			time_sec = strtol (time_text, &num_err, 10);
			if (*num_err) {
				error ("Broken M3U file: time is not a number!");
				goto err;
			}

			after_extinf = 1;
			last_added = plist_add (plist, NULL);
			plist_set_title_tags (plist, last_added, comma + 1);

			if (*time_text)
				plist_set_item_time (plist, last_added, time_sec);
		}
		else if (line[0] != '#') {
			char path[2 * PATH_MAX];

			strip_string (line);
			if (strlen (line) <= PATH_MAX) {
				make_path (path, sizeof(path), cwd, line);

				if (plist_find_fname (plist, path) == -1) {
					if (after_extinf)
						plist_set_file (plist, last_added, path);
					else
						plist_add (plist, path);
					added += 1;
				}
				else if (after_extinf)
					plist_delete (plist, last_added);
			}
			else if (after_extinf)
				plist_delete (plist, last_added);

			after_extinf = 0;
		}
		else if (load_serial &&
		         !strncmp (line, "#MOCSERIAL: ", sizeof("#MOCSERIAL: ") - 1)) {
			char *serial_str = line + sizeof("#MOCSERIAL: ") - 1;

			if (serial_str[0]) {
				char *err;
				long serial;

				serial = strtol (serial_str, &err, 0);
				if (!*err) {
					plist_set_serial (plist, serial);
					logit ("Got MOCSERIAL tag with serial %ld", serial);
				}
			}
		}
		free (line);
	}

err:
	free (line);
	fclose (file);
	return added;
}

/* Return 1 if the line contains only blank characters, 0 otherwise. */
static int is_blank_line (const char *l)
{
	while (*l && isblank(*l))
		l++;

	if (*l)
		return 0;
	return 1;
}

/* Read a value from the given section from .INI file.  File should be opened
 * and seeking will be performed on it.  Return the malloc()ed value or NULL
 * if not present or error occurred. */
static char *read_ini_value (FILE *file, const char *section, const char *key)
{
	char *line = NULL;
	int in_section = 0;
	char *value = NULL;
	int key_len;

	if (fseek(file, 0, SEEK_SET)) {
		error_errno ("File fseek() error", errno);
		return NULL;
	}

	key_len = strlen (key);

	while ((line = read_line(file))) {
		if (line[0] == '[') {
			if (in_section) {

				/* we are outside of the interesting section */
				free (line);
				break;
			}
			else {
				char *close = strchr (line, ']');

				if (!close) {
					error ("Parse error in the INI file");
					free (line);
					break;
				}

				if (!strncasecmp(line + 1, section,
							close - line - 1))
					in_section = 1;
			}
		}
		else if (in_section && line[0] != '#' && !is_blank_line(line)) {
			char *t, *t2;

			t2 = t = strchr (line, '=');

			if (!t) {
				error ("Parse error in the INI file");
				free (line);
				break;
			}

			/* go back to the last char in the name */
			while (t2 >= t && (isblank(*t2) || *t2 == '='))
				t2--;

			if (t2 == t) {
				error ("Parse error in the INI file");
				free (line);
				break;
			}

			if (!strncasecmp(line, key,
						MAX(t2 - line + 1, key_len))) {
				value = t + 1;

				while (isblank(value[0]))
					value++;

				if (value[0] == '"') {
					char *q = strchr (value + 1, '"');

					if (!q) {
						error ("Parse error in the INI file");
						free (line);
						break;
					}

					*q = 0;
				}

				value = xstrdup (value);
				free (line);
				break;
			}
		}

		free (line);
	}

	return value;
}

/* Load PLS file into plist. Return the number of items read. */
static int plist_load_pls (struct plist *plist, const char *fname,
		const char *cwd)
{
	FILE *file;
	char *e, *line = NULL;
	long i, nitems, added = 0;

	file = fopen (fname, "r");
	if (!file) {
		error_errno ("Can't open playlist file", errno);
		return 0;
	}

	line = read_ini_value (file, "playlist", "NumberOfEntries");
	if (!line) {

		/* Assume that it is a pls file version 1 - plist_load_m3u()
		 * should handle it like an m3u file without the m3u extensions. */
		fclose (file);
		return plist_load_m3u (plist, fname, cwd, 0);
	}

	nitems = strtol (line, &e, 10);
	if (*e) {
		error ("Broken PLS file");
		goto err;
	}

	for (i = 1; i <= nitems; i++) {
		int time, last_added;
		char *pls_file, *pls_title, *pls_length;
		char key[16], path[2 * PATH_MAX];

		sprintf (key, "File%ld", i);
		pls_file = read_ini_value (file, "playlist", key);
		if (!pls_file) {
			error ("Broken PLS file");
			goto err;
		}

		sprintf (key, "Title%ld", i);
		pls_title = read_ini_value (file, "playlist", key);

		sprintf (key, "Length%ld", i);
		pls_length = read_ini_value (file, "playlist", key);

		if (pls_length) {
			time = strtol (pls_length, &e, 10);
			if (*e)
				time = -1;
		}
		else
			time = -1;

		if (strlen (pls_file) <= PATH_MAX) {
			make_path (path, sizeof(path), cwd, pls_file);
			if (plist_find_fname (plist, path) == -1) {
				last_added = plist_add (plist, path);

				if (pls_title && pls_title[0])
					plist_set_title_tags (plist, last_added, pls_title);

				if (time > 0) {
					plist->items[last_added].tags = tags_new ();
					plist->items[last_added].tags->time = time;
					plist->items[last_added].tags->filled |= TAGS_TIME;
				}
			}
		}

		free (pls_file);
		if (pls_title)
			free (pls_title);
		if (pls_length)
			free (pls_length);
		added += 1;
	}

err:
	free (line);
	fclose (file);
	return added;
}

/* Load a playlist into plist. Return the number of items on the list. */
/* The playlist may have deleted items. */
int plist_load (struct plist *plist, const char *fname, const char *cwd,
		const int load_serial)
{
	int num, read_tags;
	const char *ext;

	read_tags = options_get_bool ("ReadTags");
	ext = ext_pos (fname);

	if (ext && !strcasecmp(ext, "pls"))
		num = plist_load_pls (plist, fname, cwd);
	else
		num = plist_load_m3u (plist, fname, cwd, load_serial);

	if (read_tags)
		switch_titles_tags (plist);
	else
		switch_titles_file (plist);

	return num;
}

/* Save plist in m3u format. Strip paths by strip_path bytes.
 * If save_serial is not 0, the playlist serial is saved in a
 * comment. */
static int plist_save_m3u (struct plist *plist, const char *fname,
		const int strip_path, const int save_serial)
{
	FILE *file = NULL;
	int i, ret, result = 0;

	debug ("Saving playlist to '%s'", fname);

	file = fopen (fname, "w");
	if (!file) {
		error_errno ("Can't save playlist", errno);
		return 0;
	}

	/* Lock gets released by fclose(). */
	if (lockf (fileno (file), F_LOCK, 0) == -1)
		log_errno ("Can't lock the playlist file", errno);

	if (fprintf (file, "#EXTM3U\r\n") < 0) {
		error_errno ("Error writing playlist", errno);
		goto err;
	}

	if (save_serial && fprintf (file, "#MOCSERIAL: %d\r\n",
	                                  plist_get_serial (plist)) < 0) {
		error_errno ("Error writing playlist", errno);
		goto err;
	}

	for (i = 0; i < plist->num; i++) {
		if (!plist_deleted (plist, i)) {

			/* EXTM3U */
			if (plist->items[i].tags)
				ret = fprintf (file, "#EXTINF:%d,%s\r\n",
						plist->items[i].tags->time,
						plist->items[i].title_tags ?
						plist->items[i].title_tags
						: plist->items[i].title_file);
			else
				ret = fprintf (file, "#EXTINF:%d,%s\r\n", 0,
						plist->items[i].title_file);

			/* file */
			if (ret >= 0)
				ret = fprintf (file, "%s\r\n",
				                     plist->items[i].file + strip_path);

			if (ret < 0) {
				error_errno ("Error writing playlist", errno);
				goto err;
			}
		}
	}

	ret = fclose (file);
	file = NULL;
	if (ret)
		error_errno ("Error writing playlist", errno);
	else
		result = 1;

err:
	if (file)
		fclose (file);
	return result;
}

/* Save the playlist into the file. Return 0 on error. If cwd is NULL, use
 * absolute paths. */
int plist_save (struct plist *plist, const char *file, const char *cwd,
		const int save_serial)
{
	char common_path[PATH_MAX+1];

	/* FIXME: check if it possible to just add some directories to make
	 * relative path working. */
	return plist_save_m3u (plist, file, cwd && !strcmp(common_path, cwd) ?
			strlen(common_path) : 0, save_serial);
}
