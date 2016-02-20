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
#include <strings.h>
#include <errno.h>
#include <stdlib.h>
#include <dirent.h>

#ifdef HAVE_LIBMAGIC
#include <magic.h>
#include <pthread.h>
#endif

#define DEBUG

#include "common.h"
#include "playlist.h"
#include "lists.h"
#include "interface.h"
#include "decoder.h"
#include "options.h"
#include "files.h"
#include "playlist_file.h"
#include "log.h"
#include "utf8.h"

#define READ_LINE_INIT_SIZE	256

#ifdef HAVE_LIBMAGIC
static magic_t cookie = NULL;
static char *cached_file = NULL;
static char *cached_result = NULL;
#endif

void files_init ()
{
#ifdef HAVE_LIBMAGIC
	assert (cookie == NULL);

	cookie = magic_open (MAGIC_SYMLINK | MAGIC_MIME | MAGIC_ERROR |
	                     MAGIC_NO_CHECK_COMPRESS | MAGIC_NO_CHECK_ELF |
	                     MAGIC_NO_CHECK_TAR | MAGIC_NO_CHECK_TOKENS |
	                     MAGIC_NO_CHECK_FORTRAN | MAGIC_NO_CHECK_TROFF);
	if (cookie == NULL)
		log_errno ("Error allocating magic cookie", errno);
	else if (magic_load (cookie, NULL) != 0) {
		logit ("Error loading magic database: %s", magic_error (cookie));
		magic_close (cookie);
		cookie = NULL;
	}
#endif
}

void files_cleanup ()
{
#ifdef HAVE_LIBMAGIC
	free (cached_file);
	cached_file = NULL;
	free (cached_result);
	cached_result = NULL;
	magic_close (cookie);
	cookie = NULL;
#endif
}

/* Is the string a URL? */
inline int is_url (const char *str)
{
	return !strncasecmp (str, "http://", sizeof ("http://") - 1)
		|| !strncasecmp (str, "ftp://", sizeof ("ftp://") - 1);
}

