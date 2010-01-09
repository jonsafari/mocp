#ifndef COMPAT_H
#define COMPAT_H

#ifdef HAVE_STDINT_H
# include <stdint.h>
#endif
#ifdef HAVE_LIMITS_H
# include <limits.h>
#endif
#ifdef HAVE_INTTYPES_H
# include <inttypes.h>
#endif

#ifndef INT32_MAX
# define INT32_MAX	(2147483647)
#endif

#ifndef INT32_MIN
# define INT32_MIN	(-2147483647-1)
#endif

#ifndef INT16_MAX
# define INT16_MAX	(32767)
#endif

#ifndef INT16_MIN
# define INT16_MIN	(-32767-1)
#endif

#ifndef INT8_MAX
# define INT8_MAX	(127)
#endif

#ifndef INT8_MIN
# define INT8_MIN	(-128)
#endif

#ifdef __cplusplus
extern "C" {
#endif

#ifndef HAVE_STRERROR_R
int strerror_r(int errnum, char *buf, size_t n);
#endif

void compat_cleanup ();

#ifdef __cplusplus
}
#endif

#endif
