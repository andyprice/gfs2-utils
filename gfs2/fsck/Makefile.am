MAINTAINERCLEANFILES	= Makefile.in

# When an exec_prefix setting would have us install into /usr/sbin,
# use /sbin instead.
# Accept an existing sbindir value of /usr/sbin (probably for older automake),
# or an empty value, for automake-1.11 and newer.
sbindir := $(shell rpl=0; test '$(exec_prefix):$(sbindir)' = /usr:/usr/sbin \
		       || test '$(exec_prefix):$(sbindir)' = /usr: && rpl=1; \
	     test $$rpl = 1 && echo /sbin || echo '$(exec_prefix)/sbin')

sbin_PROGRAMS		= fsck.gfs2

noinst_HEADERS		= eattr.h fs_bits.h fsck.h fs_recovery.h \
			  inode_hash.h link.h lost_n_found.h metawalk.h util.h

fsck_gfs2_SOURCES	= eattr.c fs_recovery.c initialize.c \
			  inode_hash.c link.c lost_n_found.c main.c metawalk.c \
			  pass1b.c pass1.c pass1c.c pass2.c pass3.c pass4.c \
			  pass5.c rgrepair.c util.c

fsck_gfs2_CPPFLAGS	= -D_FILE_OFFSET_BITS=64 -DHELPER_PROGRAM \
			  -I$(top_srcdir)/gfs2/include \
			  -I$(top_srcdir)/gfs2/libgfs2

fsck_gfs2_LDADD		= $(top_builddir)/gfs2/libgfs2/libgfs2.la
