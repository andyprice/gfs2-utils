MAINTAINERCLEANFILES = Makefile.in

sbin_PROGRAMS = fsck.gfs2

noinst_HEADERS = \
	afterpass1_common.h \
	fsck.h \
	fs_recovery.h \
	inode_hash.h \
	link.h \
	lost_n_found.h \
	metawalk.h \
	util.h

fsck_gfs2_SOURCES = \
	block_list.c \
	fs_recovery.c \
	initialize.c \
	inode_hash.c \
	link.c \
	lost_n_found.c \
	main.c \
	metawalk.c \
	afterpass1_common.c \
	pass1b.c \
	pass1.c \
	pass2.c \
	pass3.c \
	pass4.c \
	pass5.c \
	rgrepair.c \
	util.c

fsck_gfs2_CPPFLAGS = $(AM_CPPFLAGS)
fsck_gfs2_CFLAGS = $(AM_CFLAGS)
fsck_gfs2_LDADD = \
	$(top_builddir)/gfs2/libgfs2/libgfs2.la \
	$(LTLIBINTL) \
	$(uuid_LIBS)

if HAVE_CHECK
include checks.am
endif
