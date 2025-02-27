# gfs2-utils Developer Intro

This is a brain dump of things that a gfs2-utils developer should know (besides
knowledge of how gfs2 works). The intention is to give an overview of how the
project is currently organised and not to set any details in stone as many
things described here could be improved upon.

## Directory layout

### doc/

This is where the documentation lives (besides README.md which must be in the
top level directory).

### gfs2/

All of the utils' code lives under here. It's really a useless level of
directory nesting and dates back to when everything was in cluster.git.

### gfs2/edit/

Where the `gfs2_edit` code lives.

### gfs2/fsck/

Where the `fsck.gfs2` code lives.

### gfs2/glocktop/

Where `glocktop` lives.

### gfs2/include/

Where shared headers for list and rbtree data structures live.

### gfs2/include/linux/

Contains a copy of the `include/uapi/gfs2_ondisk.h` header which must be kept
up-to-date with the kernel. Keeping a copy lets us avoid a lot of `#if` version
mess.

### gfs2/init.d/

Init script for gfs2 mounting. Fairly useless these days now that we have
pacemaker and systemd.

### gfs2/libgfs2/

Where `libgfs2` and `gfs2l` live. `libgfs2` is only built as a static library
and used internally.  There is a long-standing goal to make it a dynamic
library but it hasn't been a high priority and there is still a lot of work
needed to do so, although it's a good thing to keep in mind as a code
structuring guide.  `gfs2l` is an incomplete interpreter for a language that can
be used to lookup and modify gfs2 on-disk structures.  It's currently only used
in some tests.

### gfs2/man/

Manpages, written in roff. Must be kept up-to-date with new options and
behaviours in gfs2 and the utils.

### gfs2/mkfs/

Where `mkfs.gfs2`, `gfs2_jadd` and `gfs2_grow` live. They used to share a
`main()` function and the functionality was decided based on the executable
name but that was overly fiddly and made testing difficult so they were
separated.

### gfs2/scripts/

Various scripts that are useful for gfs2 users. The most important of these are
the withdraw helper script and the udev rule that triggers it. They are
required on all gfs2 nodes.

### gfs2/tune/

Where `tunegfs2` lives.

### make/

Initially contains just the copyright include file but the build system also
generates a header containing configuration information and puts it here.

### po/

Translation templates. We keep these up-to-date on a best effort basis although
i18n is incomplete and difficult. The main gfs2-utils.pot file is updated
before each release, using `make gfs2-utils.pot-update` in this directory.

### tests/

Smoke/regression tests based on the Autotest framework. See doc/README.tests
for the details. Where possible, new tests should be added here to cover new
util options, prevent regressions, etc.

## Build system

gfs2-utils uses the old Autoconf/Automake suite to generate a `configure` script
from configure.ac, which in turn generates `Makefile`s from .am files. See
`./configure --help` for options that can be used to change build behaviour.

## Code Style

We've never been strict about code style in gfs2-utils but we generally follow
the kernel's code style, with the exception that tabs should not be used for
alignment (only indentation).

## Test suite

The test suite is driven by the `make check` rule and is separated into two
parts: scripted regression tests in the `tests/` directory and C unit tests which
live in the same directory as the code they test. See doc/README.tests for an
intro.

### Scripted tests

The scripted tests are based on the Autotest framework that comes with
Autoconf/Automake. The `tests/testsuite` driver script is generated from the
various `tests/*.at` files which are written in the m4 macro language. Each
util has its own `.at` file and they are included in the main `testsuite.at`
file. The generated `testsuite` script is run on `make check` using the `TOPTS`
variable to pass options to the script, but can also be run directly for easier
debugging, etc. See `tests/testsuite --help` for details. If you haven't run
`make check` yet, the script can be generated with `make -C tests/ testsuite`

### Unit tests

These are C programs based on the Check unit testing framework and live in the
same directory as the code they test. There are currently only a small number
of them but we would like to increase test coverage. Alongside the code for
each util there is a `checks.am` file which is included into the `Makefile.am`
when unit tests are enabled. The unit tests live in files named `check_*.c`
with the main source file for each util named `check_<util>.c` e.g.
`check_mkfs.c`. To keep things simple, those main files declare the test
functions that they run instead of using header files for each one.

## Continuous Integration

