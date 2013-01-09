#include "clusterautoconfig.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>
#include <sys/types.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/file.h>
#include <sys/vfs.h>
#include <sys/mount.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <stdarg.h>
#include <linux/types.h>
#include <linux/falloc.h>
#include <libintl.h>
#define _(String) gettext(String)

#include "libgfs2.h"
#include "gfs2_mkfs.h"

#define BUF_SIZE 4096
#define MB (1024 * 1024)

static uint64_t override_device_size = 0;
static int test = 0;
static uint64_t fssize = 0, fsgrowth;
static unsigned int rgsize = 0;

extern int create_new_inode(struct gfs2_sbd *sdp);
extern int rename2system(struct gfs2_sbd *sdp, char *new_dir, char *new_name);

#ifndef BLKDISCARD
#define BLKDISCARD      _IO(0x12,119)
#endif

static int discard_blocks(int fd, uint64_t start, uint64_t len)
{
	__uint64_t range[2] = { start, len };

	if (ioctl(fd, BLKDISCARD, &range) < 0)
		return errno;
	return 0;
}

/**
 * usage - Print out the usage message
 *
 * This function does not include documentation for the -D option
 * since normal users have no use for it at all. The -D option is
 * only for developers. It intended use is in combination with the
 * -T flag to find out what the result would be of trying different
 * device sizes without actually having to try them manually.
 */

static void usage(void)
{
	int i;
	const char *option, *param, *desc;
	const char *options[] = {
		"-h", NULL, _("Display this usage information"),
		"-q", NULL, _("Quiet, reduce verbosity"),
		"-T", NULL, _("Do everything except update file system"),
		"-V", NULL, _("Display version information"),
		"-v", NULL, _("Increase verbosity"),
		NULL, NULL, NULL /* Must be kept at the end */
	};

	printf("%s\n", _("Usage:"));
	printf("    gfs2_grow [%s] <%s>\n\n", _("options"), _("device"));
	printf(_("Expands a GFS2 file system after the device containing the file system has been expanded"));
	printf("\n\n%s\n", _("Options:"));

	for (i = 0; options[i] != NULL; i += 3) {
		option = options[i];
		param = options[i+1];
		desc = options[i+2];
		printf("%3s %-15s %s\n", option, param ? param : "", desc);
	}
}

static void decode_arguments(int argc, char *argv[], struct gfs2_sbd *sdp)
{
	int opt;

	while ((opt = getopt(argc, argv, "VD:hqTv?")) != EOF) {
		switch (opt) {
		case 'D':	/* This option is for testing only */
			override_device_size = atoi(optarg);
			override_device_size <<= 20;
			break;
		case 'V':
			printf(_("%s %s (built %s %s)\n"), argv[0],
			       VERSION, __DATE__, __TIME__);
			printf(REDHAT_COPYRIGHT "\n");
			exit(0);
		case 'h':
			usage();
			exit(0);
		case 'q':
			decrease_verbosity();
			break;
		case 'T':
			printf( _("(Test mode - file system will not "
			       "be changed)\n"));
			test = 1;
			break;
		case 'v':
			increase_verbosity();
			break;
		case ':':
		case '?':
			/* Unknown flag */
			fprintf(stderr, _("Please use '-h' for help.\n"));
			exit(EXIT_FAILURE);
		default:
			fprintf(stderr, _("Invalid option '%c'\n"), opt);
			exit(EXIT_FAILURE);
			break;
		}
	}

	if (optind == argc) {
		usage();
		exit(EXIT_FAILURE);
	}
}

/**
 * filesystem_size - Calculate the size of the filesystem
 *
 * Reads the lists of resource groups in order to
 * work out where the last block of the filesystem is located.
 *
 * Returns: The calculated size
 */

static uint64_t filesystem_size(struct gfs2_sbd *sdp)
{
	struct osi_node *n, *next = NULL;
	struct rgrp_tree *rgl;
	uint64_t size = 0, extent;

	for (n = osi_first(&sdp->rgtree); n; n = next) {
		next = osi_next(n);
		rgl = (struct rgrp_tree *)n;
		extent = rgl->ri.ri_addr + rgl->ri.ri_length + rgl->ri.ri_data;
		if (extent > size)
			size = extent;
	}
	return size;
}

/**
 * initialize_new_portion - Write the new rg information to disk buffers.
 */
static void initialize_new_portion(struct gfs2_sbd *sdp, int *old_rg_count)
{
	struct osi_node *n, *next = NULL;
	uint64_t rgrp = 0;
	struct rgrp_tree *rl;

	*old_rg_count = 0;
	/* Delete the old RGs from the rglist */
	for (rgrp = 0, n = osi_first(&sdp->rgtree);
	     n && rgrp < (sdp->rgrps - sdp->new_rgrps); n = next, rgrp++) {
		next = osi_next(n);
		(*old_rg_count)++;
		rl = (struct rgrp_tree *)n;
		osi_erase(&rl->node, &sdp->rgtree);
		free(rl);
	}
	/* Issue a discard ioctl for the new portion */
	rl = (struct rgrp_tree *)n;
	discard_blocks(sdp->device_fd, rl->start * sdp->bsize,
		       (sdp->device.length - rl->start) * sdp->bsize);
	/* Build the remaining resource groups */
	build_rgrps(sdp, !test);

	inode_put(&sdp->md.riinode);
	inode_put(&sdp->master_dir);

	/* We're done with the libgfs portion, so commit it to disk.      */
	fsync(sdp->device_fd);
}

