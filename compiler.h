/*
 * The purpose of this header is to configure the compiler and system
 * headers the way we want them to be.  It should be included in every
 * source code file *before* any system headers are included, and thus
 * an include for it is automatically appended to the 'config.h' header
 * by 'configure'.
 *
 * It is also included by 'configure' tests to ensure that any system
 * headers *they* include will be configured consistantly and symbols
 * they declare will not be exposed differently in the tests and the
 * code thus causing the configuration macros defined in 'config.h'
 * to be mismatched with the included system headers.
 *
 * Because it is used in both places, it should not include any code
 * which is relevant only to MOC code.
 */

#ifndef COMPILER_H
#define COMPILER_H

/* _XOPEN_SOURCE is known to break compilation on OpenBSD. */
#ifndef OPENBSD
# if defined(_XOPEN_SOURCE) && _XOPEN_SOURCE < 600
#  undef _XOPEN_SOURCE
# endif
# ifndef _XOPEN_SOURCE
#  define _XOPEN_SOURCE 600
# endif
#endif

/* _XOPEN_SOURCE_EXTENDED is known to break compilation on FreeBSD. */
#ifndef FREEBSD
# define _XOPEN_SOURCE_EXTENDED 1
#endif

/* Require POSIX.1-2001 or better. */
#if defined(_POSIX_C_SOURCE) && _POSIX_C_SOURCE < 200112L
# undef _POSIX_C_SOURCE
#endif
#ifndef _POSIX_C_SOURCE
# define _POSIX_C_SOURCE 200112L
#endif

#endif
