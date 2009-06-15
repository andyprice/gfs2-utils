MAINTAINERCLEANFILES	= Makefile.in

noinst_PROGRAMS		= mkfs.gfs2

noinst_HEADERS		= gfs2_mkfs.h

mkfs_gfs2_SOURCES	= main.c main_grow.c main_jadd.c main_mkfs.c

mkfs_gfs2_CPPFLAGS	= -D_FILE_OFFSET_BITS=64 -D_LARGEFILE64_SOURCE \
			  -D_GNU_SOURCE \
			  -I$(top_srcdir)/gfs2/include \
			  -I$(top_srcdir)/gfs2/libgfs2

mkfs_gfs2_LDADD		= $(top_builddir)/gfs2/libgfs2/libgfs2.la

relative_sbin		= $(shell perl -MFile::Spec -le 'print File::Spec->abs2rel("/sbin", "$(sbindir)")')

install-exec-local:
	$(INSTALL) -d $(DESTDIR)/sbin
	$(INSTALL) -m 755 mkfs.gfs2 $(DESTDIR)/sbin

uninstall-local:
	cd $(DESTDIR)/sbin && \
		rm -f mkfs.gfs2

install-exec-hook:
	$(INSTALL) -d $(DESTDIR)/$(sbindir)
	(cd $(DESTDIR)/$(sbindir) && \
		rm -f gfs2_grow gfs2_jadd && \
		$(LN_S) $(relative_sbin)/mkfs.gfs2 gfs2_grow && \
		$(LN_S) $(relative_sbin)/mkfs.gfs2 gfs2_jadd)

uninstall-hook:
	(cd $(DESTDIR)/$(sbindir) && rm -f gfs2_jadd gfs2_grow)
