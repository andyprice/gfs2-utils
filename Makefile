include make/defines.mk


REALSUBDIRS = group gfs2 doc \
	      bindings contrib

SUBDIRS = $(filter-out \
	  $(if ${without_group},group) \
	  $(if ${without_gfs2},gfs2) \
	  $(if ${without_bindings},bindings) \
	  , $(REALSUBDIRS))

all: ${SUBDIRS}

${SUBDIRS}:
	[ -n "${without_$@}" ] || ${MAKE} -C $@ all

group: 
gfs2: group
contrib: gfs2

oldconfig:
	@if [ -f $(OBJDIR)/.configure.sh ]; then \
		sh $(OBJDIR)/.configure.sh; \
	else \
		echo "Unable to find old configuration data"; \
	fi

install:
	set -e && for i in ${SUBDIRS}; do ${MAKE} -C $$i $@; done
	install -d ${logdir}
	install -d ${DESTDIR}/var/lib/cluster
	install -d ${DESTDIR}/var/run/cluster

uninstall:
	set -e && for i in ${SUBDIRS}; do ${MAKE} -C $$i $@; done
	rmdir ${logdir} || :;
	rmdir ${DESTDIR}/var/lib/cluster || :;
	rmdir ${DESTDIR}/var/run/cluster || :;

clean:
	set -e && for i in ${REALSUBDIRS}; do \
		contrib_code=1 \
		legacy_code=1 \
		${MAKE} -C $$i $@;\
	done

distclean: clean
	rm -f make/defines.mk
	rm -f .configure.sh
	rm -f *tar.gz
	rm -rf build

.PHONY: ${REALSUBDIRS}
