/*
 * The purpose of this header is to provide common functions and macros
 * used throughout MOC code.  It also provides (x-prefixed) functions
 * which augment or adapt their respective system functions with error
 * checking and the like.
 */

#ifndef COMMON_H
#define COMMON_H

#include <stdlib.h>
#include <stdarg.h>
#include <stdbool.h>
#include <limits.h>

#include "compat.h"

#ifdef HAVE_FUNC_ATTRIBUTE_FORMAT
# define ATTR_PRINTF(x,y) __attribute__ ((format (printf, x, y)))
#else
# define ATTR_PRINTF(...)
#endif

#ifdef HAVE_FUNC_ATTRIBUTE_NORETURN
# define ATTR_NORETURN __attribute__((noreturn))
#else
# define ATTR_NORETURN
#endif

#ifdef HAVE_VAR_ATTRIBUTE_UNUSED
# define ATTR_UNUSED __attribute__((unused))
#else
# define ATTR_UNUSED
#endif

#ifndef GCC_VERSION
#define GCC_VERSION (__GNUC__ * 10000 + \
                     __GNUC_MINOR__ * 100 + \
                     __GNUC_PATCHLEVEL__)
#endif

/* These macros allow us to use the appropriate method for manipulating
 * GCC's diagnostic pragmas depending on the compiler's version. */
#if GCC_VERSION >= 40200
# define GCC_DIAG_STR(s) #s
# define GCC_DIAG_JOINSTR(x,y) GCC_DIAG_STR(x ## y)
# define GCC_DIAG_DO_PRAGMA(x) _Pragma (#x)
# define GCC_DIAG_PRAGMA(x) GCC_DIAG_DO_PRAGMA(GCC diagnostic x)
# if GCC_VERSION >= 40600
#  define GCC_DIAG_OFF(x) GCC_DIAG_PRAGMA(push) \
                          GCC_DIAG_PRAGMA(ignored GCC_DIAG_JOINSTR(-W,x))
#  define GCC_DIAG_ON(x)  GCC_DIAG_PRAGMA(pop)
# else
#  define GCC_DIAG_OFF(x) GCC_DIAG_PRAGMA(ignored GCC_DIAG_JOINSTR(-W,x))
#  define GCC_DIAG_ON(x)  GCC_DIAG_PRAGMA(warning GCC_DIAG_JOINSTR(-W,x))
# endif
#else
# define GCC_DIAG_OFF(x)
# define GCC_DIAG_ON(x)
#endif

#define CONFIG_DIR      ".moc"
#define LOCK(mutex)     pthread_mutex_lock (&mutex)
#define UNLOCK(mutex)   pthread_mutex_unlock (&mutex)
#define ARRAY_SIZE(x)   (sizeof(x)/sizeof(x[0]))
#define ssizeof(x)      ((ssize_t) sizeof(x))

/* Maximal string length sent/received. */
#define MAX_SEND_STRING	4096

/* Exit status on fatal error. */
#define EXIT_FATAL	2

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

#ifdef NDEBUG
#define error(...) \
	internal_error (NULL, 0, NULL, ## __VA_ARGS__)
#define fatal(...) \
	internal_fatal (NULL, 0, NULL, ## __VA_ARGS__)
#define ASSERT_ONLY ATTR_UNUSED
#else
#define error(...) \
	internal_error (__FILE__, __LINE__, __func__, ## __VA_ARGS__)
#define fatal(...) \
	internal_fatal (__FILE__, __LINE__, __func__, ## __VA_ARGS__)
#define ASSERT_ONLY
#endif

#ifndef STRERROR_FN
# define STRERROR_FN xstrerror
#endif

#define error_errno(format, errnum) \
	do { \
		char *err##__LINE__ = STRERROR_FN (errnum); \
		error (format ": %s", err##__LINE__); \
		free (err##__LINE__); \
	} while (0)

#ifdef __cplusplus
extern "C" {
#endif

void *xmalloc (size_t size);
void *xcalloc (size_t nmemb, size_t size);
void *xrealloc (void *ptr, const size_t size);
char *xstrdup (const char *s);
void xsleep (size_t ticks, size_t ticks_per_sec);
char *xstrerror (int errnum);
void xsignal (int signum, void (*func)(int));

void internal_error (const char *file, int line, const char *function,
                     const char *format, ...) ATTR_PRINTF(4, 5);
void internal_fatal (const char *file, int line, const char *function,
                     const char *format, ...) ATTR_NORETURN ATTR_PRINTF(4, 5);
void set_me_server ();
char *str_repl (char *target, const char *oldstr, const char *newstr);
char *trim (const char *src, size_t len);
char *format_msg (const char *format, ...);
char *format_msg_va (const char *format, va_list va);
bool is_valid_symbol (const char *candidate);
char *create_file_name (const char *file);
void sec_to_min (char *buff, const int seconds);
const char *get_home ();
void common_cleanup ();

#ifdef __cplusplus
}
#endif

#endif
