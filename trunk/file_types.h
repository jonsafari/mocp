#ifndef FILE_TYPES_H
#define FILE_TYPES_H

#include "playlist.h"
#include "audio.h"

/* Flags for the info decoder function. */
enum tags_select
{
	TAGS_COMMENTS	= 0x01, /* artist, title, etc. */
	TAGS_TIME	= 0x02 /* time of the file. */
};

/* Functions that must be provided to support a file format.
 * void *data is a pointer to private data used by the decoder passed to each
 * function. */
struct decoder_funcs
{
	/* Open a file and return data used by the decoder. Return NULL on
	 * error*/
	void *(*open)(const char *file);

	/* Close the file and cleanup. */
	void (*close)(void *data);

	/* Decode a piece of input and write it to the buf of size buf_len.
	 * Return the number of butes written or 0 on EOF. */
	int (*decode)(void *data, char *buf, int buf_len,
			struct sound_params *sound_params);

	/* Seek in the stream to the gives second. Return the time actualy seek
	 * or -1 on error. */
	int (*seek)(void *data, int sec);

	/* Fill the tags structure for a file. */
	void (*info)(const char *file, struct file_tags *tags,
			const int tags_sel);

	/* Return the bitrate in Kbps or -1 if not available. */
	int (*get_bitrate)(void *data);

	/* Get duration of a filein seconds. Return -1 on error. */
	int (*get_duration)(void *data);
};

int is_sound_file (const char *name);
struct decoder_funcs *get_decoder_funcs (const char *file);
void file_types_init ();
void file_types_cleanup ();
char *format_name (const char *file);

#endif
