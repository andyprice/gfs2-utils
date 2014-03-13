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
 * bsize: Block size
 * rgd: The resource group descriptor
 * Returns: 0 on success, -1 on error
 */
int gfs2_compute_bitstructs(const uint32_t bsize, struct rgrp_tree *rgd)
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
			bytes = bsize - sizeof(struct gfs2_rgrp);
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
			bytes = bsize - sizeof(struct gfs2_meta_header);
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
	if (breadm(sdp, rgd->bh, length, rgd->ri.ri_addr))
		return -1;
	for (x = 0; x < length; x++){
		if(gfs2_check_meta(rgd->bh[x], (x) ? GFS2_METATYPE_RB : GFS2_METATYPE_RG))
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

	data = calloc(1, sizeof(struct rgrp_tree));
	if (!data)
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

struct rgplan {
	uint32_t num;
	uint32_t len;
};

/**
 * This structure is defined in libgfs2.h as an opaque type. It stores the
 * constants and context required for creating resource groups from any point
 * in an application.
 */
struct _lgfs2_rgrps {
	struct osi_root root;
	struct rgplan plan[2];
	unsigned bsize;
	unsigned long align;
	unsigned long align_off;
	uint64_t devlen;
};

static uint64_t align_block(const uint64_t base, const uint64_t align)
{
	if ((align > 0) && ((base % align) > 0))
		return (base - (base % align)) + align;
	return base;
}

/**
 * Calculate the aligned block address of a resource group.
 * rgs: The resource groups handle
 * base: The base address of the first resource group address, in blocks
 * Returns the aligned address of the first resource group.
 */
uint64_t lgfs2_rgrp_align_addr(const lgfs2_rgrps_t rgs, uint64_t addr)
{
	return align_block(addr, rgs->align);
}

/**
 * Calculate the aligned relative address of the next resource group (and thus
 * the aligned length of this one).
 * rgs: The resource groups handle
 * base: The base length of the current resource group, in blocks
 * Returns the length of the resource group (the aligned relative address of
 * the next one)
 */
uint32_t lgfs2_rgrp_align_len(const lgfs2_rgrps_t rgs, uint32_t len)
{
	return align_block(len, rgs->align) + rgs->align_off;
}

/**
 * Plan the sizes of resource groups for remaining free space, based on a
 * target maximum size. In order to make best use of the space while keeping
 * the resource groups aligned appropriately we need to either reduce the
 * length of every resource group or of a subset of the resource groups, so
 * we're left with either one or two resource group sizes. We keep track of
 * both of these and the numbers of each size of resource group inside the
 * resource groups descriptor.
 * rgs: The resource groups descriptor
 * space: The number of remaining blocks to be allocated
 * tgtsize: The target resource group size in blocks
 * Returns the larger of the calculated resource group sizes or 0 if the
 * smaller would be less than GFS2_MIN_RGSIZE.
 */
uint32_t lgfs2_rgrps_plan(const lgfs2_rgrps_t rgs, uint64_t space, uint32_t tgtsize)
{
	uint32_t maxlen = (GFS2_MAX_RGSIZE << 20) / rgs->bsize;
	uint32_t minlen = (GFS2_MIN_RGSIZE << 20) / rgs->bsize;

	/* Apps should already have checked that the rg size is <=
	   GFS2_MAX_RGSIZE but just in case alignment pushes it over we clamp
	   it back down while calculating the initial rgrp length.  */
	do {
		rgs->plan[0].len = lgfs2_rgrp_align_len(rgs, tgtsize);
		tgtsize -= (rgs->align + 1);
	} while (rgs->plan[0].len > maxlen);

	rgs->plan[0].num = space / rgs->plan[0].len;

	if ((space - (rgs->plan[0].num * rgs->plan[0].len)) > rgs->align) {
		unsigned adj = (rgs->align > 0) ? rgs->align : 1;

		/* Spread the adjustment required to fit a new rgrp at the end
		   over all of the rgrps so that we don't end with a single
		   tiny one.  */
		while (((rgs->plan[0].len - adj) * (rgs->plan[0].num + 1)) >= space)
			rgs->plan[0].len -= adj;

		/* We've adjusted the size of the rgrps down as far as we can
		   without leaving a large gap at the end of the device now,
		   but we still need to reduce the size of some rgrps in order
		   to make everything fit, so we use the second rgplan to
		   specify a second length for a subset of the resource groups.
		   If plan[0].len already divides the space with no remainder,
		   plan[1].num will stay 0 and it won't be used.  */
		rgs->plan[1].len = rgs->plan[0].len - adj;
		rgs->plan[1].num = 0;

		while (((rgs->plan[0].len * rgs->plan[0].num) +
		        (rgs->plan[1].len * rgs->plan[1].num)) > space) {
			/* Total number of rgrps stays constant now. We just
			   need to shift some weight around */
			rgs->plan[0].num--;
			rgs->plan[1].num++;
		}
	}

	/* Once we've reached this point,
	   (plan[0].num * plan[0].len) + (plan[1].num * plan[1].len)
	   will be less than one adjustment smaller than 'space'.  */

	if (rgs->plan[0].len < minlen)
		return 0;

	return rgs->plan[0].len;
}

/**
 * Create and initialise an empty set of resource groups
 * bsize: The block size of the fs
 * devlen: The length of the device, in fs blocks
 * align: The required stripe alignment of the resource groups. Must be a multiple of 'offset'.
 * offset: The required stripe offset of the resource groups
 * Returns an initialised lgfs2_rgrps_t or NULL if unsuccessful with errno set
 */
lgfs2_rgrps_t lgfs2_rgrps_init(unsigned bsize, uint64_t devlen, uint64_t align, uint64_t offset)
{
	lgfs2_rgrps_t rgs;

	errno = EINVAL;
	if (offset != 0 && (align % offset) != 0)
		return NULL;

	rgs = calloc(1, sizeof(*rgs));
	if (rgs == NULL)
		return NULL;

	rgs->bsize = bsize;
	rgs->devlen = devlen;
	rgs->align = align;
	rgs->align_off = offset;
	memset(&rgs->root, 0, sizeof(rgs->root));

	return rgs;
}

/**
 * Return the rindex structure relating to a a resource group.
 */
struct gfs2_rindex *lgfs2_rgrp_index(lgfs2_rgrp_t rg)
{
	return &rg->ri;
}

/**
 * Returns the total resource group size, in blocks, required to give blksreq data blocks
 */
unsigned lgfs2_rgsize_for_data(uint64_t blksreq, unsigned bsize)
{
	const uint32_t blks_rgrp = GFS2_NBBY * (bsize - sizeof(struct gfs2_rgrp));
	const uint32_t blks_meta = GFS2_NBBY * (bsize - sizeof(struct gfs2_meta_header));
	unsigned bitblocks = 1;
	if (blksreq > blks_rgrp)
		bitblocks += ((blksreq - blks_rgrp) + blks_meta - 1) / blks_meta;
	return bitblocks + blksreq;
}

// Temporary function to aid in API migration
struct osi_node *lgfs2_rgrps_root(lgfs2_rgrps_t rgs)
{
	return rgs->root.osi_node;
}

/**
 * Create a new resource group after the last resource group in a set.
 * rgs: The set of resource groups
 * addr: The address at which to place this resource group
 * rglen: The required length of the resource group, in fs blocks.
 * Returns the new resource group on success or NULL on failure with errno set.
 * If errno is ENOSPC on a NULL return from this function, it could be
 * interpreted as 'finished' unless you expected there to be enough space on
 * the device for the resource group.
 */
lgfs2_rgrp_t lgfs2_rgrp_append(lgfs2_rgrps_t rgs, uint64_t addr, uint32_t rglen, uint64_t *nextaddr)
{
	int err = 0;
	lgfs2_rgrp_t rg;

	if (rglen == 0) {
		if (rgs->plan[0].num > 0) {
			rglen = rgs->plan[0].len;
			rgs->plan[0].num--;
		} else if (rgs->plan[1].num > 0) {
			rglen = rgs->plan[1].len;
			rgs->plan[1].num--;
		} else {
			errno = ENOSPC;
			return NULL;
		}
	}
	if (addr + rglen > rgs->devlen) {
		errno = ENOSPC;
		return NULL;
	}

	rg = rgrp_insert(&rgs->root, addr);
	if (rg == NULL)
		return NULL;

	rg->ri.ri_length = rgblocks2bitblocks(rgs->bsize, rglen, &rg->ri.ri_data);
	rg->ri.ri_data0 = rg->ri.ri_addr + rg->ri.ri_length;
	rg->ri.ri_bitbytes = rg->ri.ri_data / GFS2_NBBY;
	rg->rg.rg_header.mh_magic = GFS2_MAGIC;
	rg->rg.rg_header.mh_type = GFS2_METATYPE_RG;
	rg->rg.rg_header.mh_format = GFS2_FORMAT_RG;
	rg->rg.rg_free = rg->ri.ri_data;
	err = gfs2_compute_bitstructs(rgs->bsize, rg);
	if (err != 0)
		return NULL;

	if (nextaddr)
		*nextaddr = rg->ri.ri_addr + rglen;
	return rg;
}

/**
 * Write a resource group to a file descriptor.
 * Returns 0 on success or non-zero on failure with errno set
 */
int lgfs2_rgrp_write(const lgfs2_rgrps_t rgs, int fd, const lgfs2_rgrp_t rg)
{
	ssize_t ret = 0;
	size_t len = rg->ri.ri_length * rgs->bsize;
	unsigned int i;
	const struct gfs2_meta_header bmh = {
		.mh_magic = GFS2_MAGIC,
		.mh_type = GFS2_METATYPE_RB,
		.mh_format = GFS2_FORMAT_RB,
	};
	char *buff = calloc(len, 1);
	if (buff == NULL)
		return -1;

	gfs2_rgrp_out(&rg->rg, buff);
	for (i = 1; i < rg->ri.ri_length; i++)
		gfs2_meta_header_out(&bmh, buff + (i * rgs->bsize));

	ret = pwrite(fd, buff, len, rg->ri.ri_addr * rgs->bsize);
	if (ret != len) {
		free(buff);
		return -1;
	}

	free(buff);
	return 0;
}
