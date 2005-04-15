#ifndef PLAYER_H
#define PLAYER_H

#include "out_buf.h"
#include "io.h"
#include "playlist.h"

void player_cleanup ();
void player (const char *file, const char *next_file, struct out_buf *out_buf);
void player_stop ();
void player_seek (const int n);
void player_reset ();
void player_init ();
struct file_tags *player_get_curr_tags ();

#endif
