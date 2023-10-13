#include "clusterautoconfig.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <stdarg.h>
#include <mntent.h>
#include <ctype.h>
#include <poll.h>
#include <signal.h>
#include <sys/time.h>
#include <libintl.h>
#include <sys/ioctl.h>
#include <limits.h>
#include <blkid.h>
#include <locale.h>
#include <uuid.h>

#define _(String) gettext(String)

#include "libgfs2.h"
#include "gfs2_mkfs.h"
#include "progress.h"
#include "struct_print.h"

static void print_usage(const char *prog_name)
{
	int i;
	const char *option, *param, *desc;
	const char *options[] = {
	    /* Translators: This is a usage string printed with --help.
	       <size> and <number> here are the commandline parameters,
	       e.g. mkfs.gfs2 -b <size> -j <number> /dev/sda */
	    "-b", _("<size>"),   _("File system block size, in bytes"),
	    "-c", _("<size>"),   _("Size of quota change file, in megabytes"),
	    "-D", NULL,          _("Enable debugging code"),
	    "-h", NULL,          _("Display this help, then exit"),
	    "-J", _("<size>"),   _("Size of journals, in megabytes"),
	    "-j", _("<number>"), _("Number of journals"),
	    "-K", NULL,          _("Don't try to discard unused blocks"),
	    "-O", NULL,          _("Don't ask for confirmation"),
	    "-o", _("<key>[=<value>][,...]"), _("Specify extended options. See '-o help'."),
	    "-p", _("<name>"),   _("Name of the locking protocol"),
	    "-q", NULL,          _("Don't print anything"),
	    "-r", _("<size>"),   _("Size of resource groups, in megabytes"),
	    "-t", _("<name>"),   _("Name of the lock table"),
	    "-U", _("<UUID>"),   _("The UUID of the file system"),
	    "-V", NULL,          _("Display program version information, then exit"),
	    NULL, NULL, NULL /* Must be kept at the end */
	};

	printf("%s\n", _("Usage:"));
	printf("%s [%s] <%s> [%s]\n\n", prog_name, _("options"), _("device"), _("size"));
	printf(_("Create a gfs2 file system on a device. If a size, in blocks, is not "
	         "specified, the whole device will be used."));
	printf("\n\n%s\n", _("Options:"));

	for (i = 0; options[i] != NULL; i += 3) {
		option = options[i];
		param = options[i + 1];
		desc = options[i + 2];
		printf("%3s %-22s %s\n", option, param ? param : "", desc);
	}
}

static void print_ext_opts(void)
{
	int i;
	const char *options[] = {
		"help", _("Display this help, then exit"),
		"swidth=N",  _("Specify the stripe width of the device, overriding probed values"),
		"sunit=N", _("Specify the stripe unit of the device, overriding probed values"),
		"align=[0|1]", _("Disable or enable alignment of resource groups"),
		"format=N", _("Specify the format version number"),
		NULL, NULL
	};
	printf(_("Extended options:\n"));
	for (i = 0; options[i] != NULL; i += 2) {
		printf("%15s  %-22s\n", options[i], options[i + 1]);
	}
}

/**
 * Values probed by libblkid:
 *  alignment_offset: offset, in bytes, of the start of the dev from its natural alignment
 *  logical_sector_size: smallest addressable unit
 *  minimum_io_size: device's preferred unit of I/O. RAID stripe unit.
 *  optimal_io_size: biggest I/O we can submit without incurring a penalty. RAID stripe width.
 *  physical_sector_size: the smallest unit we can write atomically
 */
struct mkfs_dev {
	int fd;
	const char *path;
	struct stat stat;
	uint64_t size;
	unsigned long alignment_offset;
	unsigned long logical_sector_size;
	unsigned long minimum_io_size;
	unsigned long optimal_io_size;
	unsigned long physical_sector_size;

	unsigned int got_topol:1;
};

/* Number of resource groups */
static uint64_t nrgrp = 0;

struct mkfs_opts {
	unsigned bsize;
	unsigned qcsize;
	unsigned jsize;
	unsigned rgsize;
	unsigned long sunit;
	unsigned long swidth;
	unsigned format;
	uint64_t fssize;
	int journals;
	const char *lockproto;
	const char *locktable;
	const char *uuid;
	struct mkfs_dev dev;
	unsigned discard:1;
	unsigned root_inherit_jd:1;

	unsigned got_bsize:1;
	unsigned got_qcsize:1;
	unsigned got_jsize:1;
	unsigned got_rgsize:1;
	unsigned got_sunit:1;
	unsigned got_swidth:1;
	unsigned got_fssize:1;
	unsigned got_journals:1;
	unsigned got_lockproto:1;
	unsigned got_locktable:1;
	unsigned got_device:1;
	unsigned got_topol:1;
	unsigned got_format:1;
	unsigned got_uuid:1;

	unsigned override:1;
	unsigned quiet:1;
	unsigned debug:1;
	unsigned confirm:1;
	unsigned align;
};

static void opts_init(struct mkfs_opts *opts)
{
	memset(opts, 0, sizeof(*opts));
	opts->discard = 1;
	opts->journals = 1;
	opts->bsize = LGFS2_DEFAULT_BSIZE;
	opts->jsize = LGFS2_DEFAULT_JSIZE;
	opts->qcsize = LGFS2_DEFAULT_QCSIZE;
	opts->rgsize = LGFS2_DEFAULT_RGSIZE;
	opts->lockproto = "lock_dlm";
	opts->locktable = "";
	opts->confirm = 1;
	opts->align = 1;
	opts->format = GFS2_FORMAT_FS;
}

static struct lgfs2_inum *mkfs_journals = NULL;

#ifndef BLKDISCARD
#define BLKDISCARD      _IO(0x12,119)
#endif

static int discard_blocks(int fd, uint64_t len, int debug)
{
        uint64_t range[2];

	range[0] = 0;
	range[1] = len;
	if (debug)
		/* Translators: "discard" is a request sent to a storage device to
		 * discard a range of blocks. */
		printf(_("Issuing discard request: range: %"PRIu64" - %"PRIu64"..."),
		       range[0], range[1]);
	if (ioctl(fd, BLKDISCARD, &range) < 0) {
		if (debug)
			printf("%s = %d\n", _("error"), errno);
		return errno;
	}
	if (debug)
		printf(_("Successful.\n"));
        return 0;
}

/**
 * Convert a human-readable size string to a long long.
 * Copied and adapted from xfs_mkfs.c.
 */
