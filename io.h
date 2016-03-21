#ifndef IO_H
#define IO_H

#include <sys/types.h>
#include <pthread.h>
#ifdef HAVE_CURL
# include <sys/socket.h>     /* curl sometimes needs this */
# include <curl/curl.h>
#endif

#include "fifo_buf.h"

#ifdef __cplusplus
extern "C" {
#endif

enum io_source
{
	IO_SOURCE_FD,
	IO_SOURCE_MMAP,
	IO_SOURCE_CURL
};

#ifdef HAVE_CURL
struct io_stream_curl
{
	CURLM *multi_handle;	/* we use the multi interface to get the
					   data in pieces */
	CURL *handle;		/* the actual used handle */
	CURLMcode multi_status;	/* curl status of the last multi operation */
	CURLcode status;	/* curl status of the last easy operation */
	char *url;
	struct curl_slist *http_headers;	/* HTTP headers to send with
						   the request */
	char *buf;		/* buffer for data that curl gives us */
	long buf_fill;
	int need_perform_loop;	/* do we need the perform() loop? */
	int got_locn;	/* received a location header */
	char *mime_type;	/* mime type of the stream */
	int wake_up_pipe[2];	/* pipes used to wake up the curl read
					   loop that does select() */
	struct curl_slist *http200_aliases; /* list of aliases for http
						response's status line */
	size_t icy_meta_int;	/* how often are icy metadata sent?
				   0 - disabled, in bytes */
	size_t icy_meta_count;	/* how many bytes was read from the last
				   metadata packet */
};
#endif

struct io_stream;

typedef void (*buf_fill_callback_t) (struct io_stream *s, size_t fill,
		size_t buf_size, void *data_ptr);

struct io_stream
{
	enum io_source source;	/* source of the file */
	int fd;
	off_t size;	/* size of the file */
	int errno_val;	/* errno value of the last operation  - 0 if ok */
	int read_error; /* set to != 0 if the last read operation dailed */
	char *strerror;	/* error string */
	int opened;	/* was the stream opened (open(), mmap(), etc.)? */
	int eof;	/* was the end of file reached? */
	int after_seek;	/* are we after seek and need to do fresh read()? */
	int buffered;	/* are we using the buffer? */
	off_t pos;	/* current position in the file from the user point of view */
	size_t prebuffer;	/* number of bytes left to prebuffer */
	pthread_mutex_t io_mtx;	/* mutex for IO operations */

#ifdef HAVE_MMAP
	void *mem;
	off_t mem_pos;
#endif

#ifdef HAVE_CURL
	struct io_stream_curl curl;
#endif

	struct fifo_buf *buf;
	pthread_mutex_t buf_mtx;
	pthread_cond_t buf_free_cond; /* some space became available in the
					 buffer */
	pthread_cond_t buf_fill_cond; /* the buffer was filled with some data */
	pthread_t read_thread;
	int stop_read_thread;		/* request for stopping the read
					   thread */

	struct stream_metadata {
		pthread_mutex_t mtx;
		char *title;	/* title of the stream */
		char *url;
	} metadata;

	/* callbacks */
	buf_fill_callback_t buf_fill_callback;
	void *buf_fill_callback_data;
};

struct io_stream *io_open (const char *file, const int buffered);
ssize_t io_read (struct io_stream *s, void *buf, size_t count);
ssize_t io_peek (struct io_stream *s, void *buf, size_t count);
off_t io_seek (struct io_stream *s, off_t offset, int whence);
void io_close (struct io_stream *s);
int io_ok (struct io_stream *s);
char *io_strerror (struct io_stream *s);
off_t io_file_size (const struct io_stream *s);
off_t io_tell (struct io_stream *s);
int io_eof (struct io_stream *s);
void io_init ();
void io_cleanup ();
void io_abort (struct io_stream *s);
char *io_get_mime_type (struct io_stream *s);
char *io_get_title (struct io_stream *s);
char *io_get_metadata_title (struct io_stream *s);
char *io_get_metadata_url (struct io_stream *s);
void io_set_metadata_title (struct io_stream *s, const char *title);
void io_set_metadata_url (struct io_stream *s, const char *url);
void io_prebuffer (struct io_stream *s, const size_t to_fill);
void io_set_buf_fill_callback (struct io_stream *s,
		buf_fill_callback_t callback, void *data_ptr);
int io_seekable (const struct io_stream *s);

#ifdef __cplusplus
}
#endif

#endif
