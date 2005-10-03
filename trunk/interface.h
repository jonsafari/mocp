#ifndef INTERFACE_H
#define INTERFACE_H

void init_interface (const int sock, const int logging, char **args,
		const int arg_num, const int recursively);
void interface_loop ();
void interface_end ();
int user_wants_interrupt ();
void interface_error (const char *msg);

#ifdef HAVE_ATTRIBUTE__
void interface_fatal (const char *format, ...)
	__attribute__ ((format (printf, 1, 2)));
#else
void interface_fatal (const char *format, ...);
#endif

void interface_cmdline_clear_plist (int server_sock);
void interface_cmdline_append (int server_sock, char **args,
		const int arg_num);
void interface_cmdline_play_first (int server_sock);
void interface_cmdline_file_info (const int server_sock);
void interface_cmdline_playit (int server_sock, char **args, const int arg_num);
void interface_cmdline_seek_by (int server_sock, const int seek_by);

#endif
