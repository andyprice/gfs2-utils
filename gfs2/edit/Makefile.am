MAINTAINERCLEANFILES = Makefile.in

sbin_PROGRAMS = gfs2_edit

noinst_HEADERS = \
	gfs2hex.h \
	hexedit.h \
	extended.h \
	journal.h

gfs2_edit_SOURCES = \
	gfs2hex.c \
	hexedit.c \
	savemeta.c \
	extended.c \
	journal.c

gfs2_edit_CPPFLAGS = \
	-D_FILE_OFFSET_BITS=64 \
	-I$(top_srcdir)/gfs2/include \
	-I$(top_srcdir)/gfs2/libgfs2

gfs2_edit_CFLAGS = \
	$(ncurses_CFLAGS) \
	$(zlib_CFLAGS) \
	$(bzip2_CFLAGS) \
	$(uuid_CFLAGS)

gfs2_edit_LDADD = \
	$(top_builddir)/gfs2/libgfs2/libgfs2.la \
	$(ncurses_LIBS) \
	$(zlib_LIBS) \
	$(bzip2_LIBS) \
	$(uuid_LIBS)

if HAVE_CHECK
include checks.am
endif