/* Return 1 if the file is a directory, 0 if not, -1 on error. */
int is_dir (const char *file)
{
	struct stat file_stat;

	if (is_url (file))
		return 0;

	if (stat (file, &file_stat) == -1) {
		char *err = xstrerror (errno);
		error ("Can't stat %s: %s", file, err);
		free (err);
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

/* Given a file name, return the mime type or NULL. */
char *file_mime_type (const char *file ASSERT_ONLY)
{
	char *result = NULL;

	assert (file != NULL);

#ifdef HAVE_LIBMAGIC
	static pthread_mutex_t magic_mtx = PTHREAD_MUTEX_INITIALIZER;

	if (cookie != NULL) {
		LOCK(magic_mtx);
		if (cached_file && !strcmp (cached_file, file))
			result = xstrdup (cached_result);
		else {
			free (cached_file);
			free (cached_result);
			cached_file = cached_result = NULL;
			result = xstrdup (magic_file (cookie, file));
			if (result == NULL)
				logit ("Error interrogating file: %s", magic_error (cookie));
			else {
				cached_file = xstrdup (file);
				cached_result = xstrdup (result);
			}
		}
		UNLOCK(magic_mtx);
	}
#endif

	return result;
}

/* Make a title from the file name for the item.  If hide_extn != 0,
 * strip the file name from extension. */
void make_file_title (struct plist *plist, const int num,
		const bool hide_extension)
{
	assert (plist != NULL);
	assert (LIMIT(num, plist->num));
	assert (!plist_deleted (plist, num));

	if (file_type (plist->items[num].file) != F_URL) {
		char *file = xstrdup (plist->items[num].file);

		if (hide_extension) {
			char *extn;

			extn = ext_pos (file);
			if (extn)
				*(extn - 1) = 0;
		}

		if (options_get_bool ("FileNamesIconv"))
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
	bool hide_extn;
	char *title;

	assert (plist != NULL);
	assert (LIMIT(num, plist->num));
	assert (!plist_deleted (plist, num));

	if (file_type (plist->items[num].file) == F_URL) {
		make_file_title (plist, num, false);
		return;
	}

	if (plist->items[num].title_tags)
		return;

	assert (plist->items[num].file != NULL);

	if (plist->items[num].tags->title) {
		title = build_title (plist->items[num].tags);
		plist_set_title_tags (plist, num, title);
		free (title);
		return;
	}

	hide_extn = options_get_bool ("HideFileExtension");
	make_file_title (plist, num, hide_extn);
}

/* Switch playlist titles to title_file */
void switch_titles_file (struct plist *plist)
{
	int i;
	bool hide_extn;

	hide_extn = options_get_bool ("HideFileExtension");

	for (i = 0; i < plist->num; i++) {
		if (plist_deleted (plist, i))
			continue;

		if (!plist->items[i].title_file)
			make_file_title (plist, i, hide_extn);

		assert (plist->items[i].title_file != NULL);
	}
}

/* Switch playlist titles to title_tags */
void switch_titles_tags (struct plist *plist)
{
	int i;
	bool hide_extn;

	hide_extn = options_get_bool ("HideFileExtension");

	for (i = 0; i < plist->num; i++) {
		if (plist_deleted (plist, i))
			continue;

		if (!plist->items[i].title_tags && !plist->items[i].title_file)
			make_file_title (plist, i, hide_extn);
	}
}

/* Add file to the directory path in buf resolving '../' and removing './'. */
/* buf must be absolute path. */
void resolve_path (char *buf, const int size, const char *file)
{
	int rc;
	char *f; /* points to the char in *file we process */
	char path[2*PATH_MAX]; /* temporary path */
	int len = 0; /* number of characters in the buffer */

	assert (buf[0] == '/');

	rc = snprintf(path, sizeof(path), "%s/%s/", buf, file);
	if (rc >= ssizeof(path))
		fatal ("Path too long!");

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
			fatal ("Path too long!");
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
		struct file_tags *tags, const int tags_sel)
{
	struct decoder *df;
	int needed_tags;

	assert (file != NULL);

	if (tags == NULL)
		tags = tags_new ();

	if (file_type (file) == F_URL)
		return tags;

	needed_tags = ~tags->filled & tags_sel;
	if (!needed_tags) {
		debug ("No need to read any tags");
		return tags;
	}

	df = get_decoder (file);
	if (!df) {
		logit ("Can't find decoder functions for %s", file);
		return tags;
	}

	/* This makes sure that we don't cause a memory leak */
	assert (!((needed_tags & TAGS_COMMENTS) &&
	          (tags->title || tags->artist || tags->album)));

	df->info (file, tags, needed_tags);
	tags->filled |= tags_sel;

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
	bool show_hidden = options_get_bool ("ShowHiddenFiles");
	int dir_is_root;

	assert (directory != NULL);
	assert (*directory == '/');
	assert (dirs != NULL);
	assert (playlists != NULL);
	assert (plist != NULL);

	if (!(dir = opendir(directory))) {
		error_errno ("Can't read directory", errno);
		return 0;
	}

	if (!strcmp(directory, "/"))
		dir_is_root = 1;
	else
		dir_is_root = 0;

	while ((entry = readdir(dir))) {
		int rc;
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

		rc = snprintf(file, sizeof(file), "%s/%s",
		              dir_is_root ? "" : directory, entry->d_name);
		if (rc >= ssizeof(file)) {
			error ("Path too long!");
			closedir (dir);
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

	if (stat (directory, &st)) {
		char *err = xstrerror (errno);
		error ("Can't stat %s: %s", directory, err);
		free (err);
		return 0;
	}

	assert (plist != NULL);
	assert (directory != NULL);

	if (*dir_stack && dir_symlink_loop(st.st_ino, *dir_stack, *depth)) {
		logit ("Detected symlink loop on %s", directory);
		return 1;
	}

	if (!(dir = opendir(directory))) {
		error_errno ("Can't read directory", errno);
		return 1;
	}

	(*depth)++;
	*dir_stack = (ino_t *)xrealloc (*dir_stack, sizeof(ino_t) * (*depth));
	(*dir_stack)[*depth - 1] = st.st_ino;

	while ((entry = readdir(dir))) {
		int rc;
		char file[PATH_MAX];
		enum file_type type;

		if (user_wants_interrupt()) {
			error ("Interrupted! Not all files read!");
			break;
		}

		if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
			continue;
		rc = snprintf(file, sizeof(file), "%s/%s", directory, entry->d_name);
		if (rc >= ssizeof(file)) {
			error ("Path too long!");
			continue;
		}
		type = file_type (file);
		if (type == F_DIR)
			read_directory_recurr_internal(file, plist, dir_stack, depth);
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

/* Read one line from a file, strip trailing end of line chars.
 * Returned memory is malloc()ed.  Return NULL on error or EOF. */
char *read_line (FILE *file)
{
	int line_alloc = READ_LINE_INIT_SIZE;
	int len = 0;
	char *line = (char *)xmalloc (sizeof(char) * line_alloc);

	while (1) {
		if (!fgets(line + len, line_alloc - len, file))
			break;
		len = strlen(line);

		if (line[len-1] == '\n')
			break;

		/* If we are here, it means that line is longer than the buffer. */
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

/* Find directories having a prefix of 'pattern'.
 * - If there are no matches, NULL is returned.
 * - If there is one such directory, it is returned with a trailing '/'.
 * - Otherwise the longest common prefix is returned (with no trailing '/').
 * (This is used for directory auto-completion.)
 * Returned memory is malloc()ed.
 * 'pattern' is temporarily modified! */
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

			if (is_dir(path) == 1) {
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

	/* Log any error other than non-existence. */
	if (errno != ENOENT)
		log_errno ("Error", errno);

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

/* Check that a file which may cause other applications to be invoked
 * is secure against tampering. */
bool is_secure (const char *file)
{
    struct stat sb;

	assert (file && file[0]);

	if (stat (file, &sb) == -1)
		return true;
	if (!S_ISREG(sb.st_mode))
		return false;
	if (sb.st_mode & (S_IWGRP|S_IWOTH))
		return false;
	if (sb.st_uid != 0 && sb.st_uid != geteuid ())
		return false;

	return true;
}
