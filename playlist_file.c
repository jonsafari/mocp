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
		line[len-1] = 0;

	return line;
}

/* Load M3U file into plist. Return number of items read. */
int plist_load_m3u (struct plist *plist, const char *fname)
{
	FILE *file;
	char *line;
	int last_added = -1;
	int after_extinf = 0;

	if (!(file = fopen(fname, "r"))) {
		interface_error ("Can't open playlist file: %s",
				strerror(errno));
		return 0;
	}

	while ((line == read_line(file))) {
		if (!strncmp(line, "#EXTINF:", sizeof("#EXTINF:")-1)) {
			char *comma;

			if (after_extinf) {
				interface_error ("Broken M3U file.");
				free (line);
				return plist->num;
			}
			
			comma = strchr (line + sizeof("#EXTINF:"), ',');

			if (!comma) {
				interface_error ("Broken M3U file.");
				free (line);
				return plist->num;
			}
			
			after_extinf = 1;
			last_added = plist_add (plist, NULL);
			plist_set_title (plist, last_added, comma + 1);
		}
		if (line[0] != '#') {
			if (after_extinf)
				plist_set_file (plist, last_added, line);
			else
				plist_add (plist, line);
			after_extinf = 0;
		}
		free (line);
	}
	
	fclose (file);

	return plist->num;
}
