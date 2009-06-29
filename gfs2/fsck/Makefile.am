MAINTAINERCLEANFILES	= Makefile.in

# install into /sbin, not /usr/sbin
sbindir := $(shell test '$(exec_prefix):$(sbindir)' = /usr:/usr/sbin \
	     && echo /sbin \
	     || echo '$(exec_prefix)/sbin')

sbin_PROGRAMS		= fsck.gfs2

noinst_HEADERS		= eattr.h fs_bits.h fsck.h fs_recovery.h hash.h \
			  inode_hash.h link.h lost_n_found.h metawalk.h util.h

fsck_gfs2_SOURCES	= eattr.c fs_recovery.c hash.c initialize.c \
			  inode_hash.c link.c lost_n_found.c main.c metawalk.c \
			  pass1b.c pass1.c pass1c.c pass2.c pass3.c pass4.c \
			  pass5.c rgrepair.c util.c

fsck_gfs2_CPPFLAGS	= -D_FILE_OFFSET_BITS=64 -DHELPER_PROGRAM \
			  -I$(top_srcdir)/gfs2/include \
			  -I$(top_srcdir)/gfs2/libgfs2

fsck_gfs2_LDADD		= $(top_builddir)/gfs2/libgfs2/libgfs2.la
