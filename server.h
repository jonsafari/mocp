#ifndef SERVER_H
#define SERVER_H

#include "playlist.h"

int server_init (int debug, int foreground);
void server_loop (int list_sock);
void server_error (const char *msg);
void state_change ();
void set_info_rate (const int rate);
void set_info_channels (const int channels);
void set_info_bitrate (const int bitrate);
void set_info_tags (struct file_tags *tags);
void ctime_change ();

#endif
