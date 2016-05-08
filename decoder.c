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
#include <strings.h>
#include <ctype.h>
#include <stdarg.h>
#include <errno.h>
#include <assert.h>
#include <ltdl.h>

#include "common.h"
#include "decoder.h"
#include "files.h"
#include "log.h"
#include "io.h"
#include "options.h"

static struct plugin {
	char *name;
	lt_dlhandle handle;
	struct decoder *decoder;
} plugins[16];

#define PLUGINS_NUM			(ARRAY_SIZE(plugins))

static int plugins_num = 0;

static bool have_tremor = false;

/* This structure holds the user's decoder preferences for audio formats. */
struct decoder_s_preference {
	struct decoder_s_preference *next;    /* chain pointer */
#ifdef DEBUG
	const char *source;                   /* entry in PreferredDecoders */
#endif
	int decoders;                         /* number of decoders */
	int decoder_list[PLUGINS_NUM];        /* decoder indices */
	char *subtype;                        /* MIME subtype or NULL */
	char type[];                          /* MIME type or filename extn */
};
typedef struct decoder_s_preference decoder_t_preference;
static decoder_t_preference *preferences = NULL;
static int default_decoder_list[PLUGINS_NUM];

static char *clean_mime_subtype (char *subtype)
{
	char *ptr;

	assert (subtype && subtype[0]);

	if (!strncasecmp (subtype, "x-", 2))
		subtype += 2;

	ptr = strchr (subtype, ';');
	if (ptr)
		*ptr = 0x00;

	return subtype;
}

/* Find a preference entry matching the given filename extension and/or
 * MIME media type, or NULL. */
static decoder_t_preference *lookup_preference (const char *extn,
                                                const char *file,
                                                char **mime)
{
	char *type, *subtype;
	decoder_t_preference *result;

	assert ((extn && extn[0]) || (file && file[0])
	                          || (mime && *mime && *mime[0]));

	type = NULL;
	subtype = NULL;
	for (result = preferences; result; result = result->next) {
		if (!result->subtype) {
			if (extn && !strcasecmp (result->type, extn))
				break;
		}
		else {

			if (!type) {
				if (mime && *mime == NULL && file && file[0]) {
					if (options_get_bool ("UseMimeMagic"))
						*mime = file_mime_type (file);
				}
				if (mime && *mime && strchr (*mime, '/'))
					type = xstrdup (*mime);
				if (type) {
					subtype = strchr (type, '/');
					*subtype++ = 0x00;
					subtype = clean_mime_subtype (subtype);
				}
			}

			if (type) {
				if (!strcasecmp (result->type, type) &&
			    	!strcasecmp (result->subtype, subtype))
					break;
			}

		}
	}

	free (type);
	return result;
}

/* Return the index of the first decoder able to handle files with the
 * given filename extension, or -1 if none can. */
static int find_extn_decoder (int *decoder_list, int count, const char *extn)
{
	int ix;

	assert (decoder_list);
	assert (RANGE(0, count, plugins_num));
	assert (extn && extn[0]);

	for (ix = 0; ix < count; ix += 1) {
		if (plugins[decoder_list[ix]].decoder->our_format_ext &&
		    plugins[decoder_list[ix]].decoder->our_format_ext (extn))
			return decoder_list[ix];
	}

	return -1;
}

/* Return the index of the first decoder able to handle audio with the
 * given MIME media type, or -1 if none can. */
static int find_mime_decoder (int *decoder_list, int count, const char *mime)
{
	int ix;

	assert (decoder_list);
	assert (RANGE(0, count, plugins_num));
	assert (mime && mime[0]);

	for (ix = 0; ix < count; ix += 1) {
		if (plugins[decoder_list[ix]].decoder->our_format_mime &&
		    plugins[decoder_list[ix]].decoder->our_format_mime (mime))
			return decoder_list[ix];
	}

	return -1;
}

/* Return the index of the first decoder able to handle audio with the
 * given filename extension and/or MIME media type, or -1 if none can. */
