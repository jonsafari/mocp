#ifndef INTERFACE_H
#define INTERFACE_H

void init_interface (const int sock, const int debug, char **args,
		const int arg_num);
void interface_loop ();
int server_connect ();
void interface_end ();
int user_wants_interrupt ();

#ifdef HAVE__ATTRIBUTE__
void interface_error (const char *format, ...)
	__attribute__((format (printf, 1, 2)));
#else
void interface_error (const char *format, ...);
#endif

#endif
