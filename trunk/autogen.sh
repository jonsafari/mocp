#!/bin/sh

set -x

if test -f Makefile; then
	make distclean >/dev/null 2>/dev/null
fi

aclocal -I m4 && \
autoheader && \
automake -W all -a && \
autoconf -W syntax

