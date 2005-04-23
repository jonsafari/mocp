#ifndef DECODER_H
#define DECODER_H

#include "audio.h"
#include "playlist.h"
#include "io.h"

/* On every change in the decoder API this number will be changed, so MOC will
 * not load plugins compiled with older/newer decoder.h. */
#define DECODER_API_VERSION	3

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

/* Functions provided by the decoder plugin. If some field is optional, it may
 * have NULL value. */
struct decoder
{
	/* Set it to DECODER_API_VERSION to recognize if MOC and the plugin can
	 * communicate. This is the first field in the structure, so even after
	 * changing other fields it will hopefully be always read properly. */
	int api_version;
	
	/* Open the resource and return pointer to the private decoder data. */
	void *(*open)(const char *uri);

	/* Open for an already opened stream. Optional. */
	void *(*open_stream)(struct io_stream *stream);

	/* Return 1 if the decoder is able to decode data from this stream.
	 * Optional. */
	int (*can_decode)(struct io_stream *stream);

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

	/* Fill the tags structure with the current tags for the stream.
	 * Return 1 if the tags were changed from the last call of this function
	 * and 0 if not. Optional. */
	int (*current_tags)(void *data, struct file_tags *tags);

	/* Get the pointer to the io_stream structure used in the decoder or
	 * NULL if this is not used. Optional. */
	struct io_stream *(*get_stream)(void *data);
};

/* Function that must be exported by a plugin. */
typedef struct decoder *(*plugin_init_func)();

int is_sound_file (const char *name);
struct decoder *get_decoder (const char *file);
struct decoder *get_decoder_by_content (struct io_stream *stream);
void decoder_init (int debug_info);
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
