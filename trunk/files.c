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

#define DEBUG

#include "playlist.h"
#include "common.h"
#include "lists.h"
#include "interface.h"
#include "decoder.h"
#include "options.h"
#include "files.h"
#include "playlist_file.h"
#include "log.h"
#include "utf8.h"

#define READ_LINE_INIT_SIZE	256


/* Is the string an URL? */
inline int is_url (const char *str)
{
	return !strncmp(str, "http://", sizeof("http://")-1)
		|| !strncmp(str, "ftp://", sizeof("ftp://")-1);
}

/* Return 1 if the file is a directory, 0 if not, -1 on error. */
int isdir (const char *file)
{
	struct stat file_stat;

	if (stat(file, &file_stat) == -1) {
		error ("Can't stat %s: %s", file, strerror(errno));
		return -1;
	}
	return S_ISDIR(file_stat.st_mode) ? 1 : 0;
}

/* Return 1 if the file can be read by this user, 0 if not */
int can_read_file (const char *file)
{
	return access(file, R_OK) == 0;
}

enum file_type file_type (const char *file)
{
	struct stat file_stat;
	
	assert (file != NULL);

	if (is_url(file))
		return F_URL;
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

/* Make a title from the file name for the item. If hide extension != 0, strip
 * the file name from extension. */
void make_file_title (struct plist *plist, const int num,
		const int hide_extension)
{
	
	assert (plist != NULL);
	assert (num >= 0 && num < plist->num);
	assert (!plist_deleted(plist, num));

	if (file_type(plist->items[num].file) != F_URL) {
		char *file = xstrdup (plist->items[num].file);
		
		if (hide_extension) {
			char *dot = strrchr (file, '.');

			if (dot)
				*dot = 0;
		}

		if (options_get_int ("FileNamesIconv")) 
		{
			char *old_title = file;
			file = files_iconv_str (file);
			free (old_title);
		}

		plist_set_title_file (plist, num, file);
		free (file);
	}
	else
		plist_set_title_file (plist, num, plist->items[num].file);
}

/* Make a title from the tags for the item. */
void make_tags_title (struct plist *plist, const int num)
{
	assert (plist != NULL);
	assert (num >= 0 && num < plist->num);
	assert (!plist_deleted(plist, num));

	if (file_type(plist->items[num].file) == F_URL)
		make_file_title (plist, num, 0);
	else if (!plist->items[num].title_tags) {
		char *title;
		assert (plist->items[num].file != NULL);

		if (plist->items[num].tags->title) {
			title = build_title (plist->items[num].tags);
			plist_set_title_tags (plist, num, title);
			free (title);
		}
		else
			make_file_title (plist, num,
					options_get_int("HideFileExtension"));
	}
}

/* Switch playlist titles to title_file */
void switch_titles_file (struct plist *plist)
{
	int i;
	int hide_extension = options_get_int("HideFileExtension");
		
	for (i = 0; i < plist->num; i++)
		if (!plist_deleted(plist, i)) {
			if (!plist->items[i].title_file)
				make_file_title (plist, i, hide_extension);
			assert (plist->items[i].title_file != NULL);
			plist->items[i].title = plist->items[i].title_file;
		}
}

/* Switch playlist titles to title_tags */
void switch_titles_tags (struct plist *plist)
{
	int i;

	for (i = 0; i < plist->num; i++)
		if (!plist_deleted(plist, i)) {
			if (plist->items[i].title_tags)
				plist->items[i].title
					= plist->items[i].title_tags;
			else {
				if (!plist->items[i].title_file)
					make_file_title (plist, i,
							options_get_int("HideFileExtension"));
				plist->items[i].title
					= plist->items[i].title_file;
			}
		}
}

/* Add file to the directory path in buf resolving '../' and removing './'. */
/* buf must be absolute path. */
void resolve_path (char *buf, const int size, const char *file)
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



/* Read selected tags for a file into tags structure (or create it if NULL).
 * If some tags are already present, don't read them.
 * If present_tags is NULL, allocate new tags. */
struct file_tags *read_file_tags (const char *file,
		struct file_tags *present_tags, const int tags_sel)
{
	struct file_tags *tags;
	struct decoder *df;
	int needed_tags;

	assert (file != NULL);