static int find_decoder (const char *extn, const char *file, char **mime)
{
	int result;
	decoder_t_preference *pref;

	assert ((extn && extn[0]) || (file && file[0]) || (mime && *mime));

	pref = lookup_preference (extn, file, mime);
	if (pref) {
		if (pref->subtype)
			return find_mime_decoder (pref->decoder_list, pref->decoders, *mime);
		else
			return find_extn_decoder (pref->decoder_list, pref->decoders, extn);
	}

	result = -1;
	if (mime && *mime)
		result = find_mime_decoder (default_decoder_list, plugins_num, *mime);
	if (result == -1 && extn && *extn)
		result = find_extn_decoder (default_decoder_list, plugins_num, extn);

	return result;
}

/* Find the index in plugins table for the given file.
 * Return -1 if not found. */
static int find_type (const char *file)
{
	int result = -1;
	char *extn, *mime;

	extn = ext_pos (file);
	mime = NULL;

	result = find_decoder (extn, file, &mime);

	free (mime);
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

	if (file_type (file) == F_URL) {
		strcpy (buf, "NET");
		return buf;
	}

	i = find_type (file);
	if (i == -1)
		return NULL;

	memset (buf, 0, sizeof (buf));
	if (plugins[i].decoder->get_name)
		plugins[i].decoder->get_name (file, buf);

	/* Attempt a default name if we have nothing else. */
	if (!buf[0]) {
		char *ext;

		ext = ext_pos (file);
		if (ext) {
			size_t len;

			len = strlen (ext);
			switch (len) {
				default:
					buf[2] = toupper (ext[len - 1]);
				case 2:
					buf[1] = toupper (ext[1]);
				case 1:
					buf[0] = toupper (ext[0]);
				case 0:
					break;
			}
		}
	}

	if (!buf[0])
		return NULL;

	return buf;
}

struct decoder *get_decoder (const char *file)
{
	int i;

	i = find_type (file);
	if (i != -1)
		return plugins[i].decoder;

	return NULL;
}

/* Given a decoder pointer, return its name. */
const char *get_decoder_name (const struct decoder *decoder)
{
	int ix;
	const char *result = NULL;

	assert (decoder);

	for (ix = 0; ix < plugins_num; ix += 1) {
		if (plugins[ix].decoder == decoder) {
			result = plugins[ix].name;
			break;
		}
	}

	assert (result);

	return result;
}

/* Use the stream's MIME type to return a decoder for it, or NULL if no
 * applicable decoder was found. */
static struct decoder *get_decoder_by_mime_type (struct io_stream *stream)
{
	int i;
	char *mime;
	struct decoder *result;

	result = NULL;
	mime = xstrdup (io_get_mime_type (stream));
	if (mime) {
		i = find_decoder (NULL, NULL, &mime);
		if (i != -1) {
			logit ("Found decoder for MIME type %s: %s", mime, plugins[i].name);
			result = plugins[i].decoder;
		}
		free (mime);
	}
	else
		logit ("No MIME type.");

	return result;
}

/* Return the decoder for this stream. */
struct decoder *get_decoder_by_content (struct io_stream *stream)
{
	char buf[8096];
	ssize_t res;
	int i;
	struct decoder *decoder_by_mime_type;

	assert (stream != NULL);

	/* Peek at the start of the stream to check if sufficient data is
	 * available.  If not, there is no sense in trying the decoders as
	 * each of them would issue an error.  The data is also needed to
	 * get the MIME type. */
	logit ("Testing the stream...");
	res = io_peek (stream, buf, sizeof (buf));
	if (res < 0) {
		error ("Stream error: %s", io_strerror (stream));
		return NULL;
	}

	if (res < 512) {
		logit ("Stream too short");
		return NULL;
	}

	decoder_by_mime_type = get_decoder_by_mime_type (stream);
	if (decoder_by_mime_type)
		return decoder_by_mime_type;

