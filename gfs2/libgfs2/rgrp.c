#include "clusterautoconfig.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "libgfs2.h"

#define RG_SYNC_TOLERANCE 1000

/**
 * gfs2_compute_bitstructs - Compute the bitmap sizes
 * @rgd: The resource group descriptor
 *
 * Returns: 0 on success, -1 on error
 */
int gfs2_compute_bitstructs(struct gfs2_sbd *sdp, struct rgrp_tree *rgd)
{
	struct gfs2_bitmap *bits;
	uint32_t length = rgd->ri.ri_length;
	uint32_t bytes_left, bytes;
	int x;

	/* Max size of an rg is 2GB.  A 2GB RG with (minimum) 512-byte blocks
	   has 4194304 blocks.  We can represent 4 blocks in one bitmap byte.
	   Therefore, all 4194304 blocks can be represented in 1048576 bytes.
	   Subtract a metadata header for each 512-byte block and we get
	   488 bytes of bitmap per block.  Divide 1048576 by 488 and we can
	   be assured we should never have more than 2149 of them. */
	if (length > 2149 || length == 0)
		return -1;
	if(rgd->bits == NULL && !(rgd->bits = (struct gfs2_bitmap *)
		 malloc(length * sizeof(struct gfs2_bitmap))))
		return -1;
	if(!memset(rgd->bits, 0, length * sizeof(struct gfs2_bitmap)))
		return -1;
	
	bytes_left = rgd->ri.ri_bitbytes;

	for (x = 0; x < length; x++){
		bits = &rgd->bits[x];

		if (length == 1){
			bytes = bytes_left;
			bits->bi_offset = sizeof(struct gfs2_rgrp);
			bits->bi_start = 0;
			bits->bi_len = bytes;
		}
		else if (x == 0){
			bytes = sdp->sd_sb.sb_bsize - sizeof(struct gfs2_rgrp);
			bits->bi_offset = sizeof(struct gfs2_rgrp);
			bits->bi_start = 0;
			bits->bi_len = bytes;
		}
		else if (x + 1 == length){
			bytes = bytes_left;
			bits->bi_offset = sizeof(struct gfs2_meta_header);
			bits->bi_start = rgd->ri.ri_bitbytes - bytes_left;
			bits->bi_len = bytes;
		}
		else{
			bytes = sdp->sd_sb.sb_bsize - sizeof(struct gfs2_meta_header);
			bits->bi_offset = sizeof(struct gfs2_meta_header);
			bits->bi_start = rgd->ri.ri_bitbytes - bytes_left;
			bits->bi_len = bytes;
		}

		bytes_left -= bytes;
	}

	if(bytes_left)
		return -1;

	if((rgd->bits[length - 1].bi_start +
	    rgd->bits[length - 1].bi_len) * GFS2_NBBY != rgd->ri.ri_data)
		return -1;

	if (rgd->bh)      /* If we already have a bh allocated */
		return 0; /* don't want to allocate another */
	if(!(rgd->bh = (struct gfs2_buffer_head **)
		 malloc(length * sizeof(struct gfs2_buffer_head *))))
		return -1;
	if(!memset(rgd->bh, 0, length * sizeof(struct gfs2_buffer_head *)))
		return -1;

	return 0;
}


/**
 * blk2rgrpd - Find resource group for a given data block number
 * @sdp: The GFS superblock
 * @n: The data block number
 *
 * Returns: Ths resource group, or NULL if not found
 */
struct rgrp_tree *gfs2_blk2rgrpd(struct gfs2_sbd *sdp, uint64_t blk)
{
	struct rgrp_tree *rgd = (struct rgrp_tree *)sdp->rgtree.osi_node;
	while (rgd) {
		if (blk < rgd->ri.ri_addr)
			rgd = (struct rgrp_tree *)rgd->node.osi_left;
		else if (blk >= rgd->ri.ri_data0 + rgd->ri.ri_data)
			rgd = (struct rgrp_tree *)rgd->node.osi_right;
		else
			return rgd;
	}
	return NULL;
}

