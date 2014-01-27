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
#include <errno.h>

#include <linux/types.h>
#include "libgfs2.h"

#define DIV_RU(x, y) (((x) + (y) - 1) / (y))

/**
 * how_many_rgrps - figure out how many RG to put in a subdevice
 * @w: the command line
 * @dev: the device
 *
 * Returns: the number of RGs
 */

uint64_t how_many_rgrps(struct gfs2_sbd *sdp, struct device *dev, int rgsize_specified)
{
	uint64_t nrgrp;
	uint32_t rgblocks1, rgblocksn, bitblocks1, bitblocksn;
	int bitmap_overflow = 0;

	while (TRUE) {
		nrgrp = DIV_RU(dev->length, (sdp->rgsize << 20) / sdp->bsize);

		/* check to see if the rg length overflows max # bitblks */
		bitblocksn = rgblocks2bitblocks(sdp->bsize, dev->length / nrgrp, &rgblocksn);
		/* calculate size of the first rgrp */
		bitblocks1 = rgblocks2bitblocks(sdp->bsize, dev->length - (nrgrp - 1) * (dev->length / nrgrp),
		                                &rgblocks1);
		if (bitblocks1 > 2149 || bitblocksn > 2149) {
			bitmap_overflow = 1;
			if (sdp->rgsize <= GFS2_DEFAULT_RGSIZE) {
				fprintf(stderr, "error: It is not possible "
					"to use the entire device with "
					"block size %u bytes.\n",
					sdp->bsize);
				exit(-1);
			}
			sdp->rgsize -= GFS2_DEFAULT_RGSIZE; /* smaller rgs */
			continue;
		}
		if (bitmap_overflow ||
		    rgsize_specified || /* If user specified an rg size or */
		    nrgrp <= GFS2_EXCESSIVE_RGS || /* not an excessive # or  */
		    sdp->rgsize >= 2048)   /* we reached the max rg size */
			break;

		sdp->rgsize += GFS2_DEFAULT_RGSIZE; /* bigger rgs */
	}

	if (sdp->debug)
		printf("  rg sz = %"PRIu32"\n  nrgrp = %"PRIu64"\n",
		       sdp->rgsize, nrgrp);

	return nrgrp;
}

/**
 * compute_rgrp_layout - figure out where the RG in a FS are
 * @w: the command line
 *
 * Returns: a list of rgrp_list_t structures
 */

void compute_rgrp_layout(struct gfs2_sbd *sdp, struct osi_root *rgtree,
			 int rgsize_specified)
{
	struct device *dev;
	struct rgrp_tree *rl, *rlast = NULL;
	struct osi_node *n, *next = NULL;
	unsigned int rgrp = 0, nrgrp, rglength;
	uint64_t rgaddr;

	sdp->new_rgrps = 0;
	dev = &sdp->device;

	/* If this is a new file system, compute the length and number */
	/* of rgs based on the size of the device.                     */
	/* If we have existing RGs (i.e. gfs2_grow) find the last one. */
	if (!rgtree->osi_node) {
		dev->length -= sdp->sb_addr + 1;
		nrgrp = how_many_rgrps(sdp, dev, rgsize_specified);
		rglength = dev->length / nrgrp;
		sdp->new_rgrps = nrgrp;
	} else {
		uint64_t old_length, new_chunk;

		log_info("Existing resource groups:\n");
		for (rgrp = 0, n = osi_first(rgtree); n; n = next, rgrp++) {
			next = osi_next(n);
			rl = (struct rgrp_tree *)n;

			log_info("%d: start: %" PRIu64 " (0x%"
				 PRIx64 "), length = %"PRIu64" (0x%"
				 PRIx64 ")\n", rgrp + 1, rl->start, rl->start,
				 rl->length, rl->length);
			rlast = rl;
		}
		rlast->start = rlast->ri.ri_addr;
		rglength = rgrp_size(rlast);
		rlast->length = rglength;
		old_length = rlast->ri.ri_addr + rglength;
		new_chunk = dev->length - old_length;
		sdp->new_rgrps = new_chunk / rglength;
		nrgrp = rgrp + sdp->new_rgrps;
	}

	if (rgrp < nrgrp)
		log_info("\nNew resource groups:\n");
	for (; rgrp < nrgrp; rgrp++) {
		if (rgrp) {
			rgaddr = rlast->start + rlast->length;
			rl = rgrp_insert(rgtree, rgaddr);
			rl->length = rglength;
		} else {
			rgaddr = sdp->sb_addr + 1;
			rl = rgrp_insert(rgtree, rgaddr);
			rl->length = dev->length -
				(nrgrp - 1) * (dev->length / nrgrp);
		}
		rl->start = rgaddr;
		log_info("%d: start: %" PRIu64 " (0x%"
			 PRIx64 "), length = %"PRIu64" (0x%"
			 PRIx64 ")\n", rgrp + 1, rl->start, rl->start,
			 rl->length, rl->length);
		rlast = rl;
	}

	sdp->rgrps = nrgrp;
}

