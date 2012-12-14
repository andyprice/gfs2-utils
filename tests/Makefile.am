TESTS_ENVIRONMENT	= TOPBUILDDIR=$(top_builddir)
TESTS			= check_libgfs2 tool_tests.sh
EXTRA_DIST		= tool_tests.sh
check_PROGRAMS		= check_libgfs2
check_libgfs2_SOURCES	= check_meta.c \
			  $(top_srcdir)/gfs2/libgfs2/libgfs2.h
check_libgfs2_CFLAGS	= -I$(top_srcdir)/gfs2/libgfs2 \
			  -I$(top_srcdir)/gfs2/include \
			  @check_CFLAGS@
check_libgfs2_LDADD	= $(top_builddir)/gfs2/libgfs2/libgfs2.la @check_LIBS@
