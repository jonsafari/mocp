#ifndef DECODER_H
#define DECODER_H

#include "audio.h"
#include "playlist.h"

enum decoder_error_type
{
	ERROR_OK,
	ERROR_STREAM,
	ERROR_FATAL
};

struct decoder_error
{
	enum decoder_error_type type;
	char *err;	/* malloc()ed error string */
};

/* Functions provided by the decoder plugin. */
struct decoder
{
#if 0
	int api_version;
#endif
	
	/* Open the resource and return pointer to the private decoder data. */
	void *(*open)(const char *uri);

	/* Close the resource and cleanup. */
	void (*close)(void *data);

	/* Decode a piece of input and write it to the buf of size buf_len.
	 * Put sound parameters into the sound_params structure.
	 * Return the number of butes written or 0 on EOF. */
	int (*decode)(void *data, char *buf, int buf_len,
			struct sound_params *sound_params);
	
	/* Seek in the stream to the given second. Return the time actualy seek
	 * or -1 on error. */
	int (*seek)(void *data, int sec);

	/* Fill the tags structure for a file. */
	void (*info)(const char *file, struct file_tags *tags,
			const int tags_sel);
	
	/* Return the current bitrate (for the last pice of sound) in Kbps or
	 * -1 if not available. */
	int (*get_bitrate)(void *data);

	/* Get duration of an opened resource in seconds. Return -1 on error. */
	int (*get_duration)(void *data);

	/* Fill the error structure. */
	void (*get_error)(void *data, struct decoder_error *error);
	
	/* Return != 0 if this file extension is supported by this decoder. */
	int (*our_format_ext)(const char *ext);

	/* Put the 3-chars null-terminated format name for this file into buf.
	 */
	void (*get_name)(const char *file, char buf[4]);
};

int is_sound_file (const char *name);
struct decoder *get_decoder (const char *file);
void decoder_init ();
void decoder_cleanup ();
char *file_type_name (const char *file);

#ifdef HAVE__ATTRIBUTE__
void decoder_error (struct decoder_error *error,
		const enum decoder_error_type type, const int add_errno,
		const char *format, ...) __attribute__((format (printf, 4, 5)));
#else
void decoder_error (struct decoder_error *error,
		const enum decoder_error_type type, const int add_errno,
		const char *format, ...);
#endif

void decoder_error_clear (struct decoder_error *error);
void decoder_error_copy (struct decoder_error *dst,
		const struct decoder_error *src);
void decoder_error_init (struct decoder_error *error);

#endif
