#ifndef INTERFACE_H
#define INTERFACE_H

void init_interface (const int sock, const int debug);
void interface_loop ();
int server_connect ();
void interface_end ();
void interface_error (const char *format, ...);

#endif
