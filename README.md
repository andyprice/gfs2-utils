# gfs2-utils

This package contains the tools needed to create, check, manipulate and analyze
gfs2 filesystems, along with important scripts required to support gfs2
clusters.

## Build instructions

### Prerequisites

The following development packages are required to build gfs2-utils:

* autoconf
* automake
* libtool
* GNU make
* ncurses
* gettext
* bison
* flex
* zlib
* bzip2
* libblkid
* libuuid
* check (optional, enables unit tests)

### How to build

To build gfs2-utils, run the following commands:

```
$ ./autogen.sh
$ ./configure
$ make
```

See `./configure --help` for more build configuration options.

## Test suite

To run the test suite, use:

```
$ make check
```

See [doc/README.tests](doc/README.tests) for more details regarding the test suite.

## Installation

gfs2-utils requires the following libraries:

* zlib
* bzip2
* ncurses
* libblkid
* libuuid

To install gfs2-utils, run:

```
# make install
```

## Support scripts

The following scripts (located in [gfs2/scripts/](gfs2/scripts)) are used to
complete the userland portion of the gfs2 withdraw feature using uevents. They
will be installed by `make install` to these directories by default:

Script                 | Default install path
---------------------- | ----------------------
82-gfs2-withdraw.rules | /usr/lib/udev/rules.d/
gfs2_withdraw_helper   | /usr/libexec/

See also [doc/README.contributing](doc/README.contributing) for details on
submitting patches.

DO NOT MERGE
