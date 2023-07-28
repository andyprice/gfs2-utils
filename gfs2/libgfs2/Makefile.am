MAINTAINERCLEANFILES = Makefile.in

CLEANFILES = \
	parser.h \
	parser.c \
	lexer.c \
	lexer.h

BUILT_SOURCES = \
	parser.h \
	lexer.h

AM_LFLAGS = --header-file=lexer.h
AM_YFLAGS = -d
AM_CPPFLAGS += \
	-D_GNU_SOURCE \
	$(uuid_CFLAGS)

noinst_HEADERS = \
	libgfs2.h \
	crc32c.h \
	lang.h \
	rgrp.h

noinst_LTLIBRARIES = libgfs2.la

noinst_PROGRAMS = gfs2l

libgfs2_la_SOURCES = \
	crc32c.c \
	fs_bits.c \
	misc.c \
	rgrp.c \
	super.c \
	buf.c \
	gfs2_disk_hash.c \
	ondisk.c \
	device_geometry.c \
	fs_ops.c \
	recovery.c \
	structures.c \
	meta.c

gfs2l_SOURCES = \
	gfs2l.c \
	lang.c \
	parser.y \
	lexer.l
gfs2l_LDADD = \
	libgfs2.la \
	$(uuid_LIBS)

# Autotools can't handle header files output by flex so we have to generate it manually
lexer.h: lexer.l
	$(LEX) -o lexer.c $(AM_LFLAGS) $^

if HAVE_CHECK
include checks.am
endif
