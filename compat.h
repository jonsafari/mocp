#ifndef COMPAT_H
#define COMPAT_H

#ifndef HAVE_STRERROR_R
int strerror_r(int errnum, char *buf, size_t n);
#endif

void compat_cleanup ();

#endif
