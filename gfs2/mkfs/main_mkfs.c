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

#define _(String) gettext(String)

#include <linux/types.h>
#include "libgfs2.h"
#include "gfs2_mkfs.h"

int discard = 1;

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

/**
 * print_usage - print out usage information
 * @prog_name: The name of this program
 */

static void
print_usage(const char *prog_name)
{
	printf( _("Usage:\n\n"
		"%s [options] <device> [ block-count ]\n\n"
		"Options:\n\n"
		"  -b <bytes>       Filesystem block size\n"
		"  -c <MB>          Size of quota change file\n"
		"  -D               Enable debugging code\n"
		"  -h               Print this help, then exit\n"
		"  -J <MB>          Size of journals\n"
		"  -j <num>         Number of journals\n"
		"  -K               Don't try to discard unused blocks\n"
		"  -O               Don't ask for confirmation\n"
		"  -p <name>        Name of the locking protocol\n"
		"  -q               Don't print anything\n"
		"  -r <MB>          Resource Group Size\n"
		"  -t <name>        Name of the lock table\n"
		"  -V               Print program version information, then exit\n"), prog_name);
}

#ifndef BLKDISCARD
#define BLKDISCARD      _IO(0x12,119)
#endif

static int discard_blocks(struct gfs2_sbd *sdp)
{
        __uint64_t range[2];

	range[0] = 0;
	range[1] = sdp->device.length * sdp->bsize;
	if (sdp->debug)
		printf("Issuing discard ioctl: range: %llu - %llu...",
		       (unsigned long long)range[0],
		       (unsigned long long)range[1]);
	if (ioctl(sdp->device_fd, BLKDISCARD, &range) < 0) {
		if (sdp->debug)
			printf("error = %d\n", errno);
		return errno;
	}
	if (sdp->debug)
		printf("Successful.\n");
        return 0;
}

/**
 * decode_arguments - decode command line arguments and fill in the struct gfs2_sbd
 * @argc:
 * @argv:
 * @sdp: the decoded command line arguments
 *
 */

static void decode_arguments(int argc, char *argv[], struct gfs2_sbd *sdp)
{
	int cont = TRUE;
	int optchar;

	memset(sdp->device_name, 0, sizeof(sdp->device_name));
	sdp->md.journals = 1;
	sdp->orig_fssize = 0;

	while (cont) {
		optchar = getopt(argc, argv, "-b:c:DhJ:j:Op:qr:t:u:VX");

		switch (optchar) {
		case 'b':
			sdp->bsize = atoi(optarg);
			break;

		case 'c':
			sdp->qcsize = atoi(optarg);
			break;

		case 'D':
			sdp->debug = TRUE;
			break;

		case 'h':
			print_usage(argv[0]);
			exit(0);
			break;

		case 'J':
			sdp->jsize = atoi(optarg);
			break;

		case 'j':
			sdp->md.journals = atoi(optarg);
			break;

		case 'K':
			discard = 0;
			break;

		case 'O':
			sdp->override = TRUE;
			break;

		case 'p':
			if (strlen(optarg) >= GFS2_LOCKNAME_LEN)
				die( _("lock protocol name %s is too long\n"),
				    optarg);
			strcpy(sdp->lockproto, optarg);
			break;

		case 'q':
			sdp->quiet = TRUE;
			break;

		case 'r':
			sdp->rgsize = atoi(optarg);
			break;

		case 't':
			if (strlen(optarg) >= GFS2_LOCKNAME_LEN)
				die( _("lock table name %s is too long\n"), optarg);
			strcpy(sdp->locktable, optarg);
			break;

		case 'u':
			break;

		case 'V':
			printf("gfs2_mkfs %s (built %s %s)\n", VERSION,
			       __DATE__, __TIME__);
			printf(REDHAT_COPYRIGHT "\n");
			exit(EXIT_SUCCESS);
			break;

		case 'X':
			sdp->expert = TRUE;
			break;

		case ':':
		case '?':
			fprintf(stderr, _("Please use '-h' for help.\n"));
			exit(EXIT_FAILURE);
			break;

		case EOF:
			cont = FALSE;
			break;

		case 1:
			if (strcmp(optarg, "gfs2") == 0)
				continue;
			if (!sdp->device_name[0])
				strcpy(sdp->device_name, optarg);
			else if (!sdp->orig_fssize &&
				 isdigit(optarg[0]))
				sdp->orig_fssize = atol(optarg);
			else
				die( _("More than one device specified (try -h for help)\n"));
			break;

		default:
			die( _("Invalid option: %c\n"), optchar);
			break;
		};
	}

	if ((sdp->device_name[0] == 0) && (optind < argc))
		strcpy(sdp->device_name, argv[optind++]);

	if (sdp->device_name[0] == '\0')
		die( _("No device specified. Please use '-h' for help\n"));

	if (optind < argc)
		sdp->orig_fssize = atol(argv[optind++]);

	if (optind < argc)
		die( _("Unrecognized argument: %s\n"), argv[optind]);

	if (sdp->debug) {
		printf( _("Command Line Arguments:\n"));
		if (sdp->bsize != -1)
			printf("  bsize = %u\n", sdp->bsize);
		printf("  qcsize = %u\n", sdp->qcsize);
		printf("  jsize = %u\n", sdp->jsize);
		printf("  journals = %u\n", sdp->md.journals);
		printf("  override = %d\n", sdp->override);
		printf("  proto = %s\n", sdp->lockproto);
		printf("  quiet = %d\n", sdp->quiet);
		if (sdp->rgsize==-1)
			printf( _("  rgsize = optimize for best performance\n"));
		else
			printf("  rgsize = %u\n", sdp->rgsize);
		printf("  table = %s\n", sdp->locktable);
		printf("  device = %s\n", sdp->device_name);
		if (sdp->orig_fssize)
			printf("  block-count = %llu\n",
			       (unsigned long long)sdp->orig_fssize);
	}
}

