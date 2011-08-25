#include "clusterautoconfig.h"

#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include <stdlib.h>
#include <libintl.h>
#define _(String) gettext(String)

#include "libgfs2.h"
#include "fsck.h"
#include "util.h"

static int gfs1_convert_mark(uint8_t q, uint32_t *count)
{
	switch(q) {

	case gfs2_meta_inval:
	case gfs2_inode_invalid:
		/* Convert invalid metadata to free blocks */
	case gfs2_block_free:
		count[0]++;
		return GFS2_BLKST_FREE;

	case gfs2_block_used:
		count[2]++;
		return GFS2_BLKST_USED;

	case gfs2_inode_dir:
	case gfs2_inode_file:
	case gfs2_inode_lnk:
	case gfs2_inode_device:
	case gfs2_inode_fifo:
	case gfs2_inode_sock:
		count[1]++;
		return GFS2_BLKST_DINODE;

	case gfs2_indir_blk:
	case gfs2_leaf_blk:
	/*case gfs2_meta_rgrp:*/
	case gfs2_jdata: /* gfs1 jdata blocks count as "metadata" and gfs1
			    metadata is marked the same as gfs2 inode in the
			    bitmap. */
	case gfs2_meta_eattr:
		count[3]++;
		return GFS2_BLKST_DINODE;

	case gfs2_freemeta:
		count[4]++;
		return GFS2_BLKST_UNLINKED;

	default:
		log_err( _("Invalid block type %d found\n"), q);
	}
	return -1;
}

static int gfs2_convert_mark(uint8_t q, uint32_t *count)
{
	switch(q) {

	case gfs2_meta_inval:
	case gfs2_inode_invalid:
		/* Convert invalid metadata to free blocks */
	case gfs2_block_free:
		count[0]++;
		return GFS2_BLKST_FREE;

	case gfs2_block_used:
		count[2]++;
		return GFS2_BLKST_USED;

	case gfs2_inode_dir:
	case gfs2_inode_file:
	case gfs2_inode_lnk:
	case gfs2_inode_device:
	case gfs2_jdata: /* gfs1 jdata blocks count as "metadata" and gfs1
			    metadata is marked the same as gfs2 inode in the
			    bitmap. */
	case gfs2_inode_fifo:
	case gfs2_inode_sock:
		count[1]++;
		return GFS2_BLKST_DINODE;

	case gfs2_indir_blk:
	case gfs2_leaf_blk:
	case gfs2_meta_eattr:
		count[2]++;
		return GFS2_BLKST_USED;

	case gfs2_freemeta:
		log_err( _("Invalid freemeta type %d found\n"), q);
		count[4]++;
		return -1;

	default:
		log_err( _("Invalid block type %d found\n"), q);
	}
	return -1;
}


static int check_block_status(struct gfs2_sbd *sdp, char *buffer,
			      unsigned int buflen, uint64_t *rg_block,
			      uint64_t rg_data, uint32_t *count)
{
	unsigned char *byte, *end;
	unsigned int bit;
	unsigned char rg_status, block_status;
	uint8_t q;
	uint64_t block;

	/* FIXME verify cast */
	byte = (unsigned char *) buffer;
	bit = 0;
	end = (unsigned char *) buffer + buflen;

	while (byte < end) {
		rg_status = ((*byte >> bit) & GFS2_BIT_MASK);
		block = rg_data + *rg_block;
		warm_fuzzy_stuff(block);
		if (skip_this_pass || fsck_abort) /* if asked to skip the rest */
			return 0;
		q = block_type(block);

		if (sdp->gfs1)
			block_status = gfs1_convert_mark(q, count);
		else
			block_status = gfs2_convert_mark(q, count);

		/* If one node opens a file and another node deletes it, we
		   may be left with a block that appears to be "unlinked" in
		   the bitmap, but nothing links to it. This is a valid case
		   and should be cleaned up by the file system eventually.
		   So we ignore it. */
		if (rg_status == GFS2_BLKST_UNLINKED &&
		    block_status == GFS2_BLKST_FREE) {
			log_err( _("Unlinked inode found at block %llu "
				   "(0x%llx).\n"),
				 (unsigned long long)block,
				 (unsigned long long)block);
			if (query(_("Do you want to reclaim the block? "
				   "(y/n) "))) {
				if (gfs2_set_bitmap(sdp, block, block_status))
					log_err(_("Unlinked block %llu "
						  "(0x%llx) bitmap not fixed."
						  "\n"),
						(unsigned long long)block,
						(unsigned long long)block);
				else
					log_err(_("Unlinked block %llu "
						  "(0x%llx) bitmap fixed.\n"),
						(unsigned long long)block,
						(unsigned long long)block);
			} else {
				log_info( _("Unlinked block found at block %llu"
					    " (0x%llx), left unchanged.\n"),
					(unsigned long long)block,
					(unsigned long long)block);
			}
		} else if (rg_status != block_status) {
			const char *blockstatus[] = {"Free", "Data",
						     "Unlinked", "inode"};

			log_err( _("Block %llu (0x%llx) bitmap says %u (%s) "
				   "but FSCK saw %u (%s)\n"),
				 (unsigned long long)block,
				 (unsigned long long)block, rg_status,
				 blockstatus[rg_status], block_status,
				 blockstatus[block_status]);
			if (q) /* Don't print redundant "free" */
				log_err( _("Metadata type is %u (%s)\n"), q,
					 block_type_string(q));

			if (query(_("Fix bitmap for block %llu (0x%llx) ? (y/n) "),
				 (unsigned long long)block,
				 (unsigned long long)block)) {
				if (gfs2_set_bitmap(sdp, block, block_status))
					log_err( _("Repair failed.\n"));
				else
					log_err( _("Fixed.\n"));
			} else
				log_err( _("Bitmap at block %llu (0x%llx) left inconsistent\n"),
					(unsigned long long)block,
					(unsigned long long)block);
		}
		(*rg_block)++;
		bit += GFS2_BIT_SIZE;
		if (bit >= 8){
			bit = 0;
			byte++;
		}
	}

	return 0;
}

