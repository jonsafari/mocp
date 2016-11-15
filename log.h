#ifndef LOG_H
#define LOG_H

#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifdef DEBUG
# define debug logit
#else
# define debug(...)  do {} while (0)
#endif

#ifndef NDEBUG
# define logit(...) \
	internal_logit (__FILE__, __LINE__, __func__, ## __VA_ARGS__)
#else
# define logit(...)  do {} while (0)
#endif

#ifndef STRERROR_FN
# define STRERROR_FN xstrerror
#endif

#ifndef NDEBUG
#define log_errno(format, errnum) \
	do { \
		char *err##__LINE__ = STRERROR_FN (errnum); \
		logit (format ": %s", err##__LINE__); \
		free (err##__LINE__); \
	} while (0)
#else
# define log_errno(...) do {} while (0)
#endif

void internal_logit (const char *file, const int line, const char *function,
		const char *format, ...) ATTR_PRINTF(4, 5);

#ifndef NDEBUG
# define LOGIT_ONLY
#else
# define LOGIT_ONLY ATTR_UNUSED
#endif

#if !defined(NDEBUG) && defined(DEBUG)
# define DEBUG_ONLY
#else
# define DEBUG_ONLY ATTR_UNUSED
#endif

void log_init_stream (FILE *f, const char *fn);
void log_circular_start ();
void log_circular_log ();
void log_circular_reset ();
void log_circular_stop ();
void log_close ();

#ifndef NDEBUG
void log_signal (int sig);
#else
# define log_signal(...) do {} while (0)
#endif

#ifdef __cplusplus
}
#endif

#endif
