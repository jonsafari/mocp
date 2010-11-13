#!/bin/sh

# Try to find where GNU libtool is hiding.
LIBTOOL_AKA="glibtoolize"
if test -z "$LIBTOOLIZE"; then
	for LIBTOOL in $LIBTOOL_AKA libtoolize; do
		LIBTOOLIZE=`which $LIBTOOL 2>/dev/null`
		if test $? -eq 0; then
			if $LIBTOOLIZE --version | grep -q "GNU libtool"; then
				break
			fi
			LIBTOOLIZE=
		fi
	done
	if test -z "$LIBTOOLIZE"; then
		echo "No suitable libtoolize found (set LIBTOOLIZE if necessary)."
		exit 1
	fi
fi

set -x

if test -f Makefile; then
	make distclean >/dev/null 2>/dev/null
fi

$LIBTOOLIZE -c -f
aclocal -I m4 && \
autoheader && \
automake -W all -a && \
autoconf -W syntax

if test "$?" != 0; then
	cat <<EOF
If you see errors it might be necessary to install additional packages like
autoconf >= 2.60
automake >= 1.9
libltdl (libltdl3-dev debian package)
and all -devel packages mentioned in the README file
EOF
fi
