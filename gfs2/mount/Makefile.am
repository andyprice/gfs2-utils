MAINTAINERCLEANFILES	= Makefile.in

# When an exec_prefix setting would have us install into /usr/sbin,
# use /sbin instead.
# Accept an existing sbindir value of /usr/sbin (probably for older automake),
# or an empty value, for automake-1.11 and newer.
sbindir := $(shell rpl=0; test '$(exec_prefix):$(sbindir)' = /usr:/usr/sbin \
		       || test '$(exec_prefix):$(sbindir)' = /usr: && rpl=1; \
	     test $$rpl = 1 && echo /sbin || echo '$(exec_prefix)/sbin')


sbin_PROGRAMS		= mount.gfs2

noinst_HEADERS		= gfs_ondisk.h util.h

mount_gfs2_SOURCES	= mount.gfs2.c mtab.c ondisk1.c util.c

mount_gfs2_CPPFLAGS	= -D_FILE_OFFSET_BITS=64 -DHELPER_PROGRAM \
			  -D_GNU_SOURCE \
			  -I$(top_srcdir)/gfs2/include \
			  -I$(top_srcdir)/group/libgfscontrol

mount_gfs2_LDADD	= $(top_builddir)/group/libgfscontrol/libgfscontrol.la