static long long cvtnum(unsigned int blocksize, unsigned int sectorsize, const char *s)
{
        long long i;
        char *sp;

        i = strtoll(s, &sp, 0);
        if (i == 0 && sp == s)
                return -1LL;
        if (*sp == '\0')
                return i;

	*sp = tolower(*sp);
        if (*sp == 'b' && sp[1] == '\0') {
                if (blocksize)
                        return i * blocksize;
                fprintf(stderr, _("Block size not available yet.\n"));
		return -1LL;
        }
        if (*sp == 's' && sp[1] == '\0') {
                if (sectorsize)
                        return i * sectorsize;
                return i * GFS2_BASIC_BLOCK;
        }
        if (*sp == 'k' && sp[1] == '\0')
                return 1024LL * i;
        if (*sp == 'm' && sp[1] == '\0')
                return 1024LL * 1024LL * i;
        if (*sp == 'g' && sp[1] == '\0')
                return 1024LL * 1024LL * 1024LL * i;
        if (*sp == 't' && sp[1] == '\0')
                return 1024LL * 1024LL * 1024LL * 1024LL * i;
        if (*sp == 'p' && sp[1] == '\0')
                return 1024LL * 1024LL * 1024LL * 1024LL * 1024LL * i;
        if (*sp == 'e' && sp[1] == '\0')
                return 1024LL * 1024LL * 1024LL * 1024LL * 1024LL * 1024LL * i;
        return -1LL;
}

static int parse_ulong(struct mkfs_opts *opts, const char *key, const char *val, unsigned long *pn,
                       unsigned long max)
{
	long long l;
	if (val == NULL || *val == '\0') {
		fprintf(stderr, _("Missing argument to '%s'\n"), key);
		return -1;
	}
	l = cvtnum(opts->bsize, 0, val);
	if ((max > 0 && l > max) || l < 0) {
		fprintf(stderr, _("Value of '%s' is invalid\n"), key);
		return -1;
	}
	*pn = (unsigned long)l;
	return 0;
}

static int parse_bool(struct mkfs_opts *opts, const char *key, const char *val, unsigned *pbool)
{
	if (strnlen(val, 2) == 1) {
		if (*val == '0') {
			*pbool = 0;
			return 0;
		}
		if (*val == '1') {
			*pbool = 1;
			return 0;
		}
	}
	fprintf(stderr, _("Option '%s' must be either 1 or 0\n"), key);
	return -1;
}

static int parse_topology(struct mkfs_opts *opts, char *str)
{
	char *opt;
	unsigned i = 0;
	unsigned long *topol[5] = {
		&opts->dev.alignment_offset,
		&opts->dev.logical_sector_size,
		&opts->dev.minimum_io_size,
		&opts->dev.optimal_io_size,
		&opts->dev.physical_sector_size
	};

	while ((opt = strsep(&str, ":")) != NULL) {
		if (i > 4) {
			fprintf(stderr, "Too many topology values.\n");
			return 1;
		}
		if (parse_ulong(opts, "test_topology", opt, topol[i], 0))
			return 1;
		i++;
	}
	if (i < 5) {
		fprintf(stderr, "Too few topology values.\n");
		return 1;
	}
	return 0;
}

static int parse_format(struct mkfs_opts *opts, char *str)
{
	unsigned long ln;

	if (parse_ulong(opts, "format", str, &ln, LGFS2_FS_FORMAT_MAX) != 0)
		return -1;

	if (ln < LGFS2_FS_FORMAT_MIN) {
		fprintf(stderr, _("Invalid filesystem format: %s\n"), str);
		return -1;
	}
	opts->format = ln;
	opts->got_format = 1;
	return 0;
}

static int parse_root_inherit_jd(struct mkfs_opts *opts, const char *str)
{
	unsigned long n = 0;

	if (str == NULL) { /* -o root_inherit_jdata */
		opts->root_inherit_jd = 1;
		return 0;
	}
	/* -o root_inherit_jdata=N */
	if (parse_ulong(opts, "root_inherit_jdata", str, &n, 1) != 0)
		return -1;
	opts->root_inherit_jd = (unsigned)n;
	return 0;
}

static int opt_parse_extended(char *str, struct mkfs_opts *opts)
{
	char *opt;
	while ((opt = strsep(&str, ",")) != NULL) {
		char *key = strsep(&opt, "=");
		char *val = strsep(&opt, "=");
		if (key == NULL || *key == '\0') {
			fprintf(stderr, _("Missing argument to '-o' option\n"));
			return -1;
		}
		if (strcmp("sunit", key) == 0) {
			if (parse_ulong(opts, "sunit", val, &opts->sunit, 0) != 0)
				return -1;
			opts->got_sunit = 1;
		} else if (strcmp("swidth", key) == 0) {
			if (parse_ulong(opts, "swidth", val, &opts->swidth, 0) != 0)
				return -1;
			opts->got_swidth = 1;
		} else if (strcmp("align", key) == 0) {
			if (parse_bool(opts, "align", val, &opts->align) != 0)
				return -1;
		} else if (strcmp("test_topology", key) == 0) {
			if (parse_topology(opts, val) != 0)
				return -1;
			opts->got_topol = (opts->dev.logical_sector_size != 0 &&
	                                   opts->dev.physical_sector_size != 0);
		} else if (strcmp("format", key) == 0) {
			if (parse_format(opts, val) != 0)
				return -1;
		} else if (strcmp("root_inherit_jdata", key) == 0) {
			if (parse_root_inherit_jd(opts, val) != 0)
				return -1;
		} else if (strcmp("help", key) == 0) {
			print_ext_opts();
			return 1;
		} else {
			fprintf(stderr, _("Invalid extended option (specified with -o): '%s'\n"), key);
			print_ext_opts();
			return -1;
		}
	}
	return 0;
}

