dnl libmpcdec

AC_ARG_WITH(musepack, AS_HELP_STRING([--without-musepack],
                                     [Compile without musepack (mpc) support]))

if test "x$with_musepack" != "xno"
then
	dnl taken from gstreamer
	AC_CHECK_HEADER([mpc/mpcdec.h],
		[have_musepack="yes"],
		[AC_CHECK_HEADER([mpcdec/mpcdec.h],
			[have_musepack="yes"
			 UPGRADE_MUSEPACK="yes"
			 AC_DEFINE(MPC_IS_OLD_API, 1, [Define if the old MusePack API is used])],
			[have_musepack="no"])])

	if test "x$have_musepack" = "xyes"
	then

		MUSEPACK_LIBS="-lmpcdec"
		AC_SUBST([MUSEPACK_LIBS])

		dnl taglib
		AC_CHECK_PROG([TAGLIB_CONFIG], [taglib-config], [yes])
		if test "x$TAGLIB_CONFIG" = "xyes"
		then
			AC_MSG_CHECKING([taglib version])
			taglib_ver=`taglib-config --version`
			AX_COMPARE_VERSION($taglib_ver, [ge], [1.3.1])
			if test "x$ax_compare_version" = "xtrue"
			then
				AC_MSG_RESULT([$taglib_ver, OK])

				TAGLIB_CFLAGS="`taglib-config --cflags`"
				dnl TAGLIB_LIBS="`taglib-config --libs`"
				TAGLIB_LIBS="-ltag_c"
				AC_SUBST([TAGLIB_CFLAGS])
				AC_SUBST([TAGLIB_LIBS])

				dnl check for tag_c.h
				old_cflags="$CFLAGS"
				old_cppflags="$CPPFLAGS"
				CFLAGS="$CFLAGS $TAGLIB_CFLAGS"
				CPPFLAGS="$CPPFLAGS $TAGLIB_CFLAGS"
				AC_CHECK_HEADER([tag_c.h], [
						 want_musepack="yes"
						 DECODER_PLUGINS="$DECODER_PLUGINS musepack"
						 ])
				CFLAGS="$old_cflags"
				CPPFLAGS="$old_cppflags"

				AX_COMPARE_VERSION($taglib_ver, [lt], [1.5])
				if test "x$ax_compare_version" = "xtrue"
				then
					UPGRADE_TAGLIB="yes"
				fi
			else
				AC_MSG_RESULT([$taglib_ver, but minimum is 1.3.1 - required for musepack])
			fi
		fi
	fi
fi

AM_CONDITIONAL([BUILD_musepack], [test "$want_musepack"])
AC_CONFIG_FILES([decoder_plugins/musepack/Makefile])