	for (i = 0; i < plugins_num; i++) {
		if (plugins[i].decoder->can_decode
				&& plugins[i].decoder->can_decode (stream)) {
			logit ("Found decoder for stream: %s", plugins[i].name);
			return plugins[i].decoder;
		}
	}

	error ("Format not supported");
	return NULL;
}

/* Extract decoder name from file name. */
static char *extract_decoder_name (const char *filename)
{
	int len;
	const char *ptr;
	char *result;

	if (!strncmp (filename, "lib", 3))
		filename += 3;
	len = strlen (filename);
	ptr = strpbrk (filename, "_.-");
	if (ptr)
		len = ptr - filename;
	result = xmalloc (len + 1);
	strncpy (result, filename, len);
	result[len] = 0x00;

	return result;
}

/* Return the index for a decoder of the given name, or plugins_num if
 * not found. */
static int lookup_decoder_by_name (const char *name)
{
	int result;

	assert (name && name[0]);

	result = 0;
	while (result < plugins_num) {
		if (!strcasecmp (plugins[result].name, name))
			break;
		result += 1;
	}

	return result;
}

/* Return a string of concatenated driver names. */
static char *list_decoder_names (int *decoder_list, int count)
{
	int ix;
	char *result;
	lists_t_strs *names;

	if (count == 0)
		return xstrdup ("");

	names = lists_strs_new (count);
	for (ix = 0; ix < count; ix += 1)
		lists_strs_append (names, plugins[decoder_list[ix]].name);

	if (have_tremor) {
		ix = lists_strs_find (names, "vorbis");
		if (ix < lists_strs_size (names))
			lists_strs_replace (names, ix, "vorbis(tremor)");
	}

	ix = lists_strs_find (names, "ffmpeg");
	if (ix < lists_strs_size (names)) {
#if defined(HAVE_FFMPEG)
			lists_strs_replace (names, ix, "ffmpeg");
#elif defined(HAVE_LIBAV)
			lists_strs_replace (names, ix, "ffmpeg(libav)");
#else
			lists_strs_replace (names, ix, "ffmpeg/libav");
#endif
	}

	result = lists_strs_fmt (names, " %s");
	lists_strs_free (names);

	return result;
}

/* Check if this handle is already present in the plugins table.
 * Returns 1 if so. */
static int present_handle (const lt_dlhandle h)
{
	int i;

	for (i = 0; i < plugins_num; i++) {
		if (plugins[i].handle == h)
			return 1;
	}

	return 0;
}

static int lt_load_plugin (const char *file, lt_ptr debug_info_ptr)
{
	int debug_info;
	const char *name;
	union {
		void *data;
		plugin_init_func *func;
	} init;

	debug_info = *(int *)debug_info_ptr;
	name = strrchr (file, '/');
	name = name ? (name + 1) : file;
	if (debug_info)
		printf ("Loading plugin %s...\n", name);

	if (plugins_num == PLUGINS_NUM) {
		fprintf (stderr, "Can't load plugin, because maximum number "
		                                    "of plugins reached!\n");
		return 0;
	}

	plugins[plugins_num].handle = lt_dlopenext (file);
	if (!plugins[plugins_num].handle) {
		fprintf (stderr, "Can't load plugin %s: %s\n", name, lt_dlerror ());
		return 0;
	}

	if (present_handle (plugins[plugins_num].handle)) {
		if (debug_info)
			printf ("Already loaded\n");
		if (lt_dlclose (plugins[plugins_num].handle))
			fprintf (stderr, "Error unloading plugin: %s\n", lt_dlerror ());
		return 0;
	}

	init.data = lt_dlsym (plugins[plugins_num].handle, "plugin_init");
	if (!init.data) {
		fprintf (stderr, "No init function in the plugin!\n");
		if (lt_dlclose (plugins[plugins_num].handle))
			fprintf (stderr, "Error unloading plugin: %s\n", lt_dlerror ());
		return 0;
	}

	/* If this call to init.func() fails with memory access or illegal
	 * instruction errors then read the commit log message for r2831. */
	plugins[plugins_num].decoder = init.func ();
	if (!plugins[plugins_num].decoder) {
		fprintf (stderr, "NULL decoder!\n");
		if (lt_dlclose (plugins[plugins_num].handle))
			fprintf (stderr, "Error unloading plugin: %s\n", lt_dlerror ());
		return 0;
	}

	if (plugins[plugins_num].decoder->api_version != DECODER_API_VERSION) {
		fprintf (stderr, "Plugin uses different API version\n");
		if (lt_dlclose (plugins[plugins_num].handle))
			fprintf (stderr, "Error unloading plugin: %s\n", lt_dlerror ());
		return 0;
	}

	plugins[plugins_num].name = extract_decoder_name (name);

	/* Is the Vorbis decoder using Tremor? */
	if (!strcmp (plugins[plugins_num].name, "vorbis")) {
		void *vorbis_has_tremor;

		vorbis_has_tremor = lt_dlsym (plugins[plugins_num].handle,
		                              "vorbis_has_tremor");
		have_tremor = vorbis_has_tremor != NULL;
	}

	debug ("Loaded %s decoder", plugins[plugins_num].name);

	if (plugins[plugins_num].decoder->init)
		plugins[plugins_num].decoder->init ();
	plugins_num += 1;

	if (debug_info)
		printf ("OK\n");

	return 0;
}