/**
 * test_locking - Make sure the GFS2 is set up to use the right lock protocol
 * @lockproto: the lock protocol to mount
 * @locktable: the locktable name
 *
 */

static void test_locking(char *lockproto, char *locktable)
{
	char *c;

	if (strcmp(lockproto, "lock_nolock") == 0) {
		/*  Nolock is always ok.  */
	} else if (strcmp(lockproto, "lock_gulm") == 0 ||
		   strcmp(lockproto, "lock_dlm") == 0) {
		for (c = locktable; *c; c++) {
			if (isspace(*c))
				die( _("locktable error: contains space characters\n"));
			if (!isprint(*c))
				die( _("locktable error: contains unprintable characters\n"));
		}

		c = strstr(locktable, ":");
		if (!c)
			die( _("locktable error: missing colon in the locktable\n"));

		if (c == locktable)
			die( _("locktable error: missing cluster name\n"));
		if (c - locktable > 16)
			die( _("locktable error: cluster name too long\n"));

		c++;
		if (!c)
			die( _("locktable error: missing filesystem name\n"));

		if (strstr(c, ":"))
			die( _("locktable error: more than one colon present\n"));

		if (!strlen(c))
			die( _("locktable error: missing filesystem name\n"));
		if (strlen(c) > 16)
			die( _("locktable error: filesystem name too long\n"));
	} else {
		die( _("lockproto error: %s unknown\n"), lockproto);
	}
}

static void verify_bsize(struct gfs2_sbd *sdp)
{
	unsigned int x;
	char input[32];

	/* Block sizes must be a power of two from 512 to 65536 */

	for (x = 512; x; x <<= 1)
		if (x == sdp->bsize)
			break;

	if (!x || sdp->bsize > getpagesize())
		die( _("block size must be a power of two between 512 and "
		       "%d\n"), getpagesize());

	if (sdp->bsize < sdp->logical_block_size) {
		die( _("Error: Block size %d is less than minimum logical "
		       "block size (%d).\n"), sdp->bsize,
		     sdp->logical_block_size);
	}

	if (sdp->bsize < sdp->physical_block_size) {
		printf( _("WARNING: Block size %d is inefficient because it "
			  "is less than the physical block size (%d).\n"),
			  sdp->bsize, sdp->physical_block_size);
		if (sdp->override)
			return;

		printf( _("\nAre you sure you want to proceed? [y/n] "));
		if(!fgets(input, 32, stdin))
			die( _("unable to read from stdin\n"));

		if (input[0] != 'y')
			die( _("aborted\n"));
		else
			printf("\n");
	}
}

