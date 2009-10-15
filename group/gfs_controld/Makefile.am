MAINTAINERCLEANFILES	= Makefile.in

sbin_PROGRAMS		= gfs_controld

noinst_HEADERS		= config.h gfs_controld.h gfs_daemon.h

gfs_controld_SOURCES	= config.c \
			  cpg-new.c \
			  crc.c \
			  logging.c \
			  main.c \
			  member_cman.c \
			  util.c

gfs_controld_CPPFLAGS	= -I$(top_srcdir)/group/libgfscontrol \
			  -I$(top_srcdir)/group/include

gfs_controld_CFLAGS	= $(ccs_CFLAGS) $(logt_CFLAGS) $(dlmcontrol_CFLAGS) \
			  $(fenced_CFLAGS) $(sackpt_CFLAGS) $(cpg_CFLAGS) \
			  $(cfg_CFLAGS) $(quorum_CFLAGS)

gfs_controld_LDFLAGS	= $(ccs_LIBS) $(logt_LIBS) $(dlmcontrol_LIBS) \
			  $(cpg_LIBS) $(cfg_LIBS) $(sackpt_LIBS) \
			  $(fenced_LIBS) $(quorum_LIBS) -lpthread