	if (present_tags) {
		tags = present_tags;
		needed_tags = ~tags->filled & tags_sel;
	}
	else {
		tags = tags_new ();
		needed_tags = tags_sel;
	}

	if (file_type(file) == F_URL)
		return tags;

	df = get_decoder (file);

	if (!df) {
		logit ("Can't find decoder functions for %s", file);
		return tags;
	}

	if (needed_tags) {

		/* This makes sure that we don't cause a memory leak */
		assert (!((needed_tags & TAGS_COMMENTS) &&
					(tags->title
					 || tags->artist
					 || tags->album)));

		df->info (file, tags, needed_tags);
		tags->filled |= tags_sel;
	}
	else
		debug ("No need to read any tags");
	
	return tags;
}

/* Read the content of the directory, make an array of absolute paths for
 * all recognized files. Put directories, playlists and sound files
 * in proper structures. Return 0 on error.*/
int read_directory (const char *directory, lists_t_strs *dirs,
		lists_t_strs *playlists, struct plist *plist)
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
		error ("Can't read directory: %s", strerror(errno));
		return 0;
	}

	if (!strcmp(directory, "/"))
		dir_is_root = 1;

	else
		dir_is_root = 0;

	while ((entry = readdir(dir))) {
		char file[PATH_MAX];
		enum file_type type;

		if (user_wants_interrupt()) {
			error ("Interrupted! Not all files read!");
			break;
		}
		
		if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
			continue;
		if (!show_hidden && entry->d_name[0] == '.')
			continue;
		if (snprintf(file, sizeof(file), "%s/%s", dir_is_root ?
					"" : directory,	entry->d_name)
				>= (int)sizeof(file)) {
			error ("Path too long!");
			return 0;
		}
		type = file_type (file);
		if (type == F_SOUND)
			plist_add (plist, file);
		else if (type == F_DIR)
			lists_strs_append (dirs, file);
		else if (type == F_PLAYLIST)
			lists_strs_append (playlists, file);
	}

	closedir (dir);

	return 1;
}

static int dir_symlink_loop (const ino_t inode_no, const ino_t *dir_stack,
		const int depth)
{
	int i;

	for (i = 0; i < depth; i++)
		if (dir_stack[i] == inode_no)
			return 1;

	return 0;
}

/* Recursively add files from the directory to the playlist. 
 * Return 1 if OK (and even some errors), 0 if the user interrupted. */
static int read_directory_recurr_internal (const char *directory, struct plist *plist,
		ino_t **dir_stack, int *depth)
{
	DIR *dir;
	struct dirent *entry;
	struct stat st;

	if (stat(directory, &st)) {
		error ("Can't stat %s: %s", directory, strerror(errno));
		return 0;
	}

	assert (plist != NULL);
	assert (directory != NULL);

	if (*dir_stack && dir_symlink_loop(st.st_ino, *dir_stack, *depth)) {
		logit ("Detected symlink loop on %s", directory);
		return 1;
	}

	if (!(dir = opendir(directory))) {
		error ("Can't read directory: %s", strerror(errno));
		return 1;
	}

	(*depth)++;
	*dir_stack = (ino_t *)xrealloc (*dir_stack, sizeof(ino_t) * (*depth));
	(*dir_stack)[*depth - 1] = st.st_ino;

	
	while ((entry = readdir(dir))) {
		char file[PATH_MAX];
		enum file_type type;
		
		if (user_wants_interrupt()) {
			error ("Interrupted! Not all files read!");
			break;
		}
			
		if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
			continue;
		if (snprintf(file, sizeof(file), "%s/%s", directory,
					entry->d_name)
				>= (int)sizeof(file)) {
			error ("Path too long!");
			continue;
		}
		type = file_type (file);
		if (type == F_DIR)
			read_directory_recurr_internal(file, plist, dir_stack,
					depth);
		else if (type == F_SOUND && plist_find_fname(plist, file) == -1)
			plist_add (plist, file);
	}

	(*depth)--;
	*dir_stack = (ino_t *)xrealloc (*dir_stack, sizeof(ino_t) * (*depth));

	closedir (dir);
	return 1;
}

int read_directory_recurr (const char *directory, struct plist *plist)
{
	int ret;
	int depth = 0;
	ino_t *dir_stack = NULL;

	ret = read_directory_recurr_internal (directory, plist, &dir_stack,
			&depth);

	if (dir_stack)
		free (dir_stack);

	return ret;
}

