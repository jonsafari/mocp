#ifndef COMPILER_H
#define COMPILER_H

#ifdef __cplusplus
extern "C" {
#endif

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

#ifdef HAVE_VAR_ATTRIBUTE_ALIGNED
# define ATTR_ALIGNED(x) __attribute__((aligned(x)))
#else
# define ATTR_ALIGNED(...)
#endif

#ifdef HAVE_VAR_ATTRIBUTE_UNUSED
# define ATTR_UNUSED __attribute__((unused))
#else
# define ATTR_UNUSED
#endif

/* __FUNCTION__ is a gcc extension */
#ifndef HAVE__FUNCTION__
# define __FUNCTION__ "UNKNOWN_FUNC"
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

#ifdef __cplusplus
}
#endif

#endif
