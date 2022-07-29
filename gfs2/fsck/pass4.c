#include "clusterautoconfig.h"

#include <inttypes.h>
#include <stdlib.h>
#include <stdio.h>
#include <libintl.h>
#define _(String) gettext(String)

#include <logging.h>
#include "libgfs2.h"
#include "fsck.h"
#include "link.h"
#include "lost_n_found.h"
#include "inode_hash.h"
#include "metawalk.h"
#include "util.h"
#include "afterpass1_common.h"

static struct metawalk_fxns pass4_fxns_delete = {
	.private = NULL,
	.check_metalist = delete_metadata,
	.check_data = delete_data,
	.check_eattr_indir = delete_eattr_indir,
	.check_eattr_leaf = delete_eattr_leaf,
};

/* Updates the link count of an inode to what the fsck has seen for
 * link count */
static int fix_link_count(uint32_t counted_links, struct lgfs2_inode *ip)
{
	log_info(_("Fixing inode link count (%d->%d) for %"PRIu64" (0x%"PRIx64") \n"),
	         ip->i_nlink, counted_links, ip->i_num.in_addr, ip->i_num.in_addr);
	if (ip->i_nlink == counted_links)
		return 0;
	ip->i_nlink = counted_links;
	lgfs2_bmodified(ip->i_bh);

	log_debug(_("Changing inode %"PRIu64" (0x%"PRIx64") to have %u links\n"),
	          ip->i_num.in_addr, ip->i_num.in_addr, counted_links);
	return 0;
}

/**
 * handle_unlinked - handle an unlinked dinode
 *
 * Note: We need to pass in *counted_links here, not counted_links because
 *       add_inode_to_lf may be called here, and that might change the original
 *       value, whether that's in the dirtree or the inodetree.
 *
 * Returns: 1 if caller should do "continue", 0 if not.
 */
static int handle_unlinked(struct fsck_cx *cx, uint64_t no_addr,
			   uint32_t *counted_links, int *lf_addition)
{
	struct lgfs2_sbd *sdp = cx->sdp;
	struct lgfs2_inode *ip;
	int q;

	log_err(_("Found unlinked inode at %"PRIu64" (0x%"PRIx64")\n"),
	        no_addr, no_addr);
	q = bitmap_type(sdp, no_addr);
	if (q == GFS2_BLKST_FREE) {
		log_err(_("Unlinked inode %"PRIu64" (0x%"PRIx64") contains bad blocks\n"),
		        no_addr, no_addr);
		if (query(_("Delete unlinked inode with bad blocks? (y/n) "))) {
			ip = fsck_load_inode(sdp, no_addr);
			check_inode_eattr(cx, ip, &pass4_fxns_delete);
			check_metatree(cx, ip, &pass4_fxns_delete);
			fsck_bitmap_set(cx, ip, no_addr, _("bad unlinked"),
					GFS2_BLKST_FREE);
			fsck_inode_put(&ip);
			return 1;
		} else {
			log_err(_("Unlinked inode with bad blocks not cleared\n"));
		}
	}
	if (q != GFS2_BLKST_DINODE) {
		log_err(_("Unlinked block %"PRIu64" (0x%"PRIx64") marked as inode is not an inode (%d)\n"),
		        no_addr, no_addr, q);
		ip = fsck_load_inode(sdp, no_addr);
		if (query(_("Delete unlinked inode? (y/n) "))) {
			check_inode_eattr(cx, ip, &pass4_fxns_delete);
			check_metatree(cx, ip, &pass4_fxns_delete);
			fsck_bitmap_set(cx, ip, no_addr, _("invalid unlinked"),
					GFS2_BLKST_FREE);
			fsck_inode_put(&ip);
			log_err( _("The inode was deleted\n"));
		} else {
			log_err( _("The inode was not deleted\n"));
			fsck_inode_put(&ip);
		}
		return 1;
	}
	ip = fsck_load_inode(sdp, no_addr);

	/* We don't want to clear zero-size files with eattrs - there might be
	   relevent info in them. */
	if (!ip->i_size && !ip->i_eattr){
		log_err( _("Unlinked inode has zero size\n"));
		if (query(_("Clear zero-size unlinked inode? (y/n) "))) {
			fsck_bitmap_set(cx, ip, no_addr, _("unlinked zero-length"),
					GFS2_BLKST_FREE);
			fsck_inode_put(&ip);
			return 1;
		}
	}
	if (query( _("Add unlinked inode to lost+found? (y/n)"))) {
		if (add_inode_to_lf(cx, ip)) {
			stack;
			fsck_inode_put(&ip);
			return -1;
		} else {
			fix_link_count(*counted_links, ip);
			*lf_addition = 1;
		}
	} else
		log_err( _("Unlinked inode left unlinked\n"));
	fsck_inode_put(&ip);
	return 0;
}

static void handle_inconsist(struct lgfs2_sbd *sdp, uint64_t no_addr,
			     uint32_t *di_nlink, uint32_t counted_links)
{
	log_err(_("Link count inconsistent for inode %"PRIu64" (0x%"PRIx64") has %u but fsck found %u.\n"),
	        no_addr, no_addr, *di_nlink, counted_links);
	/* Read in the inode, adjust the link count, and write it back out */
	if (query(_("Update link count for inode %"PRIu64" (0x%"PRIx64")? (y/n) "),
	          no_addr, no_addr)) {
		struct lgfs2_inode *ip;

		ip = fsck_load_inode(sdp, no_addr); /* lgfs2_bread, inode_get */
		fix_link_count(counted_links, ip);
		*di_nlink = counted_links;
		fsck_inode_put(&ip); /* out, lgfs2_brelse, free */
		log_warn(_("Link count updated to %d for inode %"PRIu64" (0x%"PRIx64")\n"),
		         *di_nlink, no_addr, no_addr);
	} else {
		log_err(_("Link count for inode %"PRIu64" (0x%"PRIx64") still incorrect\n"),
		        no_addr, no_addr);
	}
}

