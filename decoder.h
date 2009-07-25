#ifndef DECODER_H
#define DECODER_H

#include "audio.h"
#include "playlist.h"
#include "io.h"

/** Version of the decoder API.
 *
 * On every change in the decoder API this number will be changed, so MOC will
 * not load plugins compiled with older/newer decoder.h. */
#define DECODER_API_VERSION	7

/** Type of the decoder error. */
enum decoder_error_type
{
	ERROR_OK, /*!< There was no error. */
	ERROR_STREAM, /*!< Recoverable error in the stream. */
	ERROR_FATAL /*!< Fatal error in the stream - further decoding can't be
		      performed. */
};

/** Decoder error.
 *
 * Describes decoder error. Fields don't need to be accessed directly, there are
 * functions to modify/access decoder_error object. */
struct decoder_error
{
	enum decoder_error_type type; /*!< Type of the error. */
	char *err;	/*!< malloc()ed error string or NULL. */
};

/** @struct decoder
 * Functions provided by the decoder plugin.
 *
 * Describes decoder - contains pointers to decoder's functions. If some field
 * is optional, it may have NULL value. */
struct decoder
{
	/** API version used by the plugin.
	 *
	 * Set it to DECODER_API_VERSION to recognize if MOC and the plugin can
	 * communicate. This is the first field in the structure, so even after
	 * changing other fields it will hopefully be always read properly.
	 */
	int api_version;

	/** Initialize the plugin.
	 *
	 * This function is called once at MOC startup (once for the client and
	 * once for the server). Optional. */
	void (*init) ();
	
	/** Cleanup the plugin.
	 *
	 * This function is called once at exit (once for the client and
	 * once for the server). Optional. */
	void (*destroy) ();
	
	
	/** Open the resource.
	 *
	 * Open the given resource (file).
	 *
	 * \param uri URL to the resource that can be used as the file parameter and return pointer
	 * to io_open().
	 * 
	 * \return Private decoder data. This pointer will be passed to every
	 * other function that operates on the stream.
	 */
	void *(*open)(const char *uri);

	/** Open the resource for an already opened stream.
	 *
	 * Handle the stream that was already opened, but no data were read.
	 * You must operate on the stream using io_*() functions. This is used
	 * for internet streams, so seeking is not possible. This function is
	 * optional.
	 *
	 * \param stream Opened stream from which the decoder must read.
	 *
	 * \return Private decoder data. This pointer will be passed to every
	 * other function that operates on the stream.
	 */
	void *(*open_stream)(struct io_stream *stream);

	/** Check if the decoder is able to decode from this stream.
	 *
	 * Used to check if the decoder is able to read from an already opened
	 * stream. This is used to find the proper decoder for an internet
	 * stream when searching by the MIME type failed. The decoder must not
	 * read from this stream (io_read()), but can peek data (io_peek()).
	 * The decoder is expected to peek a few bytes to recognize it's format.
	 * Optional.
	 * 
	 * \param stream Opened stream.
	 *
	 * \return 1 if the decoder is able to decode data from this stream.
	 */
	int (*can_decode)(struct io_stream *stream);

	/** Close the resource and cleanup.
	 *
	 * Free all decoder's private data and allocated resources.
	 *
	 * \param data Decoder's private data.
	 */
	void (*close)(void *data);

	/** Decode a piece of input.
	 *
	 * Decode a piece of input and write it to the buffer. The buffer size
	 * is at least 32KB, but don't make any assumptions that it is always
	 * true. It is prefered to decode as few bytes as possible without
	 * loosing much performance to cause very small delays.
	 *
	 * \param data Decoder's provate data.
	 * \param buf Buffer to put data in.
	 * \param buf_len Size of the buffer in bytes.
	 * \param sound_params Parameters of the decoded sound. This must be
	 * always filled.
	 *
	 * \return Number of butes written or 0 on EOF.
	 */
	int (*decode)(void *data, char *buf, int buf_len,
			struct sound_params *sound_params);
	
	/** Seek in the stream.
	 *
	 * Seek to the given position.
	 *
	 * \param data Decoder's private data.
	 * \param sec Where to seek in seconds.
	 *
	 * \return The position that we actually seek to or -1 on error. 
	 * -1 is not a fatal error and further decoding will be performed.
	 */
	int (*seek)(void *data, int sec);

	/** Get tags for a file.
	 *
	 * Get requested file's tags. Is some tags are not available, the
	 * decoder can just not fill the field. The function can even not
	 * fill any field.
	 *
	 * \param file File to get tags for.
	 * \param tags Pointer to the tags structure where we must put
	 * the tags. All strings must be malloc()ed.
	 * \param tags_sel OR'ed list of requested tags (values of
	 * enum tags_select).
	 */
	void (*info)(const char *file, struct file_tags *tags,
			const int tags_sel);
	
	/** Get the current bitrate.
	 *
	 * Get the bitrate of the last decoded piece of sound.
	 *
	 * \param data Decoder's private data.
	 * 
	 * \return Current bitrate in kbps or -1 if not available.
	 */
	int (*get_bitrate)(void *data);

