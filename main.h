#ifndef MAIN_H
#define MAIN_H

#include <stdlib.h>
#include <stdarg.h>

#define CONFIG_DIR      ".moc"

/* Exit status on fatal exit. */
#define EXIT_FATAL	2

/* Maximal string length sent/received. */
#define MAX_SEND_STRING	4096

/* Maximum path length, we don't consider exceptions like mounted NFS */
#ifndef PATH_MAX
# if defined(_POSIX_PATH_MAX)
#  define MAX_PATH	_POSIX_PATH_MAX /* Posix */
# elif defined(MAXPATHLEN)
#  define MAX_PATH	MAXPATHLEN      /* Solaris? Also linux...*/
# else
#  define PATH_MAX	512             /* Suppose, we have 512 */
# endif
#endif

#define LOCK(mutex)	pthread_mutex_lock (&mutex)
#define UNLOCK(mutex)	pthread_mutex_unlock (&mutex)

void *xmalloc (const size_t size);
void *xrealloc (void *ptr, const size_t size);
char *xstrdup (const char *s);
void fatal (const char *format, ...);
char *create_file_name (const char *file);

#endif
