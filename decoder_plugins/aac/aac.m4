dnl libfaad2 (aac)

AC_ARG_WITH(aac, AS_HELP_STRING([--without-aac],
                                [Compile without AAC support (libfaad2)]))

if test "x$with_aac" != "xno"
then
	faad2_OK="no"
	AC_CHECK_LIB(faad, NeAACDecInit, [faad2_OK="yes"])

	if test "x$faad2_OK" = "xyes"; then
		AC_CHECK_HEADER([neaacdec.h], ,
			AC_MSG_ERROR([You need a more recent libfaad2 (libfaad2 devel package).]))
	fi

	if test "x$faad2_OK" = "xyes" -a "$HAVE_ID3TAG" = "yes"
	then
		FAAD2_LIBS="-lfaad"
		AC_SUBST([FAAD2_CFLAGS])
		AC_SUBST([FAAD2_LIBS])
		want_aac="yes"
		DECODER_PLUGINS="$DECODER_PLUGINS aac"
	fi
fi

AM_CONDITIONAL([BUILD_aac], [test "$want_aac"])
AC_CONFIG_FILES([decoder_plugins/aac/Makefile])
