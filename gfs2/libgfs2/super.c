#include "clusterautoconfig.h"

#include <unistd.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include "libgfs2.h"
#include "osi_list.h"

/**
 * check_sb - Check superblock
 * @sb: The superblock
 *
 * Checks the version code of the FS is one that we understand how to
 * read and that the sizes of the various on-disk structures have not
 * changed.
 *
 * Returns: -1 on failure, 1 if this is gfs (gfs1), 2 if this is gfs2
 */
int check_sb(struct gfs2_sb *sb)
{
	if (sb->sb_header.mh_magic != GFS2_MAGIC ||
	    sb->sb_header.mh_type != GFS2_METATYPE_SB) {
		errno = EIO;
		return -1;
	}
	if (sb->sb_fs_format == GFS_FORMAT_FS &&
	    sb->sb_header.mh_format == GFS_FORMAT_SB &&
	    sb->sb_multihost_format == GFS_FORMAT_MULTI) {
		return 1;
	}
	return 2;
}


/*
 * read_sb: read the super block from disk
 * sdp: in-core super block
 *
 * This function reads in the super block from disk and
 * initializes various constants maintained in the super
 * block
 *
 * Returns: 0 on success, -1 on failure
 * sdp->gfs1 will be set if this is gfs (gfs1)
 */
int read_sb(struct gfs2_sbd *sdp)
{
	struct gfs2_buffer_head *bh;
	uint64_t space = 0;
	unsigned int x;
	int ret;

	bh = bread(sdp, GFS2_SB_ADDR >> sdp->sd_fsb2bb_shift);
	gfs2_sb_in(&sdp->sd_sb, bh);
	brelse(bh);

	ret = check_sb(&sdp->sd_sb);
	if (ret < 0)
		return ret;
	if (ret == 1)
		sdp->gfs1 = 1;
	sdp->sd_fsb2bb_shift = sdp->sd_sb.sb_bsize_shift - GFS2_BASIC_BLOCK_SHIFT;
	sdp->bsize = sdp->sd_sb.sb_bsize;
	if (sdp->gfs1) {
		sdp->sd_diptrs = (sdp->sd_sb.sb_bsize -
				  sizeof(struct gfs_dinode)) /
			sizeof(uint64_t);
		sdp->sd_inptrs = (sdp->sd_sb.sb_bsize -
				  sizeof(struct gfs_indirect)) /
			sizeof(uint64_t);
	} else {
		sdp->sd_diptrs = (sdp->sd_sb.sb_bsize -
				  sizeof(struct gfs2_dinode)) /
			sizeof(uint64_t);
		sdp->sd_inptrs = (sdp->sd_sb.sb_bsize -
				  sizeof(struct gfs2_meta_header)) /
			sizeof(uint64_t);
	}
	sdp->sd_jbsize = sdp->sd_sb.sb_bsize - sizeof(struct gfs2_meta_header);
	sdp->sd_hash_bsize = sdp->bsize / 2;
	sdp->sd_hash_bsize_shift = sdp->sd_sb.sb_bsize_shift - 1;
	sdp->sd_hash_ptrs = sdp->sd_hash_bsize / sizeof(uint64_t);
	sdp->sd_heightsize[0] = sdp->sd_sb.sb_bsize - sizeof(struct gfs2_dinode);
	sdp->sd_heightsize[1] = sdp->sd_sb.sb_bsize * sdp->sd_diptrs;
	for (x = 2; ; x++){
		space = sdp->sd_heightsize[x - 1] * sdp->sd_inptrs;
		/* FIXME: Do we really need this first check?? */
		if (space / sdp->sd_inptrs != sdp->sd_heightsize[x - 1] ||
		    space % sdp->sd_inptrs != 0)
			break;
		sdp->sd_heightsize[x] = space;
	}
	if (x > GFS2_MAX_META_HEIGHT){
		errno = E2BIG;
		return -1;
	}

	sdp->sd_jheightsize[0] = sdp->sd_sb.sb_bsize - sizeof(struct gfs2_dinode);
	sdp->sd_jheightsize[1] = sdp->sd_jbsize * sdp->sd_diptrs;
	for (x = 2; ; x++){
		space = sdp->sd_jheightsize[x - 1] * sdp->sd_inptrs;
		if (space / sdp->sd_inptrs != sdp->sd_jheightsize[x - 1] ||
			space % sdp->sd_inptrs != 0)
			break;
		sdp->sd_jheightsize[x] = space;
	}
	sdp->sd_max_jheight = x;
	if(sdp->sd_max_jheight > GFS2_MAX_META_HEIGHT) {
		errno = E2BIG;
		return -1;
	}
	sdp->fssize = lseek(sdp->device_fd, 0, SEEK_END) / sdp->sd_sb.sb_bsize;
	sdp->sb_addr = GFS2_SB_ADDR * GFS2_BASIC_BLOCK / sdp->bsize;
	sdp->sd_blocks_per_bitmap = (sdp->sd_sb.sb_bsize - sizeof(struct gfs2_meta_header))
	                             * GFS2_NBBY;

	return 0;
}

