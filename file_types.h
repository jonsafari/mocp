#ifndef FILE_TYPES_H
#define FILE_TYPES_H

#include "playlist.h"

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
	int (*decode)(void *data, char *buf, int buf_len);

	/* Seek in the stream by n seconds. */
	void (*seek)(void *data, const int n);

	/* Fill the tags structure for a file. */
	void (*info)(const char *file, struct file_tags *tags);
};

int is_sound_file (char *name);
struct decoder_funcs *get_decoder_funcs (char *file);
void file_types_init ();

#endif
