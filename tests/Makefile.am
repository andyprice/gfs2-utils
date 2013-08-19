EXTRA_DIST = $(TESTSUITE_AT) package.m4 $(TESTSUITE) atlocal atconfig
DISTCLEANFILES = atlocal atconfig
CLEANFILES = testvol

if BUILD_TESTS
check_PROGRAMS = check_libgfs2

check_libgfs2_SOURCES = \
	check_meta.c \
	$(top_srcdir)/gfs2/libgfs2/libgfs2.h

check_libgfs2_CFLAGS = \
	-I$(top_srcdir)/gfs2/libgfs2 \
        -I$(top_srcdir)/gfs2/include \
	@check_CFLAGS@

check_libgfs2_LDADD = \
	$(top_builddir)/gfs2/libgfs2/libgfs2.la \
	@check_LIBS@
endif

# The `:;' works around a Bash 3.2 bug when the output is not writable.
$(srcdir)/package.m4: $(top_srcdir)/configure.ac
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
	libgfs2.at

TESTSUITE = $(srcdir)/testsuite

check-local: atconfig atlocal $(TESTSUITE)
	$(SHELL) '$(TESTSUITE)' $(TESTSUITEFLAGS)

installcheck-local: atconfig atlocal $(TESTSUITE)
	$(SHELL) '$(TESTSUITE)' AUTOTEST_PATH='$(sbindir):tests' $(TESTSUITEFLAGS)

clean-local:
	test ! -f '$(TESTSUITE)' || $(SHELL) '$(TESTSUITE)' --clean

atconfig: $(top_builddir)/config.status
	cd $(top_builddir) && ./config.status tests/$@

AUTOM4TE = $(SHELL) $(top_srcdir)/missing --run autom4te
AUTOTEST = $(AUTOM4TE) --language=autotest

$(TESTSUITE): $(TESTSUITE_AT) package.m4
	$(AUTOTEST) -I '$(srcdir)' -o $@.tmp $@.at
	mv $@.tmp $@