static int adjust_lf_links(struct fsck_cx *cx, int lf_addition)
{
	struct dir_info *lf_di;

	if (lf_dip == NULL)
		return 0;

	if (!lf_addition)
		return 0;

	if (!(lf_di = dirtree_find(cx, lf_dip->i_num.in_addr))) {
		log_crit(_("Unable to find lost+found inode in "
			   "inode_hash!!\n"));
		return -1;
	} else {
		fix_link_count(lf_di->counted_links, lf_dip);
	}
	return 0;
}

static int scan_inode_list(struct fsck_cx *cx)
{
	struct lgfs2_sbd *sdp = cx->sdp;
	struct osi_node *tmp, *next = NULL;
	struct inode_info *ii;
	int lf_addition = 0;

	/* FIXME: should probably factor this out into a generic
	 * scanning fxn */
	for (tmp = osi_first(&inodetree); tmp; tmp = next) {
		if (skip_this_pass || fsck_abort) /* if asked to skip the rest */
			return 0;
		next = osi_next(tmp);
		ii = (struct inode_info *)tmp;
		/* Don't check reference counts on the special gfs files */
		if (sdp->gfs1 &&
		    ((ii->num.in_addr == sdp->md.riinode->i_num.in_addr) ||
		     (ii->num.in_addr == sdp->md.qinode->i_num.in_addr) ||
		     (ii->num.in_addr == sdp->md.statfs->i_num.in_addr)))
			continue;
		if (ii->counted_links == 0) {
			if (handle_unlinked(cx, ii->num.in_addr,
					    &ii->counted_links, &lf_addition))
				continue;
		} /* if (ii->counted_links == 0) */
		else if (ii->di_nlink != ii->counted_links) {
			handle_inconsist(sdp, ii->num.in_addr,
					 &ii->di_nlink, ii->counted_links);
		}
		log_debug(_("block %"PRIu64" (0x%"PRIx64") has link count %d\n"),
		          ii->num.in_addr, ii->num.in_addr, ii->di_nlink);
	} /* osi_list_foreach(tmp, list) */

	return adjust_lf_links(cx, lf_addition);
}

static int scan_dir_list(struct fsck_cx *cx)
{
	struct lgfs2_sbd *sdp = cx->sdp;
	struct osi_node *tmp, *next = NULL;
	struct dir_info *di;
	int lf_addition = 0;

	/* FIXME: should probably factor this out into a generic
	 * scanning fxn */
	for (tmp = osi_first(&cx->dirtree); tmp; tmp = next) {
		if (skip_this_pass || fsck_abort) /* if asked to skip the rest */
			return 0;
		next = osi_next(tmp);
		di = (struct dir_info *)tmp;
		/* Don't check reference counts on the special gfs files */
		if (sdp->gfs1 &&
		    di->dinode.in_addr == sdp->md.jiinode->i_num.in_addr)
			continue;
		if (di->counted_links == 0) {
			if (handle_unlinked(cx, di->dinode.in_addr,
					    &di->counted_links, &lf_addition))
				continue;
		} else if (di->di_nlink != di->counted_links) {
			handle_inconsist(sdp, di->dinode.in_addr,
					 &di->di_nlink, di->counted_links);
		}
		log_debug(_("block %"PRIu64" (0x%"PRIx64") has link count %d\n"),
		          di->dinode.in_addr, di->dinode.in_addr, di->di_nlink);
	} /* osi_list_foreach(tmp, list) */

	return adjust_lf_links(cx, lf_addition);
}

static int scan_nlink1_list(struct fsck_cx *cx)
{
	uint64_t blk;
	uint32_t counted_links;
	int lf_addition = 0;

	for (blk = 0; blk < last_fs_block; blk++) {
		if (skip_this_pass || fsck_abort)
			return 0;
		if (link1_type(&nlink1map, blk) == 0)
			continue;

		if (link1_type(&clink1map, blk) == 0) {
			/* In other cases, counted_links is a pointer to a
			   real count that gets incremented when it's added
			   to lost+found. In this case, however, there's not a
			   real count, so we fake it out to be 1. */
			counted_links = 1;
			if (handle_unlinked(cx, blk, &counted_links, &lf_addition))
				continue;
		}
	}
	return adjust_lf_links(cx, lf_addition);
}

/**
 * pass4 - Check reference counts (pass 2 & 6 in current fsck)
 *
 * handle unreferenced files
 * lost+found errors (missing, not a directory, no space)
 * adjust link count
 * handle unreferenced inodes of other types
 * handle bad blocks
 */
int pass4(struct fsck_cx *cx)
{
	if (lf_dip)
		log_debug( _("At beginning of pass4, lost+found entries is %u\n"),
				  lf_dip->i_entries);
	log_info( _("Checking inode reference counts: multi-links.\n"));
	if (scan_inode_list(cx)) {
		stack;
		return FSCK_ERROR;
	}
	log_info( _("Checking inode reference counts: directories.\n"));
	if (scan_dir_list(cx)) {
		stack;
		return FSCK_ERROR;
	}
	log_info( _("Checking inode reference counts: normal links.\n"));
	if (scan_nlink1_list(cx)) {
		stack;
		return FSCK_ERROR;
	}

	if (lf_dip)
		log_debug( _("At end of pass4, lost+found entries is %u\n"),
				  lf_dip->i_entries);
	return FSCK_OK;
}