static void verify_arguments(struct gfs2_sbd *sdp)
{
	if (!sdp->expert)
		test_locking(sdp->lockproto, sdp->locktable);
	if (sdp->expert) {
		if (GFS2_EXP_MIN_RGSIZE > sdp->rgsize || sdp->rgsize > GFS2_MAX_RGSIZE)
			die( _("bad resource group size\n"));
	} else {
		if (GFS2_MIN_RGSIZE > sdp->rgsize || sdp->rgsize > GFS2_MAX_RGSIZE)
			die( _("bad resource group size\n"));
	}

	if (!sdp->md.journals)
		die( _("no journals specified\n"));

	if (sdp->jsize < 8 || sdp->jsize > 1024)
		die( _("bad journal size\n"));

	if (!sdp->qcsize || sdp->qcsize > 64)
		die( _("bad quota change size\n"));
}

static int get_file_output(int fd, char *buffer, size_t buflen)
{
	struct pollfd pf = { .fd = fd, .events = POLLIN|POLLRDHUP };
	int flags;
	int pos = 0;
	int rv;

	flags = fcntl(fd, F_GETFL, 0);
	if (flags < 0)
		return flags;

	flags |= O_NONBLOCK;
	rv = fcntl(fd, F_SETFL, flags);
	if (rv < 0)
		return rv;

	while (1) {
		rv = poll(&pf, 1, 10 * 1000);
		if (rv == 0)
			break;
		if (rv < 0)
			return rv;
		if (pf.revents & POLLIN) {
			rv = read(fd, buffer + pos,
				   buflen - pos);
			if (rv < 0) {
				if (errno == EAGAIN)
					continue;
				return rv;
			}
			if (rv == 0)
				break;
			pos += rv;
			if (pos >= buflen)
				return -1;
			buffer[pos] = 0;
			continue;
		}
		if (pf.revents & (POLLRDHUP | POLLHUP | POLLERR))
			break;
	}
	return 0;
}

static void check_dev_content(const char *devname)
{
	struct sigaction sa;
	char content[1024] = { 0, };
	char * args[] = {
		(char *)"/usr/bin/file",
		(char *)"-bs",
		(char *)devname,
		NULL };
	int p[2];
	int ret;
	int pid;

	ret = sigaction(SIGCHLD, NULL, &sa);
	if  (ret)
		return;
	sa.sa_handler = SIG_IGN;
	sa.sa_flags |= (SA_NOCLDSTOP | SA_NOCLDWAIT);
	ret = sigaction(SIGCHLD, &sa, NULL);
	if (ret)
		goto fail;

	ret = pipe(p);
	if (ret)
		goto fail;

	pid = fork();

	if (pid < 0) {
		close(p[1]);
		goto fail;
	}

	if (pid) {
		close(p[1]);
		ret = get_file_output(p[0], content, sizeof(content));
		if (ret) {
fail:
			printf( _("Content of file or device unknown (do you have GNU fileutils installed?)\n"));
		} else {
			if (*content == 0)
				goto fail;
			printf( _("It appears to contain: %s"), content);
		}
		close(p[0]);
		return;
	}

	close(p[0]);
	dup2(p[1], STDOUT_FILENO);
	close(STDIN_FILENO);
	exit(execv(args[0], args));
}

/**
 * are_you_sure - protect lusers from themselves
 * @sdp: the command line
 *
 */

