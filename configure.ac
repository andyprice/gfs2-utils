
# Process this file with autoconf to produce a configure script.

AC_PREREQ([2.63])
AC_INIT([gfs2-utils], [master], [linux-cluster@redhat.com])
AM_INIT_AUTOMAKE([-Wno-portability])
AM_SILENT_RULES([yes])
LT_PREREQ([2.2.6])
LT_INIT

AC_CONFIG_MACRO_DIR([m4])
AC_CONFIG_SRCDIR([group/gfs_controld/config.c])
AC_CONFIG_HEADERS([make/clusterautoconfig.h])

AC_CANONICAL_HOST
AC_PROG_LIBTOOL

AC_LANG([C])

#i18n support
AM_GNU_GETTEXT([external])
AM_GNU_GETTEXT_VERSION([0.18])

# Sanitize path

if test "$prefix" = "NONE"; then
	prefix="/usr"
	if test "$localstatedir" = "\${prefix}/var"; then
		localstatedir="/var"
	fi
	if test "$sysconfdir" = "\${prefix}/etc"; then
		sysconfdir="/etc"
	fi
	if test "$libdir" = "\${exec_prefix}/lib"; then
		if test -e /usr/lib64; then
			libdir="/usr/lib64"
		else
			libdir="/usr/lib"
		fi
	fi
fi

case $exec_prefix in
  NONE)   exec_prefix=$prefix;;
  prefix) exec_prefix=$prefix;;
esac

# Checks for programs.

