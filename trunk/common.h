#ifndef COMMON_H
#define COMMON_H

#include <stdlib.h>
#include <stdarg.h>
#include <ctype.h>

#include "compiler.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CONFIG_DIR      ".moc"

/* Maximal string length sent/received. */
#define MAX_SEND_STRING	4096

/* Maximum path length, we don't consider exceptions like mounted NFS */
#ifndef PATH_MAX
# if defined(_POSIX_PATH_MAX)
#  define PATH_MAX	_POSIX_PATH_MAX /* Posix */
# elif defined(MAXPATHLEN)
#  define PATH_MAX	MAXPATHLEN      /* Solaris? Also linux...*/
# else
#  define PATH_MAX	4096             /* Suppose, we have 4096 */
# endif
#endif

/* Exit status on fatal exit. */
#define EXIT_FATAL	2

#define LOCK(mutex)	pthread_mutex_lock (&mutex)
#define UNLOCK(mutex)	pthread_mutex_unlock (&mutex)

#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif

#ifndef MAX
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#endif

#ifndef LIMIT
#define LIMIT(val, lim) ((val) >= 0 && (val) < (lim))
#endif

#ifndef RANGE
#define RANGE(min, val, max) ((val) >= (min) && (val) <= (max))
#endif

#ifndef CLAMP
#define CLAMP(min, val, max) ((val) < (min) ? (min) : \
                              (val) > (max) ? (max) : (val))
#endif

#if HAVE_STDBOOL_H
# include <stdbool.h>
#else
# if ! HAVE__BOOL
#  ifdef __cplusplus
typedef bool _Bool;
#  else
typedef unsigned char _Bool;
#  endif
# endif
# define bool _Bool
# define false 0
# define true 1
# define __bool_true_false_are_defined 1
#endif

/* isblank() is a GNU extension */
#ifndef isblank
#define isblank(c) ((c) == ' ' || (c) == '\t')
#endif

#define ARRAY_SIZE(x)	(sizeof(x)/sizeof(x[0]))

void *xmalloc (size_t size);
void *xcalloc (size_t nmemb, size_t size);
void *xrealloc (void *ptr, const size_t size);
char *xstrdup (const char *s);

#ifdef NEED_XSLEEP
void xsleep (size_t ticks, size_t ticks_per_sec);
#endif

#ifdef NDEBUG
#define fatal(format, ...) \
	internal_fatal (NULL, 0, NULL, format, ## __VA_ARGS__)
#else
#define fatal(format, ...) \
	internal_fatal (__FILE__, __LINE__, __FUNCTION__, format, \
	## __VA_ARGS__)
#endif

#ifdef HAVE_FUNC_ATTRIBUTE_FORMAT
void internal_fatal (const char *file, int line, const char *function,
                     const char *format, ...) ATTR_NORETURN
                     __attribute__ ((format (printf, 4, 5)));
void error (const char *format, ...) __attribute__((format (printf, 1, 2)));
#else
void internal_fatal (const char *file, int line, const char *function,
                     const char *format, ...) ATTR_NORETURN;
void error (const char *format, ...);
#endif

void set_me_server ();
char *str_repl (char *target, const char *oldstr, const char *newstr);
char *trim (const char *src, size_t len);
char *format_msg (const char *format, ...);
char *format_msg_va (const char *format, va_list va);
bool is_valid_symbol (const char *candidate);
char *create_file_name (const char *file);
void sec_to_min (char *buff, const int seconds);
const char *get_home ();

#ifdef __cplusplus
}
#endif

#endif
