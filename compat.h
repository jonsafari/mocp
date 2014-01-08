#ifndef COMPAT_H
#define COMPAT_H

#ifdef HAVE_BYTESWAP_H
# include <byteswap.h>
#else
/* Given an unsigned 16-bit argument X, return the value corresponding to
   X with reversed byte order.  */
# define bswap_16(x) ((((x) & 0x00FF) << 8) | \
                      (((x) & 0xFF00) >> 8))

/* Given an unsigned 32-bit argument X, return the value corresponding to
   X with reversed byte order.  */
# define bswap_32(x) ((((x) & 0x000000FF) << 24) | \
                      (((x) & 0x0000FF00) << 8) | \
                      (((x) & 0x00FF0000) >> 8) | \
                      (((x) & 0xFF000000) >> 24))
#endif

#ifdef __cplusplus
extern "C" {
#endif

#ifndef HAVE_STRCASESTR
char *strcasestr (const char *haystack, const char *needle);
#endif

#ifndef HAVE_STRERROR_R
int strerror_r (int errnum, char *buf, size_t n);
#endif

void compat_cleanup ();

#ifdef __cplusplus
}
#endif

#endif
