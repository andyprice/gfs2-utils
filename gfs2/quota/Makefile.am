MAINTAINERCLEANFILES	= Makefile.in

sbin_PROGRAMS		= gfs2_quota

noinst_HEADERS		= gfs2_quota.h

gfs2_quota_SOURCES	= check.c main.c names.c

gfs2_quota_CPPFLAGS	= -D_FILE_OFFSET_BITS=64 -D_GNU_SOURCE \
			  -DHELPER_PROGRAM \
			  -I$(top_srcdir)/gfs2/include \
			  -I$(top_srcdir)/gfs2/libgfs2

gfs2_quota_LDADD	= $(top_builddir)/gfs2/libgfs2/libgfs2.la
