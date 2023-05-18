MAINTAINERCLEANFILES = Makefile.in

sbin_PROGRAMS = gfs2_edit

noinst_HEADERS = \
	gfs2hex.h \
	hexedit.h \
	extended.h \
	struct_print.h \
	journal.h

gfs2_edit_SOURCES = \
	gfs2hex.c \
	hexedit.c \
	savemeta.c \
	extended.c \
	struct_print.c \
	journal.c

gfs2_edit_CPPFLAGS = $(AM_CPPFLAGS)
gfs2_edit_CFLAGS = \
	$(AM_CFLAGS) \
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
