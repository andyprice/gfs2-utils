MAINTAINERCLEANFILES    = Makefile.in

# When an exec_prefix setting would have us install into /usr/sbin,
# use /sbin instead.
# Accept an existing sbindir value of /usr/sbin (probably for older automake),
# or an empty value, for automake-1.11 and newer.
sbindir := $(shell rpl=0; test '$(exec_prefix):$(sbindir)' = /usr:/usr/sbin \
                        || test '$(exec_prefix):$(sbindir)' = /usr: && rpl=1; \
                                     test $$rpl = 1 && echo /sbin || echo '$(exec_prefix)/sbin')


dist_sbin_SCRIPTS           = gfs2_lockgather