# check stolen from gnulib/m4/gnu-make.m4
if ! ${MAKE-make} --version /cannot/make/this >/dev/null 2>&1; then
	AC_MSG_ERROR([you don't seem to have GNU make; it is required])
fi

AC_PROG_CC
AM_PROG_CC_C_O
AC_PROG_LN_S
AC_PROG_INSTALL
AC_PROG_MAKE_SET
AC_PROG_LEX
AC_CHECK_PROG([YACC], [bison], [bison -y])
test x"$YACC" = x && AC_MSG_ERROR([bison not found])

## local helper functions

# this function checks if CC support options passed as
# args. Global CFLAGS are ignored during this test.
cc_supports_flag() {
	local CFLAGS="$@"
	AC_MSG_CHECKING([whether $CC supports "$@"])
	AC_COMPILE_IFELSE([AC_LANG_SOURCE([int main(){return 0;}])],
			  [RC=0; AC_MSG_RESULT([yes])],
			  [RC=1; AC_MSG_RESULT([no])])
	return $RC
}

# this function tests if a library has a certain function
# by using AC_CHECK_LIB but restores the original LIBS global
# envvar. This is required to avoid libtool to link everything
# with everything.
check_lib_no_libs() {
	AC_CHECK_LIB([$1], [$2],,
		     [AC_MSG_ERROR([Unable to find $1 library])])
	LIBS=$ac_check_lib_save_LIBS
}

# local options
AC_ARG_ENABLE([debug],
	[  --enable-debug          enable debug build. ],
	[ default="no" ])

AC_ARG_ENABLE([gfs_controld],
	[  --enable-gfs_controld   build gfs_controld. ],
	[ default="no" ])

AC_ARG_WITH([syslogfacility],
	[  --syslogfacility=FACILITY
                          cluster default syslog facility. ],
	[ SYSLOGFACILITY="$withval" ],
	[ SYSLOGFACILITY="LOG_LOCAL4" ])

AC_ARG_WITH([sysloglevel],
	[  --sysloglevel=LEVEL
                          cluster default syslog level. ],
	[ SYSLOGLEVEL="$withval" ],
	[ SYSLOGLEVEL="LOG_INFO" ])

# gfs_controld isn't required in the latest versions of cluster
AM_CONDITIONAL([BUILD_GFS_CONTROLD], [test "x$enable_gfs_controld" = "xyes"])
AS_IF([test "x$enable_gfs_controld" = "xyes"], [
	PKG_CHECK_MODULES([corosync],[corosync])
	PKG_CHECK_MODULES([cpg],[libcpg])
	PKG_CHECK_MODULES([sackpt],[libSaCkpt])
	PKG_CHECK_MODULES([logt],[liblogthread])
	PKG_CHECK_MODULES([ccs],[libccs])
	PKG_CHECK_MODULES([cfg],[libcfg])
	PKG_CHECK_MODULES([fenced],[libfenced])
	PKG_CHECK_MODULES([dlmcontrol],[libdlmcontrol])
	PKG_CHECK_MODULES([quorum],[libquorum])
])

# We use the Check framework for unit tests
PKG_CHECK_MODULES([check], [check >= 0.9.8],
                  [have_check=yes],
                  [have_check=no])
AM_CONDITIONAL([BUILD_TESTS], [test "x$have_check" = "xyes"])

PKG_CHECK_MODULES([zlib],[zlib])

# old versions of ncurses don't ship pkg-config files
PKG_CHECK_MODULES([ncurses],[ncurses],,
		  [check_lib_no_libs ncurses printw])

if test -z "$ncurses_CFLAGS"  && test -z "$ncurses_LIBS"; then
	ncurses_LIBS=-lncurses
fi

# external libs (no pkgconfig)
check_lib_no_libs pthread pthread_mutex_lock

# Checks for header files.

AC_CHECK_HEADERS([arpa/inet.h fcntl.h inttypes.h libintl.h limits.h locale.h mntent.h netdb.h netinet/in.h stddef.h stdint.h stdlib.h string.h sys/file.h sys/ioctl.h sys/mount.h sys/param.h sys/socket.h sys/time.h sys/vfs.h syslog.h termios.h unistd.h])

AC_CHECK_HEADERS([linux/dlmconstants.h linux/limits.h linux/types.h linux/netlink.h linux/fs.h],,
		 [AC_MSG_ERROR([Unable to find all required kernel headers.])])

AC_CHECK_HEADERS([linux/gfs2_ondisk.h],
		 [AC_CHECK_MEMBERS([struct gfs2_sb.sb_uuid],,
			[AC_MSG_ERROR([Unable to find gfs2 uuid support in your headers.
Please update your kernel headers to a more recent version])],
			[#include <linux/gfs2_ondisk.h>])],
		 [AC_MSG_ERROR([Unable to find required kernel headers.])])

# Checks for typedefs, structures, and compiler characteristics.
AC_C_INLINE
AC_TYPE_INT64_T
AC_TYPE_MODE_T
AC_TYPE_OFF_T
AC_TYPE_PID_T
AC_TYPE_SIZE_T
AC_TYPE_SSIZE_T
AC_STRUCT_ST_BLOCKS
AC_TYPE_UINT16_T
AC_TYPE_UINT32_T
AC_TYPE_UINT64_T
AC_TYPE_UINT8_T

# Checks for library functions.
AC_FUNC_CHOWN
AC_FUNC_FORK
AC_FUNC_LSTAT_FOLLOWS_SLASHED_SYMLINK
AC_HEADER_MAJOR
AC_FUNC_MALLOC
AC_FUNC_REALLOC
AC_CHECK_FUNCS([ftruncate gettimeofday memset realpath rmdir select setlocale socket strcasecmp strchr strdup strerror strstr])

## random vars

LOGDIR=${localstatedir}/log/cluster
CLUSTERVARRUN=${localstatedir}/run/cluster

## do subst

AC_SUBST([LOGDIR])
AC_DEFINE_UNQUOTED([LOGDIR], "$(eval echo ${LOGDIR})",
		   [Default logging directory])

AC_SUBST([CLUSTERVARRUN])
AC_DEFINE_UNQUOTED([CLUSTERVARRUN], "$(eval echo ${CLUSTERVARRUN})",
		   [Default cluster var/run directory])

AC_DEFINE_UNQUOTED([SYSLOGFACILITY], $(eval echo ${SYSLOGFACILITY}),
		   [Default syslog facility])

AC_DEFINE_UNQUOTED([SYSLOGLEVEL], $(eval echo ${SYSLOGLEVEL}),
		   [Default syslog level])

## *FLAGS handling

ENV_CFLAGS="$CFLAGS"
ENV_CPPFLAGS="$CPPFLAGS"
ENV_LDFLAGS="$LDFLAGS"

# debug build stuff
if test "x${enable_debug}" = xyes; then
	AC_DEFINE_UNQUOTED([DEBUG], [1], [Compiling Debugging code])
	OPT_CFLAGS="-O0"
else
	OPT_CFLAGS="-O2"
fi

# gdb flags
if test "x${GCC}" = xyes; then
	GDB_FLAGS="-ggdb3"
else
	GDB_FLAGS="-g"
fi

# extra warnings
EXTRA_WARNINGS=""

WARNLIST="
	all
	shadow
	missing-prototypes
	missing-declarations
	strict-prototypes
	declaration-after-statement
	pointer-arith
	write-strings
	cast-align
	bad-function-cast
	missing-format-attribute
	format=2
	format-security
	format-nonliteral
	no-long-long
	unsigned-char
	gnu89-inline
	no-strict-aliasing
	"

for j in $WARNLIST; do
	if cc_supports_flag -W$j; then
		EXTRA_WARNINGS="$EXTRA_WARNINGS -W$j";
	fi
done

CFLAGS="$ENV_CFLAGS $OPT_CFLAGS $GDB_FLAGS \
	$EXTRA_WARNINGS $WERROR_CFLAGS"
CPPFLAGS="-I\$(top_builddir)/make -I\$(top_srcdir)/make \
	  -I. $ENV_CPPFLAGS"
LDFLAGS="$ENV_LDFLAGS"

AC_CONFIG_FILES([Makefile
		 group/Makefile
		 group/libgfscontrol/Makefile
		 group/gfs_control/Makefile
		 group/gfs_controld/Makefile
		 group/man/Makefile
		 group/include/Makefile
		 gfs2/Makefile
		 gfs2/include/Makefile
		 gfs2/libgfs2/Makefile
		 gfs2/convert/Makefile
		 gfs2/edit/Makefile
		 gfs2/fsck/Makefile
		 gfs2/mkfs/Makefile
		 gfs2/tune/Makefile
		 gfs2/man/Makefile
		 gfs2/scripts/Makefile
		 doc/Makefile
		 tests/Makefile
		 po/Makefile.in
		 ])

AC_OUTPUT

test x"$BUILD_TESTS" = x && AC_MSG_NOTICE([package 'check' not found; unit tests will not be built])
