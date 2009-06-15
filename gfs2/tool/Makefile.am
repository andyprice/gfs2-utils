MAINTAINERCLEANFILES	= Makefile.in

sbin_PROGRAMS		= gfs2_tool

noinst_HEADERS		= gfs2_tool.h iflags.h

gfs2_tool_SOURCES	= main.c misc.c ondisk.c sb.c tune.c

gfs2_tool_CPPFLAGS	= -D_FILE_OFFSET_BITS=64 \
			  -I$(top_srcdir)/gfs2/include \
			  -I$(top_srcdir)/gfs2/libgfs2

gfs2_tool_LDADD		= $(top_builddir)/gfs2/libgfs2/libgfs2.la
