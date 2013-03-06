MAINTAINERCLEANFILES	= Makefile.in

# When an exec_prefix setting would have us install into /usr/sbin,
# use /sbin instead.
# Accept an existing sbindir value of /usr/sbin (probably for older automake),
# or an empty value, for automake-1.11 and newer.
sbindir := $(shell rpl=0; test '$(exec_prefix):$(sbindir)' = /usr:/usr/sbin \
		       || test '$(exec_prefix):$(sbindir)' = /usr: && rpl=1; \
	     test $$rpl = 1 && echo /sbin || echo '$(exec_prefix)/sbin')


sbin_PROGRAMS		= mkfs.gfs2

noinst_HEADERS		= gfs2_mkfs.h

mkfs_gfs2_SOURCES	= main.c main_grow.c main_jadd.c main_mkfs.c

mkfs_gfs2_CPPFLAGS	= -D_FILE_OFFSET_BITS=64 -D_LARGEFILE64_SOURCE \
			  -D_GNU_SOURCE \
			  -I$(top_srcdir)/gfs2/include \
			  -I$(top_srcdir)/gfs2/libgfs2

mkfs_gfs2_CFLAGS	= $(blkid_CFLAGS)
mkfs_gfs2_LDFLAGS	= $(blkid_LIBS)

mkfs_gfs2_LDADD		= $(top_builddir)/gfs2/libgfs2/libgfs2.la

relative_sbin		= $(shell perl -MFile::Spec -le 'print File::Spec->abs2rel("/sbin", "$(sbindir)")')

install-exec-hook:
	$(INSTALL) -d $(DESTDIR)/$(sbindir)
	cd $(DESTDIR)/$(sbindir) && \
		rm -f gfs2_grow gfs2_jadd && \
		$(LN_S) $(relative_sbin)/mkfs.gfs2 gfs2_grow && \
		$(LN_S) $(relative_sbin)/mkfs.gfs2 gfs2_jadd

uninstall-hook:
	cd $(DESTDIR)/$(sbindir) && rm -f gfs2_jadd gfs2_grow
