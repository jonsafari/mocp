#!/bin/sh

set -x

if test -f Makefile; then
	make distclean >/dev/null 2>/dev/null
fi

libtoolize -c -f
autopoint -f
aclocal -I m4 && \
autoheader && \
automake -W all -a && \
autoconf -W syntax

if [ "$?" != 0 ]
then
	cat <<EOF
If you see errors it maight be necassary to install additional packages like
autoconf >= 2.60
automake >= 1.9
libltdl (libltdl3-dev debian package)
and all -devel packages mentioned in the README file
EOF
fi