/* Create a new preferences entry and initialise it. */
static decoder_t_preference *make_preference (const char *prefix)
{
	decoder_t_preference *result;

	assert (prefix && prefix[0]);

	result = (decoder_t_preference *)xmalloc (
		offsetof (decoder_t_preference, type) + strlen (prefix) + 1
	);
	result->next = NULL;
	result->decoders = 0;
	strcpy (result->type, prefix);
	result->subtype = strchr (result->type, '/');
	if (result->subtype) {
		*result->subtype++ = 0x00;
		result->subtype = clean_mime_subtype (result->subtype);
	}

	return result;
}

/* Is the given decoder (by index) already in the decoder list for 'pref'? */
static bool is_listed_decoder (decoder_t_preference *pref, int d)
{
	int ix;
	bool result;

	assert (pref);
	assert (d >= 0);

	result = false;
	for (ix = 0; ix < pref->decoders; ix += 1) {
		if (d == pref->decoder_list[ix]) {
			result = true;
			break;
		}
	}

	return result;
}

/* Add the named decoder (if valid) to a preferences decoder list. */
static void load_each_decoder (decoder_t_preference *pref, const char *name)
{
	int d;

	assert (pref);
	assert (name && name[0]);

	d = lookup_decoder_by_name (name);

	/* Drop unknown decoders. */
	if (d == plugins_num)
		return;

	/* Drop duplicate decoders. */
	if (is_listed_decoder (pref, d))
		return;

	pref->decoder_list[pref->decoders++] = d;

	return;
}

/* Build a preference's decoder list. */
static void load_decoders (decoder_t_preference *pref, lists_t_strs *tokens)
{
	int ix, dx, asterisk_at;
	int decoder[PLUGINS_NUM];
	const char *name;

	assert (pref);
	assert (tokens);

	asterisk_at = -1;

	/* Add the index of each known decoder to the decoders list.
	 * Note the position following the first asterisk. */
	for (ix = 1; ix < lists_strs_size (tokens); ix += 1) {
		name = lists_strs_at (tokens, ix);
		if (strcmp (name, "*"))
			load_each_decoder (pref, name);
		else if (asterisk_at == -1)
			asterisk_at = pref->decoders;
	}

	if (asterisk_at == -1)
		return;

	dx = 0;

	/* Find decoders not already listed. */
	for (ix = 0; ix < plugins_num; ix += 1) {
		if (!is_listed_decoder (pref, ix))
			decoder[dx++] = ix;
	}

	/* Splice asterisk decoders into the decoder list. */
	for (ix = 0; ix < dx; ix += 1) {
		pref->decoder_list[pref->decoders++] =
		      pref->decoder_list[asterisk_at + ix];
		pref->decoder_list[asterisk_at + ix] = decoder[ix];
	}

	assert (RANGE(0, pref->decoders, plugins_num));
}

