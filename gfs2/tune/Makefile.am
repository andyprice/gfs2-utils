MAINTAINERCLEANFILES = Makefile.in

sbin_PROGRAMS = tunegfs2

noinst_HEADERS = tunegfs2.h

tunegfs2_SOURCES = \
	main.c \
	super.c
tunegfs2_CPPFLAGS = \
	-D_FILE_OFFSET_BITS=64 \
	-I$(top_srcdir)/gfs2/include \
	-I$(top_srcdir)/gfs2/libgfs2
tunegfs2_CFLAGS = \
	$(uuid_CFLAGS)
tunegfs2_LDADD = \
	$(top_builddir)/gfs2/libgfs2/libgfs2.la \
	$(LTLIBINTL) \
	$(uuid_LIBS)

if HAVE_CHECK
include checks.am
endif
