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

#define _XOPEN_SOURCE	600 /* we need the POSIX version of strerror_r() */

#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <errno.h>
#include <assert.h>
#include <ltdl.h>

#include "main.h"
#include "decoder.h"
#include "files.h"
#include "log.h"
#include "io.h"

static struct plugin {
	lt_dlhandle handle;
	struct decoder *decoder;
} plugins[8];

static int plugins_num = 0;

/* Find the index in table types for the given file. Return -1 if not found. */
static int find_type (const char *file)
{
	char *ext = ext_pos (file);
	int i;

	if (ext)
		for (i = 0; i < plugins_num; i++)
			if (plugins[i].decoder->our_format_ext(ext))
				return i;
	return -1;
}

int is_sound_file (const char *name)
{
	return find_type(name) != -1 ? 1 : 0;
}

/* Return short type name for the given file or NULL if not found.
 * Not thread safe! */
char *file_type_name (const char *file)
{
	int i;
	static char buf[4];
	
	if ((i = find_type(file)) != -1) {
		plugins[i].decoder->get_name(file, buf);
		return buf;
	}

	return NULL;
}

struct decoder *get_decoder (const char *file)
{
	int i;
	
	if ((i = find_type(file)) != -1)
		return plugins[i].decoder;

	return NULL;
}

struct decoder *get_decoder_by_content (struct io_stream *stream)
{
	int i;
	
	for (i = 0; i < plugins_num; i++)
		if (plugins[i].decoder->can_decode
				&& plugins[i].decoder->can_decode(stream))
			return plugins[i].decoder;

	return NULL;
}

/* Check if this handle is already presend in the plugins table.
 * Returns 1 if so. */
static int present_handle (const lt_dlhandle h)
{
	int i;

	for (i = 0; i < plugins_num; i++)
		if (plugins[i].handle == h)
			return 1;
	return 0;
}

static int lt_load_plugin (const char *file, lt_ptr data ATTR_UNUSED)
{
	const char *name = strrchr (file, '/') ? strrchr(file, '/') + 1 : file;
	
	printf ("Loading plugin %s...\n", name);
	
	if (plugins_num == sizeof(plugins)/sizeof(plugins[0]))
		fprintf (stderr, "Can't load plugin, besause maximum number "
				"of plugins reached!\n");
	else {
		if (!(plugins[plugins_num].handle = lt_dlopenext(file))) {
			fprintf (stderr, "Can't load plugin %s: %s\n", name,
					lt_dlerror());
		}
		else if (!present_handle(plugins[plugins_num].handle)) {
			plugin_init_func init_func;

			if (!(init_func = lt_dlsym(plugins[plugins_num].handle,
							"plugin_init")))
				fprintf (stderr, "No init function in the "
						"plugin!\n");
			else {
				plugins[plugins_num].decoder = init_func ();
				if (!plugins[plugins_num].decoder)
					fprintf (stderr, "NULL decoder!\n");
				else {
					plugins_num++;
					printf ("OK\n");
				}
			}
		}
		else
			printf ("Already loaded\n");
	}
	
	return 0;
}

void decoder_init ()
{
	printf ("Loading plugins from %s...\n", PLUGIN_DIR);
	if (lt_dlinit())
		fatal ("lt_dlinit() failed: %s", lt_dlerror());

	if (lt_dlforeachfile(PLUGIN_DIR, &lt_load_plugin, NULL))
		fatal ("Can't load plugins: %s", lt_dlerror());

	if (plugins_num == 0)
		fatal ("No decoder plugins has been loaded!");
}

void decoder_cleanup ()
{
	if (lt_dlexit())
		logit ("lt_exit() failed: %s", lt_dlerror());
}

/* Fill the error structure with an error of a given type and message.
 * strerror(add_errno) is appended at the end of the message if add_errno != 0.
 * The old error message is free()ed.
 * This is thread safe, use this instead of constructions with strerror(). */
void decoder_error (struct decoder_error *error,
		const enum decoder_error_type type, const int add_errno,
		const char *format, ...)
{
	char errno_buf[256] = "";
	char err_str[256];
	va_list va;

	if (error->err)
		free (error->err);
	
	error->type = type;

	va_start (va, format);
	vsnprintf (err_str, sizeof(err_str), format, va);
	err_str[sizeof(err_str)-1] = 0;

	if (add_errno)
		strerror_r(add_errno, errno_buf, sizeof(errno_buf));

	error->err = (char *)xmalloc (sizeof(char) *
			(strlen(err_str) + strlen(errno_buf) + 1));
	strcpy (error->err, err_str);
	strcat (error->err, errno_buf);

	va_end (va);
}

/* Initialize the decoder_error structure. */
void decoder_error_init (struct decoder_error *error)
{
	error->type = ERROR_OK;
	error->err = NULL;
}

/* Set the decoder_error structure to contain "success" information. */
void decoder_error_clear (struct decoder_error *error)
{
	error->type = ERROR_OK;
	if (error->err) {
		free (error->err);
		error->err = NULL;
	}
}

void decoder_error_copy (struct decoder_error *dst,
		const struct decoder_error *src)
{
	dst->type = src->type;
	dst->err = xstrdup (src->err);
}
