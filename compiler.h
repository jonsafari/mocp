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

/* For now, these GCC_* macros need to remain in compiler.h to avoid FFmpeg
 * header deprecation warnings, but these will be resolved with the FFmpeg
 * 1.0 requirement and these macros moved to their proper location. */

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

#endif
