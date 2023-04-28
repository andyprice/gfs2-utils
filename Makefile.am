EXTRA_DIST = autogen.sh README.md

AUTOMAKE_OPTIONS = foreign

MAINTAINERCLEANFILES = \
	Makefile.in \
	aclocal.m4 \
	configure \
	depcomp \
	config.guess \
	config.sub \
	missing \
	install-sh \
	autoheader \
	automake \
	autoconf \
	libtool \
	libtoolize \
	ltmain.sh \
	compile \
	make/clusterautoconfig.h.in \
	make/clusterautoconfig.h.in~

noinst_HEADERS = make/copyright.cf

ACLOCAL_AMFLAGS = -I m4

SUBDIRS = \
	po \
	gfs2 \
	doc \
	tests

maintainer-clean-local:
	rm -rf m4

RPMSPEC = tests/gfs2-utils.spec
GITVERSION := \
	$(shell git describe --dirty 2>/dev/null | grep - | cut -d- -f2- | tr - .)
RPMRELEASE = $(or $(GITVERSION), "0")

$(RPMSPEC): $(RPMSPEC).in $(top_builddir)/config.status
	cd $(top_builddir) && ./config.status $@

RPMBUILDDIR = "$(abs_builddir)/rpmbuild"
RPMBUILDOPTS = \
	--buildroot $(RPMBUILDDIR)/buildroot \
	--define "_sourcedir $(abs_builddir)" \
	--define "_specdir $(abs_builddir)" \
	--define "_builddir $(abs_builddir)" \
	--define "_srcrpmdir $(RPMBUILDDIR)" \
	--define "_rpmdir $(RPMBUILDDIR)" \
	--define "rpmrelease $(RPMRELEASE)"

rpm: $(RPMSPEC)
	mkdir -p $(RPMBUILDDIR)
	rpmbuild $(RPMBUILDOPTS) -bb $(RPMSPEC)