static int opts_get(int argc, char *argv[], struct mkfs_opts *opts)
{
	int ret;
	int c;
	while (1) {
		c = getopt(argc, argv, "-b:c:DhJ:j:KOo:p:qr:t:U:V");
		if (c == -1)
			break;

		switch (c) {
		case 'b':
			opts->bsize = atoi(optarg);
			opts->got_bsize = 1;
			break;
		case 'c':
			opts->qcsize = atoi(optarg);
			opts->got_qcsize = 1;
			break;
		case 'D':
			opts->debug = 1;
			break;
		case 'h':
			print_usage(argv[0]);
			return 1;
		case 'J':
			opts->jsize = atoi(optarg);
			opts->got_jsize = 1;
			break;
		case 'j':
			opts->journals = atoi(optarg);
			opts->got_journals = 1;
			break;
		case 'K':
			opts->discard = 0;
			break;
		case 'O':
			opts->override = 1;
			break;
		case 'p':
			opts->lockproto = optarg;
			opts->got_lockproto = 1;
			break;
		case 't':
			opts->locktable = optarg;
			opts->got_locktable = 1;
			break;
		case 'q':
			opts->quiet = 1;
			break;
		case 'r':
			opts->rgsize = atoi(optarg);
			opts->got_rgsize = 1;
			break;
		case 'o':
			ret = opt_parse_extended(optarg, opts);
			if (ret != 0)
				return ret;
			break;
		case 'U':
			opts->uuid = optarg;
			opts->got_uuid = 1;
			break;
		case 'V':
			printf("mkfs.gfs2 %s (built %s %s)\n", VERSION,
			       __DATE__, __TIME__);
			printf(REDHAT_COPYRIGHT "\n");
			return 1;
			break;
		case ':':
		case '?':
			fprintf(stderr, _("Please use '-h' for help.\n"));
			return -1;
			break;
		case 1:
			if (strcmp(optarg, "gfs2") == 0)
				continue;
			if (!opts->got_device) {
				opts->dev.path = optarg;
				opts->got_device = 1;
			} else if (!opts->got_fssize && isdigit(optarg[0])) {
				opts->fssize = atol(optarg);
				opts->got_fssize = 1;
			} else {
				fprintf(stderr, _("More than one device specified (try -h for help)\n"));
				return -1;
			}
			break;
		default:
			fprintf(stderr, _("Invalid option: %c\n"), c);
			return -1;
		};
	}
	return 0;
}

/**
 * Make sure the GFS2 is set up to use the right lock protocol
 * @lockproto: the lock protocol to mount
 * @locktable: the locktable name
 *
 */

static int test_locking(struct mkfs_opts *opts)
{
	const char *c;
	/* Translators: A lock table is a string identifying a gfs2 file system
	 * in a cluster, e.g. cluster_name:fs_name */
	const char *errprefix = _("Invalid lock table:");
	int table_required = (strcmp(opts->lockproto, "lock_gulm") == 0)
	                  || (strcmp(opts->lockproto, "lock_dlm") == 0);

	if ((strcmp(opts->lockproto, "lock_nolock") != 0) && !table_required) {
		fprintf(stderr, _("Invalid lock protocol: %s\n"), opts->lockproto);
		return -1;
	}
	/* When lock_*lm is given as the lock protocol, require a lock table too */
	if (!opts->got_locktable) {
		if (table_required) {
			fprintf(stderr, _("No lock table specified.\n"));
			return -1;
		}
		return 0;
	}
	/* User gave a lock table option, validate it */
	if (*opts->locktable == '\0') {
		fprintf(stderr, _("Lock table is empty.\n"));
		return -1;
	}
	for (c = opts->locktable; *c; c++) {
		if (!isalnum(*c) && (*c != '-') && (*c != '_') && (*c != ':')) {
			fprintf(stderr, "%s %s '%c'\n", errprefix, _("invalid character"), *c);
			return -1;
		}
	}
	c = strstr(opts->locktable, ":");
	if (!c) {
		fprintf(stderr, "%s %s\n", errprefix, _("missing colon"));
		return -1;
	}

	if (c == opts->locktable) {
		fprintf(stderr, "%s %s\n", errprefix, _("cluster name is missing"));
		return -1;
	}
	if (c - opts->locktable > 32) {
		fprintf(stderr, "%s %s\n", errprefix, _("cluster name is too long"));
		return -1;
	}

	c++;
	if (strstr(c, ":")) {
		fprintf(stderr, "%s %s\n", errprefix, _("contains more than one colon"));
		return -1;
	}
	if (!strlen(c)) {
		fprintf(stderr, "%s %s\n", errprefix, _("file system name is missing"));
		return -1;
	}
	if (strlen(c) > 30) {
		fprintf(stderr, "%s %s\n", errprefix, _("file system name is too long"));
		return -1;
	}
	return 0;
}

static int are_you_sure(void)
{
	while (1) {
		char *line = NULL;
		size_t len = 0;
		int ret;
		int res;

		/* Translators: We use rpmatch(3) to match the answers to y/n
		   questions in the user's own language, so the [y/n] here must also be
		   translated to match one of the letters in the pattern printed by
		   `locale -k yesexpr` and one of the letters in the pattern printed by
		   `locale -k noexpr` */
		printf( _("Are you sure you want to proceed? [y/n] "));
		ret = getline(&line, &len, stdin);
		if (ret < 0) {
			printf("\n");
			free(line);
			return 0;
		}
		res = rpmatch(line);
		free(line);
		if (ret == 0)
			continue;
		if (res == 1) /* Yes */
			return 1;
		if (res == 0) /* No */
			return 0;
		/* Unrecognized input; go again. */
	}
}

static int choose_blocksize(struct mkfs_opts *opts, unsigned *pbsize)
{
	unsigned int x;
	unsigned int bsize = opts->bsize;
	struct mkfs_dev *dev = &opts->dev;
	int got_topol = (dev->got_topol || opts->got_topol);

	if (got_topol && opts->debug) {
		printf("alignment_offset: %lu\n", dev->alignment_offset);
		printf("logical_sector_size: %lu\n", dev->logical_sector_size);
		printf("minimum_io_size: %lu\n", dev->minimum_io_size);
		printf("optimal_io_size: %lu\n", dev->optimal_io_size);
		printf("physical_sector_size: %lu\n", dev->physical_sector_size);
	}
	if (got_topol && dev->alignment_offset != 0) {
		fprintf(stderr,
		  _("Warning: device is not properly aligned. This may harm performance.\n"));
		dev->physical_sector_size = dev->logical_sector_size;
	}
	if (!opts->got_bsize && got_topol) {
		if (dev->optimal_io_size <= getpagesize() &&
		    dev->optimal_io_size >= LGFS2_DEFAULT_BSIZE)
			bsize = dev->optimal_io_size;
		else if (dev->physical_sector_size <= getpagesize() &&
		         dev->physical_sector_size >= LGFS2_DEFAULT_BSIZE)
			bsize = dev->physical_sector_size;
	}
	/* Block sizes must be a power of two from 512 to 65536 */
	for (x = 512; x; x <<= 1)
		if (x == bsize)
			break;

	if (!x || bsize > getpagesize()) {
		fprintf(stderr, _("Block size must be a power of two between 512 and %d\n"),
		        getpagesize());
		return -1;
	}
	if (bsize < dev->logical_sector_size) {
		fprintf(stderr, _("Error: Block size %d is less than minimum logical "
		       "block size (%lu).\n"), bsize, dev->logical_sector_size);
		return -1;
	}
	if (bsize < dev->physical_sector_size) {
		printf( _("Warning: Block size %d is inefficient because it "
			  "is less than the physical block size (%lu).\n"),
			  bsize, dev->physical_sector_size);
		opts->confirm = 1;
	}
	*pbsize = bsize;
	return 0;
}

