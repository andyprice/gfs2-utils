MAINTAINERCLEANFILES	= Makefile.in

# install into /sbin, not /usr/sbin
sbindir := $(shell test '$(exec_prefix):$(sbindir)' = /usr:/usr/sbin \
	     && echo /sbin \
	     || echo '$(exec_prefix)/sbin')

sbin_PROGRAMS		= mount.gfs2

noinst_HEADERS		= gfs_ondisk.h util.h

mount_gfs2_SOURCES	= mount.gfs2.c mtab.c ondisk1.c util.c

mount_gfs2_CPPFLAGS	= -D_FILE_OFFSET_BITS=64 -DHELPER_PROGRAM \
			  -D_GNU_SOURCE \
			  -I$(top_srcdir)/gfs2/include \
			  -I$(top_srcdir)/group/libgfscontrol

mount_gfs2_LDADD	= $(top_builddir)/group/libgfscontrol/libgfscontrol.la