/* Return the file extension position or NULL if the file has no extension. */
char *ext_pos (const char *file)
{
	char *ext = strrchr (file, '.');
	char *slash = strrchr (file, '/');

	/* don't treat dot in ./file or /.file as a dot before extension */
	if (ext && (!slash || slash < ext) && ext != file && *(ext-1) != '/')
		ext++;
	else
		ext = NULL;

	return ext;
}

/* Read one line from a file, strip trailing end of line chars. Returned memory
 * is malloc()ed. Return NULL on error or EOF. */
char *read_line (FILE *file)
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

/* Return malloc()ed string in form "base/name". */
static char *add_dir_file (const char *base, const char *name)
{
	char *path;
	int base_is_root;

	base_is_root = !strcmp (base, "/") ? 1 : 0;
	path = (char *)xmalloc (sizeof(char) *
			(strlen(base) + strlen(name) + 2));
	
	sprintf (path, "%s/%s", base_is_root ? "" : base, name);

	return path;
}

/* Find a directory that the beginning part of the path matches dir.
 * Returned path has slash at the end if the name was unambiguous.
 * Complete the name of the directory if possible to the place where it it
 * ambiguous.
 * Return NULL if nothing was found.
 * Returned memory is malloc()ed.
 * patterm is modified! */
char *find_match_dir (char *pattern)
{
	char *slash;
	DIR *dir;
	struct dirent *entry;
	int name_len;
	char *name;
	char *matching_dir = NULL;
	char *search_dir;
	int unambiguous = 1;

	if (!pattern[0])
		return NULL;

	/* strip the last directory */
	slash = strrchr (pattern, '/');
	if (!slash)
		return NULL;
	if (slash == pattern) {
		
		/* only '/dir' */
		search_dir = xstrdup ("/");
	}
	else {
		*slash = 0;
		search_dir = xstrdup (pattern);
		*slash = '/';
	}

	name = slash + 1;
	name_len = strlen (name);

	if (!(dir = opendir(search_dir)))
		return NULL;

	while ((entry = readdir(dir))) {
		if (strcmp(entry->d_name, ".") && strcmp(entry->d_name, "..")
				&& !strncmp(entry->d_name, name, name_len)) {
			char *path = add_dir_file (search_dir, entry->d_name);

			if (isdir(path) == 1) {
				if (matching_dir) {

					/* More matching directories - strip
					 * matching_dir to the part that is
					 * common to both paths */
					int i = 0;

					while (matching_dir[i] == path[i]
							&& path[i])
						i++;
					matching_dir[i] = 0;
					free (path);
					unambiguous = 0;
				}
				else
					matching_dir = path;
			}
			else
				free (path);
		}
	}
	
	closedir (dir);
	free (search_dir);

	if (matching_dir && unambiguous) {
		matching_dir = (char *)xrealloc (matching_dir,
				sizeof(char) * (strlen(matching_dir) + 2));
		strcat (matching_dir, "/");
	}

	return matching_dir;
}

/* Return != 0 if the file exists. */
int file_exists (const char *file)
{
	struct stat file_stat;

	if (!stat(file, &file_stat))
		return 1;
	return 0;
}

/* Get the modification time of a file. Return (time_t)-1 on error */
time_t get_mtime (const char *file)
{
	struct stat stat_buf;

	if (stat(file, &stat_buf) != -1)
		return stat_buf.st_mtime;
	
	return (time_t)-1;
}

/* Convert file path to absolute path;
 * resulting string is allocated and must be freed afterwards. */
char *absolute_path (const char *path, const char *cwd)
{
	char tmp[2*PATH_MAX];
	char *result;

	assert (path);
	assert (cwd);

	if(path[0] != '/' && !is_url(path)) {
		strncpy (tmp, cwd, sizeof(tmp));
		tmp[sizeof(tmp)-1] = 0;

		resolve_path (tmp, sizeof(tmp), path);

		result = (char *)xmalloc (sizeof(char) * (strlen(tmp)+1));
		strcpy (result, tmp);
	}
	else {
		result = (char *)xmalloc (sizeof(char) * (strlen(path)+1));
		strcpy (result, path);
	}

	return result;
}