/**
 * fix_rindex - Add the new entries to the end of the rindex file.
 */
static void fix_rindex(struct gfs2_sbd *sdp, int rindex_fd, int old_rg_count)
{
	struct osi_node *n, *next = NULL;
	int count, rg;
	struct rgrp_tree *rl;
	char *buf, *bufptr;
	ssize_t writelen;
	struct stat statbuf;

	/* Count the number of new RGs. */
	rg = 0;
	for (n = osi_first(&sdp->rgtree); n; n = next) {
		next = osi_next(n);
		rg++;
	}
	log_info( _("%d new rindex entries.\n"), rg);
	writelen = rg * sizeof(struct gfs2_rindex);
	buf = calloc(1, writelen);
	if (buf == NULL) {
		perror(__FUNCTION__);
		exit(EXIT_FAILURE);
	}
	/* Now add the new rg entries to the rg index.  Here we     */
	/* need to use the gfs2 kernel code rather than the libgfs2 */
	/* code so we have a live update while mounted.             */
	bufptr = buf;
	for (n = osi_first(&sdp->rgtree); n; n = next) {
		next = osi_next(n);
		rg++;
		rl = (struct rgrp_tree *)n;
		gfs2_rindex_out(&rl->ri, bufptr);
		bufptr += sizeof(struct gfs2_rindex);
	}
	gfs2_rgrp_free(&sdp->rgtree);
	fsync(sdp->device_fd);
	if (!test) {
		if (fstat(rindex_fd, &statbuf) != 0) {
			perror("rindex");
			goto out;
		}
		if (statbuf.st_size !=
		    old_rg_count * sizeof(struct gfs2_rindex)) {
			log_crit(_("Incorrect rindex size. want %ld(%d resource groups), "
				 "have %ld\n"),
				 old_rg_count * sizeof(struct gfs2_rindex),
				 old_rg_count, statbuf.st_size);
			goto out;
		}
		/* Now write the new RGs to the end of the rindex */
		lseek(rindex_fd, 0, SEEK_END);
		count = write(rindex_fd, buf, sizeof(struct gfs2_rindex));
		if (count != sizeof(struct gfs2_rindex)) {
			log_crit(_("Error writing first new rindex entry; aborted.\n"));
			if (count > 0)
				goto trunc;
			else
				goto out;
		}
		count = write(rindex_fd, buf + sizeof(struct gfs2_rindex),
			      writelen - sizeof(struct gfs2_rindex));
		if (count != writelen - sizeof(struct gfs2_rindex)) {
			log_crit(_("Error writing new rindex entries; aborted.\n"));
			if (count > 0)
				goto trunc;
			else
				goto out;
		}
		if (fallocate(rindex_fd, FALLOC_FL_KEEP_SIZE, statbuf.st_size + writelen, sizeof(struct gfs2_rindex)) != 0)
			perror("fallocate");
		fsync(rindex_fd);
	}
out:
	free(buf);
	return;
trunc:
	count = (count / sizeof(struct gfs2_rindex)) + old_rg_count;
	log_crit(_("truncating rindex to %ld entries\n"),
		 (off_t)count * sizeof(struct gfs2_rindex));
	ftruncate(rindex_fd, (off_t)count * sizeof(struct gfs2_rindex));
	free(buf);
}

/**
 * print_info - Print out various bits of (interesting?) information
 *
 */
static void print_info(struct gfs2_sbd *sdp)
{
	log_notice("FS: %-22s%s\n", _("Mount point:"), sdp->path_name);
	log_notice("FS: %-22s%s\n", _("Device:"), sdp->device_name);
	log_notice("FS: %-22s%llu (0x%llx)\n", _("Size:"),
		   (unsigned long long)fssize, (unsigned long long)fssize);
	log_notice("FS: %-22s%u (0x%x)\n", _("Resource group size:"), rgsize, rgsize);
	log_notice("DEV: %-22s%llu (0x%llx)\n", _("Length:"),
		   (unsigned long long)sdp->device.length,
		   (unsigned long long)sdp->device.length);
	log_notice(_("The file system grew by %lluMB.\n"),
		   (unsigned long long)fsgrowth / MB);
}

void debug_print_rgrps(struct gfs2_sbd *sdp, struct osi_root *rgtree)
{
	struct osi_node *n, *next;
	struct rgrp_tree *rl;

	if (sdp->debug) {
		log_info("\n");

		for (n = osi_first(rgtree); n; n = next) {
			next = osi_next(n);
			rl = (struct rgrp_tree *)n;
			log_info("rg_o = %llu, rg_l = %llu\n",
				 (unsigned long long)rl->start,
				 (unsigned long long)rl->length);
		}
	}
}

