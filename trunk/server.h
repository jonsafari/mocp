#ifndef SERVER_H
#define SERVER_H

int server_init (int debug, int foreground);
void server_loop (int list_sock);
void error (const char *format, ...);
void state_change ();
void set_info_rate (const int rate);
void set_info_channels (const int channels);
void set_info_bitrate (const int bitrate);
void ctime_change ();

#endif
