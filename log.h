#ifndef LOG_H
#define LOG_H

#include <stdio.h>

void logit (const char *format, ...) __attribute__ ((format (printf, 1, 2)));
void log_init_stream (FILE *f);
void log_close ();

#endif
