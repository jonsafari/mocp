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
#include <assert.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <stdlib.h>

/* Include dirent for various systems */
#ifdef HAVE_DIRENT_H
# include <dirent.h>
#else
# define dirent direct
# if HAVE_SYS_NDIR_H
#  include <sys/ndir.h>
# endif
#endif

#include "playlist.h"
#include "main.h"
#include "interface.h"
#include "file_types.h"
#include "options.h"
#include "files.h"
#include "playlist_file.h"

#define FILE_LIST_INIT_SIZE	64

static enum file_type file_type (char *file)
{
	struct stat file_stat;
	
	assert (file != NULL);

	if (stat(file, &file_stat) == -1)
		return F_OTHER; /* Ignore the file if stat() failed */
	if (S_ISDIR(file_stat.st_mode))
		return F_DIR;
	if (is_sound_file(file))
		return F_SOUND;
	if (is_plist_file(file))
		return F_PLAYLIST;
	return F_OTHER;
}

/* Make titles for the playlist items from the file names. */
void make_titles_file (struct plist *plist)
{
	int i;

	for (i = 0; i < plist->num; i++) {
		char *fname;

		assert (plist->items[i].file != NULL);

		fname = strrchr (plist->items[i].file, '/');

		if (fname)
			fname++;
		else
			fname = plist->items[i].file;

		plist_set_title (plist, i, fname);
	}
}

/* Make titles for the playlist items from the tags. */
void make_titles_tags (struct plist *plist)
{
	int i;

	for (i = 0; i < plist->num; i++) {

		assert (plist->items[i].file != NULL);
		
		if (plist->items[i].tags)
			plist_set_title (plist, i,
					build_title (plist->items[i].tags));
		else {
			char *fname = strrchr (plist->items[i].file, '/');

			if (fname)
				fname++;
			else
				fname = plist->items[i].file;

			plist_set_title (plist, i, fname);
		}
	}
}

/* Add file to the directory path in buf resolveing '../' and removing './'. */
/* buf must be absolute path. */
void resolve_path (char *buf, const int size, char *file)
{
	char *f; /* points to the char in *file we process */
	char path[2*PATH_MAX]; /* temporary path */
	int len = 0; /* number of vharacters in the buffer */

	assert (buf[0] == '/');

	if (snprintf(path, sizeof(path), "%s/%s/", buf, file)
			>= (int)sizeof(path))
		fatal ("Path too long");

	f = path;
	while (*f) {
		if (!strncmp(f, "/../", 4)) {
			char *slash = strrchr (buf, '/');

			assert (slash != NULL);

			if (slash == buf) {

				/* make '/' from '/directory' */
				buf[1] = 0;
				len = 1;
			}
			else {

				/* strip one element */
				*(slash) = 0;
				len -= len - (slash - buf);
				buf[len] = 0;
			}

			f+= 3;
		}
		else if (!strncmp(f, "/./", 3))

			/* skip '/.' */
			f += 2;
		else if (!strncmp(f, "//", 2))
			
			/* remove double slash */
			f++;
		else if (len == size - 1)
			fatal ("Path too long");
		else  {
			buf[len++] = *(f++);
			buf[len] = 0;
		}
	}
	
	/* remove dot from '/dir/.' */
	if (len >= 2 && buf[len-1] == '.' && buf[len-2] == '/')
		buf[--len] = 0;

	/* strip trailing slash */
	if (len > 1 && buf[len-1] == '/')
		buf[--len] = 0;
}


struct file_tags *read_file_tags (char *file)
{
	struct file_tags *tags;
	struct decoder_funcs *df;

	assert (file != NULL);
	df = get_decoder_funcs (file);
	assert (df != NULL);

	tags = tags_new ();
	df->info (file, tags);
	if (tags->title)
		return tags;

	tags_free (tags);
	return NULL;
}

