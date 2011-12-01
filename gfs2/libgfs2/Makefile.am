MAINTAINERCLEANFILES	= Makefile.in

noinst_HEADERS		= libgfs2.h

noinst_LTLIBRARIES	= libgfs2.la

libgfs2_la_SOURCES	= block_list.c fs_bits.c gfs1.c misc.c rgrp.c super.c \
			  buf.c fs_geometry.c gfs2_disk_hash.c ondisk.c \
			  device_geometry.c fs_ops.c gfs2_log.c recovery.c \
			  structures.c

libgfs2_la_CPPFLAGS	= -D_FILE_OFFSET_BITS=64 \
			  -D_LARGEFILE64_SOURCE \
			  -D_GNU_SOURCE \
			  -I$(top_srcdir)/gfs2/include
