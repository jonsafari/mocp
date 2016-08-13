#ifndef SERVER_H
#define SERVER_H

#ifdef __cplusplus
extern "C" {
#endif

#include "playlist.h"

#define CLIENTS_MAX	10

void server_init (int debug, int foreground);
void server_loop ();
void server_error (const char *file, int line, const char *function,
                   const char *msg);
void state_change ();
void set_info_rate (const int rate);
void set_info_channels (const int channels);
void set_info_bitrate (const int bitrate);
void set_info_avg_bitrate (const int avg_bitrate);
void tags_change ();
void ctime_change ();
void status_msg (const char *msg);
void tags_response (const int client_id, const char *file,
		const struct file_tags *tags);
void ev_audio_start ();
void ev_audio_stop ();
void server_queue_pop (const char *filename);

#ifdef __cplusplus
}
#endif

#endif
