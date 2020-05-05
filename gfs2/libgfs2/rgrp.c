#include "clusterautoconfig.h"

#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "libgfs2.h"
#include "rgrp.h"

#define ROUND_UP(N, S) ((((N) + (S) - 1) / (S)) * (S))

static void compute_bitmaps(lgfs2_rgrp_t rg, const unsigned bsize)
{
	int x;

	rg->bits[0].bi_offset = sizeof(struct gfs2_rgrp);
	rg->bits[0].bi_start = 0;
	rg->bits[0].bi_len = bsize - sizeof(struct gfs2_rgrp);

	for (x = 1; x < rg->ri.ri_length; x++) {
		rg->bits[x].bi_offset = sizeof(struct gfs2_meta_header);
		rg->bits[x].bi_start = rg->bits[x - 1].bi_start + rg->bits[x - 1].bi_len;
		rg->bits[x].bi_len = bsize - sizeof(struct gfs2_meta_header);
	}
	x--;
	rg->bits[x].bi_len = rg->ri.ri_bitbytes - rg->bits[x].bi_start;
}

/**
 * gfs2_compute_bitstructs - Compute the bitmap sizes
 * bsize: Block size
 * rgd: The resource group descriptor
 * Returns: 0 on success, -1 on error
 */
int gfs2_compute_bitstructs(const uint32_t bsize, struct rgrp_tree *rgd)
{
	uint32_t length = rgd->ri.ri_length;
	uint32_t bytes_left;
	int ownbits = 0;

	/* Max size of an rg is 2GB.  A 2GB RG with (minimum) 512-byte blocks
	   has 4194304 blocks.  We can represent 4 blocks in one bitmap byte.
	   Therefore, all 4194304 blocks can be represented in 1048576 bytes.
	   Subtract a metadata header for each 512-byte block and we get
	   488 bytes of bitmap per block.  Divide 1048576 by 488 and we can
	   be assured we should never have more than 2149 of them. */
	errno = EINVAL;
	if (length > 2149 || length == 0)
		return -1;

	if(rgd->bits == NULL) {
		rgd->bits = calloc(length, sizeof(struct gfs2_bitmap));
		if(rgd->bits == NULL)
			return -1;
		ownbits = 1;
	}

	compute_bitmaps(rgd, bsize);
	bytes_left = rgd->ri.ri_bitbytes - (rgd->bits[rgd->ri.ri_length - 1].bi_start +
	                                    rgd->bits[rgd->ri.ri_length - 1].bi_len);
	errno = EINVAL;
	if(bytes_left)
		goto errbits;

	if((rgd->bits[length - 1].bi_start +
	    rgd->bits[length - 1].bi_len) * GFS2_NBBY != rgd->ri.ri_data)
		goto errbits;

	return 0;
errbits:
	if (ownbits) {
		free(rgd->bits);
		rgd->bits = NULL;
	}
	return -1;
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
 * Allocate a multi-block buffer for a resource group's bitmaps. This is done
 * as one chunk and should be freed using lgfs2_rgrp_bitbuf_free().
 * Returns 0 on success with the bitmap buffer allocated in the resource group,
 * or non-zero on failure with errno set.
 */
int lgfs2_rgrp_bitbuf_alloc(lgfs2_rgrp_t rg)
{
	struct gfs2_sbd *sdp = rg->rgrps->sdp;
	size_t len = rg->ri.ri_length * sdp->bsize;
	unsigned long io_align = sdp->bsize;
	unsigned i;
	char *bufs;

	if (rg->rgrps->align > 0) {
		len = ROUND_UP(len, rg->rgrps->align * sdp->bsize);
		io_align = rg->rgrps->align_off * sdp->bsize;
	}
	if (posix_memalign((void **)&bufs, io_align, len) != 0) {
		errno = ENOMEM;
		return 1;
	}
	memset(bufs, 0, len);

	for (i = 0; i < rg->ri.ri_length; i++) {
		rg->bits[i].bi_data = bufs + (i * sdp->bsize);
		rg->bits[i].bi_modified = 0;
	}
	return 0;
}

/**
 * Free the multi-block bitmap buffer from a resource group. The buffer should
 * have been allocated as a single chunk as in lgfs2_rgrp_bitbuf_alloc().
 * This does not implicitly write the bitmaps to disk. Use lgfs2_rgrp_write()
 * for that.
 * rg: The resource groups whose bitmap buffer should be freed.
 */
void lgfs2_rgrp_bitbuf_free(lgfs2_rgrp_t rg)
{
	unsigned i;

	free(rg->bits[0].bi_data);
	for (i = 0; i < rg->ri.ri_length; i++) {
		rg->bits[i].bi_data = NULL;
		rg->bits[i].bi_modified = 0;
	}
}

/**
 * Check a resource group's crc
 * Returns 0 on success, non-zero if crc is bad
 */
int lgfs2_rgrp_crc_check(char *buf)
{
	int ret = 0;
#ifdef GFS2_HAS_RG_RI_FIELDS
	struct gfs2_rgrp *rg = (struct gfs2_rgrp *)buf;
	uint32_t crc = rg->rg_crc;

	if (crc == 0)
		return 0;

	rg->rg_crc = 0;
	if (be32_to_cpu(crc) != gfs2_disk_hash(buf, sizeof(struct gfs2_rgrp)))
		ret = 1;
	rg->rg_crc = crc;
#endif
	return ret;
}

/**
 * Set the crc of an on-disk resource group
 */
void lgfs2_rgrp_crc_set(char *buf)
{
#ifdef GFS2_HAS_RG_RI_FIELDS
	struct gfs2_rgrp *rg = (struct gfs2_rgrp *)buf;
	uint32_t crc;

	rg->rg_crc = 0;
	crc = gfs2_disk_hash(buf, sizeof(struct gfs2_rgrp));
	rg->rg_crc = cpu_to_be32(crc);
#endif
}

/**
 * gfs2_rgrp_read - read in the resource group information from disk.
 * @rgd - resource group structure
 * returns: 0 if no error, otherwise the block number that failed
 */
uint64_t gfs2_rgrp_read(struct gfs2_sbd *sdp, struct rgrp_tree *rgd)
{
	unsigned length = rgd->ri.ri_length * sdp->bsize;
	off_t offset = rgd->ri.ri_addr * sdp->bsize;
	char *buf;

	if (length == 0 || gfs2_check_range(sdp, rgd->ri.ri_addr))
		return -1;

	buf = calloc(1, length);
	if (buf == NULL)
		return -1;

	if (pread(sdp->device_fd, buf, length, offset) != length) {
		free(buf);
		return -1;
	}

	for (unsigned i = 0; i < rgd->ri.ri_length; i++) {
		int mtype = (i ? GFS2_METATYPE_RB : GFS2_METATYPE_RG);

		rgd->bits[i].bi_data = buf + (i * sdp->bsize);
		if (gfs2_check_meta(rgd->bits[i].bi_data, mtype)) {
			free(buf);
			return rgd->ri.ri_addr + i;
		}
	}
	if (sdp->gfs1)
		gfs_rgrp_in((struct gfs_rgrp *)&rgd->rg, buf);
	else {
		if (lgfs2_rgrp_crc_check(buf)) {
			free(buf);
			return rgd->ri.ri_addr;
		}
		gfs2_rgrp_in(&rgd->rg, buf);
	}
	return 0;
}

void gfs2_rgrp_relse(struct gfs2_sbd *sdp, struct rgrp_tree *rgd)
{
	if (rgd->bits == NULL)
		return;
	for (unsigned i = 0; i < rgd->ri.ri_length; i++) {
		off_t offset = sdp->bsize * (rgd->ri.ri_addr + i);
		ssize_t ret;

		if (rgd->bits[i].bi_data == NULL || !rgd->bits[i].bi_modified)
			continue;

		ret = pwrite(sdp->device_fd, rgd->bits[i].bi_data, sdp->bsize, offset);
		if (ret != sdp->bsize) {
			fprintf(stderr, "Failed to write modified resource group at block %"PRIu64": %s\n",
			        (uint64_t)rgd->ri.ri_addr, strerror(errno));
		}
		rgd->bits[i].bi_modified = 0;
	}
	free(rgd->bits[0].bi_data);
	for (unsigned i = 0; i < rgd->ri.ri_length; i++)
		rgd->bits[i].bi_data = NULL;
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

void gfs2_rgrp_free(struct gfs2_sbd *sdp, struct osi_root *rgrp_tree)
{
	struct rgrp_tree *rgd;
	struct osi_node *n;

	if (OSI_EMPTY_ROOT(rgrp_tree))
		return;
	while ((n = osi_first(rgrp_tree))) {
		rgd = (struct rgrp_tree *)n;

		gfs2_rgrp_relse(sdp, rgd);
		free(rgd->bits);
		rgd->bits = NULL;
		osi_erase(&rgd->node, rgrp_tree);
		free(rgd);
	}
}

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
	return align_block(addr, rgs->align_off);
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
 * Returns the number of resource groups planned to fit in the given space, or
 * 0 if the smallest resource group would be smaller than GFS2_MIN_RGSIZE.
 */
uint32_t lgfs2_rgrps_plan(const lgfs2_rgrps_t rgs, uint64_t space, uint32_t tgtsize)
{
	uint32_t maxlen = (GFS2_MAX_RGSIZE << 20) / rgs->sdp->bsize;
	uint32_t minlen = (GFS2_MIN_RGSIZE << 20) / rgs->sdp->bsize;
	struct rg_spec *spec = rgs->plan->rg_specs;

	/* Apps should already have checked that the rg size is <=
	   GFS2_MAX_RGSIZE but just in case alignment pushes it over we clamp
	   it back down while calculating the initial rgrp length.  */
	do {
		spec[0].len = lgfs2_rgrp_align_len(rgs, tgtsize);
		tgtsize -= (rgs->align + 1);
	} while (spec[0].len > maxlen);

	spec[0].num = space / spec[0].len;

	if ((space - (spec[0].num * spec[0].len)) > rgs->align) {
		unsigned adj = (rgs->align > 0) ? rgs->align : 1;

		/* Spread the adjustment required to fit a new rgrp at the end
		   over all of the rgrps so that we don't end with a single
		   tiny one.  */
		spec[0].num++;
		while (((spec[0].len - adj) * (uint64_t)spec[0].num) >= space)
			spec[0].len -= adj;

		/* We've adjusted the size of the rgrps down as far as we can
		   without leaving a large gap at the end of the device now,
		   but we still need to reduce the size of some rgrps in order
		   to make everything fit, so we use the second rgplan to
		   specify a second length for a subset of the resource groups.
		   If plan[0].len already divides the space with no remainder,
		   plan[1].num will stay 0 and it won't be used.  */
		spec[1].len = spec[0].len - adj;
		spec[1].num = 0;

		while (((spec[0].len * spec[0].num) +
		        (spec[1].len * spec[1].num)) >= space) {
			/* Total number of rgrps stays constant now. We just
			   need to shift some weight around */
			spec[0].num--;
			spec[1].num++;
		}
	}

	/* Once we've reached this point,
	   (spec[0].num * spec[0].len) + (spec[1].num * spec[1].len)
	   will be less than one adjustment smaller than 'space'.  */
	if (spec[0].len < minlen)
		return 0;

	return spec[0].num + spec[1].num;
}

/**
 * Create and initialise an empty set of resource groups
 * bsize: The block size of the fs
 * devlen: The length of the device, in fs blocks
 * align: The required stripe alignment of the resource groups. Must be a multiple of 'offset'.
 * offset: The required stripe offset of the resource groups
 * Returns an initialised lgfs2_rgrps_t or NULL if unsuccessful with errno set
 */
lgfs2_rgrps_t lgfs2_rgrps_init(struct gfs2_sbd *sdp, uint64_t align, uint64_t offset)
{
	lgfs2_rgrps_t rgs;

	errno = EINVAL;
	if (offset != 0 && (align % offset) != 0)
		return NULL;

	rgs = calloc(1, sizeof(*rgs));
	if (rgs == NULL)
		return NULL;

	rgs->plan = calloc(1, sizeof(struct rgs_plan) + (5 * sizeof(struct rg_spec)));
	if (rgs->plan == NULL) {
		free(rgs);
		return NULL;
	}
	rgs->plan->length = 0;
	rgs->plan->capacity = 5;
	rgs->sdp = sdp;
	rgs->align = align;
	rgs->align_off = offset;
	memset(&rgs->root, 0, sizeof(rgs->root));

	return rgs;
}

/**
 * Populate a set of resource groups from a gfs2 rindex file.
 * fd: An open file descriptor for the rindex file.
 * rgs: The set of resource groups.
 * Returns the number of resource groups added to the set or 0 on error with
 * errno set.
 */
unsigned lgfs2_rindex_read_fd(int fd, lgfs2_rgrps_t rgs)
{
	unsigned count = 0;
	char buf[sizeof(struct gfs2_rindex)];

	errno = EINVAL;
	if (fd < 0 || rgs == NULL)
		return 0;

	while (1) {
		lgfs2_rgrp_t rg;
		struct gfs2_rindex ri;
		ssize_t ret = read(fd, buf, sizeof(struct gfs2_rindex));
		if (ret == 0)
			break;

		if (ret != sizeof(struct gfs2_rindex))
			return 0;

		gfs2_rindex_in(&ri, buf);
		rg = lgfs2_rgrps_append(rgs, &ri, 0);
		if (rg == NULL)
			return 0;
		count++;
	}
	return count;
}

/**
 * Read a rindex entry into a set of resource groups
 * rip: The inode of the rindex file
 * rgs: The set of resource groups.
 * i: The index of the entry to read from the rindex file
 * Returns the new rindex entry added to the set or NULL on error with errno
 * set.
 */
const struct gfs2_rindex *lgfs2_rindex_read_one(struct gfs2_inode *rip, lgfs2_rgrps_t rgs, unsigned i)
{
	uint64_t off = i * sizeof(struct gfs2_rindex);
	char buf[sizeof(struct gfs2_rindex)];
	struct gfs2_rindex ri;
	lgfs2_rgrp_t rg;
	int ret;

	errno = EINVAL;
	if (rip == NULL || rgs == NULL)
		return NULL;

	ret = gfs2_readi(rip, buf, off, sizeof(struct gfs2_rindex));
	if (ret != sizeof(struct gfs2_rindex))
		return NULL;

	gfs2_rindex_in(&ri, buf);
	rg = lgfs2_rgrps_append(rgs, &ri, 0);
	if (rg == NULL)
		return NULL;

	return &rg->ri;
}

/**
 * Free a set of resource groups created with lgfs2_rgrps_append() etc. This
 * does not write any dirty buffers to disk. See lgfs2_rgrp_write().
 * rgs: A pointer to the set of resource groups to be freed.
 */
void lgfs2_rgrps_free(lgfs2_rgrps_t *rgs)
{
	lgfs2_rgrp_t rg;
	struct osi_root *tree = &(*rgs)->root;

	while ((rg = (struct rgrp_tree *)osi_first(tree))) {
		int i;
		free(rg->bits[0].bi_data);
		for (i = 0; i < rg->ri.ri_length; i++) {
			rg->bits[i].bi_data = NULL;
		}
		osi_erase(&rg->node, tree);
		free(rg);
	}
	free((*rgs)->plan);
	free(*rgs);
	*rgs = NULL;
}

/**
 * Calculate the fields for a new entry in the resource group index.
 * ri: A pointer to the resource group index entry to be calculated.
 * addr: The address at which to place this resource group
 * len: The required length of the resource group, in fs blocks.
 *        If rglen is 0, geometry previously calculated by lgfs2_rgrps_plan() will be used.
 * Returns the calculated address of the next resource group or 0 with errno set:
 *   EINVAL - The entry pointer is NULL
 *   ENOSPC - This rgrp would extend past the end of the device
 */
uint64_t lgfs2_rindex_entry_new(lgfs2_rgrps_t rgs, struct gfs2_rindex *ri, uint64_t addr, uint32_t len)
{
	struct rg_spec *spec = rgs->plan->rg_specs;
	int plan = -1;
	errno = EINVAL;
	if (!ri)
		return 0;

	errno = ENOSPC;
	if (spec[0].num > 0)
		plan = 0;
	else if (spec[1].num > 0)
		plan = 1;
	else if (len == 0)
		return 0;

	if (plan >= 0 && (len == 0 || len == spec[plan].len)) {
		len = spec[plan].len;
		spec[plan].num--;
	}

	if (addr + len > rgs->sdp->device.length)
		return 0;

	ri->ri_addr = addr;
	ri->ri_length = rgblocks2bitblocks(rgs->sdp->bsize, len, &ri->ri_data);
	ri->__pad = 0;
	ri->ri_data0 = ri->ri_addr + ri->ri_length;
	ri->ri_bitbytes = ri->ri_data / GFS2_NBBY;
	memset(&ri->ri_reserved, 0, sizeof(ri->ri_reserved));

	return ri->ri_addr + len;
}

/**
 * Return the rindex structure relating to a resource group.
 * The return type is const to advise callers that making changes to this
 * structure directly isn't wise. libgfs2 functions should be used instead.
 */
const struct gfs2_rindex *lgfs2_rgrp_index(lgfs2_rgrp_t rg)
{
	return &rg->ri;
}

/**
 * Return the rgrp structure relating to a resource group.
 * The return type is const to advise callers that making changes to this
 * structure directly isn't wise. libgfs2 functions should be used instead.
 */
const struct gfs2_rgrp *lgfs2_rgrp_rgrp(lgfs2_rgrp_t rg)
{
	return &rg->rg;
}

/**
 * Returns the total resource group size, in blocks, required to give blksreq data blocks
 */
unsigned lgfs2_rgsize_for_data(uint64_t blksreq, unsigned bsize)
{
	const uint32_t blks_rgrp = GFS2_NBBY * (bsize - sizeof(struct gfs2_rgrp));
	const uint32_t blks_meta = GFS2_NBBY * (bsize - sizeof(struct gfs2_meta_header));
	unsigned bitblocks = 1;
	blksreq = (blksreq + 3) & ~3;
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
 * Insert a new resource group after the last resource group in a set.
 * rgs: The set of resource groups
 * entry: The entry to be added
 * rg_skip: The value to be used for this resource group's rg_skip field
 * Returns the new resource group on success or NULL on failure with errno set.
 */
lgfs2_rgrp_t lgfs2_rgrps_append(lgfs2_rgrps_t rgs, struct gfs2_rindex *entry, uint32_t rg_skip)
{
	lgfs2_rgrp_t rg;
	struct osi_node **link = &rgs->root.osi_node;
	struct osi_node *parent = osi_last(&rgs->root);
	lgfs2_rgrp_t lastrg = (lgfs2_rgrp_t)parent;

	errno = EINVAL;
	if (entry == NULL)
		return NULL;

	if (lastrg != NULL) { /* Tree is not empty */
		if (entry->ri_addr <= lastrg->ri.ri_addr)
			return NULL; /* Appending with a lower address doesn't make sense */
		link = &lastrg->node.osi_right;
	}

	rg = calloc(1, sizeof(*rg) + (entry->ri_length * sizeof(struct gfs2_bitmap)));
	if (rg == NULL)
		return NULL;

	rg->bits = (struct gfs2_bitmap *)(rg + 1);

	osi_link_node(&rg->node, parent, link);
	osi_insert_color(&rg->node, &rgs->root);

	memcpy(&rg->ri, entry, sizeof(struct gfs2_rindex));
	rg->rg.rg_header.mh_magic = GFS2_MAGIC;
	rg->rg.rg_header.mh_type = GFS2_METATYPE_RG;
	rg->rg.rg_header.mh_format = GFS2_FORMAT_RG;
	rg->rg.rg_free = rg->ri.ri_data;
#ifdef GFS2_HAS_RG_SKIP
	rg->rg.rg_skip = rg_skip;
#endif
#ifdef GFS2_HAS_RG_RI_FIELDS
	rg->rg.rg_data0 = rg->ri.ri_data0;
	rg->rg.rg_data = rg->ri.ri_data;
	rg->rg.rg_bitbytes = rg->ri.ri_bitbytes;
	rg->rg.rg_crc = 0;
#endif
	compute_bitmaps(rg, rgs->sdp->bsize);
	rg->rgrps = rgs;
	return rg;
}

/**
 * Write a resource group to a file descriptor.
 * Returns 0 on success or non-zero on failure with errno set
 */
int lgfs2_rgrp_write(int fd, const lgfs2_rgrp_t rg)
{
	struct gfs2_sbd *sdp = rg->rgrps->sdp;
	unsigned int i;
	const struct gfs2_meta_header bmh = {
		.mh_magic = GFS2_MAGIC,
		.mh_type = GFS2_METATYPE_RB,
		.mh_format = GFS2_FORMAT_RB,
	};
	int freebufs = 0;
	ssize_t ret;
	size_t len;

	if (rg->bits[0].bi_data == NULL) {
		freebufs = 1;
		if (lgfs2_rgrp_bitbuf_alloc(rg) != 0)
			return -1;
	}
	gfs2_rgrp_out(&rg->rg, rg->bits[0].bi_data);
	for (i = 1; i < rg->ri.ri_length; i++) {
		gfs2_meta_header_out(&bmh, rg->bits[i].bi_data);
	}

	len = sdp->bsize * rg->ri.ri_length;
	if (rg->rgrps->align > 0)
		len = ROUND_UP(len, rg->rgrps->align * sdp->bsize);

	ret = pwrite(sdp->device_fd, rg->bits[0].bi_data, len, rg->ri.ri_addr * sdp->bsize);

	if (freebufs)
		lgfs2_rgrp_bitbuf_free(rg);

	return ret == len ? 0 : -1;
}

lgfs2_rgrp_t lgfs2_rgrp_first(lgfs2_rgrps_t rgs)
{
	return (lgfs2_rgrp_t)osi_first(&rgs->root);
}

lgfs2_rgrp_t lgfs2_rgrp_next(lgfs2_rgrp_t rg)
{
	return (lgfs2_rgrp_t)osi_next(&rg->node);
}

lgfs2_rgrp_t lgfs2_rgrp_prev(lgfs2_rgrp_t rg)
{
	return (lgfs2_rgrp_t)osi_prev(&rg->node);
}

lgfs2_rgrp_t lgfs2_rgrp_last(lgfs2_rgrps_t rgs)
{
	return (lgfs2_rgrp_t)osi_last(&rgs->root);
}

/**
 * gfs2_rbm_from_block - Set the rbm based upon rgd and block number
 * @rbm: The rbm with rgd already set correctly
 * @block: The block number (filesystem relative)
 *
 * This sets the bi and offset members of an rbm based on a
 * resource group and a filesystem relative block number. The
 * resource group must be set in the rbm on entry, the bi and
 * offset members will be set by this function.
 *
 * Returns: 0 on success, or non-zero with errno set
 */
int lgfs2_rbm_from_block(struct lgfs2_rbm *rbm, uint64_t block)
{
	uint64_t rblock = block - rbm->rgd->ri.ri_data0;
	struct gfs2_sbd *sdp = rbm->rgd->rgrps->sdp;

	if (rblock > UINT_MAX) {
		errno = EINVAL;
		return 1;
	}
	if (block >= rbm->rgd->ri.ri_data0 + rbm->rgd->ri.ri_data) {
		errno = E2BIG;
		return 1;
	}

	rbm->bii = 0;
	rbm->offset = (uint32_t)(rblock);
	/* Check if the block is within the first block */
	if (rbm->offset < (rbm_bi(rbm)->bi_len * GFS2_NBBY))
		return 0;

	/* Adjust for the size diff between gfs2_meta_header and gfs2_rgrp */
	rbm->offset += (sizeof(struct gfs2_rgrp) -
			sizeof(struct gfs2_meta_header)) * GFS2_NBBY;
	rbm->bii = rbm->offset / sdp->sd_blocks_per_bitmap;
	rbm->offset -= rbm->bii * sdp->sd_blocks_per_bitmap;
	return 0;
}

/**
 * lgfs2_rbm_incr - increment an rbm structure
 * @rbm: The rbm with rgd already set correctly
 *
 * This function takes an existing rbm structure and increments it to the next
 * viable block offset.
 *
 * Returns: If incrementing the offset would cause the rbm to go past the
 *          end of the rgrp, true is returned, otherwise false.
 *
 */
static int lgfs2_rbm_incr(struct lgfs2_rbm *rbm)
{
	if (rbm->offset + 1 < (rbm_bi(rbm)->bi_len * GFS2_NBBY)) { /* in the same bitmap */
		rbm->offset++;
		return 0;
	}
	if (rbm->bii == rbm->rgd->ri.ri_length - 1) /* at the last bitmap */
		return 1;

	rbm->offset = 0;
	rbm->bii++;
	return 0;
}

/**
 * lgfs2_testbit - test a bit in the bitmaps
 * @rbm: The bit to test
 *
 * Returns: The two bit block state of the requested bit
 */
static inline uint8_t lgfs2_testbit(const struct lgfs2_rbm *rbm)
{
	struct gfs2_bitmap *bi = rbm_bi(rbm);
	const uint8_t *buffer = (uint8_t *)bi->bi_data + bi->bi_offset;
	const uint8_t *byte;
	unsigned int bit;

	byte = buffer + (rbm->offset / GFS2_NBBY);
	bit = (rbm->offset % GFS2_NBBY) * GFS2_BIT_SIZE;

	return (*byte >> bit) & GFS2_BIT_MASK;
}

/**
 * lgfs2_unaligned_extlen - Look for free blocks which are not byte aligned
 * @rbm: Position to search (value/result)
 * @n_unaligned: Number of unaligned blocks to check
 * @len: Decremented for each block found (terminate on zero)
 *
 * Returns: true if a non-free block is encountered
 */
static int lgfs2_unaligned_extlen(struct lgfs2_rbm *rbm, uint32_t n_unaligned, uint32_t *len)
{
	uint32_t n;
	uint8_t res;

	for (n = 0; n < n_unaligned; n++) {
		res = lgfs2_testbit(rbm);
		if (res != GFS2_BLKST_FREE)
			return 1;
		(*len)--;
		if (*len == 0)
			return 1;
		if (lgfs2_rbm_incr(rbm))
			return 1;
	}

	return 0;
}

static uint8_t *check_bytes8(const uint8_t *start, uint8_t value, unsigned bytes)
{
	while (bytes) {
		if (*start != value)
			return (void *)start;
		start++;
		bytes--;
	}
	return NULL;
}

/**
 * lgfs2_free_extlen - Return extent length of free blocks
 * @rbm: Starting position
 * @len: Max length to check
 *
 * Starting at the block specified by the rbm, see how many free blocks
 * there are, not reading more than len blocks ahead. This can be done
 * using check_bytes8 when the blocks are byte aligned, but has to be done
 * on a block by block basis in case of unaligned blocks. Also this
 * function can cope with bitmap boundaries (although it must stop on
 * a resource group boundary)
 *
 * Returns: Number of free blocks in the extent
 */
static uint32_t lgfs2_free_extlen(const struct lgfs2_rbm *rrbm, uint32_t len)
{
	struct lgfs2_rbm rbm = *rrbm;
	uint32_t n_unaligned = rbm.offset & 3;
	uint32_t size = len;
	uint32_t bytes;
	uint32_t chunk_size;
	uint8_t *ptr, *start, *end;
	uint64_t block;
	struct gfs2_bitmap *bi;
	struct gfs2_sbd *sdp = rbm.rgd->rgrps->sdp;

	if (n_unaligned &&
	    lgfs2_unaligned_extlen(&rbm, 4 - n_unaligned, &len))
		goto out;

	n_unaligned = len & 3;
	/* Start is now byte aligned */
	while (len > 3) {
		bi = rbm_bi(&rbm);
		start = (uint8_t *)bi->bi_data;
		end = start + sdp->bsize;
		start += bi->bi_offset;
		start += (rbm.offset / GFS2_NBBY);
		bytes = (len / GFS2_NBBY) < (end - start) ? (len / GFS2_NBBY):(end - start);
		ptr = check_bytes8(start, 0, bytes);
		chunk_size = ((ptr == NULL) ? bytes : (ptr - start));
		chunk_size *= GFS2_NBBY;
		len -= chunk_size;
		block = lgfs2_rbm_to_block(&rbm);
		if (lgfs2_rbm_from_block(&rbm, block + chunk_size)) {
			n_unaligned = 0;
			break;
		}
		if (ptr) {
			n_unaligned = 3;
			break;
		}
		n_unaligned = len & 3;
	}

	/* Deal with any bits left over at the end */
	if (n_unaligned)
		lgfs2_unaligned_extlen(&rbm, n_unaligned, &len);
out:
	return size - len;
}

/**
 * gfs2_rbm_find - Look for blocks of a particular state
 * @rbm: Value/result starting position and final position
 * @state: The state which we want to find
 * @minext: Pointer to the requested extent length (NULL for a single block)
 *          This is updated to be the actual reservation size.
 *
 * Returns: 0 on success, non-zero with errno == ENOSPC if there is no block of the requested state
 */
int lgfs2_rbm_find(struct lgfs2_rbm *rbm, uint8_t state, uint32_t *minext)
{
	int initial_bii;
	uint32_t offset;
	int n = 0;
	int iters = rbm->rgd->ri.ri_length;
	uint32_t extlen;

	/* If we are not starting at the beginning of a bitmap, then we
	 * need to add one to the bitmap count to ensure that we search
	 * the starting bitmap twice.
	 */
	if (rbm->offset != 0)
		iters++;

	for (n = 0; n < iters; n++) {
		struct gfs2_bitmap *bi = rbm_bi(rbm);
		uint8_t *buf = (uint8_t *)bi->bi_data + bi->bi_offset;
		uint64_t block;
		int ret;

		if ((rbm->rgd->rg.rg_free < *minext) && (state == GFS2_BLKST_FREE))
			goto next_bitmap;

		offset = gfs2_bitfit(buf, bi->bi_len, rbm->offset, state);
		if (offset == BFITNOENT)
			goto next_bitmap;

		rbm->offset = offset;
		initial_bii = rbm->bii;
		block = lgfs2_rbm_to_block(rbm);
		extlen = 1;

		if (*minext != 0)
			extlen = lgfs2_free_extlen(rbm, *minext);

		if (extlen >= *minext)
			return 0;

		ret = lgfs2_rbm_from_block(rbm, block + extlen);
		if (ret == 0) {
			n += (rbm->bii - initial_bii);
			continue;
		}

		if (errno == E2BIG) {
			rbm->bii = 0;
			rbm->offset = 0;
			n += (rbm->bii - initial_bii);
			goto res_covered_end_of_rgrp;
		}

		return ret;

next_bitmap:	/* Find next bitmap in the rgrp */
		rbm->offset = 0;
		rbm->bii++;
		if (rbm->bii == rbm->rgd->ri.ri_length)
			rbm->bii = 0;

res_covered_end_of_rgrp:
		if (rbm->bii == 0)
			break;
	}

	errno = ENOSPC;
	return 1;
}

/**
 * lgfs2_alloc_extent - allocate an extent from a given bitmap
 * @rbm: the resource group information
 * @state: The state of the first block, GFS2_BLKST_DINODE or GFS2_BLKST_USED
 * @elen: The requested extent length
 * Returns the length of the extent allocated.
 */
unsigned lgfs2_alloc_extent(const struct lgfs2_rbm *rbm, int state, const unsigned elen)
{
	struct lgfs2_rbm pos = { .rgd = rbm->rgd, };
	const uint64_t block = lgfs2_rbm_to_block(rbm);
	unsigned len;

	gfs2_set_bitmap(rbm->rgd, block, state);

	for (len = 1; len < elen; len++) {
		int ret = lgfs2_rbm_from_block(&pos, block + len);
		if (ret || lgfs2_testbit(&pos) != GFS2_BLKST_FREE)
			break;
		gfs2_set_bitmap(pos.rgd, block + len, GFS2_BLKST_USED);
	}
	return len;
}
