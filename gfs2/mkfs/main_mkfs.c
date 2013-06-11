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

#define _(String) gettext(String)

#include <linux/types.h>
#include "libgfs2.h"
#include "gfs2_mkfs.h"

/**
 * This function is for libgfs2's sake.
 */
void print_it(const char *label, const char *fmt, const char *fmt2, ...)
{
	va_list args;

	va_start(args, fmt2);
	printf("%s: ", label);
	vprintf(fmt, args);
	va_end(args);
}

static void print_usage(const char *prog_name)
{
	int i;
	const char *option, *param, *desc;
	const char *options[] = {
	    /* Translators: This is a usage string printed with --help.
	       <size> and <number> here are  to commandline parameters,
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
		NULL, NULL
	};
	printf(_("Extended options:\n"));
	for (i = 0; options[i] != NULL; i += 2) {
		printf("%15s  %-22s\n", options[i], options[i + 1]);
	}
}

struct mkfs_opts {
	unsigned bsize;
	unsigned qcsize;
	unsigned jsize;
	unsigned rgsize;
	unsigned long sunit;
	unsigned long swidth;
	uint64_t fssize;
	uint32_t journals;
	const char *lockproto;
	const char *locktable;
	const char *device;
	unsigned discard:1;

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

	unsigned override:1;
	unsigned quiet:1;
	unsigned expert:1;
	unsigned debug:1;
	unsigned confirm:1;
	unsigned align:1;
};

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
	struct stat stat;
	uint64_t size;
	unsigned long alignment_offset;
	unsigned long logical_sector_size;
	unsigned long minimum_io_size;
	unsigned long optimal_io_size;
	unsigned long physical_sector_size;

	unsigned int got_topol:1;
};

/**
 * A representation of the state of resource group calculation. Allows mkfs to create
 * resource groups at any point instead of creating them all in one batch.
 */
struct mkfs_rgs {
	struct osi_root *root;
	uint64_t nextaddr;
	unsigned bsize;
	unsigned long align;
	unsigned long align_off;
	unsigned long curr_offset;
	uint64_t maxrgsz;
	uint64_t minrgsz;
	uint64_t devlen;
	uint64_t rgsize;
	uint64_t count;
	uint64_t blks_total;
};

static void opts_init(struct mkfs_opts *opts)
{
	memset(opts, 0, sizeof(*opts));
	opts->discard = 1;
	opts->journals = 1;
	opts->bsize = GFS2_DEFAULT_BSIZE;
	opts->jsize = GFS2_DEFAULT_JSIZE;
	opts->qcsize = GFS2_DEFAULT_QCSIZE;
	opts->rgsize = GFS2_DEFAULT_RGSIZE;
	opts->lockproto = "lock_dlm";
	opts->locktable = "";
	opts->confirm = 1;
	opts->align = 1;
}

#ifndef BLKDISCARD
#define BLKDISCARD      _IO(0x12,119)
#endif

static int discard_blocks(int fd, uint64_t len, int debug)
{
        __uint64_t range[2];

	range[0] = 0;
	range[1] = len;
	if (debug)
		/* Translators: "discard" is a request sent to a storage device to
		 * discard a range of blocks. */
		printf(_("Issuing discard request: range: %llu - %llu..."),
		       (unsigned long long)range[0],
		       (unsigned long long)range[1]);
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
		exit(1);
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

static unsigned long parse_ulong(struct mkfs_opts *opts, const char *key, const char *val)
{
	long long l;
	if (val == NULL || *val == '\0') {
		fprintf(stderr, _("Missing argument to '%s'\n"), key);
		exit(-1);
	}
	l = cvtnum(opts->bsize, 0, val);
	if (l > ULONG_MAX || l < 0) {
		fprintf(stderr, _("Value of '%s' is invalid\n"), key);
		exit(-1);
	}
	return (unsigned long)l;
}

static unsigned parse_bool(struct mkfs_opts *opts, const char *key, const char *val)
{
	if (strnlen(val, 2) == 1) {
		if (*val == '0')
			return 0;
		if (*val == '1')
			return 1;
	}
	fprintf(stderr, _("Option '%s' must be either 1 or 0\n"), key);
	exit(-1);
}

static void opt_parse_extended(char *str, struct mkfs_opts *opts)
{
	char *opt;
	while ((opt = strsep(&str, ",")) != NULL) {
		char *key = strsep(&opt, "=");
		char *val = strsep(&opt, "=");
		if (key == NULL || *key == '\0') {
			fprintf(stderr, _("Missing argument to '-o' option\n"));
			exit(-1);
		}
		if (strcmp("sunit", key) == 0) {
			opts->sunit = parse_ulong(opts, "sunit", val);
			opts->got_sunit = 1;
		} else if (strcmp("swidth", key) == 0) {
			opts->swidth = parse_ulong(opts, "swidth", val);
			opts->got_swidth = 1;
		} else if (strcmp("align", key) == 0) {
			opts->align = parse_bool(opts, "align", val);
		} else if (strcmp("help", key) == 0) {
			print_ext_opts();
			exit(0);
		} else {
			fprintf(stderr, _("Invalid option '%s'\n"), key);
			exit(-1);
		}
	}
}

static void opts_get(int argc, char *argv[], struct mkfs_opts *opts)
{
	int c;
	while (1) {
		c = getopt(argc, argv, "-b:c:DhJ:j:KOo:p:qr:t:VX");
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
			exit(0);
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
			opt_parse_extended(optarg, opts);
			break;
		case 'V':
			printf("mkfs.gfs2 %s (built %s %s)\n", VERSION,
			       __DATE__, __TIME__);
			printf(REDHAT_COPYRIGHT "\n");
			exit(EXIT_SUCCESS);
			break;
		case 'X':
			opts->expert = 1;
			break;
		case ':':
		case '?':
			fprintf(stderr, _("Please use '-h' for help.\n"));
			exit(EXIT_FAILURE);
			break;
		case 1:
			if (strcmp(optarg, "gfs2") == 0)
				continue;
			if (!opts->got_device) {
				opts->device = optarg;
				opts->got_device = 1;
			} else if (!opts->got_fssize && isdigit(optarg[0])) {
				opts->fssize = atol(optarg);
				opts->got_fssize = 1;
			} else
				die( _("More than one device specified (try -h for help)\n"));
			break;
		default:
			die( _("Invalid option: %c\n"), c);
			break;
		};
	}
}

/**
 * test_locking - Make sure the GFS2 is set up to use the right lock protocol
 * @lockproto: the lock protocol to mount
 * @locktable: the locktable name
 *
 */

static void test_locking(const char *lockproto, const char *locktable)
{
	const char *c;
	/* Translators: A lock table is a string identifying a gfs2 file system
	 * in a cluster, e.g. cluster_name:fs_name */
	const char *errprefix = _("Invalid lock table:");

	if (strcmp(lockproto, "lock_nolock") == 0) {
		/*  Nolock is always ok.  */
	} else if (strcmp(lockproto, "lock_gulm") == 0 ||
		   strcmp(lockproto, "lock_dlm") == 0) {
		if (locktable == NULL || *locktable == '\0') {
			fprintf(stderr, _("No lock table specified.\n"));
			exit(-1);
		}
		for (c = locktable; *c; c++) {
			if (!isalnum(*c) && (*c != '-') && (*c != '_') && (*c != ':'))
				die("%s %s '%c'\n", errprefix, _("invalid character"), *c);
		}

		c = strstr(locktable, ":");
		if (!c)
			die("%s %s\n", errprefix, _("missing colon"));

		if (c == locktable)
			die("%s %s\n", errprefix, _("cluster name is missing"));
		if (c - locktable > 16)
			die("%s %s\n", errprefix, _("cluster name is too long"));

		c++;
		if (strstr(c, ":"))
			die("%s %s\n", errprefix, _("contains more than one colon"));
		if (!strlen(c))
			die("%s %s\n", errprefix, _("file system name is missing"));
		if (strlen(c) > 16)
			die("%s %s\n", errprefix, _("file system name is too long"));
	} else {
		die( _("Invalid lock protocol: %s\n"), lockproto);
	}
}

static void are_you_sure(void)
{
	char *line = NULL;
	size_t len = 0;
	int ret = -1;
	int res = 0;

	do{
		/* Translators: We use rpmatch(3) to match the answers to y/n
		   questions in the user's own language, so the [y/n] here must also be
		   translated to match one of the letters in the pattern printed by
		   `locale -k yesexpr` and one of the letters in the pattern printed by
		   `locale -k noexpr` */
		printf( _("Are you sure you want to proceed? [y/n]"));
		ret = getline(&line, &len, stdin);
		res = rpmatch(line);
		
		if (res > 0){
			free(line);
			return;
		}
		if (!res){
			printf("\n");
			die( _("Aborted.\n"));
		}
		
	}while(ret >= 0);

	if(line)
		free(line);
}

static unsigned choose_blocksize(struct mkfs_opts *opts, const struct mkfs_dev *dev)
{
	unsigned int x;
	unsigned int bsize = opts->bsize;

	if (dev->got_topol && opts->debug) {
		printf("alignment_offset: %lu\n", dev->alignment_offset);
		printf("logical_sector_size: %lu\n", dev->logical_sector_size);
		printf("minimum_io_size: %lu\n", dev->minimum_io_size);
		printf("optimal_io_size: %lu\n", dev->optimal_io_size);
		printf("physical_sector_size: %lu\n", dev->physical_sector_size);
	}

	if (!opts->got_bsize && dev->got_topol) {
		if (dev->optimal_io_size <= getpagesize() &&
		    dev->optimal_io_size >= dev->minimum_io_size)
			bsize = dev->optimal_io_size;
		else if (dev->physical_sector_size <= getpagesize() &&
		         dev->physical_sector_size >= GFS2_DEFAULT_BSIZE)
			bsize = dev->physical_sector_size;
	}

	/* Block sizes must be a power of two from 512 to 65536 */
	for (x = 512; x; x <<= 1)
		if (x == bsize)
			break;

	if (!x || bsize > getpagesize())
		die( _("Block size must be a power of two between 512 and %d\n"),
		       getpagesize());

	if (bsize < dev->logical_sector_size) {
		die( _("Error: Block size %d is less than minimum logical "
		       "block size (%lu).\n"), bsize, dev->logical_sector_size);
	}

	if (bsize < dev->physical_sector_size) {
		printf( _("Warning: Block size %d is inefficient because it "
			  "is less than the physical block size (%lu).\n"),
			  bsize, dev->physical_sector_size);
		opts->confirm = 1;
	}
	return bsize;
}

static void opts_check(struct mkfs_opts *opts)
{
	if (!opts->got_device) {
		fprintf(stderr, _("No device specified. Use -h for help\n"));
		exit(1);
	}

	if (!opts->expert)
		test_locking(opts->lockproto, opts->locktable);
	if (opts->expert) {
		if (GFS2_EXP_MIN_RGSIZE > opts->rgsize || opts->rgsize > GFS2_MAX_RGSIZE)
			/* Translators: gfs2 file systems are split into equal sized chunks called
			   resource groups. We're checking that the user gave a valid size for them. */
			die( _("bad resource group size\n"));
	} else {
		if (GFS2_MIN_RGSIZE > opts->rgsize || opts->rgsize > GFS2_MAX_RGSIZE)
			die( _("bad resource group size\n"));
	}

	if (!opts->journals)
		die( _("no journals specified\n"));

	if (opts->jsize < 8 || opts->jsize > 1024)
		die( _("bad journal size\n"));

	if (!opts->qcsize || opts->qcsize > 64)
		die( _("bad quota change size\n"));

	if ((opts->got_sunit && !opts->got_swidth) || (!opts->got_sunit && opts->got_swidth)) {
		fprintf(stderr, _("Stripe unit and stripe width must be specified together\n"));
		exit(1);
	}
}

static void print_results(struct gfs2_sbd *sdp, uint64_t real_device_size,
                          struct mkfs_opts *opts, unsigned char uuid[16])
{
	printf("%-27s%s\n", _("Device:"), opts->device);
	printf("%-27s%u\n", _("Block size:"), sdp->bsize);
	printf("%-27s%.2f %s (%llu %s)\n", _("Device size:"),
	       /* Translators: "GB" here means "gigabytes" */
	       real_device_size / ((float)(1 << 30)), _("GB"),
	       (unsigned long long)real_device_size / sdp->bsize, _("blocks"));
	printf("%-27s%.2f %s (%llu %s)\n", _("Filesystem size:"),
	       sdp->fssize / ((float)(1 << 30)) * sdp->bsize, _("GB"),
	       (unsigned long long)sdp->fssize, _("blocks"));
	printf("%-27s%u\n", _("Journals:"), sdp->md.journals);
	printf("%-27s%llu\n", _("Resource groups:"), (unsigned long long)sdp->rgrps);
	printf("%-27s\"%s\"\n", _("Locking protocol:"), sdp->lockproto);
	printf("%-27s\"%s\"\n", _("Lock table:"), sdp->locktable);
	/* Translators: "UUID" = universally unique identifier. */
	printf("%-27s%s\n", _("UUID:"), str_uuid(uuid));
}

static void warn_of_destruction(const char *path)
{
	struct stat lnkstat;
	char *abspath = NULL;

	if (lstat(path, &lnkstat) == -1) {
		perror(_("Failed to lstat the device"));
		exit(EXIT_FAILURE);
	}
	if (S_ISLNK(lnkstat.st_mode)) {
		abspath = canonicalize_file_name(path);
		if (abspath == NULL) {
			perror(_("Could not find the absolute path of the device"));
			exit(EXIT_FAILURE);
		}
		/* Translators: Example: "/dev/vg/lv is a symbolic link to /dev/dm-2" */
		printf( _("%s is a symbolic link to %s\n"), path, abspath);
		path = abspath;
	}
	printf(_("This will destroy any data on %s\n"), path);
	free(abspath);
}

static lgfs2_rgrps_t rgs_init(struct mkfs_opts *opts, struct mkfs_dev *dev, struct gfs2_sbd *sdp)
{
	uint64_t rgsize = (opts->rgsize << 20) / sdp->bsize;
	struct lgfs2_rgrp_align align = {.base = 0, .offset = 0};
	lgfs2_rgrps_t rgs;

	if (opts->align && opts->got_sunit) {
		if ((opts->sunit % sdp->bsize) != 0) {
			fprintf(stderr, _("Stripe unit (%lu) must be a multiple of block size (%u)\n"),
			        opts->sunit, sdp->bsize);
			exit(1);
		} else if ((opts->swidth % opts->sunit) != 0) {
			fprintf(stderr, _("Stripe width (%lu) must be a multiple of stripe unit (%lu)\n"),
			        opts->swidth, opts->sunit);
			exit(1);
		} else {
			align.base = opts->swidth / sdp->bsize;
			align.offset = opts->sunit / sdp->bsize;
		}
	} else if (opts->align) {
		if ((dev->minimum_io_size > dev->physical_sector_size) &&
		    (dev->optimal_io_size > dev->physical_sector_size)) {
			align.base = dev->optimal_io_size / sdp->bsize;
			align.offset = dev->minimum_io_size / sdp->bsize;
		}
	}

	rgs = lgfs2_rgrps_init(sdp->bsize, sdp->sb_addr + 1, sdp->device.length, rgsize, &align);
	if (rgs == NULL) {
		perror(_("Could not initialise resource groups"));
		exit(-1);
	}

	if (opts->debug) {
		printf("  rgrp align = ");
		if (opts->align)
			printf("%lu+%lu blocks\n", align.base, align.offset);
		else
			printf("(disabled)\n");
	}

	return rgs;
}

static uint64_t place_rgrps(struct gfs2_sbd *sdp, lgfs2_rgrps_t rgs, struct mkfs_opts *opts, const struct mkfs_dev *dev)
{
	int err = 0;
	lgfs2_rgrp_t rg = NULL;
	struct gfs2_rindex *ri = NULL;

	while (!lgfs2_rgrps_end(rgs)) {
		rg = lgfs2_rgrp_append(rgs, 0, !opts->got_rgsize);
		if (rg == NULL) {
			perror(_("Failed to create resource group"));
			return 0;
		}
		err = lgfs2_rgrp_write(sdp->device_fd, rg, sdp->bsize);
		if (err != 0) {
			perror(_("Failed to write resource group"));
			return 0;
		}
		ri = lgfs2_rgrp_index(rg);
		if (opts->debug) {
			gfs2_rindex_print(ri);
			printf("\n");
		}
		sdp->blks_total += ri->ri_data;
		sdp->rgrps++;
	}

	if (ri == NULL)
		return 0;
	else
		return ri->ri_data0 + ri->ri_data;
}

static void sbd_init(struct gfs2_sbd *sdp, struct mkfs_opts *opts, struct mkfs_dev *dev, unsigned bsize)
{
	memset(sdp, 0, sizeof(struct gfs2_sbd));
	sdp->time = time(NULL);
	sdp->rgtree.osi_node = NULL;
	sdp->rgsize = opts->rgsize;
	sdp->qcsize = opts->qcsize;
	sdp->jsize = opts->jsize;
	sdp->md.journals = opts->journals;
	sdp->device_fd = dev->fd;
	sdp->bsize = bsize;

	if (compute_constants(sdp)) {
		perror(_("Failed to compute file system constants"));
		exit(1);
	}
	sdp->device.length = dev->size / sdp->bsize;
	if (opts->got_fssize) {
		if (opts->fssize > sdp->device.length) {
			fprintf(stderr, _("Specified size is bigger than the device."));
			die("%s %.2f %s (%"PRIu64" %s)\n", _("Device size:"),
			       dev->size / ((float)(1 << 30)), _("GB"),
			       dev->size / sdp->bsize, _("blocks"));
		}
		/* TODO: Check if the fssize is too small, somehow */
		sdp->device.length = opts->fssize;
	}
	strcpy(sdp->lockproto, opts->lockproto);
	strcpy(sdp->locktable, opts->locktable);
}

static int probe_contents(struct mkfs_dev *dev)
{
	int ret;
	const char *contents;
	blkid_probe pr = blkid_new_probe();
	if (pr == NULL || blkid_probe_set_device(pr, dev->fd, 0, 0) != 0
	               || blkid_probe_enable_superblocks(pr, TRUE) != 0
	               || blkid_probe_enable_partitions(pr, TRUE) != 0) {
		fprintf(stderr, _("Failed to create probe\n"));
		return -1;
	}

	if (!S_ISREG(dev->stat.st_mode) && blkid_probe_enable_topology(pr, TRUE) != 0) {
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
			dev->got_topol = 1;
		}
	}

	blkid_free_probe(pr);
	return 0;
}

static void open_dev(const char *path, struct mkfs_dev *dev)
{
	int error;

	memset(dev, 0, sizeof(*dev));
	dev->fd = open(path, O_RDWR | O_CLOEXEC);
	if (dev->fd < 0) {
		perror(path);
		exit(1);
	}

	error = fstat(dev->fd, &dev->stat);
	if (error < 0) {
		perror(path);
		exit(1);
	}

	if (S_ISREG(dev->stat.st_mode)) {
		dev->size = dev->stat.st_size;
	} else if (S_ISBLK(dev->stat.st_mode)) {
		dev->size = lseek(dev->fd, 0, SEEK_END);
		if (dev->size < 1) {
			fprintf(stderr, _("Device '%s' is too small\n"), path);
			exit(1);
		}
	} else {
		fprintf(stderr, _("'%s' is not a block device or regular file\n"), path);
		exit(1);
	}

	error = probe_contents(dev);
	if (error)
		exit(1);
}

void main_mkfs(int argc, char *argv[])
{
	struct gfs2_sbd sbd;
	struct mkfs_opts opts;
	struct mkfs_dev dev;
	lgfs2_rgrps_t rgs;
	int error;
	unsigned char uuid[16];
	unsigned bsize;

	opts_init(&opts);
	opts_get(argc, argv, &opts);
	opts_check(&opts);

	open_dev(opts.device, &dev);
	bsize = choose_blocksize(&opts, &dev);

	if (S_ISREG(dev.stat.st_mode)) {
		opts.got_bsize = 1; /* Use default block size for regular files */
	}

	sbd_init(&sbd, &opts, &dev, bsize);
	if (opts.debug) {
		printf(_("File system options:\n"));
		printf("  bsize = %u\n", sbd.bsize);
		printf("  qcsize = %u\n", sbd.qcsize);
		printf("  jsize = %u\n", sbd.jsize);
		printf("  journals = %u\n", sbd.md.journals);
		printf("  proto = %s\n", sbd.lockproto);
		printf("  rgsize = %u\n", sbd.rgsize);
		printf("  table = %s\n", sbd.locktable);
		printf("  fssize = %"PRIu64"\n", opts.fssize);
		printf("  sunit = %lu\n", opts.sunit);
		printf("  swidth = %lu\n", opts.swidth);
	}
	rgs = rgs_init(&opts, &dev, &sbd);
	warn_of_destruction(opts.device);

	if (opts.confirm && !opts.override)
		are_you_sure();

	if (!S_ISREG(dev.stat.st_mode) && opts.discard)
		discard_blocks(dev.fd, dev.size, opts.debug);

	sbd.fssize = place_rgrps(&sbd, rgs, &opts, &dev);
	if (sbd.fssize == 0) {
		fprintf(stderr, _("Failed to build resource groups\n"));
		exit(1);
	}
	sbd.rgtree.osi_node = lgfs2_rgrps_root(rgs); // Temporary
	build_root(&sbd);
	build_master(&sbd);
	error = build_jindex(&sbd);
	if (error) {
		fprintf(stderr, _("Error building '%s': %s\n"), "jindex", strerror(errno));
		exit(EXIT_FAILURE);
	}
	error = build_per_node(&sbd);
	if (error) {
		fprintf(stderr, _("Error building '%s': %s\n"), "per_node", strerror(errno));
		exit(EXIT_FAILURE);
	}
	error = build_inum(&sbd);
	if (error) {
		fprintf(stderr, _("Error building '%s': %s\n"), "inum", strerror(errno));
		exit(EXIT_FAILURE);
	}
	gfs2_lookupi(sbd.master_dir, "inum", 4, &sbd.md.inum);
	error = build_statfs(&sbd);
	if (error) {
		fprintf(stderr, _("Error building '%s': %s\n"), "statfs", strerror(errno));
		exit(EXIT_FAILURE);
	}
	gfs2_lookupi(sbd.master_dir, "statfs", 6, &sbd.md.statfs);
	error = build_rindex(&sbd);
	if (error) {
		fprintf(stderr, _("Error building '%s': %s\n"), "rindex", strerror(errno));
		exit(EXIT_FAILURE);
	}
	error = build_quota(&sbd);
	if (error) {
		fprintf(stderr, _("Error building '%s': %s\n"), "quota", strerror(errno));
		exit(EXIT_FAILURE);
	}
	get_random_bytes(uuid, sizeof(uuid));
	build_sb(&sbd, uuid);

	do_init_inum(&sbd);
	do_init_statfs(&sbd);

	inode_put(&sbd.md.rooti);
	inode_put(&sbd.master_dir);
	inode_put(&sbd.md.inum);
	inode_put(&sbd.md.statfs);

	gfs2_rgrp_free(&sbd.rgtree);
	error = fsync(dev.fd);
	if (error){
		perror(opts.device);
		exit(EXIT_FAILURE);
	}

	error = close(dev.fd);
	if (error){
		perror(opts.device);
		exit(EXIT_FAILURE);
	}

	if (!opts.quiet)
		print_results(&sbd, dev.size, &opts, uuid);
}