static int opts_check(struct mkfs_opts *opts)
{
	if (!opts->got_device) {
		fprintf(stderr, _("No device specified. Use -h for help\n"));
		return -1;
	}

	if (test_locking(opts) != 0)
		return -1;

	if (LGFS2_MIN_RGSIZE > opts->rgsize || opts->rgsize > LGFS2_MAX_RGSIZE) {
		/* Translators: gfs2 file systems are split into equal sized chunks called
		   resource groups. We're checking that the user gave a valid size for them. */
		fprintf(stderr, _("Bad resource group size\n"));
		return -1;
	}

	if (!opts->journals) {
		fprintf(stderr, _("No journals specified\n"));
		return -1;
	}
	if (opts->journals < 0) {
		fprintf(stderr, _("Number of journals cannot be negative: %d\n"), opts->journals);
		return -1;
	}
	if (opts->jsize < LGFS2_MIN_JSIZE || opts->jsize > LGFS2_MAX_JSIZE) {
		fprintf(stderr, _("Bad journal size\n"));
		return -1;
	}

	if (!opts->qcsize || opts->qcsize > 64) {
		fprintf(stderr, _("Bad quota change size\n"));
		return -1;
	}

	if ((opts->got_sunit && !opts->got_swidth) || (!opts->got_sunit && opts->got_swidth)) {
		fprintf(stderr, _("Stripe unit and stripe width must be specified together\n"));
		return -1;
	}
	return 0;
}

static void print_results(struct lgfs2_sbd *sb, struct mkfs_opts *opts)
{
	char readable_uuid[36+1];

	uuid_unparse(sb->sd_uuid, readable_uuid);

	printf("%-27s%s\n", _("Device:"), opts->dev.path);
	printf("%-27s%u\n", _("Block size:"), sb->sd_bsize);
	printf("%-27s%.2f %s (%"PRIu64" %s)\n", _("Device size:"),
	       /* Translators: "GB" here means "gigabytes" */
	       (opts->dev.size / ((float)(1 << 30))), _("GB"),
	       (opts->dev.size / sb->sd_bsize), _("blocks"));
	printf("%-27s%.2f %s (%"PRIu64" %s)\n", _("Filesystem size:"),
	       (sb->fssize / ((float)(1 << 30)) * sb->sd_bsize), _("GB"), sb->fssize, _("blocks"));
	printf("%-27s%u\n", _("Journals:"), opts->journals);
	printf("%-27s%uMB\n", _("Journal size:"), opts->jsize);
	printf("%-27s%"PRIu64"\n", _("Resource groups:"), nrgrp);
	printf("%-27s\"%s\"\n", _("Locking protocol:"), opts->lockproto);
	printf("%-27s\"%s\"\n", _("Lock table:"), opts->locktable);
	/* Translators: "UUID" = universally unique identifier. */
	printf("%-27s%s\n", _("UUID:"), readable_uuid);
}

static int warn_of_destruction(const char *path)
{
	struct stat lnkstat;
	char *abspath = NULL;

	if (lstat(path, &lnkstat) == -1) {
		perror(_("Failed to lstat the device"));
		return -1;
	}
	if (S_ISLNK(lnkstat.st_mode)) {
		/* coverity[toctou:SUPPRESS] */
		abspath = realpath(path, NULL);
		if (abspath == NULL) {
			perror(_("Could not find the absolute path of the device"));
			return -1;
		}
		/* Translators: Example: "/dev/vg/lv is a symbolic link to /dev/dm-2" */
		printf( _("%s is a symbolic link to %s\n"), path, abspath);
		path = abspath;
	}
	printf(_("This will destroy any data on %s\n"), path);
	free(abspath);
	return 0;
}

static int build_per_node(struct lgfs2_sbd *sdp, struct mkfs_opts *opts)
{
	struct lgfs2_inode *per_node;
	unsigned int j;

	/* coverity[identity_transfer:SUPPRESS] False positive */
	per_node = lgfs2_createi(sdp->master_dir, "per_node", S_IFDIR | 0700,
			   GFS2_DIF_SYSTEM);
	if (per_node == NULL) {
		fprintf(stderr, _("Error building '%s': %s\n"), "per_node", strerror(errno));
		return -1;
	}
	for (j = 0; j < sdp->md.journals; j++) {
		struct lgfs2_inode *ip;

		/* coverity[identity_transfer:SUPPRESS] False positive */
		ip = lgfs2_build_inum_range(per_node, j);
		if (ip == NULL) {
			fprintf(stderr, _("Error building '%s': %s\n"), "inum_range",
			        strerror(errno));
			lgfs2_inode_put(&per_node);
			return 1;
		}
		if (opts->debug) {
			printf("\nInum Range %u:\n", j);
			dinode_print(ip->i_bh->b_data);
		}
		lgfs2_inode_put(&ip);

		/* coverity[identity_transfer:SUPPRESS] False positive */
		ip = lgfs2_build_statfs_change(per_node, j);
		if (ip == NULL) {
			fprintf(stderr, _("Error building '%s': %s\n"), "statfs_change",
			        strerror(errno));
			lgfs2_inode_put(&per_node);
			return 1;
		}
		if (opts->debug) {
			printf("\nStatFS Change %u:\n", j);
			dinode_print(ip->i_bh->b_data);
		}
		lgfs2_inode_put(&ip);

		/* coverity[identity_transfer:SUPPRESS] False positive */
		ip = lgfs2_build_quota_change(per_node, j, LGFS2_DEFAULT_QCSIZE);
		if (ip == NULL) {
			fprintf(stderr, _("Error building '%s': %s\n"), "quota_change",
			        strerror(errno));
			/* coverity[deref_arg:SUPPRESS] */
			lgfs2_inode_put(&per_node);
			return 1;
		}
		if (opts->debug) {
			printf("\nQuota Change %u:\n", j);
			dinode_print(ip->i_bh->b_data);
		}
		lgfs2_inode_put(&ip);
	}
	if (opts->debug) {
		printf("\nper_node:\n");
		dinode_print(per_node->i_bh->b_data);
	}
	lgfs2_inode_put(&per_node);
	return 0;
}

