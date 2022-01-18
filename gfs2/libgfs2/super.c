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
 * lgfs2_check_sb - Check superblock
 * @sb: The superblock
 *
 * Checks the version code of the FS is one that we understand how to
 * read and that the sizes of the various on-disk structures have not
 * changed.
 *
 * Returns: -1 on failure, 1 if this is gfs (gfs1), 2 if this is gfs2
 */
int lgfs2_check_sb(void *sbp)
{
	struct gfs2_sb *sb = sbp;

	if (be32_to_cpu(sb->sb_header.mh_magic) != GFS2_MAGIC ||
	    be32_to_cpu(sb->sb_header.mh_type) != GFS2_METATYPE_SB) {
		errno = EIO;
		return -1;
	}
	/* Check for gfs1 */
	if (be32_to_cpu(sb->sb_fs_format) == GFS_FORMAT_FS &&
	    be32_to_cpu(sb->sb_header.mh_format) == GFS_FORMAT_SB &&
	    be32_to_cpu(sb->sb_multihost_format) == GFS_FORMAT_MULTI) {
		return 1;
	}
	/* It's gfs2. Check format number is in a sensible range. */
	if (be32_to_cpu(sb->sb_fs_format) < LGFS2_FS_FORMAT_MIN ||
	    be32_to_cpu(sb->sb_fs_format) > 1899) {
		errno = EINVAL;
		return -1;
	}
	return 2;
}


/*
 * lgfs2_read_sb: read the super block from disk
 * sdp: in-core super block
 *
 * This function reads in the super block from disk and
 * initializes various constants maintained in the super
 * block
 *
 * Returns: 0 on success, -1 on failure
 * sdp->gfs1 will be set if this is gfs (gfs1)
 */
int lgfs2_read_sb(struct lgfs2_sbd *sdp)
{
	struct lgfs2_buffer_head *bh;
	uint64_t space = 0;
	unsigned int x;
	int ret;

	bh = lgfs2_bread(sdp, GFS2_SB_ADDR >> sdp->sd_fsb2bb_shift);

	ret = lgfs2_check_sb(bh->b_data);
	if (ret < 0) {
		lgfs2_brelse(bh);
		return ret;
	}
	if (ret == 1)
		sdp->gfs1 = 1;

	lgfs2_sb_in(sdp, bh->b_data);
	lgfs2_brelse(bh);
	sdp->sd_fsb2bb_shift = sdp->sd_bsize_shift - GFS2_BASIC_BLOCK_SHIFT;
	if (sdp->sd_bsize < 512 || sdp->sd_bsize != (sdp->sd_bsize & -sdp->sd_bsize)) {
		return -1;
	}
	if (sdp->gfs1) {
		sdp->sd_diptrs = (sdp->sd_bsize -
				  sizeof(struct gfs_dinode)) /
			sizeof(uint64_t);
		sdp->sd_inptrs = (sdp->sd_bsize -
				  sizeof(struct gfs_indirect)) /
			sizeof(uint64_t);
	} else {
		sdp->sd_diptrs = (sdp->sd_bsize -
				  sizeof(struct gfs2_dinode)) /
			sizeof(uint64_t);
		sdp->sd_inptrs = (sdp->sd_bsize -
				  sizeof(struct gfs2_meta_header)) /
			sizeof(uint64_t);
	}
	sdp->sd_jbsize = sdp->sd_bsize - sizeof(struct gfs2_meta_header);
	sdp->sd_hash_bsize = sdp->sd_bsize / 2;
	sdp->sd_hash_bsize_shift = sdp->sd_bsize_shift - 1;
	sdp->sd_hash_ptrs = sdp->sd_hash_bsize / sizeof(uint64_t);
	sdp->sd_heightsize[0] = sdp->sd_bsize - sizeof(struct gfs2_dinode);
	sdp->sd_heightsize[1] = sdp->sd_bsize * sdp->sd_diptrs;
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

	sdp->sd_jheightsize[0] = sdp->sd_bsize - sizeof(struct gfs2_dinode);
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
	sdp->fssize = lseek(sdp->device_fd, 0, SEEK_END) / sdp->sd_bsize;
	sdp->sd_blocks_per_bitmap = (sdp->sd_bsize - sizeof(struct gfs2_meta_header))
	                             * GFS2_NBBY;
	sdp->qcsize = LGFS2_DEFAULT_QCSIZE;

	return 0;
}