/**
 * Given a number of blocks in a resource group, return the number of blocks
 * needed for bitmaps. Also calculate the adjusted number of free data blocks
 * in the resource group and store it in *ri_data.
 */
uint32_t rgblocks2bitblocks(const unsigned int bsize, const uint32_t rgblocks, uint32_t *ri_data)
{
	uint32_t mappable = 0;
	uint32_t bitblocks = 0;
	/* Number of blocks mappable by bitmap blocks with these header types */
	const uint32_t blks_rgrp = GFS2_NBBY * (bsize - sizeof(struct gfs2_rgrp));
	const uint32_t blks_meta = GFS2_NBBY * (bsize - sizeof(struct gfs2_meta_header));

	while (blks_rgrp + (blks_meta * bitblocks) < ((rgblocks - bitblocks) & ~(uint32_t)3))
		bitblocks++;

	if (bitblocks > 0)
		mappable = blks_rgrp + (blks_meta * (bitblocks - 1));

	*ri_data = (rgblocks - (bitblocks + 1)) & ~(uint32_t)3;
	if (mappable < *ri_data)
		bitblocks++;

	return bitblocks;
}

/**
 * build_rgrps - write a bunch of resource groups to disk.
 * If fd > 0, write the data to the given file handle.
 * Otherwise, use gfs2 buffering in buf.c.
 */
int build_rgrps(struct gfs2_sbd *sdp, int do_write)
{
	struct osi_node *n, *next = NULL;
	struct rgrp_tree *rl;
	uint32_t rgblocks, bitblocks;
	struct gfs2_rindex *ri;
	struct gfs2_meta_header mh;
	unsigned int x;

	mh.mh_magic = GFS2_MAGIC;
	mh.mh_type = GFS2_METATYPE_RB;
	mh.mh_format = GFS2_FORMAT_RB;
	if (do_write)
		n = osi_first(&sdp->rgtree);
	else
		n = osi_first(&sdp->rgcalc);

	for (; n; n = next) {
		next = osi_next(n);
		rl = (struct rgrp_tree *)n;
		ri = &rl->ri;

		bitblocks = rgblocks2bitblocks(sdp->bsize, rl->length, &rgblocks);

		ri->ri_addr = rl->start;
		ri->ri_length = bitblocks;
		ri->ri_data0 = rl->start + bitblocks;
		ri->ri_data = rgblocks;
		ri->ri_bitbytes = rgblocks / GFS2_NBBY;

		memset(&rl->rg, 0, sizeof(rl->rg));
		rl->rg.rg_header.mh_magic = GFS2_MAGIC;
		rl->rg.rg_header.mh_type = GFS2_METATYPE_RG;
		rl->rg.rg_header.mh_format = GFS2_FORMAT_RG;
		rl->rg.rg_free = rgblocks;

		if (gfs2_compute_bitstructs(sdp->sd_sb.sb_bsize, rl))
			return -1;

		if (do_write) {
			for (x = 0; x < bitblocks; x++) {
				rl->bh[x] = bget(sdp, rl->start + x);
				if (x)
					gfs2_meta_header_out_bh(&mh, rl->bh[x]);
				else
					gfs2_rgrp_out_bh(&rl->rg, rl->bh[x]);
			}
		}

		if (sdp->debug) {
			printf("\n");
			gfs2_rindex_print(ri);
		}

		sdp->blks_total += rgblocks;
		sdp->fssize = ri->ri_data0 + ri->ri_data;
	}
	return 0;
}
