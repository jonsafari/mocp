#ifndef IO_CURL_H
#define IO_CURL_H

#include "io.h"

#ifdef __cplusplus
extern "C" {
#endif

void io_curl_init ();
void io_curl_cleanup ();
void io_curl_open (struct io_stream *s, const char *url);
void io_curl_close (struct io_stream *s);
ssize_t io_curl_read (struct io_stream *s, char *buf, size_t count);
void io_curl_strerror (struct io_stream *s);
void io_curl_wake_up (struct io_stream *s);

#ifdef __cplusplus
}
#endif

#endif
