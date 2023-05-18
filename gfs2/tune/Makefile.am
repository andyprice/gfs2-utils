MAINTAINERCLEANFILES = Makefile.in

sbin_PROGRAMS = tunegfs2

noinst_HEADERS = tunegfs2.h

tunegfs2_SOURCES = \
	main.c \
	super.c
tunegfs2_CPPFLAGS = $(AM_CPPFLAGS)
tunegfs2_CFLAGS = \
	$(AM_CFLAGS) \
	$(uuid_CFLAGS)
tunegfs2_LDADD = \
	$(top_builddir)/gfs2/libgfs2/libgfs2.la \
	$(LTLIBINTL) \
	$(uuid_LIBS)

if HAVE_CHECK
include checks.am
endif