static int zero_gap(struct lgfs2_sbd *sdp, uint64_t addr, size_t blocks)
{
	struct iovec *iov;
	char *zerobuf;
	ssize_t wrote;
	unsigned i;

	if (blocks == 0)
		return 0;
	iov = calloc(blocks, sizeof(*iov));
	if (iov == NULL) {
		perror(_("Failed to zero blocks\n"));
		return 1;
	}
	zerobuf = calloc(1, sdp->sd_bsize);
	if (zerobuf == NULL) {
		perror(_("Failed to zero blocks\n"));
		free(iov);
		return 1;
	}
	for (i = 0; i < blocks; i++) {
		iov[i].iov_base = zerobuf;
		iov[i].iov_len = sdp->sd_bsize;
	}
	wrote = pwritev(sdp->device_fd, iov, blocks, addr * sdp->sd_bsize);
	if (wrote != blocks * sdp->sd_bsize) {
		fprintf(stderr, _("Zeroing write failed at block %"PRIu64"\n"), addr);
		free(zerobuf);
		free(iov);
		return 1;
	}
	free(zerobuf);
	free(iov);
	return 0;
}

static lgfs2_rgrps_t rgs_init(struct mkfs_opts *opts, struct lgfs2_sbd *sdp)
{
	lgfs2_rgrps_t rgs;
	uint64_t al_base = 0;
	uint64_t al_off = 0;

	if (opts->align && opts->got_sunit) {
		if ((opts->sunit % sdp->sd_bsize) != 0) {
			fprintf(stderr, _("Stripe unit (%lu) must be a multiple of block size (%u)\n"),
			        opts->sunit, sdp->sd_bsize);
			return NULL;
		} else if ((opts->swidth % opts->sunit) != 0) {
			fprintf(stderr, _("Stripe width (%lu) must be a multiple of stripe unit (%lu)\n"),
			        opts->swidth, opts->sunit);
			return NULL;
		} else {
			al_base = opts->swidth / sdp->sd_bsize;
			al_off = opts->sunit / sdp->sd_bsize;
		}
	} else if (opts->align) {
		if (opts->dev.optimal_io_size <= opts->dev.physical_sector_size ||
		    opts->dev.minimum_io_size <= opts->dev.physical_sector_size ||
		    (opts->dev.optimal_io_size % opts->dev.minimum_io_size) != 0) {
			/* If optimal_io_size is not a multiple of minimum_io_size then
			   the values are not reliable swidth and sunit values, so disable
			   rgrp alignment */
			opts->align = 0;
		} else {
			al_base = opts->dev.optimal_io_size / sdp->sd_bsize;
			al_off = opts->dev.minimum_io_size / sdp->sd_bsize;
		}
	}

	rgs = lgfs2_rgrps_init(sdp, al_base, al_off);
	if (rgs == NULL) {
		perror(_("Could not initialise resource groups"));
		return NULL;
	}

	if (opts->debug) {
		printf("  rgrp align = ");
		if (opts->align)
			printf("%"PRIu64"+%"PRIu64" blocks\n", al_base, al_off);
		else
			printf("(disabled)\n");
	}

	return rgs;
}

static int place_rgrp(struct lgfs2_sbd *sdp, lgfs2_rgrp_t rg, int debug)
{
	uint64_t prev_end = (GFS2_SB_ADDR * GFS2_BASIC_BLOCK / sdp->sd_bsize) + 1;
	lgfs2_rgrp_t prev = lgfs2_rgrp_prev(rg);
	struct gfs2_rindex ri;
	uint64_t addr;
	int err = 0;

	if (prev != NULL) {
		lgfs2_rindex_out(prev, &ri);
		prev_end = be64_to_cpu(ri.ri_data0) + be32_to_cpu(ri.ri_data);
	}

	lgfs2_rindex_out(rg, &ri);
	addr = be64_to_cpu(ri.ri_addr);

	while (prev_end < addr) {
		size_t gap_len = addr - prev_end;

		if (gap_len > IOV_MAX)
			gap_len = IOV_MAX;
		err = zero_gap(sdp, prev_end, gap_len);
		if (err != 0)
			return -1;
		prev_end += gap_len;
	}
	err = lgfs2_rgrp_write(sdp->device_fd, rg);
	if (err != 0) {
		perror(_("Failed to write resource group"));
		return -1;
	}
	if (debug) {
		rindex_print(&ri);
		printf("\n");
	}
	sdp->blks_total += be32_to_cpu(ri.ri_data);
	sdp->fssize = be64_to_cpu(ri.ri_data0) + be32_to_cpu(ri.ri_data);
	nrgrp++;
	return 0;
}

static int add_rgrp(lgfs2_rgrps_t rgs, uint64_t *addr, uint32_t len, lgfs2_rgrp_t *rg)
{
	struct gfs2_rindex ri;
	uint64_t nextaddr;

	/* When we get to the end of the device, it's only an error if we have
	   more structures left to write, i.e. when len is != 0. */
	nextaddr = lgfs2_rindex_entry_new(rgs, &ri, *addr, len);
	if (nextaddr == 0) {
		if (len != 0) {
			perror(_("Failed to create resource group index entry"));
			return -1;
		} else {
			return 1;
		}
	}
	*rg = lgfs2_rgrps_append(rgs, &ri, nextaddr - *addr);
	if (*rg == NULL) {
		perror(_("Failed to create resource group"));
		return -1;
	}
	*addr = nextaddr;
	return 0;
}

