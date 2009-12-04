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
#include <stdarg.h>
#include <errno.h>
#include <assert.h>
#include <ltdl.h>

#include "common.h"
#include "decoder.h"
#include "files.h"
#include "log.h"
#include "io.h"
#include "compat.h"
#include "options.h"

static struct plugin {
	lt_dlhandle handle;
	struct decoder *decoder;
} plugins[16];

static int plugins_num = 0;

/* Find the index in table types for the given file. Return -1 if not found. */
static int find_type (const char *file)
{
	int i, result;

	result = -1;

#ifdef HAVE_LIBMAGIC
	if (options_get_bool ("UseMimeMagic")) {
		char *mime;

		mime = xstrdup (file_mime_type (file));
		if (mime) {
			for (i = 0; i < plugins_num; i++) {
				if (plugins[i].decoder->our_format_mime &&
			    	plugins[i].decoder->our_format_mime (mime)) {
					result = i;
					break;
				}
			}
			free (mime);
		}
	}
#endif

	if (result == -1) {
		char *ext;

		ext = ext_pos (file);
		if (ext) {
			for (i = 0; i < plugins_num; i++) {
				if (plugins[i].decoder->our_format_ext &&
				    plugins[i].decoder->our_format_ext (ext)) {
					result = i;
					break;
				}
			}
		}
	}

	return result;
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

	if (file_type(file) == F_URL) {
		strcpy (buf, "NET");
		return buf;
	}
	else if ((i = find_type(file)) != -1) {
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

/* Return the decoder by the mime type of the stream or NULL if not found. */
static struct decoder *get_decoder_by_mime_type (struct io_stream *stream)
{
	int i;
	char *mime = io_get_mime_type (stream);

	if (mime) {
		for (i = 0; i < plugins_num; i++)
			if (plugins[i].decoder->our_format_mime
					&& plugins[i].decoder->our_format_mime(
						mime)) {
				logit ("Found decoder for mime type %s",
						mime);
				return plugins[i].decoder;
			}
	}
	else
		logit ("No mime type.");

	return NULL;
}

/* Return the decoder for this stream. */
struct decoder *get_decoder_by_content (struct io_stream *stream)
{
	char buf[8096];
	ssize_t res;
	int i;
	struct decoder *decoder_by_mime_type;

	assert (stream != NULL);

	/* Peek up some data to check if they are available. If not, there is
	 * no sense to try decoders, each of them would issue an error.
	 * This is also needed to actually get the mime type. */
	logit ("Testing the stream...");
	res = io_peek (stream, buf, sizeof(buf));
	if (res < 0) {
		error ("Stream error: %s", io_strerror(stream));
		return NULL;
	}

	if (res < 512) {
		logit ("Stream too short");
		return NULL;
	}
	
	if ((decoder_by_mime_type = get_decoder_by_mime_type(stream)))
		return decoder_by_mime_type;

	for (i = 0; i < plugins_num; i++)
		if (plugins[i].decoder->can_decode
				&& plugins[i].decoder->can_decode(stream))
			return plugins[i].decoder;

	error ("Format not supported");
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

static int lt_load_plugin (const char *file, lt_ptr debug_info_ptr)
{
	const char *name = strrchr (file, '/') ? strrchr(file, '/') + 1 : file;
	int debug_info = *(int *)debug_info_ptr;

	if (debug_info)
		printf ("Loading plugin %s...\n", name);
	
	if (plugins_num == sizeof(plugins)/sizeof(plugins[0])) {
		fprintf (stderr, "Can't load plugin, besause maximum number "
				"of plugins reached!\n");
		return 0;
	}

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
			else if (plugins[plugins_num].decoder->api_version
					!= DECODER_API_VERSION) {
				fprintf (stderr, "Plugin uses different API "
						"version\n");
				if (lt_dlclose(plugins[plugins_num].handle))
					fprintf (stderr, "Error unloading "
							"plugin: %s\n",
							lt_dlerror());
			}
			else {
				if (plugins[plugins_num].decoder->init)
					plugins[plugins_num].decoder->init ();
				plugins_num++;
				if (debug_info)
					printf ("OK\n");
			}
		}
	}
	else if (debug_info)
		printf ("Already loaded\n");
	
	return 0;
}

void decoder_init (int debug_info)
{
	if (debug_info)
		printf ("Loading plugins from %s...\n", PLUGIN_DIR);
	if (lt_dlinit())
		fatal ("lt_dlinit() failed: %s", lt_dlerror());

	if (lt_dlforeachfile(PLUGIN_DIR, &lt_load_plugin, &debug_info))
		fatal ("Can't load plugins: %s", lt_dlerror());

	if (plugins_num == 0)
		fatal ("No decoder plugins have been loaded!");
}

void decoder_cleanup ()
{
	int i;
	
	for (i = 0; i < plugins_num; i++)
		if (plugins[i].decoder->destroy)
			plugins[i].decoder->destroy ();

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
