#ifndef PLAYER_H
#define PLAYER_H

#include "out_buf.h"
#include "io.h"

void player_cleanup ();
void player (char *file, char *next_file, struct out_buf *out_buf);
void player_by_stream (struct io_stream *stream, const struct decoder *df);
void player_stop ();
void player_seek (const int n);
void player_reset ();
void player_init ();

#endif
