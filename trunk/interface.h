#ifndef INTERFACE_H
#define INTERFACE_H

void init_interface (const int sock, const int logging, char **args,
		const int arg_num);
void interface_loop ();
int server_connect ();
void interface_end ();
int user_wants_interrupt ();
void interface_error (const char *msg);
#ifdef HAVE_ATTRIBUTE__
void interface_fatal (const char *format, ...)
	__attribute__ ((format (printf, 1, 2)));
#else
void interface_fatal (const char *format, ...);
#endif


#endif
