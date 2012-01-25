dnl libsidplay2

AC_ARG_WITH(sidplay2, AS_HELP_STRING([--without-sidplay2],
                                     [Compile without libsidplay2]))

if test "x$with_sidplay2" != "xno"
then
	PKG_CHECK_MODULES(sidplay2, libsidplay2 >= 2.1.1,
			   [sidplay2_OK="yes"],
			   [true])

	PKG_CHECK_MODULES(sidutils, libsidutils >= 1.0.4,
			   [sidutils_OK="yes"],
			   [true])
dnl This is a rather ugly hack to find the builder
dnl as libsidplay2 works fine without it but the
dnl decoder uses it...
	if test "x$sidplay2_OK" = "xyes"; then
		if test "x$sidutils_OK" = "xyes"; then
			s2lib=`$PKG_CONFIG --variable=libdir libsidplay2 2>/dev/null`
			resid_OK="no"
			AC_CHECK_FILE([$s2lib/libresid-builder.la],
			              [resid_lib="$s2lib/libresid-builder.la"
			               resid_OK="yes"],
			              [resid_lib="$s2lib/sidplay/builders/libresid-builder.la"
			               AC_CHECK_FILE($resid_lib, [resid_OK="yes"],)])
			if test "x$resid_OK" = "xyes"; then
				sidplay2_LDFLAGS="$resid_lib"
				AC_SUBST(sidplay2_LDFLAGS)
				AC_SUBST(sidplay2_LIBS)
				AC_SUBST(sidplay2_CFLAGS)
				AC_SUBST(sidutils_LIBS)
				AC_SUBST(sidutils_CFLAGS)
				want_sidplay2="yes"
				DECODER_PLUGINS="$DECODER_PLUGINS sidplay2"
			fi
		fi
	fi
fi

AM_CONDITIONAL([BUILD_sidplay2], [test "$want_sidplay2"])
AC_CONFIG_FILES([decoder_plugins/sidplay2/Makefile])