static void are_you_sure(struct gfs2_sbd *sdp)
{
	char input[32];
	int fd;

	fd = open(sdp->device_name, O_RDONLY|O_CLOEXEC);
	if (fd < 0)
		die( _("Error: device %s not found.\n"), sdp->device_name);
	printf( _("This will destroy any data on %s.\n"), sdp->device_name);
	check_dev_content(sdp->device_name);
	close(fd);
	printf( _("\nAre you sure you want to proceed? [y/n] "));
	if(!fgets(input, 32, stdin))
		die( _("unable to read from stdin\n"));

	if (input[0] != 'y')
		die( _("aborted\n"));
	else
		printf("\n");
}

/**
 * print_results - print out summary information
 * @sdp: the command line
 *
 */

static void
print_results(struct gfs2_sbd *sdp, uint64_t real_device_size,
	      unsigned char uuid[16])
{
	if (sdp->debug)
		printf("\n");
	else if (sdp->quiet)
		return;

	if (sdp->expert)
		printf( _("Expert mode:               on\n"));

	printf( _("Device:                    %s\n"), sdp->device_name);

	printf( _("Blocksize:                 %u\n"), sdp->bsize);
	printf( _("Device Size                %.2f GB (%llu blocks)\n"),
	       real_device_size / ((float)(1 << 30)),
	       (unsigned long long)real_device_size / sdp->bsize);
	printf( _("Filesystem Size:           %.2f GB (%llu blocks)\n"),
	       sdp->fssize / ((float)(1 << 30)) * sdp->bsize,
	       (unsigned long long)sdp->fssize);
	printf( _("Journals:                  %u\n"), sdp->md.journals);
	printf( _("Resource Groups:           %llu\n"),
	       (unsigned long long)sdp->rgrps);
	printf( _("Locking Protocol:          \"%s\"\n"), sdp->lockproto);
	printf( _("Lock Table:                \"%s\"\n"), sdp->locktable);

	if (sdp->debug) {
		printf("\n");
		printf( _("Writes:                    %u\n"), sdp->writes);
	}

	printf( _("UUID:                      %s\n"), str_uuid(uuid));
	printf("\n");
}

/**
 * main_mkfs - do everything
 * @argc:
 * @argv:
 *
 */

