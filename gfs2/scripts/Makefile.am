MAINTAINERCLEANFILES = Makefile.in
CLEANFILES = 82-gfs2-withdraw.rules
EXTRA_DIST = 82-gfs2-withdraw.rules.in

dist_sbin_SCRIPTS = \
	gfs2_lockcapture \
	gfs2_trace

dist_libexec_SCRIPTS = \
	gfs2_withdraw_helper

82-gfs2-withdraw.rules: 82-gfs2-withdraw.rules.in
	sed -e 's,[@]libexecdir[@],$(libexecdir),g' $< > $@

udevrulesdir=@udevdir@/rules.d
udevrules_DATA = \
	82-gfs2-withdraw.rules
