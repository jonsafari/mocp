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
#include <errno.h>
#include <assert.h>
#include "playlist.h"
#include "playlist_file.h"
#include "log.h"
#include "interface.h"
#include "main.h"
#include "files.h"

#define READ_LINE_INIT_SIZE	512

int is_plist_file (char *name)
{
	char *ext = ext_pos (name);

	if (ext && !strcasecmp(ext, "m3u"))
		return 1;
	
	return 0;
}

/* Read one line from a file, strip trailing end of line chars. Returned memory
 * is malloc()ed. Return NULL on error or EOF. */
static char *read_line (FILE *file)
{
	int line_alloc = READ_LINE_INIT_SIZE;
	int len = 0;
	char *line = (char *)xmalloc (sizeof(char) * READ_LINE_INIT_SIZE);

	while (1) {
		if (!fgets(line + len, line_alloc - len, file))
			break;
		len = strlen(line);

		if (line[len-1] == '\n')
			break;
		
		/* if we are hear, it means that line is longer than the
		 * buffer */
		line_alloc *= 2;
		line = (char *)xrealloc (line, sizeof(char) * line_alloc);
	}

	if (len == 0) {
		free (line);
		return NULL;
	}

	if (line[len-1] == '\n')
		line[--len] = 0;
	if (len > 0 && line[len-1] == '\r')
		line[--len] = 0;

	return line;
}

static void make_path (char *buf, const int buf_size,
		const char *cwd, char *path)
{
	if (path[0] != '/')
		strcpy (buf, cwd);
	else
		strcpy (buf, "/");

	resolve_path (buf, buf_size, path);
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
		interface_error ("Can't open playlist file: %s",
				strerror(errno));
		return 0;
	}

	while ((line = read_line(file))) {
		if (!strncmp(line, "#EXTINF:", sizeof("#EXTINF:")-1)) {
			char *comma;
			char *num_err;
			char time_text[10];
			int time_sec;

			if (after_extinf) {
				interface_error ("Broken M3U file: double "
						"#EXTINF.");
				free (line);
				plist_delete (plist, last_added);
				return added;
			}
			
			comma = strchr (line + (sizeof("#EXTINF:") - 1), ',');
			if (!comma) {
				interface_error ("Broken M3U file: no comma "
						"in #EXTINF.");
				free (line);
				return added;
			}
	
			time_text[sizeof(time_text)-1] = 0;
			strncpy (time_text, line + sizeof("#EXTINF:") - 1,
					MIN(comma - line - (sizeof("#EXTINF:")
						- 1), sizeof(time_text)));
			if (time_text[sizeof(time_text)-1]) {
				interface_error ("Broken M3U file: "
						"wrong time.");
				free (line);
				return added;
			}

			time_sec = strtol (time_text, &num_err, 10);
			if (*num_err) {
				interface_error ("Broken M3U file: "
						"time is not a number.");
				free (line);
				return added;
			}

		
			after_extinf = 1;
			last_added = plist_add (plist, NULL);
			plist_set_title (plist, last_added, comma + 1);

			plist->items[last_added].tags = tags_new ();
			if (*time_text) {
				plist->items[last_added].tags->time = time_sec;
				plist->items[last_added].tags->filled
					= TAGS_TIME;
			}
		}
		if (line[0] != '#') {
			char path[2*PATH_MAX];

			make_path (path, sizeof(path), cwd, line);

			if (plist_find_fname(plist, path) == -1) {
				if (after_extinf)
					plist_set_file (plist, last_added,
							path);
				else
					plist_add (plist, path);
				added++;
			}
			else if (after_extinf)
				plist_delete (plist, last_added);
			after_extinf = 0;
		}
		free (line);
	}
	
	fclose (file);

	return added;
}

/* Load a playlist into plist. Return the number of items on the list. */
/* The playlist may have deleted items. */
int plist_load (struct plist *plist, const char *fname, const char *cwd)
{
	int num;
	int i;

	num = plist_load_m3u (plist, fname, cwd);

	/* make titles if not present */
	for (i = 0; i < plist->num; i++)
		if (!plist_deleted(plist, i) && !plist->items[i].title) {
			plist->items[i].tags = read_file_tags (
						plist->items[i].file,
						plist->items[i].tags,
						TAGS_COMMENTS);
			if (plist->items[i].tags->title)
				plist->items[i].title = build_title (
						plist->items[i].tags);
			else
				plist->items[i].title = xstrdup (
						strrchr(plist->items[i].file,
							'/') + 1);
		}

	return num;
}

/* Save plist in m3u format. Strip pathes by strip_path bytes. */
static int plist_save_m3u (struct plist *plist, const char *fname,
		const int strip_path)
{
	FILE *file;
	int i;

	if (!(file = fopen(fname, "w"))) {
		interface_error ("Can't save playlist: %s", strerror(errno));
		return 0;
	}

	if (fprintf(file, "#EXTM3U\r\n") < 0) {
		interface_error ("Error writing playlist: %s", strerror(errno));
		fclose (file);
		return 0;
	}
	
	for (i = 0; i < plist->num; i++)
		if (!plist_deleted(plist, i)) {

			/* EXTM3U */
			if (plist->items[i].tags->time != -1
					&& fprintf(file, "#EXTINF:%d,%s\r\n",
						plist->items[i].tags->time,
						plist->items[i].title) < 0) {
				interface_error ("Error writing playlist: %s",
						strerror(errno));
				fclose (file);
				return 0;
			}

			/* file */
			if (fprintf(file, "%s\r\n", plist->items[i].file
						+ strip_path) < 0) {
				interface_error ("Error writing playlist: %s",
						strerror(errno));
				fclose (file);
				return 0;
			}
		}
				
	if (fclose(file)) {
		interface_error ("Error writing playlist: %s", strerror(errno));
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

	for (i = 0; i < plist->num; i++)
		if (!plist_deleted(plist, i))
			plist->items[i].tags = read_file_tags (
					plist->items[i].file,
					plist->items[i].tags,
					TAGS_COMMENTS | TAGS_TIME);
	make_titles_tags (plist);

	/* FIXME: checkif it possible to just add some directories to make
	 * relative path working. */
	return plist_save_m3u (plist, file, cwd && !strcmp(common_path, cwd) ?
			strlen(common_path) : 0);
}