	/** Get duration of the stream.
	 *
	 * Get duration of the stream. It is used as a faster alternative
	 * for getting duration using info() if the file is opened.
	 *
	 * \param data Decoder's private data.
	 *
	 * \return Duration in seconds or -1 on error. -1 is not a fatal
	 * error, further decoding fill be performed.
	 */
	int (*get_duration)(void *data);

	/** Get error for the last decode() invokation.
	 *
	 * Get the error for the last decode() invokation. If there was no
	 * error the type of the error must be ERROR_OK. Don't access the
	 * error object's fields directly, there are proper functions for
	 * that.
	 *
	 * \param data Decoder's private data.
	 * \param error Pointer to the decoder_error opiect to fill.
	 */
	void (*get_error)(void *data, struct decoder_error *error);
	
	/** Check if the file extension is for a file that this decoder
	 * supports.
	 *
	 * \param ext Extension (chars after the last dot in the file name).
	 *
	 * \return Value other than 0 if the extension if a file with this
	 * extension is supported.
	 */
	int (*our_format_ext)(const char *ext);

	/** Check if a stream with the given MIME type is supported by this
	 * decoder. Optional.
	 *
	 * \param mime_type MIME type.
	 *
	 * \return Value other than 0 if a stream with this mime type is
	 * supported.
	 */
	int (*our_format_mime)(const char *mime_type);

	/** Get a 3-chars format name for a file.
	 *
	 * Get an abbreviated format name (up to 3 chars) for a file.
	 *
	 * \param file File for which we want the format name.
	 * \param buf Buffer where the format name and ending zero-byte
	 * must be put.
	 */
	void (*get_name)(const char *file, char buf[4]);

	/** Get current tags for the stream.
	 *
	 * Fill the tags structure with the current tags for the stream. This
	 * is intended for internet streams and used when the source of the
	 * stream doesn't provide tags while broadcasting. This function is
	 * optional.
	 *
	 * \param data Decoder's private data.
	 * 
	 * \return 1 if the tags were changed from the last call of this
	 * function or 0 if not.
	 */
	int (*current_tags)(void *data, struct file_tags *tags);

	/** Get the IO stream used by the decoder.
	 *
	 * Get the pointer to the io_stream object used by the decoder. This is
	 * used for fast interrupting especially when the stream reads from
	 * a network. This function is optional.
	 *
	 * \param data Decoder's private data.
	 *
	 * \return Pointer to the used IO stream.
	 */
	struct io_stream *(*get_stream)(void *data);

	/** Get the average bitrate.
	 *
	 * Get the bitrate of the whole file.
	 *
	 * \param data Decoder's private data.
	 * 
	 * \return Average bitrate in kbps or -1 if not available.
	 */
	int (*get_avg_bitrate)(void *data);
};

/** Initialize decoder plugin.
 *
 * Each decoder plugin must export a function name plugin_init of this type.
 * The function must return a pointer to the struct decoder variable filled
 * with pointers to decoder's functions.
 */
typedef struct decoder *(*plugin_init_func)();

int is_sound_file (const char *name);
struct decoder *get_decoder (const char *file);
struct decoder *get_decoder_by_content (struct io_stream *stream);
void decoder_init (int debug_info);
void decoder_cleanup ();
char *file_type_name (const char *file);

/** @defgroup decoder_error_funcs Decoder error functions
 *
 * Those functions can be used to modify variables of type struct decoder_error.
 */
/*@{*/

/** Fill decoder_error structure with an error.
 *
 * Fills decoder error variable with an error. It can be used like printf().
 *
 * \param error Pointer to the decoder error variable to fill.
 * \param type Type of the error.
 * \param add_errno If this value is non-zero, a space and a string describing
 * system error for errno equal to the value of add_errno is appended to the
 * error message.
 * \param format Format, like in the printf() function.
 */
#ifdef HAVE__ATTRIBUTE__
void decoder_error (struct decoder_error *error,
		const enum decoder_error_type type, const int add_errno,
		const char *format, ...) __attribute__((format (printf, 4, 5)));
#else
void decoder_error (struct decoder_error *error,
		const enum decoder_error_type type, const int add_errno,
		const char *format, ...);
#endif

/** Clear decoder_error structure.
 *
 * Clear decoder_error structure. Set the system type to ERROR_OK and
 * the error message to NULL. Frees all memory used by the error's fields.
 */
void decoder_error_clear (struct decoder_error *error);

/** Copy decoder_error variable.
 *
 * Copies the decoder_error variable to another decoder_error variable.
 *
 * \param dst Destination.
 * \param src Source.
 */
void decoder_error_copy (struct decoder_error *dst,
		const struct decoder_error *src);

/** Initialize decoder_error variable.
 *
 * Initialize decoder_error variable and set the error to ERROR_OK with no
 * message.
 */
void decoder_error_init (struct decoder_error *error);

/*@}*/

#endif