/* rgd_seems_ok - check some general things about the rindex entry
 *
 * If rg lengths are not consistent, it's not sane (or it's converted from
 * gfs1). The first RG will be a different length due to space reserved for
 * the superblock, so we can't detect this until we check rgrp 3, when we
 * can compare the distance between rgrp 1 and rgrp 2.
 *
 * Returns: 1 if the rgd seems relatively sane
 */
static int rgd_seems_ok(struct lgfs2_sbd *sdp, struct lgfs2_rgrp_tree *rgd)
{
	uint32_t most_bitmaps_possible;

	/* rg length must be at least 1 */
	if (rgd->rt_length == 0)
		return 0;

	/* A max rgrp, 2GB, divided into blocksize, divided by blocks/byte
	   represented in the bitmap, NBBY. Rough approximation only, due to
	   metadata headers. I'm doing the math this way to avoid overflow. */
	most_bitmaps_possible = (LGFS2_MAX_RGSIZE * 1024 * 256) / sdp->sd_bsize;
	if (rgd->rt_length > most_bitmaps_possible)
		return 0;

	if (rgd->rt_data0 != rgd->rt_addr + rgd->rt_length)
		return 0;

	if (rgd->rt_bitbytes != rgd->rt_data / GFS2_NBBY)
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
static int good_on_disk(struct lgfs2_sbd *sdp, struct lgfs2_rgrp_tree *rgd)
{
	struct lgfs2_buffer_head *bh;
	int is_rgrp;

	bh = lgfs2_bread(sdp, rgd->rt_addr);
	is_rgrp = (lgfs2_check_meta(bh->b_data, GFS2_METATYPE_RG) == 0);
	lgfs2_brelse(bh);
	return is_rgrp;
}

/**
 * lgfs2_rindex_read - read in the rg index file
 * @sdp: the incore superblock pointer
 * @rgcount: return count of the rgs.
 * @ok: return whether rindex is consistent
 *
 * Returns: 0 on success, -1 on failure
 */
int lgfs2_rindex_read(struct lgfs2_sbd *sdp, uint64_t *rgcount, int *ok)
{
	unsigned int rg;
	int error;
	struct lgfs2_rgrp_tree *rgd = NULL, *prev_rgd = NULL;
	uint64_t prev_length = 0;

	*ok = 1;
	*rgcount = 0;
	if (sdp->md.riinode->i_size % sizeof(struct gfs2_rindex))
		*ok = 0; /* rindex file size must be a multiple of 96 */
	for (rg = 0; ; rg++) {
		struct gfs2_rindex ri;
		uint64_t addr;

		error = lgfs2_readi(sdp->md.riinode, &ri,
		                   rg * sizeof(struct gfs2_rindex),
		                   sizeof(struct gfs2_rindex));
		if (!error)
			break;
		if (error != sizeof(struct gfs2_rindex))
			return -1;

		addr = be64_to_cpu(ri.ri_addr);
		if (lgfs2_check_range(sdp, addr) != 0) {
			*ok = 0;
			if (prev_rgd == NULL)
				continue;
			addr = prev_rgd->rt_data0 + prev_rgd->rt_data;
		}
		rgd = lgfs2_rgrp_insert(&sdp->rgtree, addr);
		rgd->rt_length = be32_to_cpu(ri.ri_length);
		rgd->rt_data0 = be64_to_cpu(ri.ri_data0);
		rgd->rt_data = be32_to_cpu(ri.ri_data);
		rgd->rt_bitbytes = be32_to_cpu(ri.ri_bitbytes);
		if (prev_rgd) {
			/* If rg addresses go backwards, it's not sane
			   (or it's converted from gfs1). */
			if (!sdp->gfs1) {
				if (prev_rgd->rt_addr >= rgd->rt_addr)
					*ok = 0;
				else if (!rgd_seems_ok(sdp, rgd))
					*ok = 0;
				else if (*ok && rg > 2 && prev_length &&
				    prev_length != rgd->rt_addr - prev_rgd->rt_addr)
					*ok = good_on_disk(sdp, rgd);
			}
			prev_length = rgd->rt_addr - prev_rgd->rt_addr;
		}

		if(lgfs2_compute_bitstructs(sdp->sd_bsize, rgd))
			*ok = 0;

		(*rgcount)++;
		prev_rgd = rgd;
	}
	if (*rgcount == 0)
		return -1;
	return 0;
}