/**
 * main_grow - do everything
 * @argc:
 * @argv:
 */
void
main_grow(int argc, char *argv[])
{
	struct gfs2_sbd sbd, *sdp = &sbd;
	int rgcount, rindex_fd;
	char rindex_name[PATH_MAX];

	memset(sdp, 0, sizeof(struct gfs2_sbd));
	sdp->bsize = GFS2_DEFAULT_BSIZE;
	sdp->rgsize = -1;
	sdp->jsize = GFS2_DEFAULT_JSIZE;
	sdp->qcsize = GFS2_DEFAULT_QCSIZE;
	sdp->md.journals = 1;
	decode_arguments(argc, argv, sdp);
	
	while ((argc - optind) > 0) {
		int sane;
		struct rgrp_tree *last_rgrp;

		sdp->path_name = argv[optind++];
		sdp->path_fd = open(sdp->path_name, O_RDONLY | O_CLOEXEC);
		if (sdp->path_fd < 0){
			perror(sdp->path_name);
			exit(EXIT_FAILURE);
		}

		if (check_for_gfs2(sdp)) {
			perror(sdp->path_name);
			exit(EXIT_FAILURE);
		}
		sdp->device_fd = open(sdp->device_name,
				      (test ? O_RDONLY : O_RDWR) | O_CLOEXEC);
		if (sdp->device_fd < 0){
			perror(sdp->device_name);
			exit(EXIT_FAILURE);
		}

		if (lgfs2_get_dev_info(sdp->device_fd, &sdp->dinfo) < 0) {
			perror(sdp->device_name);
			exit(EXIT_FAILURE);
		}
		log_info( _("Initializing lists...\n"));
		sdp->rgtree.osi_node = NULL;
		sdp->rgcalc.osi_node = NULL;

		sdp->sd_sb.sb_bsize = GFS2_DEFAULT_BSIZE;
		sdp->bsize = sdp->sd_sb.sb_bsize;
		if (compute_constants(sdp)) {
			perror(_("Bad constants (1)"));
			exit(EXIT_FAILURE);
		}
		if (read_sb(sdp) < 0)
			die( _("Error reading superblock.\n"));
		if (sdp->gfs1) {
			die( _("cannot grow gfs1 filesystem\n"));
		}
		fix_device_geometry(sdp);
		if (mount_gfs2_meta(sdp)) {
			perror(_("Failed to mount GFS2 meta file system"));
			exit(EXIT_FAILURE);
		}

		sprintf(rindex_name, "%s/rindex", sdp->metafs_path);
		rindex_fd = open(rindex_name, (test ? O_RDONLY : O_RDWR) | O_CLOEXEC);
		if (rindex_fd < 0) {
			cleanup_metafs(sdp);
			die( _("GFS2 rindex not found.  Please run fsck.gfs2.\n"));
		}
		/* Get master dinode */
		sdp->master_dir = lgfs2_inode_read(sdp, sdp->sd_sb.sb_master_dir.no_addr);
		if (sdp->master_dir == NULL) {
			perror(_("Could not read master directory"));
			exit(EXIT_FAILURE);
		}
		gfs2_lookupi(sdp->master_dir, "rindex", 6, &sdp->md.riinode);
		/* Fetch the rindex from disk.  We aren't using gfs2 here,  */
		/* which means that the bitmaps will most likely be cached  */
		/* and therefore out of date.  It shouldn't matter because  */
		/* we're only going to write out new RG information after   */
		/* the existing RGs, and only write to the index at EOF.    */
		ri_update(sdp, rindex_fd, &rgcount, &sane);
		fssize = filesystem_size(sdp);
		if (!sdp->rgtree.osi_node) {
			log_err(_("Error: No resource groups found.\n"));
			goto out;
		}
		last_rgrp = (struct rgrp_tree *)osi_last(&sdp->rgtree);
		sdp->rgsize = GFS2_DEFAULT_RGSIZE;
		rgsize = rgrp_size(last_rgrp);
		fsgrowth = ((sdp->device.length - fssize) * sdp->bsize);
		if (fsgrowth < rgsize * sdp->bsize) {
			log_err( _("Error: The device has grown by less than "
				"one resource group.\n"));
			log_err( _("The device grew by %lluMB. "),
				(unsigned long long)fsgrowth / MB);
			log_err( _("One resource group is %uMB for this file system.\n"),
				(rgsize * sdp->bsize) / MB);
		}
		else {
			int old_rg_count;

			compute_rgrp_layout(sdp, &sdp->rgtree, TRUE);
			debug_print_rgrps(sdp, &sdp->rgtree);
			print_info(sdp);
			initialize_new_portion(sdp, &old_rg_count);
			fix_rindex(sdp, rindex_fd, old_rg_count);
		}
	out:
		/* Delete the remaining RGs from the rglist */
		gfs2_rgrp_free(&sdp->rgtree);
		close(rindex_fd);
		cleanup_metafs(sdp);
		close(sdp->device_fd);
	}
	close(sdp->path_fd);
	sync();
	log_notice( _("gfs2_grow complete.\n"));
}