/**
 * rindex_read - read in the rg index file
 * @sdp: the incore superblock pointer
 * fd: optional file handle for rindex file (if meta_fs file system is mounted)
 *     (if fd is <= zero, it will read from raw device)
 * @count1: return count of the rgs.
 * @sane: return whether rindex is consistent
 *
 * Returns: 0 on success, -1 on failure
 */
int rindex_read(struct gfs2_sbd *sdp, int fd, int *count1, int *sane)
{
	unsigned int rg;
	int error;
	union {
		struct gfs2_rindex bufgfs2;
	} buf;
	struct gfs2_rindex ri;
	struct rgrp_tree *rgd = NULL, *prev_rgd = NULL;
	uint64_t prev_length = 0;

	*sane = 1;
	*count1 = 0;
	if (!fd && sdp->md.riinode->i_di.di_size % sizeof(struct gfs2_rindex))
		*sane = 0; /* rindex file size must be a multiple of 96 */
	for (rg = 0; ; rg++) {
		if (fd > 0)
			error = read(fd, &buf, sizeof(struct gfs2_rindex));
		else
			error = gfs2_readi(sdp->md.riinode,
					   (char *)&buf.bufgfs2,
					   rg * sizeof(struct gfs2_rindex),
					   sizeof(struct gfs2_rindex));
		if (!error)
			break;
		if (error != sizeof(struct gfs2_rindex))
			return -1;

		gfs2_rindex_in(&ri, (char *)&buf.bufgfs2);
		rgd = rgrp_insert(&sdp->rgtree, ri.ri_addr);
		memcpy(&rgd->ri, &ri, sizeof(struct gfs2_rindex));

		rgd->start = rgd->ri.ri_addr;
		if (prev_rgd) {
			/* If rg addresses go backwards, it's not sane
			   (or it's converted from gfs1). */
			if (!sdp->gfs1) {
				if (prev_rgd->start >= rgd->start)
					*sane = 0;
				/* If rg lengths are not consistent, it's not
				   sane (or it's converted from gfs1).  The
				   first RG will be a different length due to
				   space allocated for the superblock, so we
				   can't detect this until we check rgrp 3,
				   when we can compare the distance between
				   rgrp 1 and rgrp 2. */
				if (rg > 2 && prev_length &&
				    prev_length != rgd->start -
				    prev_rgd->start)
					*sane = 0;
			}
			prev_length = rgd->start - prev_rgd->start;
			prev_rgd->length = rgrp_size(prev_rgd);
		}

		if(gfs2_compute_bitstructs(sdp, rgd))
			*sane = 0;

		(*count1)++;
		prev_rgd = rgd;
	}
	if (prev_rgd)
		prev_rgd->length = rgrp_size(prev_rgd);
	return 0;
}

/**
 * ri_update - attach rgrps to the super block
 * @sdp: incore superblock data
 * fd: optional file handle for rindex (through the meta_fs)
 * @rgcount: returned count of rgs
 *
 * Given the rgrp index inode, link in all rgrps into the super block
 * and be sure that they can be read.
 *
 * Returns: 0 on success, -1 on failure.
 */
static int __ri_update(struct gfs2_sbd *sdp, int fd, int *rgcount, int *sane,
		       int quiet)
{
	struct rgrp_tree *rgd;
	struct gfs2_rindex *ri;
	int count1 = 0, count2 = 0;
	uint64_t errblock = 0;
	uint64_t rmax = 0;
	struct osi_node *n, *next = NULL;

	if (rindex_read(sdp, fd, &count1, sane))
		goto fail;
	for (n = osi_first(&sdp->rgtree); n; n = next) {
		next = osi_next(n);
		rgd = (struct rgrp_tree *)n;
		errblock = gfs2_rgrp_read(sdp, rgd);
		if (errblock)
			return errblock;
		count2++;
		if (!quiet && count2 % 100 == 0) {
			printf(".");
			fflush(stdout);
		}
		ri = &rgd->ri;
		if (ri->ri_data0 + ri->ri_data - 1 > rmax)
			rmax = ri->ri_data0 + ri->ri_data - 1;
	}

	sdp->fssize = rmax;
	*rgcount = count1;
	if (count1 != count2)
		goto fail;

	return 0;

 fail:
	gfs2_rgrp_free(&sdp->rgtree);
	return -1;
}

int ri_update(struct gfs2_sbd *sdp, int fd, int *rgcount, int *sane)
{
	return __ri_update(sdp, fd, rgcount, sane, 1);
}

/**
 * gfs1_ri_update - attach rgrps to the super block
 *                  Stolen from libgfs2/super.c, but modified to handle gfs1.
 * @sdp:
 *
 * Given the rgrp index inode, link in all rgrps into the super block
 * and be sure that they can be read.
 *
 * Returns: 0 on success, -1 on failure.
 */
int gfs1_ri_update(struct gfs2_sbd *sdp, int fd, int *rgcount, int quiet)
{
	int sane;

	return __ri_update(sdp, fd, rgcount, &sane, quiet);
}

int write_sb(struct gfs2_sbd *sbp)
{
	struct gfs2_buffer_head *bh;

	bh = bread(sbp, GFS2_SB_ADDR >> sbp->sd_fsb2bb_shift);
	gfs2_sb_out(&sbp->sd_sb, bh);
	brelse(bh);
	fsync(sbp->device_fd); /* make sure the change gets to disk ASAP */
	return 0;
}

