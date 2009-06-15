MAINTAINERCLEANFILES	= Makefile.in

EXTRA_DIST		= setup.py INSTALL PLUGINAPI README \
			  scripts/askant \
			  askant/__init__.py askant/about.py \
			  askant/askant.py askant/blktrace.py \
			  askant/commands.py askant/sysfs.py \
			  askant/fs/__init__.py \
			  fsplugins/__init__.py \
			  fsplugins/gfs2/gfs2.c fsplugins/gfs2/gfs2.h \
			  fsplugins/gfs2/gfs2module.c

CFLAGS			+= -I$(top_srcdir)/gfs2/libgfs2
CFLAGS			+= -I$(top_srcdir)/gfs2/include

LDFLAGS			+= -L$(abs_builddir)/$(top_builddir)/gfs2/libgfs2/.libs

all-local: linkcode
	CFLAGS="$(CFLAGS)" LDFLAGS="$(LDFLAGS)" \
		$(PYTHON) setup.py build 

install-exec-local: linkcode
	if [ -n "$(DESTDIR)" ]; then \
		python setup.py install --root $(DESTDIR); \
	else \
		python setup.py install --prefix $(prefix); \
	fi

uninstall-local:
	if [ -n "$(DESTDIR)" ]; then \
		cd $(DESTDIR)/$(bindir) && \
			rm -f askant; \
		cd $(DESTDIR)/$(pythondir) && \
			rm -rf askant && rm askant*.egg-info; \
	else \
		cd $(prefix)/$(bindir) && \
			rm -f askant; \
		cd $(prefix)/$(pythondir) && \
			rm -rf askant && rm askant*.egg-info; \
	fi

linkcode: Makefile
	if [ ! -e setup.py ]; then \
		touch linkcode; \
		cd $(srcdir); \
			find . -type d -exec mkdir -p $(abs_builddir)/{} \; ; \
			find . -type f -exec ln -sf $(abs_srcdir)/{} $(abs_builddir)/{} \; ; \
	fi

clean-local:
	-$(PYTHON) setup.py clean
	rm -rf askant/*.pyc
	rm -rf build
	if [ -e linkcode ]; then \
		find . -type l -exec rm -f {} \; ; \
		rm -rf askant fsplugins scripts linkcode; \
	fi
