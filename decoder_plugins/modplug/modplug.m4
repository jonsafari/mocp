dnl libmodplug

AC_ARG_WITH(modplug, AS_HELP_STRING([--without-modplug],
                                    [Compile without libmodplug]))

if test "x$with_modplug" != "xno"
then
	PKG_CHECK_MODULES(modplug, libmodplug >= 0.7,
			   [AC_SUBST(modplug_LIBS)
			   AC_SUBST(modplug_CFLAGS)
			   want_modplug="yes"
			   DECODER_PLUGINS="$DECODER_PLUGINS modplug"],
			   [true])
fi

AM_CONDITIONAL([BUILD_modplug], [test "$want_modplug"])
AC_CONFIG_FILES([decoder_plugins/modplug/Makefile])
