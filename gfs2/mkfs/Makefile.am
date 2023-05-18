MAINTAINERCLEANFILES = Makefile.in

COMMON_CPPFLAGS = \
	$(AM_CPPFLAGS) \
	-D_GNU_SOURCE

sbin_PROGRAMS = \
	mkfs.gfs2 \
	gfs2_jadd \
	gfs2_grow

noinst_HEADERS = \
	gfs2_mkfs.h \
	struct_print.h \
	metafs.h

mkfs_gfs2_SOURCES = \
	main_mkfs.c \
	struct_print.c \
	progress.c \
	progress.h

mkfs_gfs2_CPPFLAGS = $(COMMON_CPPFLAGS)
mkfs_gfs2_CFLAGS = \
	$(AM_CFLAGS) \
	$(blkid_CFLAGS) \
	$(uuid_CFLAGS)
mkfs_gfs2_LDADD	= \
	$(top_builddir)/gfs2/libgfs2/libgfs2.la \
	$(LTLIBINTL) \
	$(blkid_LIBS) \
	$(uuid_LIBS)

gfs2_grow_SOURCES = \
	main_grow.c \
	metafs.c

gfs2_grow_CPPFLAGS = $(COMMON_CPPFLAGS)
gfs2_grow_CFLAGS = \
	$(AM_CFLAGS) \
	$(blkid_CFLAGS)
gfs2_grow_LDADD = \
	$(top_builddir)/gfs2/libgfs2/libgfs2.la \
	$(LTLIBINTL) \
	$(blkid_LIBS) \
	$(uuid_LIBS)

gfs2_jadd_SOURCES = \
	main_jadd.c \
	metafs.c
gfs2_jadd_CFLAGS = $(AM_CFLAGS)
gfs2_jadd_CPPFLAGS = $(COMMON_CPPFLAGS)
gfs2_jadd_LDADD = \
	$(top_builddir)/gfs2/libgfs2/libgfs2.la \
	$(LTLIBINTL) \
	$(uuid_LIBS)

if HAVE_CHECK
include checks.am
endif
