TESTSCRIPTS = \
	fsck.gfs2-tester.sh \
	rgrifieldscheck.sh \
	rgskipcheck.sh

EXTRA_DIST = \
	$(TESTSUITE_AT) \
	$(TESTSCRIPTS) \
	package.m4 \
	gfs2-utils.spec.in \
	atlocal.in

DISTCLEANFILES = \
	atlocal \
	atconfig

CLEANFILES = \
	testvol \
	gfs2-utils.spec

noinst_PROGRAMS = nukerg

nukerg_SOURCES = nukerg.c
nukerg_CPPFLAGS = \
	-D_FILE_OFFSET_BITS=64 \
	-D_LARGEFILE64_SOURCE \
	-D_GNU_SOURCE
nukerg_CFLAGS = \
	-I$(top_srcdir)/gfs2/libgfs2 \
	-I$(top_srcdir)/gfs2/include
nukerg_LDADD = \
	$(top_builddir)/gfs2/libgfs2/libgfs2.la \
	$(uuid_LIBS)

# The `:;' works around a Bash 3.2 bug when the output is not writable.
package.m4: $(top_srcdir)/configure.ac
	:;{ \
		echo '# Signature of the current package.' && \
		echo 'm4_define([AT_PACKAGE_NAME],' && \
		echo '  [$(PACKAGE_NAME)])' && \
		echo 'm4_define([AT_PACKAGE_TARNAME],' && \
		echo '  [$(PACKAGE_TARNAME)])' && \
		echo 'm4_define([AT_PACKAGE_VERSION],' && \
		echo '  [$(PACKAGE_VERSION)])' && \
		echo 'm4_define([AT_PACKAGE_STRING],' && \
		echo '  [$(PACKAGE_STRING)])' && \
		echo 'm4_define([AT_PACKAGE_BUGREPORT],' && \
		echo '  [$(PACKAGE_BUGREPORT)])'; \
		echo 'm4_define([AT_PACKAGE_URL],' && \
		echo '  [$(PACKAGE_URL)])'; \
	} >'$(srcdir)/package.m4'

TESTSUITE_AT = \
	testsuite.at \
	mkfs.at \
	fsck.at \
	edit.at \
	tune.at

TESTSUITE = testsuite

check-local: atconfig atlocal $(TESTSUITE)
	$(SHELL) '$(TESTSUITE)' $(TOPTS)

installcheck-local: atconfig atlocal $(TESTSUITE)
	$(SHELL) '$(TESTSUITE)' AUTOTEST_PATH='$(sbindir):gfs2/libgfs2:tests' $(TOPTS)

clean-local:
	test ! -f '$(TESTSUITE)' || $(SHELL) '$(TESTSUITE)' --clean
	rm -f '$(TESTSUITE)'

atconfig: $(top_builddir)/config.status
	cd $(top_builddir) && ./config.status tests/$@

AUTOM4TE = $(SHELL) $(top_srcdir)/missing --run autom4te
AUTOTEST = $(AUTOM4TE) --language=autotest

$(TESTSUITE): $(TESTSUITE_AT) package.m4
	$(AUTOTEST) -I '$(srcdir)' -o $@.tmp $@.at
	mv $@.tmp $@