void main_mkfs(int argc, char *argv[])
{
	struct gfs2_sbd sbd, *sdp = &sbd;
	int error;
	int rgsize_specified = 0;
	uint64_t real_device_size;
	unsigned char uuid[16];
	struct stat st_buf;

	memset(sdp, 0, sizeof(struct gfs2_sbd));
	sdp->bsize = -1;
	sdp->jsize = GFS2_DEFAULT_JSIZE;
	sdp->rgsize = -1;
	sdp->qcsize = GFS2_DEFAULT_QCSIZE;
	strcpy(sdp->lockproto, GFS2_DEFAULT_LOCKPROTO);
	sdp->time = time(NULL);
	sdp->rgtree.osi_node = NULL;

	decode_arguments(argc, argv, sdp);
	if (sdp->rgsize == -1)                 /* if rg size not specified */
		sdp->rgsize = GFS2_DEFAULT_RGSIZE; /* default it for now */
	else
		rgsize_specified = TRUE;

	verify_arguments(sdp);

	sdp->device_fd = open(sdp->device_name, O_RDWR | O_CLOEXEC);
	if (sdp->device_fd < 0)
		die( _("can't open device %s: %s\n"),
		    sdp->device_name, strerror(errno));

	if (fstat(sdp->device_fd, &st_buf) < 0) {
		fprintf(stderr, _("could not fstat fd %d: %s\n"),
			sdp->device_fd, strerror(errno));
		exit(-1);
	}

	if (!sdp->override)
		are_you_sure(sdp);

	if (!S_ISREG(st_buf.st_mode) && device_topology(sdp)) {
		fprintf(stderr, _("Device topology error\n"));
		exit(-1);
	}

	if (sdp->bsize == -1) {
		if (S_ISREG(st_buf.st_mode))
			sdp->bsize = GFS2_DEFAULT_BSIZE;
		/* See if optimal_io_size (the biggest I/O we can submit
		   without incurring a penalty) is a suitable block size. */
		else if (sdp->optimal_io_size <= getpagesize() &&
		    sdp->optimal_io_size >= sdp->minimum_io_size)
			sdp->bsize = sdp->optimal_io_size;
		/* See if physical_block_size (the smallest unit we can write
		   without incurring read-modify-write penalty) is suitable. */
		else if (sdp->physical_block_size <= getpagesize() &&
			 sdp->physical_block_size >= GFS2_DEFAULT_BSIZE)
			sdp->bsize = sdp->physical_block_size;
		else
			sdp->bsize = GFS2_DEFAULT_BSIZE;

		if (sdp->debug)
			printf("\nUsing block size: %u\n", sdp->bsize);
	}
	verify_bsize(sdp);

	if (compute_constants(sdp)) {
		fprintf(stderr, _("Bad constants (1)\n"));
		exit(-1);
	}

	/* Get the device geometry */

	device_size(sdp->device_fd, &real_device_size);
	if (device_geometry(sdp)) {
		fprintf(stderr, _("Geometry error\n"));
		exit(-1);
	}
	/* Convert optional block-count to basic blocks */
	if (sdp->orig_fssize) {
		sdp->orig_fssize *= sdp->bsize;
		sdp->orig_fssize >>= GFS2_BASIC_BLOCK_SHIFT;
		if (sdp->orig_fssize > sdp->device.length) {
			fprintf(stderr, _("%s: Specified block count is bigger "
				"than the actual device.\n"), argv[0]);
			die( _("Device Size is %.2f GB (%llu blocks)\n"),
			       real_device_size / ((float)(1 << 30)),
			       (unsigned long long)real_device_size / sdp->bsize);
		}
		sdp->device.length = sdp->orig_fssize;
	}
	if (fix_device_geometry(sdp)) {
		fprintf(stderr, _("Device is too small (%llu bytes)\n"),
			(unsigned long long)sdp->device.length << GFS2_BASIC_BLOCK_SHIFT);
		exit(-1);
	}

	if (discard)
		discard_blocks(sdp);

	/* Compute the resource group layouts */

	compute_rgrp_layout(sdp, &sdp->rgtree, rgsize_specified);

	/* Generate a random uuid */
	get_random_bytes(uuid, sizeof(uuid));

	/* Build ondisk structures */

	build_rgrps(sdp, TRUE);
	build_root(sdp);
	build_master(sdp);
	build_sb(sdp, uuid);
	error = build_jindex(sdp);
	if (error) {
		fprintf(stderr, _("Error building jindex: %s\n"), strerror(error));
		exit(-1);
	}
	error = build_per_node(sdp);
	if (error) {
		fprintf(stderr, _("Error building per-node directory: %s\n"), strerror(error));
		exit(-1);
	}
	error = build_inum(sdp);
	if (error) {
		fprintf(stderr, _("Error building inum inode: %s\n"), strerror(error));
		exit(-1);
	}
	gfs2_lookupi(sdp->master_dir, "inum", 4, &sdp->md.inum);
	error = build_statfs(sdp);
	if (error) {
		fprintf(stderr, _("Error building statfs inode: %s\n"), strerror(error));
		exit(-1);
	}
	gfs2_lookupi(sdp->master_dir, "statfs", 6, &sdp->md.statfs);
	error = build_rindex(sdp);
	if (error) {
		fprintf(stderr, _("Error building rindex inode: %s\n"), strerror(error));
		exit(-1);
	}
	error = build_quota(sdp);
	if (error) {
		fprintf(stderr, _("Error building quota inode: %s\n"), strerror(error));
		exit(-1);
	}

	do_init_inum(sdp);
	do_init_statfs(sdp);

	/* Cleanup */

	inode_put(&sdp->md.rooti);
	inode_put(&sdp->master_dir);
	inode_put(&sdp->md.inum);
	inode_put(&sdp->md.statfs);

	gfs2_rgrp_free(&sdp->rgtree);
	error = fsync(sdp->device_fd);
	if (error)
		die( _("can't fsync device (%d): %s\n"),
		    error, strerror(errno));
	error = close(sdp->device_fd);
	if (error)
		die( _("error closing device (%d): %s\n"),
		    error, strerror(errno));

	print_results(sdp, real_device_size, uuid);
}
