#ifndef LOG_H
#define LOG_H

#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifdef DEBUG
# define debug logit
#else
# define debug fake_logit
#endif

#ifndef NDEBUG
# define logit(format, ...) \
	internal_logit (__FILE__, __LINE__, __FUNCTION__, format, \
	## __VA_ARGS__)
#else
# define logit fake_logit
#endif

#ifdef HAVE__ATTRIBUTE__
void internal_logit (const char *file, const int line, const char *function,
		const char *format, ...)
	__attribute__ ((format (printf, 4, 5)));
#else
void internal_logit (const char *file, const int line, const char *function,
		const char *format, ...);
#endif

void fake_logit (const char *format, ...);
void log_init_stream (FILE *f, const char *fn);
void log_close ();

#ifdef __cplusplus
}
#endif

#endif
