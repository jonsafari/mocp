#ifndef SERVER_H
#define SERVER_H

int server_init (int debug, int foreground);
void server_loop (int list_sock);
#ifdef HAVE_ATTRIBUTE__
void error (const char *format, ...) __attribute__ ((format (printf, 1, 2)));
#else
void error (const char *format, ...);
#endif
void state_change ();
void set_info_rate (const int rate);
void set_info_channels (const int channels);
void set_info_bitrate (const int bitrate);
void ctime_change ();

#endif
