EXTRA_DIST		= autogen.sh

AUTOMAKE_OPTIONS	= foreign

MAINTAINERCLEANFILES	= Makefile.in aclocal.m4 configure depcomp \
			  config.guess config.sub missing install-sh \
			  autoheader automake autoconf libtool libtoolize \
			  ltmain.sh compile make/clusterautoconfig.h.in \
			  make/clusterautoconfig.h.in~

noinst_HEADERS		= make/copyright.cf

ACLOCAL_AMFLAGS		= -I m4

SUBDIRS			= group gfs2 doc

install-exec-local:
			$(INSTALL) -d $(DESTDIR)/$(LOGDIR)
			$(INSTALL) -d $(DESTDIR)/$(CLUSTERVARRUN)
			$(INSTALL) -d $(DESTDIR)/$(CLUSTERVARLIB)

uninstall-local:
			rmdir $(DESTDIR)/$(LOGDIR) || :;
			rmdir $(DESTDIR)/$(CLUSTERVARRUN) || :;
			rmdir $(DESTDIR)/$(CLUSTERVARLIB) || :;

maintainer-clean-local:
	rm -rf m4