static int place_journals(struct lgfs2_sbd *sdp, lgfs2_rgrps_t rgs, struct mkfs_opts *opts, uint64_t *rgaddr)
{
	struct gfs2_progress_bar progress;
	uint64_t jfsize = lgfs2_space_for_data(sdp, sdp->sd_bsize, opts->jsize << 20);
	uint32_t rgsize = lgfs2_rgsize_for_data(jfsize, sdp->sd_bsize);
	unsigned j;

	gfs2_progress_init(&progress, opts->journals, _("Adding journals: "), opts->quiet);

	/* We'll build the jindex later so remember where we put the journals */
	mkfs_journals = calloc(opts->journals, sizeof(*mkfs_journals));
	if (mkfs_journals == NULL)
		return 1;
	*rgaddr = lgfs2_rgrp_align_addr(rgs, LGFS2_SB_ADDR(sdp) + 1);
	rgsize = lgfs2_rgrp_align_len(rgs, rgsize);

	for (j = 0; j < opts->journals; j++) {
		int result;
		lgfs2_rgrp_t rg;
		struct lgfs2_inode in = {0};
		struct gfs2_rindex ri;

		gfs2_progress_update(&progress, (j + 1));

		if (opts->debug)
			printf(_("Placing resource group for journal%u\n"), j);

		result = add_rgrp(rgs, rgaddr, rgsize, &rg);
		if (result > 0)
			break;
		else if (result < 0)
			return result;

		result = lgfs2_rgrp_bitbuf_alloc(rg);
		if (result != 0) {
			perror(_("Failed to allocate space for bitmap buffer"));
			return result;
		}
		/* Allocate at the beginning of the rgrp, bypassing extent search */
		lgfs2_rindex_out(rg, &ri);
		in.i_num.in_addr = be64_to_cpu(ri.ri_data0);
		/* In order to keep writes sequential here, we have to allocate
		   the journal, then write the rgrp header (which is now in its
		   final form) and then write the journal out */
		result = lgfs2_file_alloc(rg, opts->jsize << 20, &in, GFS2_DIF_SYSTEM, S_IFREG | 0600);
		if (result != 0) {
			fprintf(stderr, _("Failed to allocate space for journal %u\n"), j);
			return result;
		}

		result = place_rgrp(sdp, rg, opts->debug);
		if (result != 0)
			return result;

		lgfs2_rgrp_bitbuf_free(rg);

		result = lgfs2_write_filemeta(&in);
		if (result != 0) {
			fprintf(stderr, _("Failed to write journal %u: %s\n"),
			        j, strerror(errno));
			return result;
		}

		result = lgfs2_write_journal_data(&in);
		if (result != 0) {
			fprintf(stderr, _("Failed to write data blocks for journal %u: %s\n"),
			        j, strerror(errno));
			return result;
		}
		mkfs_journals[j] = in.i_num;
	}
	gfs2_progress_close(&progress, _("Done\n"));

	return 0;
}

static int place_rgrps(struct lgfs2_sbd *sdp, lgfs2_rgrps_t rgs, uint64_t *rgaddr, struct mkfs_opts *opts)
{
	struct gfs2_progress_bar progress;
	uint32_t rgblks = ((opts->rgsize << 20) / sdp->sd_bsize);
	uint32_t rgnum;
	int result;

	rgnum = lgfs2_rgrps_plan(rgs, sdp->device.length - *rgaddr, rgblks);
	gfs2_progress_init(&progress, (rgnum + opts->journals), _("Building resource groups: "), opts->quiet);

	while (1) {
		lgfs2_rgrp_t rg;
		result = add_rgrp(rgs, rgaddr, 0, &rg);
		if (result > 0)
			break;
		else if (result < 0)
			return result;

		result = place_rgrp(sdp, rg, opts->debug);
		if (result != 0) {
			fprintf(stderr, _("Failed to build resource groups\n"));
			return result;
		}
		gfs2_progress_update(&progress, nrgrp);
	}
	if (lgfs2_rgrps_write_final(sdp->device_fd, rgs) != 0) {
		perror(_("Failed to write final resource group"));
		return 0;
	}
	gfs2_progress_close(&progress, _("Done\n"));

	return 0;
}

static int create_jindex(struct lgfs2_sbd *sdp, struct mkfs_opts *opts, struct lgfs2_inum *jnls)
{
	struct lgfs2_inode *jindex;

	jindex = lgfs2_build_jindex(sdp->master_dir, jnls, opts->journals);
	if (jindex == NULL) {
		fprintf(stderr, _("Error building '%s': %s\n"), "jindex", strerror(errno));
		return 1;
	}
	if (opts->debug) {
		printf("Jindex:\n");
		dinode_print(jindex->i_bh->b_data);
	}
	lgfs2_inode_put(&jindex);
	return 0;
}


/*
 * Find a reasonable journal file size (in blocks) given the number of blocks
 * in the filesystem.  For very small filesystems, it is not reasonable to
 * have a journal that fills more than half of the filesystem.
 *
 * n.b. comments assume 4k blocks
 *
 * This was copied and adapted from e2fsprogs.
 */
static int default_journal_size(unsigned bsize, uint64_t num_blocks)
{
	int min_blocks = (LGFS2_MIN_JSIZE << 20) / bsize;

	if (num_blocks < 2 * min_blocks)
		return -1;
	if (num_blocks < 131072)        /* 512 MB */
		return min_blocks;              /* 8 MB */
	if (num_blocks < 512*1024)      /* 2 GB */
		return (4096);                  /* 16 MB */
	if (num_blocks < 2048*1024)     /* 8 GB */
		return (8192);                  /* 32 MB */
	if (num_blocks < 4096*1024)     /* 16 GB */
		return (16384);                 /* 64 MB */
	if (num_blocks < 262144*1024)   /*  1 TB */
		return (32768);                 /* 128 MB */
	if (num_blocks < 2621440UL*1024)  /* 10 TB */
		return (131072);                /* 512 MB */
	return 262144;                          /*   1 GB */
}

