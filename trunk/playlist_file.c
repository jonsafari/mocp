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

#define DEBUG

#include <sys/file.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <assert.h>
#include "playlist.h"
#include "playlist_file.h"
#include "log.h"
#include "main.h"
#include "files.h"
#include "options.h"
#include "interface.h"
#include "decoder.h"

int is_plist_file (const char *name)
{
	const char *ext = ext_pos (name);

	if (ext && !strcasecmp(ext, "m3u"))
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

/* Load M3U file into plist. Return number of items read. */
static int plist_load_m3u (struct plist *plist, const char *fname,
		const char *cwd)
{
	FILE *file;
	char *line;
	int last_added = -1;
	int after_extinf = 0;
	int added = 0;

	if (!(file = fopen(fname, "r"))) {
		error ("Can't open playlist file: %s",
				strerror(errno));
		return 0;
	}

	if (flock(fileno(file), LOCK_SH) == -1)
		logit ("Can't flock() the playlist file: %s", strerror(errno));

	while ((line = read_line(file))) {
		if (!strncmp(line, "#EXTINF:", sizeof("#EXTINF:")-1)) {
			char *comma;
			char *num_err;
			char time_text[10] = "";
			int time_sec;
			char *title;

			if (after_extinf) {
				error ("Broken M3U file: double "
						"#EXTINF.");
				free (line);
				plist_delete (plist, last_added);
				return added;
			}
			
			/* Find the comma */
			comma = strchr (line + (sizeof("#EXTINF:") - 1), ',');
			if (!comma) {
				error ("Broken M3U file: no comma "
						"in #EXTINF.");
				free (line);
				return added;
			}
	
			/* Get the time string */
			time_text[sizeof(time_text)-1] = 0;
			strncpy (time_text, line + sizeof("#EXTINF:") - 1,
					MIN(comma - line - (sizeof("#EXTINF:")
						- 1), sizeof(time_text)));
			if (time_text[sizeof(time_text)-1]) {
				error ("Broken M3U file: "
						"wrong time.");
				free (line);
				return added;
			}

			/* Extract the time */
			time_sec = strtol (time_text, &num_err, 10);
			if (*num_err) {
				error ("Broken M3U file: "
						"time is not a number.");
				free (line);
				return added;
			}

			after_extinf = 1;
			last_added = plist_add (plist, NULL);

			/* We must xstrdup() here, because iconv_str()
			 * expects malloc()ed memory */
			title = iconv_str (xstrdup(comma + 1));
			plist_set_title_tags (plist, last_added, title);
			free (title);

			plist->items[last_added].tags = tags_new ();
			if (*time_text) {
				plist->items[last_added].tags->time = time_sec;
				plist->items[last_added].tags->filled
					|= TAGS_TIME;
			}
		}
		else if (line[0] != '#') {
			char path[2*PATH_MAX];

			strip_string (line);
			if (strlen(line) <= PATH_MAX) {
				make_path (path, sizeof(path), cwd, line);

				if (plist_find_fname(plist, path) == -1
						&& is_sound_file(path)) {
					if (after_extinf)
						plist_set_file (plist,
								last_added,
								path);
					else
						plist_add (plist, path);
					added++;
				}
				else if (after_extinf)
					plist_delete (plist, last_added);
			}
			else if (after_extinf)
				plist_delete (plist, last_added);

			after_extinf = 0;
		}
		free (line);
	}
	
	if (flock(fileno(file), LOCK_UN) == -1)
		logit ("Can't flock() (unlock) the playlist file: %s",
				strerror(errno));
	fclose (file);

	return added;
}

/* Load a playlist into plist. Return the number of items on the list. */
/* The playlist may have deleted items. */
int plist_load (struct plist *plist, const char *fname, const char *cwd)
{
	int num;
	int read_tags = options_get_int ("ReadTags");

	num = plist_load_m3u (plist, fname, cwd);

	if (read_tags)
		switch_titles_tags (plist);
	else
		switch_titles_file (plist);

	return num;
}

/* Save plist in m3u format. Strip pathes by strip_path bytes. */
static int plist_save_m3u (struct plist *plist, const char *fname,
		const int strip_path)
{
	FILE *file;
	int i;

	debug ("Saving playlist to '%s'", fname);

	if (!(file = fopen(fname, "w"))) {
		error ("Can't save playlist: %s", strerror(errno));
		return 0;
	}

	if (flock(fileno(file), LOCK_EX) == -1)
		logit ("Can't flock() the playlist file: %s", strerror(errno));
	
	if (fprintf(file, "#EXTM3U\r\n") < 0) {
		error ("Error writing playlist: %s", strerror(errno));
		fclose (file);
		return 0;
	}
	
	for (i = 0; i < plist->num; i++)
		if (!plist_deleted(plist, i)) {

			/* EXTM3U */
			if (plist->items[i].tags->time != -1
					&& fprintf(file, "#EXTINF:%d,%s\r\n",
						plist->items[i].tags->time,
						plist->items[i].title_tags ?
						plist->items[i].title_tags
						: plist->items[i].title_file)
					< 0) {
				error ("Error writing playlist: %s",
						strerror(errno));
				fclose (file);
				return 0;
			}

			/* file */
			if (fprintf(file, "%s\r\n", plist->items[i].file
						+ strip_path) < 0) {
				error ("Error writing playlist: %s",
						strerror(errno));
				fclose (file);
				return 0;
			}
		}
				
	if (flock(fileno(file), LOCK_UN) == -1)
		logit ("Can't flock() (unlock) the playlist file: %s",
				strerror(errno));
	if (fclose(file)) {
		error ("Error writing playlist: %s", strerror(errno));
		return 0;
	}
	return 1;
}

/* Strip buf at the point where paths buf and path differs. */
static void strip_uncommon (char *buf, const char *path)
{
	int i = 0;
	char *slash;

	while (buf[i] == path[i])
		i++;
	buf[i] = 0;

	slash = strrchr(buf, '/');
	*slash = 0;
}

/* Find common element at the beginning of all files on the playlist. */
static void find_common_path (char *buf, const int buf_size,
		struct plist *plist)
{
	int i;
	char *slash;
	
	i = plist_next (plist, -1); /* find the first not deleted item */
	if (i == -1) {
		buf[0] = 0;
		return;
	}
	
	strncpy(buf, plist->items[i].file, buf_size);
	if (buf[buf_size-1])
		fatal ("Path too long");
	slash = strrchr(buf, '/');
	assert (slash != NULL);
	*slash = 0;

	for (++i; i < plist->num; i++)
		if (!plist_deleted(plist, i)) {
			strip_uncommon (buf, plist->items[i].file);
			if (!buf[0])
				break;
		}
}

/* Save the playlist into the file. Return 0 on error. if cwd is NULL, use
 * absolute paths. */
int plist_save (struct plist *plist, const char *file, const char *cwd)
{
	char common_path[PATH_MAX+1];
	int i;

	if (cwd)
		
		/* TODO: make this a configurable bahaviour (absolute or
		 * relative paths) */
		find_common_path (common_path, sizeof(common_path), plist);

	make_titles_tags (plist);

	if (user_wants_interrupt()) {
		error ("Saving the playlist aborted");
		return 0;
	}

	/* Get times */
	for (i = 0; i < plist->num; i++)
		if (!plist_deleted(plist, i)) {
			if (user_wants_interrupt()) {
				error ("Saving the playlist aborted");
				return 0;
			}
			plist->items[i].tags = read_file_tags (
					plist->items[i].file,
					plist->items[i].tags,
					TAGS_TIME);
		}

	/* FIXME: checkif it possible to just add some directories to make
	 * relative path working. */
	return plist_save_m3u (plist, file, cwd && !strcmp(common_path, cwd) ?
			strlen(common_path) : 0);
}
