MAINTAINERCLEANFILES = Makefile.in

sbin_PROGRAMS = \
	glocktop

glocktop_SOURCES = \
	glocktop.c

glocktop_CFLAGS = \
	$(AM_CFLAGS) \
	$(ncurses_CFLAGS)

glocktop_CPPFLAGS = \
	$(AM_CPPFLAGS) \
	-D_GNU_SOURCE

glocktop_LDADD = \
	$(top_builddir)/gfs2/libgfs2/libgfs2.la \
	$(ncurses_LIBS) \
	$(uuid_LIBS)