static int sbd_init(struct lgfs2_sbd *sdp, struct mkfs_opts *opts, unsigned bsize)
{
	memset(sdp, 0, sizeof(struct lgfs2_sbd));
	sdp->sd_time = time(NULL);
	sdp->rgtree.osi_node = NULL;
	sdp->rgsize = opts->rgsize;
	sdp->md.journals = opts->journals;
	sdp->device_fd = opts->dev.fd;
	sdp->sd_bsize = bsize;
	sdp->sd_fs_format = opts->format;
	sdp->sd_multihost_format = GFS2_FORMAT_MULTI;
	sdp->sd_bsize = bsize;
	sdp->sd_bsize_shift = ffs(bsize) - 1;

	if (opts->got_uuid) {
		int err = uuid_parse(opts->uuid, sdp->sd_uuid);
		if (err != 0) {
			fprintf(stderr, _("Failed to parse UUID option."));
			return -1;
		}
	} else
		uuid_generate(sdp->sd_uuid);

	if (lgfs2_compute_constants(sdp)) {
		perror(_("Failed to compute file system constants"));
		return -1;
	}
	sdp->device.length = opts->dev.size / sdp->sd_bsize;
	if (opts->got_fssize) {
		if (opts->fssize > sdp->device.length) {
			fprintf(stderr, _("Specified size is bigger than the device."));
			fprintf(stderr, "%s %.2f %s (%"PRIu64" %s)\n", _("Device size:"),
			       opts->dev.size / ((float)(1 << 30)), _("GB"),
			       opts->dev.size / sdp->sd_bsize, _("blocks"));
			return -1;
		}
		sdp->device.length = opts->fssize;
	}
	/* opts->jsize has already been max/min checked but we need to check it
	   makes sense for the device size, or set a sensible default, if one
	   will fit. For user-provided journal sizes, limit it to half of the fs. */
	if (!opts->got_jsize) {
		int default_jsize = default_journal_size(sdp->sd_bsize, sdp->device.length / opts->journals);
		unsigned jsize_mb;

		if (default_jsize < 0) {
			fprintf(stderr, _("gfs2 will not fit on this device.\n"));
			return -1;
		}
		jsize_mb = (default_jsize * sdp->sd_bsize) >> 20;
		if (jsize_mb < LGFS2_MIN_JSIZE)
			opts->jsize = LGFS2_MIN_JSIZE;
		else
			opts->jsize = jsize_mb;
	} else if ((((opts->jsize * opts->journals) << 20) / sdp->sd_bsize) > (sdp->device.length / 2)) {
		unsigned max_jsize = (sdp->device.length / 2 * sdp->sd_bsize / opts->journals) >> 20;

		fprintf(stderr, _("gfs2 will not fit on this device.\n"));
		if (max_jsize >= LGFS2_MIN_JSIZE)
			fprintf(stderr, _("Maximum size for %u journals on this device is %uMB.\n"),
			        opts->journals, max_jsize);
		return -1;
	}
	return 0;
}

static int probe_contents(struct mkfs_dev *dev)
{
	int ret;
	const char *contents;
	blkid_probe pr = blkid_new_probe();
	if (pr == NULL || blkid_probe_set_device(pr, dev->fd, 0, 0) != 0
	               || blkid_probe_enable_superblocks(pr, 1) != 0
	               || blkid_probe_enable_partitions(pr, 1) != 0) {
		fprintf(stderr, _("Failed to create probe\n"));
		return -1;
	}

	if (!S_ISREG(dev->stat.st_mode) && blkid_probe_enable_topology(pr, 1) != 0) {
		fprintf(stderr, _("Failed to create probe\n"));
		return -1;
	}

	ret = blkid_do_fullprobe(pr);
	if (ret == -1) {
		fprintf(stderr, _("Failed to probe device\n"));
		return -1;
	}

	if (ret == 1)
		return 0;

	if (!blkid_probe_lookup_value(pr, "TYPE", &contents, NULL)) {
		printf(_("It appears to contain an existing filesystem (%s)\n"), contents);
	} else if (!blkid_probe_lookup_value(pr, "PTTYPE", &contents, NULL)) {
		printf(_("It appears to contain a partition table (%s).\n"), contents);
	}

	if (!S_ISREG(dev->stat.st_mode)) {
		blkid_topology tp = blkid_probe_get_topology(pr);
		if (tp != NULL) {
			dev->alignment_offset = blkid_topology_get_alignment_offset(tp);
			dev->logical_sector_size = blkid_topology_get_logical_sector_size(tp);
			dev->minimum_io_size = blkid_topology_get_minimum_io_size(tp);
			dev->optimal_io_size = blkid_topology_get_optimal_io_size(tp);
			dev->physical_sector_size = blkid_topology_get_physical_sector_size(tp);
			dev->got_topol = (dev->logical_sector_size != 0 &&
	                                  dev->physical_sector_size != 0);
		}
	}

	blkid_free_probe(pr);
	return 0;
}

static int open_dev(struct mkfs_dev *dev, int withprobe)
{
	int error;

	dev->fd = open(dev->path, O_RDWR|O_CLOEXEC|O_EXCL);
	if (dev->fd < 0) {
		perror(dev->path);
		return 1;
	}

	/* Freshen up the cache */
	(void)posix_fadvise(dev->fd, 0, 0, POSIX_FADV_DONTNEED);

	error = fstat(dev->fd, &dev->stat);
	if (error < 0) {
		perror(dev->path);
		return 1;
	}

	if (S_ISREG(dev->stat.st_mode)) {
		dev->size = dev->stat.st_size;
	} else if (S_ISBLK(dev->stat.st_mode)) {
		dev->size = lseek(dev->fd, 0, SEEK_END);
		if (dev->size < 1) {
			fprintf(stderr, _("Device '%s' is too small\n"), dev->path);
			return 1;
		}
	} else {
		fprintf(stderr, _("'%s' is not a block device or regular file\n"), dev->path);
		return 1;
	}
	if (withprobe && (probe_contents(dev) != 0))
		return 1;
	return 0;
}

