#include "clusterautoconfig.h"

#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include <stdlib.h>
#include <libintl.h>
#define _(String) gettext(String)

#include <logging.h>
#include "libgfs2.h"
#include "fsck.h"
#include "util.h"

#define GFS1_BLKST_USEDMETA 4

static int check_block_status(struct lgfs2_sbd *sdp,  struct bmap *bl,
			      char *buffer, unsigned int buflen,
			      uint64_t *rg_block, uint64_t rg_data,
			      uint32_t *count)
{
	unsigned char *byte, *end;
	unsigned int bit;
	unsigned char rg_status;
	int q;
	uint64_t block;

	/* FIXME verify cast */
	byte = (unsigned char *) buffer;
	bit = 0;
	end = (unsigned char *) buffer + buflen;

	while (byte < end) {
		rg_status = ((*byte >> bit) & GFS2_BIT_MASK);
		block = rg_data + *rg_block;
		display_progress(block);
		if (skip_this_pass || fsck_abort) /* if asked to skip the rest */
			return 0;

		q = block_type(bl, block);
		/* GFS1 file systems will have to suffer from slower fsck run
		 * times because in GFS, there's no 1:1 relationship between
		 * bits and counts. If a bit is marked "dinode" in GFS1, it
		 * may be dinode -OR- any kind of metadata. I consider GFS1 to
		 * be a rare exception, so acceptable loss at this point. So
		 * we must determine whether it's really a dinode or other
		 * metadata by reading it in. */
		if (sdp->gfs1 && q == GFS2_BLKST_DINODE) {
			struct lgfs2_buffer_head *bh;

			bh = lgfs2_bread(sdp, block);
			if (lgfs2_check_meta(bh->b_data, GFS2_METATYPE_DI) == 0)
				count[GFS2_BLKST_DINODE]++;
			else
				count[GFS1_BLKST_USEDMETA]++;
			lgfs2_brelse(bh);
		} else {
			count[q]++;
		}

		/* If one node opens a file and another node deletes it, we
		   may be left with a block that appears to be "unlinked" in
		   the bitmap, but nothing links to it. This is a valid case
		   and should be cleaned up by the file system eventually.
		   So we ignore it. */
		if (q == GFS2_BLKST_UNLINKED) {
			log_err(_("Unlinked inode found at block %"PRIu64" (0x%"PRIx64").\n"),
			        block, block);
			if (query(_("Do you want to reclaim the block? "
				   "(y/n) "))) {
				lgfs2_rgrp_t rg = lgfs2_blk2rgrpd(sdp, block);
				if (lgfs2_set_bitmap(rg, block, GFS2_BLKST_FREE))
					log_err(_("Unlinked block %"PRIu64" (0x%"PRIx64") bitmap not fixed.\n"),
					        block, block);
				else {
					log_err(_("Unlinked block %"PRIu64" (0x%"PRIx64") bitmap fixed.\n"),
					        block, block);
					count[GFS2_BLKST_UNLINKED]--;
					count[GFS2_BLKST_FREE]++;
				}
			} else {
				log_info(_("Unlinked block found at block %"PRIu64" (0x%"PRIx64"), left unchanged.\n"),
				         block, block);
			}
		} else if (rg_status != q) {
			log_err(_("Block %"PRIu64" (0x%"PRIx64") bitmap says %u (%s) but FSCK saw %u (%s)\n"),
				 block, block, rg_status,
				 block_type_string(rg_status), q,
				 block_type_string(q));
			if (q) /* Don't print redundant "free" */
				log_err( _("Metadata type is %u (%s)\n"), q,
					 block_type_string(q));

			if (query(_("Fix bitmap for block %"PRIu64" (0x%"PRIx64")? (y/n) "),
			          block, block)) {
				lgfs2_rgrp_t rg = lgfs2_blk2rgrpd(sdp, block);
				if (lgfs2_set_bitmap(rg, block, q))
					log_err( _("Repair failed.\n"));
				else
					log_err( _("Fixed.\n"));
			} else
				log_err(_("Bitmap at block %"PRIu64" (0x%"PRIx64") left inconsistent\n"),
				        block, block);
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

static void update_rgrp(struct fsck_cx *cx, struct lgfs2_rgrp_tree *rgp,
			struct bmap *bl, uint32_t *count)
{
	struct lgfs2_sbd *sdp = cx->sdp;
	uint32_t i;
	struct lgfs2_bitmap *bits;
	uint64_t rg_block = 0;
	int update = 0;

	for(i = 0; i < rgp->rt_length; i++) {
		bits = &rgp->bits[i];

		/* update the bitmaps */
		if (check_block_status(sdp, bl, bits->bi_data + bits->bi_offset,
		                       bits->bi_len, &rg_block, rgp->rt_data0, count))
			return;
		if (skip_this_pass || fsck_abort) /* if asked to skip the rest */
			return;
	}

	/* actually adjust counters and write out to disk */
	if (rgp->rt_free != count[GFS2_BLKST_FREE]) {
		log_err(_("RG #%"PRIu64" (0x%"PRIx64") free count inconsistent: "
			"is %u should be %u\n"),
		        rgp->rt_addr, rgp->rt_addr, rgp->rt_free, count[GFS2_BLKST_FREE]);
		rgp->rt_free = count[GFS2_BLKST_FREE];
		update = 1;
	}
	if (rgp->rt_dinodes != count[GFS2_BLKST_DINODE]) {
		log_err(_("RG #%"PRIu64" (0x%"PRIx64") Inode count inconsistent: is "
		          "%u should be %u\n"),
		        rgp->rt_addr, rgp->rt_addr, rgp->rt_dinodes, count[GFS2_BLKST_DINODE]);
		rgp->rt_dinodes = count[GFS2_BLKST_DINODE];
		update = 1;
	}
	if (sdp->gfs1 && rgp->rt_usedmeta != count[GFS1_BLKST_USEDMETA]) {
		log_err(_("RG #%"PRIu64" (0x%"PRIx64") Used metadata count "
		          "inconsistent: is %u should be %u\n"),
		        rgp->rt_addr, rgp->rt_addr, rgp->rt_usedmeta, count[GFS1_BLKST_USEDMETA]);
		rgp->rt_usedmeta = count[GFS1_BLKST_USEDMETA];
		update = 1;
	}
	if (sdp->gfs1 && rgp->rt_freemeta != count[GFS2_BLKST_UNLINKED]) {
		log_err(_("RG #%"PRIu64" (0x%"PRIx64") Free metadata count "
		          "inconsistent: is %u should be %u\n"),
		        rgp->rt_addr, rgp->rt_addr, rgp->rt_freemeta, count[GFS2_BLKST_UNLINKED]);
		rgp->rt_freemeta = count[GFS2_BLKST_UNLINKED];
		update = 1;
	}
	if (!sdp->gfs1 && (rgp->rt_data != count[GFS2_BLKST_FREE] +
			   count[GFS2_BLKST_USED] +
			   count[GFS2_BLKST_UNLINKED] +
			   count[GFS2_BLKST_DINODE])) {
		/* FIXME not sure how to handle this case ATM - it
		 * means that the total number of blocks we've counted
		 * exceeds the blocks in the rg */
		log_err( _("Internal fsck error: %u != %u + %u + %u + %u\n"),
			 rgp->rt_data, count[GFS2_BLKST_FREE],
			 count[GFS2_BLKST_USED], count[GFS2_BLKST_UNLINKED],
			 count[GFS2_BLKST_DINODE]);
		exit(FSCK_ERROR);
	}
	if (update) {
		if (query( _("Update resource group counts? (y/n) "))) {
			log_warn( _("Resource group counts updated\n"));
			/* write out the rgrp */
			if (sdp->gfs1)
				lgfs2_gfs_rgrp_out(rgp, rgp->bits[0].bi_data);
			else
				lgfs2_rgrp_out(rgp, rgp->bits[0].bi_data);
			rgp->bits[0].bi_modified = 1;
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
int pass5(struct fsck_cx *cx, struct bmap *bl)
{
	struct lgfs2_sbd *sdp = cx->sdp;
	struct osi_node *n, *next = NULL;
	struct lgfs2_rgrp_tree *rgp = NULL;
	uint32_t count[5]; /* we need 5 because of GFS1 usedmeta */
	uint64_t rg_count = 0;

	/* Reconcile RG bitmaps with fsck bitmap */
	for (n = osi_first(&sdp->rgtree); n; n = next) {
		next = osi_next(n);
		if (skip_this_pass || fsck_abort) /* if asked to skip the rest */
			return FSCK_OK;
		log_info(_("Verifying resource group %"PRIu64"\n"), rg_count);
		memset(count, 0, sizeof(count));
		rgp = (struct lgfs2_rgrp_tree *)n;

		rg_count++;
		/* Compare the bitmaps and report the differences */
		update_rgrp(cx, rgp, bl, count);
	}
	/* Fix up superblock info based on this - don't think there's
	 * anything to do here... */

	return FSCK_OK;
}
