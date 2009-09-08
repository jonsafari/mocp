#ifndef COMMON_H
#define COMMON_H

#include <stdlib.h>
#include <stdarg.h>
#include <ctype.h>

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

#define MIN(a, b) ((a) < (b) ? (a) : (b))

#ifdef HAVE__ATTRIBUTE__
# define ATTR_UNUSED __attribute__((unused))
#else
# define ATTR_UNUSED
#endif

/* isblank() is a GNU extension */
#ifndef isblank
#define isblank(c) ((c) == ' ' || (c) == '\t')
#endif

#ifdef ENABLE_NLS
# include "gettext.h"
# define _(x) gettext(x)
#else
# define _(x) x
#endif

#define ARRAY_SIZE(x)	(sizeof(x)/sizeof(x[0]))

void *xmalloc (const size_t size);
void *xcalloc (size_t nmemb, size_t size);
void *xrealloc (void *ptr, const size_t size);
char *xstrdup (const char *s);

char *str_repl (char *target, const char *oldstr, const char *newstr);

#ifdef HAVE__ATTRIBUTE__
void fatal (const char *format, ...) __attribute__((format (printf, 1, 2)));
void error (const char *format, ...) __attribute__((format (printf, 1, 2)));
#else
void fatal (const char *format, ...);
void error (const char *format, ...);
#endif

void set_me_server ();
char *create_file_name (const char *file);

void sec_to_min (char *buff, const int seconds);

#endif
