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
#include "menu.h"
#include "main.h"
#include "interface.h"
#include "file_types.h"
#include "options.h"
#include "files.h"

enum file_type
{
	F_DIR,
	F_SOUND,
	F_OTHER
};

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
	return F_OTHER;
}

void free_dir_tab (char **tab, int i)
{
	while (--i >= 0)
		free (tab[i]);
	free (tab);
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

		if (plist->items[i].title)
			free (plist->items[i].title);
		plist->items[i].title = xstrdup (fname);
	}
}

/* Make titles for the playlist items from the tags. */
void make_titles_tags (struct plist *plist)
{
	int i;

	for (i = 0; i < plist->num; i++) {

		assert (plist->items[i].file != NULL);
		
		if (plist->items[i].title)
			free (plist->items[i].title);
		
		if (plist->items[i].tags)
			plist->items[i].title =
				build_title (plist->items[i].tags);
		else {
			char *fname = strrchr (plist->items[i].file, '/');

			if (fname)
				fname++;
			else
				fname = plist->items[i].file;

			plist->items[i].title = xstrdup (fname);
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
		if (!plist->items[i].tags)
			plist->items[i].tags = read_file_tags(
					plist->items[i].file);
}

/* Read the content of the current directory. Fill the playlist by the sound
 * files and make a table of directories (malloced). Return 1 if ok. */
int read_directory (const char *directory, struct plist *plist,
		char ***dir_tab, int *num_dirs)
{
	DIR *dir;
	struct dirent *entry;
	int ndirs, dir_alloc = 64;
	int show_hidden = options_get_int ("ShowHiddenFiles");
	
	assert (plist != NULL);
	assert (dir_tab != NULL);

	if (!(dir = opendir(directory))) {
		interface_error ("Can't read directory: %s", strerror(errno));
		return 0;
	}

	*dir_tab = (char **)xmalloc (sizeof(char *) * dir_alloc);
	(*dir_tab)[0] = strdup ("../");
	ndirs = 1;

	while ((entry = readdir(dir))) {
		char file[PATH_MAX];
		enum file_type type;
		
		if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
			continue;
		if (!show_hidden && entry->d_name[0] == '.')
			continue;
		if (snprintf(file, sizeof(file), "%s/%s", directory,
					entry->d_name)
				>= (int)sizeof(file)) {
			interface_error ("Path too long!");
			free_dir_tab (*dir_tab, ndirs);
			return 0;
		}
		type = file_type (file);
		if (type == F_DIR) {
			if (dir_alloc == ndirs) {
				dir_alloc *= 2;
				*dir_tab = (char **)xrealloc(*dir_tab,
						sizeof(char *) * dir_alloc);
			}
			(*dir_tab)[ndirs] = (char *)xmalloc(sizeof(char) *
					(strlen(entry->d_name) + 2));
			sprintf ((*dir_tab)[ndirs], "%s/", entry->d_name);
			ndirs++;
		}
		else if (type == F_SOUND) {
			plist_add (plist, file);
		}
	}

	*num_dirs = ndirs;
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
