MAINTAINERCLEANFILES = Makefile.in

sbin_PROGRAMS = gfs2_convert

gfs2_convert_CPPFLAGS = $(AM_CPPFLAGS)
gfs2_convert_CFLAGS = $(AM_CFLAGS)
gfs2_convert_LDADD = \
	$(top_builddir)/gfs2/libgfs2/libgfs2.la \
	$(LTLIBINTL) \
	$(uuid_LIBS)

if HAVE_CHECK
include checks.am
endif
