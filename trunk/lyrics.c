/*
 * MOC - music on console
 * Copyright (C) 2008 Geraud Le Falher
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

#include <string.h>

#include "lyrics.h"
#include "common.h"
#include "files.h"
#include "log.h"

#define LYRICS_LINE_NUMBER	128
static char *lyrics[LYRICS_LINE_NUMBER];

const unsigned short LINE_SIZE = 128;

void lyrics_remove_prefix (const char *filename, char **new_name)
{
	unsigned int filelen;
	const char *dot = strrchr(filename, '.');

	filelen = strlen(filename) - (dot ? strlen(dot) : 0);
	*new_name = xmalloc(sizeof(char) * (filelen + 1));
	strncpy(*new_name, filename, filelen);
	(*new_name)[filelen] = 0x00;
}

void lyrics_cleanup (const unsigned int n)
{
	unsigned int i;
	for (i = 0; i < n && lyrics[i] != NULL; i++)
		free (lyrics[i]);
}

char **get_lyrics_text (const WINDOW *w, const char *filename, int *num)
{
	char 			*lyrics_filename;
	char 			*lyrics_line;
	FILE			*lyrics_file = NULL;
	unsigned short 	i = 0;
	int 			x, y, space;

	getmaxyx(w,x,y);
	if (y > LINE_SIZE)
		y = LINE_SIZE;

	if (filename == NULL) {
		lyrics[0] = xmalloc (sizeof(char) * 20); 
		strncpy (lyrics[0], "No file reading", 20);
		*num = 1;
		return lyrics;
	}

	if (is_url (filename)) {
		lyrics[0] = xmalloc (sizeof(char) * 30); 
		strncpy (lyrics[0], "URL lyrics is not supported", 30);
		*num = 1;
		return lyrics;
	}

	lyrics_remove_prefix (filename, &lyrics_filename);

	lyrics_file = fopen (lyrics_filename, "r");
	if (lyrics_file != NULL) {
		lyrics_line = xmalloc (sizeof(char) * LINE_SIZE);
		while (fgets(lyrics_line, y, lyrics_file) != NULL) {
			if (i == LYRICS_LINE_NUMBER) {
				iface_error ("Lyrics file exceeds maximum line limit");
				break;
			}
			lyrics[i] = xmalloc (sizeof(char) * LINE_SIZE); 
			if ((int)strlen(lyrics_line) < (y-1)) {
				space = (y-strlen(lyrics_line))/2;
				memset(lyrics[i], ' ', space);
				lyrics[i][space] = '\0';
				strcat(lyrics[i], lyrics_line);
			}
			else {
				strncpy (lyrics[i], lyrics_line, y-1);
				lyrics[i][y] = '\0';
			}
			i++;
		}
		*num = i;
		fclose (lyrics_file);
		free (lyrics_line);
		free (lyrics_filename);
		return lyrics;
	}
	else {
		lyrics[0] = xmalloc (sizeof(char) * 20); 
		strncpy (lyrics[0], "No lyrics found !", 20);
		*num = 1;
		free (lyrics_filename);
		return lyrics;
	}
	free (lyrics_filename);
	abort ();
	return lyrics;
}