/* Add a new preference for an audio format. */
static void load_each_preference (const char *preference)
{
	const char *prefix;
	lists_t_strs *tokens;
	decoder_t_preference *pref;

	assert (preference && preference[0]);

	tokens = lists_strs_new (4);
	lists_strs_split (tokens, preference, "(,)");
	prefix = lists_strs_at (tokens, 0);
	pref = make_preference (prefix);
#ifdef DEBUG
	pref->source = preference;
#endif
	load_decoders (pref, tokens);
	pref->next = preferences;
	preferences = pref;
	lists_strs_free (tokens);
}

/* Load all preferences given by the user in PreferredDecoders. */
static void load_preferences ()
{
	int ix;
	const char *preference;
	lists_t_strs *list;

	list = options_get_list ("PreferredDecoders");

	for (ix = 0; ix < lists_strs_size (list); ix += 1) {
		preference = lists_strs_at (list, ix);
		load_each_preference (preference);
	}

#ifdef DEBUG
	{
		char *names;
		decoder_t_preference *pref;

		for (pref = preferences; pref; pref = pref->next) {
			names = list_decoder_names (pref->decoder_list, pref->decoders);
			debug ("%s:%s", pref->source, names);
			free (names);
		}
	}
#endif
}

static void load_plugins (int debug_info)
{
	int ix;
	char *names;

	if (debug_info)
		printf ("Loading plugins from %s...\n", PLUGIN_DIR);
	if (lt_dlinit ())
		fatal ("lt_dlinit() failed: %s", lt_dlerror ());

	if (lt_dlforeachfile (PLUGIN_DIR, &lt_load_plugin, &debug_info))
		fatal ("Can't load plugins: %s", lt_dlerror ());

	if (plugins_num == 0)
		fatal ("No decoder plugins have been loaded!");

	for (ix = 0; ix < plugins_num; ix += 1)
		default_decoder_list[ix] = ix;

	names = list_decoder_names (default_decoder_list, plugins_num);
	logit ("Loaded %d decoders:%s", plugins_num, names);
	free (names);
}

void decoder_init (int debug_info)
{
	load_plugins (debug_info);
	load_preferences ();
}

static void cleanup_decoders ()
{
	int ix;

	for (ix = 0; ix < plugins_num; ix++) {
		if (plugins[ix].decoder->destroy)
			plugins[ix].decoder->destroy ();
		free (plugins[ix].name);
		if (plugins[ix].handle)
			lt_dlclose (plugins[ix].handle);
	}

	if (lt_dlexit ())
		logit ("lt_exit() failed: %s", lt_dlerror ());
}

static void cleanup_preferences ()
{
	decoder_t_preference *pref, *next;

	pref = preferences;
	for (pref = preferences; pref; pref = next) {
		next = pref->next;
		free (pref);
	}

	preferences = NULL;
}

void decoder_cleanup ()
{
	cleanup_decoders ();
	cleanup_preferences ();
}

/* Fill the error structure with an error of a given type and message.
 * strerror(add_errno) is appended at the end of the message if add_errno != 0.
 * The old error message is free()ed.
 * This is thread safe; use this instead of constructs using strerror(). */
void decoder_error (struct decoder_error *error,
		const enum decoder_error_type type, const int add_errno,
		const char *format, ...)
{
	char *err_str;
	va_list va;

	if (error->err)
		free (error->err);

	error->type = type;

	va_start (va, format);
	err_str = format_msg_va (format, va);
	va_end (va);

	if (add_errno) {
		char *err_buf;

		err_buf = xstrerror (add_errno);
		error->err = format_msg ("%s%s", err_str, err_buf);
		free (err_buf);
	}
	else
		error->err = format_msg ("%s", err_str);

	free (err_str);
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

/* Return the error text from the decoder_error variable. */
const char *decoder_error_text (const struct decoder_error *error)
{
	return error->err;
}