void read_tags (struct plist *plist)
{
	int i;

	assert (plist != NULL);

	for (i = 0; i < plist->num; i++)
		if (!plist_deleted(plist, i) && !plist->items[i].tags)
			plist->items[i].tags = read_file_tags(
					plist->items[i].file);
}

struct file_list *file_list_new ()
{
	struct file_list *list;

	list = (struct file_list *)xmalloc(sizeof(struct file_list));
	list->num = 0;
	list->allocated = FILE_LIST_INIT_SIZE;
	list->items = (char **)xmalloc (
			sizeof(char *) * FILE_LIST_INIT_SIZE);

	return list;
}

static void file_list_add (struct file_list *list, const char *file)
{
	assert (list != NULL);
	
	if (list->allocated == list->num) {
		list->allocated *= 2;
		list->items = (char **)xrealloc (list->items,
				sizeof(char *) * list->allocated);
	}

	list->items[list->num] = xstrdup (file);
	list->num++;
}

void file_list_free (struct file_list *list)
{
	int i;
	
	for (i = 0; i < list->num; i++)
		free (list->items[i]);
	free (list->items);
	free (list);
}

/* Read the content of the directory, make an array of absolute paths for
 * all recognized files. Put directories, playlists and sound files
 * in proper structures. Return 0 on error.*/
int read_directory (const char *directory, struct file_list *dirs,
		struct file_list *playlists, struct plist *plist)
{
	DIR *dir;
	struct dirent *entry;
	int show_hidden = options_get_int ("ShowHiddenFiles");
	int dir_is_root;
	
	assert (directory != NULL);
	assert (*directory == '/');
	assert (dirs != NULL);
	assert (playlists != NULL);
	assert (plist != NULL);

	if (!(dir = opendir(directory))) {
		interface_error ("Can't read directory: %s", strerror(errno));
		return 0;
	}

	if (!strcmp(directory, "/"))
		dir_is_root = 1;

	else
		dir_is_root = 0;

	while ((entry = readdir(dir))) {
		char file[PATH_MAX];
		enum file_type type;
		
		if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
			continue;
		if (!show_hidden && entry->d_name[0] == '.')
			continue;
		if (snprintf(file, sizeof(file), "%s/%s", dir_is_root ?
					"" : directory,	entry->d_name)
				>= (int)sizeof(file)) {
			interface_error ("Path too long!");
			return 0;
		}
		type = file_type (file);
		if (type == F_SOUND)
			plist_add (plist, file);
		else if (type == F_DIR)
			file_list_add (dirs, file);
		else if (type == F_PLAYLIST)
			file_list_add (playlists, file);
	}

	closedir (dir);

	return 1;
}

/* Recursively add files from the directory to the playlist. */
void read_directory_recurr (const char *directory, struct plist *plist)
{
	DIR *dir;
	struct dirent *entry;

	assert (plist != NULL);
	assert (directory != NULL);

	if (!(dir = opendir(directory))) {
		interface_error ("Can't read directory: %s", strerror(errno));
		return;
	}
	
	while ((entry = readdir(dir))) {
		char file[PATH_MAX];
		enum file_type type;
		
		if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
			continue;
		if (snprintf(file, sizeof(file), "%s/%s", directory,
					entry->d_name)
				>= (int)sizeof(file)) {
			interface_error ("Path too long!");
			continue;
		}
		type = file_type (file);
		if (type == F_DIR)
			read_directory_recurr (file, plist);
		else if (type == F_SOUND && plist_find_fname(plist, file) == -1)
			plist_add (plist, file);
	}

	closedir (dir);
}

/* Return the file extension position or NULL if the file has no extension. */
char *ext_pos (char *file)
{
	char *ext = strrchr (file, '.');
	char *slash = strrchr (file, '/');

	/* don't treat dot in ./file as a dot before extension */
	if (ext && (!slash || slash < ext))
		ext++;
	else
		ext = NULL;

	return ext;
}