static void update_rgrp(struct gfs2_sbd *sdp, struct rgrp_tree *rgp,
			uint32_t *count)
{
	uint32_t i;
	struct gfs2_bitmap *bits;
	uint64_t rg_block = 0;
	int update = 0;
	struct gfs_rgrp *gfs1rg = (struct gfs_rgrp *)&rgp->rg;

	for(i = 0; i < rgp->ri.ri_length; i++) {
		bits = &rgp->bits[i];

		/* update the bitmaps */
		check_block_status(sdp, rgp->bh[i]->b_data + bits->bi_offset,
				   bits->bi_len, &rg_block, rgp->ri.ri_data0,
				   count);
		if (skip_this_pass || fsck_abort) /* if asked to skip the rest */
			return;
	}

	/* actually adjust counters and write out to disk */
	if (rgp->rg.rg_free != count[0]) {
		log_err( _("RG #%llu (0x%llx) free count inconsistent: "
			"is %u should be %u\n"),
			(unsigned long long)rgp->ri.ri_addr,
			(unsigned long long)rgp->ri.ri_addr,
			rgp->rg.rg_free, count[0]);
		rgp->rg.rg_free = count[0];
		update = 1;
	}
	if (rgp->rg.rg_dinodes != count[1]) {
		log_err( _("RG #%llu (0x%llx) Inode count inconsistent: is "
			   "%u should be %u\n"),
			 (unsigned long long)rgp->ri.ri_addr,
			 (unsigned long long)rgp->ri.ri_addr,
			 rgp->rg.rg_dinodes, count[1]);
		rgp->rg.rg_dinodes = count[1];
		update = 1;
	}
	if (sdp->gfs1 && gfs1rg->rg_usedmeta != count[3]) {
		log_err( _("RG #%llu (0x%llx) Used metadata count "
			   "inconsistent: is %u should be %u\n"),
			 (unsigned long long)rgp->ri.ri_addr,
			 (unsigned long long)rgp->ri.ri_addr,
			 gfs1rg->rg_usedmeta, count[3]);
		gfs1rg->rg_usedmeta = count[3];
		update = 1;
	}
	if (sdp->gfs1 && gfs1rg->rg_freemeta != count[4]) {
		log_err( _("RG #%llu (0x%llx) Free metadata count "
			   "inconsistent: is %u should be %u\n"),
			 (unsigned long long)rgp->ri.ri_addr,
			 (unsigned long long)rgp->ri.ri_addr,
			 gfs1rg->rg_freemeta, count[4]);
		gfs1rg->rg_freemeta = count[4];
		update = 1;
	}
	if (!sdp->gfs1 && (rgp->ri.ri_data - count[0] - count[1]) != count[2]) {
		/* FIXME not sure how to handle this case ATM - it
		 * means that the total number of blocks we've counted
		 * exceeds the blocks in the rg */
		log_err( _("Internal fsck error - AAHHH!\n"));
		exit(FSCK_ERROR);
	}
	if (update) {
		if (query( _("Update resource group counts? (y/n) "))) {
			log_warn( _("Resource group counts updated\n"));
			/* write out the rgrp */
			if (sdp->gfs1)
				gfs_rgrp_out(gfs1rg, rgp->bh[0]);
			else
				gfs2_rgrp_out(&rgp->rg, rgp->bh[0]);
		} else
			log_err( _("Resource group counts left inconsistent\n"));
	}
}

/**
 * pass5 - check resource groups
 *
 * fix free block maps
 * fix used inode maps
 */
int pass5(struct gfs2_sbd *sdp)
{
	struct osi_node *n, *next = NULL;
	struct rgrp_tree *rgp = NULL;
	uint32_t count[5];
	uint64_t rg_count = 0;

	/* Reconcile RG bitmaps with fsck bitmap */
	for (n = osi_first(&sdp->rgtree); n; n = next) {
		next = osi_next(n);
		if (skip_this_pass || fsck_abort) /* if asked to skip the rest */
			return FSCK_OK;
		log_info( _("Verifying Resource Group #%llu\n"), (unsigned long long)rg_count);
		memset(count, 0, sizeof(count));
		rgp = (struct rgrp_tree *)n;

		rg_count++;
		/* Compare the bitmaps and report the differences */
		update_rgrp(sdp, rgp, count);
	}
	/* Fix up superblock info based on this - don't think there's
	 * anything to do here... */

	return FSCK_OK;
}
