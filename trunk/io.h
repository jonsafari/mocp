#ifndef IO_H
#define IO_H

/* curl sometimes needs this */

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#ifdef HAVE_SYS_SELECT_H
# include <sys/select.h>
#endif
#include <unistd.h> /* fot [s]size_t */
#include <pthread.h>
#ifdef HAVE_CURL
# include <curl/curl.h>
#endif
#include "fifo_buf.h"

enum io_source
{
	IO_SOURCE_FD,
	IO_SOURCE_MMAP,
	IO_SOURCE_CURL
};

struct io_stream
{
	enum io_source source;
	int fd;
	size_t size;	/* source of the file if needed */
	int errno_val;	/* errno value of the last operation  - 0 if ok */
	char *strerror;	/* error string */
	int opened;	/* was the stream opened (open(), mmap(), etc.)? */
	int eof;	/* was the end of file reached? */
	int after_seek;	/* are we after seek and need to do fresh read()? */
	int buffered;	/* are we using the buffer? */
	size_t pos;	/* current position in the file from the user point of
			   view */
	pthread_mutex_t io_mutex;	/* mutex for IO operations */

#ifdef HAVE_MMAP
	void *mem;
	size_t mem_pos;
#endif

#ifdef HAVE_CURL
	CURLM *curl_multi_handle;	/* we use the multi interface to get the
					   data in pieces */
	CURL *curl_handle;		/* the actual used handle */
	CURLMcode curl_multi_status;	/* curl status of the last multi
					   operation */
	CURLcode curl_status;		/* curl status of the last easy
					   operation */
	char *url;
	struct curl_slist *http_headers;	/* HTTP headers to send with
						   the request */
	char *curl_buf;			/* buffer for data the curl gives us */
	long curl_buf_fill;
	
#endif

	struct fifo_buf buf;
	pthread_mutex_t buf_mutex;
	pthread_cond_t buf_free_cond; /* some space became available in the
					 buffer */
	pthread_cond_t buf_fill_cond; /* the buffer was filled with some data */
	pthread_t read_thread;
	int stop_read_thread;		/* request for stopping the read
					   thread */
};

struct io_stream *io_open (const char *file, const int buffered);
ssize_t io_read (struct io_stream *s, void *buf, size_t count);
ssize_t io_peek (struct io_stream *s, void *buf, size_t count);
off_t io_seek (struct io_stream *s, off_t offset, int whence);
void io_close (struct io_stream *s);
int io_ok (struct io_stream *s);
char *io_strerror (struct io_stream *s);
ssize_t io_file_size (const struct io_stream *s);
long io_tell (struct io_stream *s);
int io_eof (struct io_stream *s);
void io_init ();
void io_cleanup ();

#endif