/**
 * gfs2_rgrp_read - read in the resource group information from disk.
 * @rgd - resource group structure
 * returns: 0 if no error, otherwise the block number that failed
 */
uint64_t gfs2_rgrp_read(struct gfs2_sbd *sdp, struct rgrp_tree *rgd)
{
	int x, length = rgd->ri.ri_length;
	uint64_t max_rgrp_bitbytes, max_rgrp_len;

	/* Max size of an rgrp is 2GB.  Figure out how many blocks that is: */
	max_rgrp_bitbytes = ((2147483648 / sdp->bsize) / GFS2_NBBY);
	max_rgrp_len = max_rgrp_bitbytes / sdp->bsize;
	if (!length && length > max_rgrp_len)
		return -1;
	if (gfs2_check_range(sdp, rgd->ri.ri_addr))
		return -1;
	for (x = 0; x < length; x++){
		rgd->bh[x] = bread(sdp, rgd->ri.ri_addr + x);
		if(gfs2_check_meta(rgd->bh[x],
				   (x) ? GFS2_METATYPE_RB : GFS2_METATYPE_RG))
		{
			uint64_t error;

			error = rgd->ri.ri_addr + x;
			for (; x >= 0; x--) {
				brelse(rgd->bh[x]);
				rgd->bh[x] = NULL;
			}
			return error;
		}
	}
	if (rgd->bh && rgd->bh[0]) {
		if (sdp->gfs1)
			gfs_rgrp_in((struct gfs_rgrp *)&rgd->rg, rgd->bh[0]);
		else
			gfs2_rgrp_in(&rgd->rg, rgd->bh[0]);
	}
	return 0;
}

void gfs2_rgrp_relse(struct rgrp_tree *rgd)
{
	int x, length = rgd->ri.ri_length;

	for (x = 0; x < length; x++) {
		if (rgd->bh) {
			if (rgd->bh[x])
				brelse(rgd->bh[x]);
			rgd->bh[x] = NULL;
		}
	}
}

struct rgrp_tree *rgrp_insert(struct osi_root *rgtree, uint64_t rgblock)
{
	struct osi_node **newn = &rgtree->osi_node, *parent = NULL;
	struct rgrp_tree *data;

	/* Figure out where to put new node */
	while (*newn) {
		struct rgrp_tree *cur = (struct rgrp_tree *)*newn;

		parent = *newn;
		if (rgblock < cur->ri.ri_addr)
			newn = &((*newn)->osi_left);
		else if (rgblock > cur->ri.ri_addr)
			newn = &((*newn)->osi_right);
		else
			return cur;
	}

	data = malloc(sizeof(struct rgrp_tree));
	if (!data)
		return NULL;
	if (!memset(data, 0, sizeof(struct rgrp_tree)))
		return NULL;
	/* Add new node and rebalance tree. */
	data->ri.ri_addr = rgblock;
	osi_link_node(&data->node, parent, newn);
	osi_insert_color(&data->node, rgtree);

	return data;
}

void gfs2_rgrp_free(struct osi_root *rgrp_tree)
{
	struct rgrp_tree *rgd;
	int rgs_since_sync = 0;
	struct osi_node *n;
	struct gfs2_sbd *sdp = NULL;

	while ((n = osi_first(rgrp_tree))) {
		rgd = (struct rgrp_tree *)n;
		if (rgd->bh && rgd->bh[0]) { /* if a buffer exists        */
			rgs_since_sync++;
			if (rgs_since_sync >= RG_SYNC_TOLERANCE) {
				if (!sdp)
					sdp = rgd->bh[0]->sdp;
				fsync(sdp->device_fd);
				rgs_since_sync = 0;
			}
			gfs2_rgrp_relse(rgd); /* free them all. */
		}
		if(rgd->bits)
			free(rgd->bits);
		if(rgd->bh) {
			free(rgd->bh);
			rgd->bh = NULL;
		}
		osi_erase(&rgd->node, rgrp_tree);
		free(rgd);
	}
}