CI is provided by https://ci.kronosnet.org/. It runs the test suite in various
environments on every push directly to the gfs2-utils.git repository and for
every PR submitted/updated. Submitting a draft PR can be a good way to ensure
you haven't broken the build for any of the supported distros, but as the test
suite isn't 100% we cannot conclude that any change is fine entirely based on
CI results. Targeted testing for each change is still required during
development. Besides running the tests, CI also runs static analysis (coverity)
and flags a failure if a pull request adds new warnings. False-positives can be
suppressed using special `/* coverity[<event>:SUPPRESS] */`  code comments that
tell the scanner to ignore them.

## Pull Requests

Contributions are accepted via email or pull requests to
https://pagure.io/gfs2-utils but even if the patches are sent by email it's a
good idea to create a pull request to get them checked by CI before merging.
The slow-moving nature of gfs2-utils development means that most changes are
fast-forward merges without merge commits, but merge commits are fine if they
make sense. It can be a better experience for a contributor if we're not overly
strict about that sort of thing.

## Useful Build-time Tips

### Default make rule: all

If you don't specify a make rule on the command line then the rule that is
invoked is `all`.  This is handy to know when you want to specify more rules
before building, e.g. `make clean all CC=cgcc` will rebuild everything with the
cgcc compiler (the Sparse static analyzer).

### CFLAGS / LDFLAGS

When you want to add or override a compiler flag for a single build, you can
use `make CFLAGS=...` and the specified flags will be **appended** to the
compiler flags. For example, to turn warnings into errors, build with `make
CFLAGS=-Werror`. To turn off a warning flag you can override it with the no-
prefix, e.g. `make CFLAGS=-Wno-cast-align` will disable the `-Wcast-align` flag
that is specified in `configure.ac`. `LDFLAGS` works the same, only with the
flags used in the linking stage. To completely override all flags (which is
rarely needed), the `AM_CFLAGS` and `AM_LDFLAGS` variables can be used.

### make check TOPTS=...

This will pass options to the `tests/testsuite` script for convenience. For
example, to run only the mkfs.gfs2 tests, use `make check TOPTS='-k mkfs'`. The
possible options can be listed by running `tests/testsuite --help`

### Verbose Build

By default build commands are hidden (by the `AM_SILENT_RULES([yes])` call in
configure.ac). Use `make V=1` to make the build commands visible.

## Releases

gfs2-utils releases are based on annotated tags. Run `git tag -l -n1` to see a
list of them. The release process which has been used for the last many years
is:

1. Make sure everything looks stable.
2. Update translation template and commit.
3. Generate an annotated tag for the release and push --tags.
4. Generate gz-, bz2- and xz-compressed tarballs using 'git archive'.
5. Unpack each tarball and test it.
6. Generate a gpg-signed checksum file for each tarball.
7. Upload the tarballs, checksums and signatures to the pagure 'releases' space.
8. Announce the new version to the gfs2 mailing list and the users@clusterlabs list.
9. Update the gfs2-utils RPM in Fedora Rawhide and, if needed, the stable Fedora releases.
10. If needed, open issues for each RHEL version that should be rebased to the new gfs2-utils.

Assuming the annotated tag is checked out and the default gpg signing key is correct, this script can be used to automate steps 4 to 6:

```bash
#!/bin/bash

set -x
set -e

V=`git describe`
NV=gfs2-utils-${V}
TB=${NV}.tar

# Requires the following git config options:
# $ git config --global tar.tar.bz2.command 'bzip2 -c'
# $ git config --global tar.tar.xz.command 'xz -c'
for ext in gz bz2 xz; do
    ARCHIVE=${TB}.${ext}

    git archive --format="tar.${ext}" --prefix=${NV}/ -o ${ARCHIVE} ${V}

    # Check
    D=`mktemp -d`
    tar -C ${D} -xaf ${ARCHIVE}
    (cd ${D}/${NV}; ./autogen.sh && ./configure)
    make -C ${D}/${NV} check
    rm -rf ${D}
done

for ext in gz bz2 xz; do
    ARCHIVE=${TB}.${ext}
    CHECKSUM=${ARCHIVE}.sha256
    SIG=${CHECKSUM}.asc

    # Sign off
    sha256sum ${ARCHIVE} > ${CHECKSUM}
    gpg --output ${SIG} --armor --detach-sig ${CHECKSUM}
    gpg --verify ${SIG}
    sha256sum -c ${CHECKSUM}
done
```