#ifndef UNITTESTS
int main(int argc, char *argv[])
{
	struct gfs2_statfs_change sc;
	struct lgfs2_sbd sbd;
	struct mkfs_opts opts;
	struct lgfs2_inode *ip;
	lgfs2_rgrps_t rgs;
	uint64_t rgaddr;
	int error;
	unsigned bsize;

	setlocale(LC_ALL, "");
	textdomain("gfs2-utils");
	srandom(time(NULL) ^ getpid());

	opts_init(&opts);
	error = opts_get(argc, argv, &opts);
	if (error == 1)
		exit(0);
	if (error != 0)
		exit(-1);
	error = opts_check(&opts);
	if (error != 0)
		exit(error);

	error = open_dev(&opts.dev, !opts.got_topol);
	if (error != 0)
		exit(error);
	error = choose_blocksize(&opts, &bsize);
	if (error != 0)
		exit(-1);

	if (S_ISREG(opts.dev.stat.st_mode)) {
		opts.got_bsize = 1; /* Use default block size for regular files */
	}
	if (sbd_init(&sbd, &opts, bsize) != 0)
		exit(-1);
	if (opts.debug) {
		printf(_("File system options:\n"));
		printf("  bsize = %u\n", sbd.sd_bsize);
		printf("  qcsize = %u\n", opts.qcsize);
		printf("  jsize = %u\n", opts.jsize);
		printf("  journals = %u\n", sbd.md.journals);
		printf("  proto = %s\n", opts.lockproto);
		printf("  table = %s\n", opts.locktable);
		printf("  rgsize = %u\n", sbd.rgsize);
		printf("  fssize = %"PRIu64"\n", opts.fssize);
		printf("  sunit = %lu\n", opts.sunit);
		printf("  swidth = %lu\n", opts.swidth);
		printf("  format = %u\n", opts.format);
	}
	rgs = rgs_init(&opts, &sbd);
	if (rgs == NULL)
		exit(-1);
	if (warn_of_destruction(opts.dev.path) != 0)
		exit(-1);

	if (opts.confirm && !opts.override)
		if (!are_you_sure())
			exit(-1);

	if (!S_ISREG(opts.dev.stat.st_mode) && opts.discard) {
		if (!opts.quiet) {
			printf("%s", _("Discarding device contents (may take a while on large devices): "));
			fflush(stdout);
		}
		discard_blocks(opts.dev.fd, opts.dev.size, opts.debug);

		if (!opts.quiet)
			printf("%s", _("Done\n"));
	}
	rgaddr = lgfs2_rgrp_align_addr(rgs, LGFS2_SB_ADDR(&sbd) + 1);
	error = place_journals(&sbd, rgs, &opts, &rgaddr);
	if (error != 0) {
		fprintf(stderr, _("Failed to create journals\n"));
		exit(1);
	}
	error = place_rgrps(&sbd, rgs, &rgaddr, &opts);
	if (error) {
		fprintf(stderr, _("Failed to build resource groups\n"));
		exit(1);
	}
	lgfs2_attach_rgrps(&sbd, rgs); // Temporary

	error = lgfs2_build_master(&sbd);
	if (error) {
		fprintf(stderr, _("Error building '%s': %s\n"), "master", strerror(errno));
		exit(EXIT_FAILURE);
	}
	if (opts.debug) {
		printf("Metafs inode:\n");
		dinode_print(sbd.master_dir->i_bh->b_data);
	}
	sbd.sd_meta_dir = sbd.master_dir->i_num;

	error = create_jindex(&sbd, &opts, mkfs_journals);
	free(mkfs_journals);
	if (error != 0)
		exit(1);

	error = build_per_node(&sbd, &opts);
	if (error != 0)
		exit(1);

	sbd.md.inum = lgfs2_build_inum(&sbd);
	if (sbd.md.inum == NULL) {
		fprintf(stderr, _("Error building '%s': %s\n"), "inum", strerror(errno));
		exit(EXIT_FAILURE);
	}
	if (opts.debug) {
		printf("\nInum Inode:\n");
		dinode_print(sbd.md.inum->i_bh->b_data);
	}
	sbd.md.statfs = lgfs2_build_statfs(&sbd);
	if (sbd.md.statfs == NULL) {
		fprintf(stderr, _("Error building '%s': %s\n"), "statfs", strerror(errno));
		exit(EXIT_FAILURE);
	}
	if (opts.debug) {
		printf("\nStatFS Inode:\n");
		dinode_print(sbd.md.statfs->i_bh->b_data);
	}
	ip = lgfs2_build_rindex(&sbd);
	if (ip == NULL) {
		fprintf(stderr, _("Error building '%s': %s\n"), "rindex", strerror(errno));
		exit(EXIT_FAILURE);
	}
	if (opts.debug) {
		printf("\nResource Index:\n");
		dinode_print(ip->i_bh->b_data);
	}
	lgfs2_inode_put(&ip);
	if (!opts.quiet) {
		printf("%s", _("Creating quota file: "));
		fflush(stdout);
	}
	ip = lgfs2_build_quota(&sbd);
	if (ip == NULL) {
		fprintf(stderr, _("Error building '%s': %s\n"), "quota", strerror(errno));
		exit(EXIT_FAILURE);
	}
	if (opts.debug) {
		printf("\nQuota:\n");
		dinode_print(ip->i_bh->b_data);
	}
	lgfs2_inode_put(&ip);
	if (!opts.quiet)
		printf("%s", _("Done\n"));

	lgfs2_build_root(&sbd);
	if (opts.root_inherit_jd) {
		sbd.md.rooti->i_flags |= GFS2_DIF_INHERIT_JDATA;
		lgfs2_dinode_out(sbd.md.rooti, sbd.md.rooti->i_bh->b_data);
	}
	if (opts.debug) {
		printf("\nRoot directory:\n");
		dinode_print(sbd.md.rooti->i_bh->b_data);
	}
	sbd.sd_root_dir = sbd.md.rooti->i_num;

	strncpy(sbd.sd_lockproto, opts.lockproto, GFS2_LOCKNAME_LEN - 1);
	strncpy(sbd.sd_locktable, opts.locktable, GFS2_LOCKNAME_LEN - 1);
	sbd.sd_lockproto[GFS2_LOCKNAME_LEN - 1] = '\0';
	sbd.sd_locktable[GFS2_LOCKNAME_LEN - 1] = '\0';

	lgfs2_init_inum(&sbd);
	if (opts.debug)
		printf("\nNext Inum: %"PRIu64"\n", sbd.md.next_inum);

	lgfs2_init_statfs(&sbd, &sc);
	if (opts.debug) {
		printf("\nStatfs:\n");
		statfs_change_print(&sc);
	}
	lgfs2_inode_put(&sbd.md.rooti);
	lgfs2_inode_put(&sbd.master_dir);
	lgfs2_inode_put(&sbd.md.inum);
	lgfs2_inode_put(&sbd.md.statfs);

	lgfs2_rgrps_free(&rgs);

	if (!opts.quiet) {
		printf("%s", _("Writing superblock and syncing: "));
		fflush(stdout);
	}

	error = lgfs2_sb_write(&sbd, opts.dev.fd);
	if (error) {
		perror(_("Failed to write superblock\n"));
		exit(EXIT_FAILURE);
	}

	error = fsync(opts.dev.fd);
	if (error){
		perror(opts.dev.path);
		exit(EXIT_FAILURE);
	}
	error = close(opts.dev.fd);
	if (error){
		perror(opts.dev.path);
		exit(EXIT_FAILURE);
	}

	if (!opts.quiet) {
		printf("%s", _("Done\n"));
		print_results(&sbd, &opts);
	}
	return 0;
}
#endif /* UNITTESTS */
