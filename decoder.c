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

#include "main.h"
#include "decoder.h"
#include "files.h"
#include "log.h"

#ifdef HAVE_MAD
# include "mp3.h"
#endif

#ifdef HAVE_VORBIS
# include "ogg.h"
#endif

#ifdef HAVE_FLAC
# include "flac.h"
#endif

#ifdef HAVE_SNDFILE
# include "sndfile_formats.h"
#endif

static struct decoder *decoders[8];
static int types_num = 0;

/* Find the index in table types for the given file. Return -1 if not found. */
static int find_type (const char *file)
{
	char *ext = ext_pos (file);
	int i;

	if (ext)
		for (i = 0; i < types_num; i++)
			if (decoders[i]->our_format_ext(ext))
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
		decoders[i]->get_name(file, buf);
		return buf;
	}

	return NULL;
}

struct decoder *get_decoder (const char *file)
{
	int i;
	
	if ((i = find_type(file)) != -1)
		return decoders[i];

	return NULL;
}

void decoder_init ()
{
#ifdef HAVE_MAD
	decoders[types_num++] = mp3_get_funcs ();
#endif

#ifdef HAVE_VORBIS
	decoders[types_num++] = ogg_get_funcs ();
#endif

#ifdef HAVE_FLAC
	decoders[types_num++] = flac_get_funcs ();
#endif

#ifdef HAVE_SNDFILE
	decoders[types_num++] = sndfile_get_funcs ();
#endif
}

void decoder_cleanup ()
{
}

/* Fill the error structure with an error of a given type and message.
 * strerror(errno) is appended at the end of the message if add_errno != 0.
 * The old error message is free()ed.
 * This is thread safe, use this instead of constructions with strerror(). */
void decoder_error (struct decoder_error *error,
		const enum decoder_error_type type, const int add_errno,
		const char *format, ...)
{
	char errno_buf[256] = "0";
	char err_str[256];
	va_list va;

	if (error->err)
		free (error->err);
	
	error->type = type;

	va_start (va, format);
	vsnprintf (err_str, sizeof(err_str), format, va);
	err_str[sizeof(err_str)-1] = 0;

	if (add_errno)
		strerror_r(errno, errno_buf, sizeof(errno_buf));

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
