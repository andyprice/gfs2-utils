#include "clusterautoconfig.h"

#include <unistd.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>

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
	/* Check for gfs1 */
	if (sb->sb_fs_format == GFS_FORMAT_FS &&
	    sb->sb_header.mh_format == GFS_FORMAT_SB &&
	    sb->sb_multihost_format == GFS_FORMAT_MULTI) {
		return 1;
	}
	/* It's gfs2. Check format number is in a sensible range. */
	if (sb->sb_fs_format < LGFS2_FS_FORMAT_MIN ||
	    sb->sb_fs_format > 1899) {
		errno = EINVAL;
		return -1;
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
	gfs2_sb_in(&sdp->sd_sb, bh->b_data);
	brelse(bh);

	ret = check_sb(&sdp->sd_sb);
	if (ret < 0)
		return ret;
	if (ret == 1)
		sdp->gfs1 = 1;
	sdp->sd_fsb2bb_shift = sdp->sd_sb.sb_bsize_shift - GFS2_BASIC_BLOCK_SHIFT;
	sdp->bsize = sdp->sd_sb.sb_bsize;
	if (sdp->bsize < 512 || sdp->bsize != (sdp->bsize & -sdp->bsize)) {
		return -1;
	}
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
	for (x = 2; x < GFS2_MAX_META_HEIGHT; x++){
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
	sdp->sd_blocks_per_bitmap = (sdp->sd_sb.sb_bsize - sizeof(struct gfs2_meta_header))
	                             * GFS2_NBBY;
	sdp->qcsize = GFS2_DEFAULT_QCSIZE;

	return 0;
}

/* rgd_seems_sane - check some general things about the rindex entry
 *
 * If rg lengths are not consistent, it's not sane (or it's converted from
 * gfs1). The first RG will be a different length due to space reserved for
 * the superblock, so we can't detect this until we check rgrp 3, when we
 * can compare the distance between rgrp 1 and rgrp 2.
 *
 * Returns: 1 if the rgd seems relatively sane
 */
static int rgd_seems_sane(struct gfs2_sbd *sdp, struct rgrp_tree *rgd)
{
	uint32_t most_bitmaps_possible;

	/* rg length must be at least 1 */
	if (rgd->ri.ri_length == 0)
		return 0;

	/* A max rgrp, 2GB, divided into blocksize, divided by blocks/byte
	   represented in the bitmap, NBBY. Rough approximation only, due to
	   metadata headers. I'm doing the math this way to avoid overflow. */
	most_bitmaps_possible = (GFS2_MAX_RGSIZE * 1024 * 256) / sdp->bsize;
	if (rgd->ri.ri_length > most_bitmaps_possible)
		return 0;

	if (rgd->ri.ri_data0 != rgd->ri.ri_addr + rgd->ri.ri_length)
		return 0;

	if (rgd->ri.ri_bitbytes != rgd->ri.ri_data / GFS2_NBBY)
		return 0;

	return 1;
}

/* good_on_disk - check if the rindex points to what looks like an rgrp on disk
 *
 * This is only called when the rindex pointers aren't spaced evenly, which
 * isn't often. The rindex is pointing to an unexpected location, so we
 * check if the block it is pointing to is really an rgrp. If so, we count the
 * rindex entry as "sane" (after all, it did pass the previous checks above.)
 * If not, we count it as not sane, and therefore, the whole rindex is not to
 * be trusted by fsck.gfs2.
 */
static int good_on_disk(struct gfs2_sbd *sdp, struct rgrp_tree *rgd)
{
	struct gfs2_buffer_head *bh;
	int is_rgrp;

	bh = bread(sdp, rgd->ri.ri_addr);
	is_rgrp = (gfs2_check_meta(bh->b_data, GFS2_METATYPE_RG) == 0);
	brelse(bh);
	return is_rgrp;
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
int rindex_read(struct gfs2_sbd *sdp, int fd, uint64_t *count1, int *sane)
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
		if (gfs2_check_range(sdp, ri.ri_addr) != 0) {
			*sane = 0;
			if (prev_rgd == NULL)
				continue;
			ri.ri_addr = prev_rgd->ri.ri_addr + prev_rgd->length;
		}
		rgd = rgrp_insert(&sdp->rgtree, ri.ri_addr);
		memcpy(&rgd->ri, &ri, sizeof(struct gfs2_rindex));

		rgd->start = rgd->ri.ri_addr;
		if (prev_rgd) {
			/* If rg addresses go backwards, it's not sane
			   (or it's converted from gfs1). */
			if (!sdp->gfs1) {
				if (prev_rgd->start >= rgd->start)
					*sane = 0;
				else if (!rgd_seems_sane(sdp, rgd))
					*sane = 0;
				else if (*sane && rg > 2 && prev_length &&
				    prev_length != rgd->start -
				    prev_rgd->start)
					*sane = good_on_disk(sdp, rgd);
			}
			prev_length = rgd->start - prev_rgd->start;
			prev_rgd->length = rgrp_size(prev_rgd);
		}

		if(gfs2_compute_bitstructs(sdp->sd_sb.sb_bsize, rgd))
			*sane = 0;

		(*count1)++;
		prev_rgd = rgd;
	}
	if (prev_rgd)
		prev_rgd->length = rgrp_size(prev_rgd);
	if (*count1 == 0)
		return -1;
	return 0;
}
