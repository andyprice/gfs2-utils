#include <inttypes.h>
#include <stdlib.h>
#include <stdio.h>
#include <libintl.h>
#define _(String) gettext(String)

#include "libgfs2.h"
#include "fsck.h"
#include "lost_n_found.h"
#include "inode_hash.h"

/* Updates the link count of an inode to what the fsck has seen for
 * link count */
static int fix_inode_count(struct gfs2_sbd *sbp, struct inode_info *ii,
					struct gfs2_inode *ip)
{
	log_info( _("Fixing inode count for %llu (0x%llx) \n"),
		 (unsigned long long)ip->i_di.di_num.no_addr,
		 (unsigned long long)ip->i_di.di_num.no_addr);
	if(ip->i_di.di_nlink == ii->counted_links)
		return 0;
	ip->i_di.di_nlink = ii->counted_links;

	log_debug( _("Changing inode %llu (0x%llx) to have %u links\n"),
		  (unsigned long long)ip->i_di.di_num.no_addr,
		  (unsigned long long)ip->i_di.di_num.no_addr,
		  ii->counted_links);
	return 0;
}

static int scan_inode_list(struct gfs2_sbd *sbp, osi_list_t *list) {
	osi_list_t *tmp;
	struct inode_info *ii;
	struct gfs2_inode *ip;
	int lf_addition = 0;
	struct gfs2_block_query q;
	enum update_flags f;

	/* FIXME: should probably factor this out into a generic
	 * scanning fxn */
	osi_list_foreach(tmp, list) {
		if (skip_this_pass || fsck_abort) /* if asked to skip the rest */
			return 0;
		f = not_updated;
		if(!(ii = osi_list_entry(tmp, struct inode_info, list))) {
			log_crit( _("osi_list_foreach broken in scan_info_list!!\n"));
			exit(FSCK_ERROR);
		}
		log_debug( _("Checking reference count on inode at block %" PRIu64
				  " (0x%" PRIx64 ")\n"), ii->inode, ii->inode);
		if(ii->counted_links == 0) {
			log_err( _("Found unlinked inode at %" PRIu64 " (0x%" PRIx64 ")\n"),
					ii->inode, ii->inode);
			if(gfs2_block_check(sbp, bl, ii->inode, &q)) {
				stack;
				return -1;
			}
			if(q.bad_block) {
				log_err( _("Unlinked inode %llu (0x%llx) contains"
					"bad blocks\n"),
					(unsigned long long)ii->inode,
					(unsigned long long)ii->inode);
				errors_found++;
				if(query(&opts,
						 _("Clear unlinked inode with bad blocks? (y/n) "))) {
					errors_corrected++;
					gfs2_block_set(sbp, bl, ii->inode,
						       gfs2_block_free);
					continue;
				} else
					log_err( _("Unlinked inode with bad blocks not cleared\n"));
			}
			if(q.block_type != gfs2_inode_dir &&
			   q.block_type != gfs2_inode_file &&
			   q.block_type != gfs2_inode_lnk &&
			   q.block_type != gfs2_inode_blk &&
			   q.block_type != gfs2_inode_chr &&
			   q.block_type != gfs2_inode_fifo &&
			   q.block_type != gfs2_inode_sock) {
				log_err( _("Unlinked block marked as inode not an inode\n"));
				gfs2_block_set(sbp, bl, ii->inode,
					       gfs2_block_free);
				log_err( _("Cleared\n"));
				continue;
			}
			ip = fsck_load_inode(sbp, ii->inode);

			/* We don't want to clear zero-size files with
			 * eattrs - there might be relevent info in
			 * them. */
			if(!ip->i_di.di_size && !ip->i_di.di_eattr){
				log_err( _("Unlinked inode has zero size\n"));
				errors_found++;
				if(query(&opts, _("Clear zero-size unlinked inode? (y/n) "))) {
					errors_corrected++;
					gfs2_block_set(sbp, bl, ii->inode,
						       gfs2_block_free);
					fsck_inode_put(ip, not_updated);
					continue;
				}

			}
			errors_found++;
			if(query(&opts, _("Add unlinked inode to lost+found? (y/n)"))) {
				errors_corrected++;
				f = updated;
				if(add_inode_to_lf(ip)) {
					stack;
					fsck_inode_put(ip, not_updated);
					return -1;
				}
				else {
					fix_inode_count(sbp, ii, ip);
					lf_addition = 1;
				}
			} else
				log_err( _("Unlinked inode left unlinked\n"));
			fsck_inode_put(ip, f);
		} /* if(ii->counted_links == 0) */
		else if(ii->link_count != ii->counted_links) {
			log_err( _("Link count inconsistent for inode %" PRIu64
					" (0x%" PRIx64 ") has %u but fsck found %u.\n"), ii->inode, 
					ii->inode, ii->link_count, ii->counted_links);
			/* Read in the inode, adjust the link count,
			 * and write it back out */
			errors_found++;
			if(query(&opts, _("Update link count for inode %"
				 PRIu64 " (0x%" PRIx64 ") ? (y/n) "), ii->inode, ii->inode)) {
				errors_corrected++;
				ip = fsck_load_inode(sbp, ii->inode); /* bread, inode_get */
				fix_inode_count(sbp, ii, ip);
				fsck_inode_put(ip, updated); /* out, brelse, free */
				log_warn( _("Link count updated for inode %"
						 PRIu64 " (0x%" PRIx64 ") \n"), ii->inode, ii->inode);
			} else {
				log_err( _("Link count for inode %" PRIu64 " (0x%" PRIx64
						") still incorrect\n"), ii->inode, ii->inode);
			}
		}
		log_debug( _("block %" PRIu64 " (0x%" PRIx64 ") has link count %d\n"),
				  ii->inode, ii->inode, ii->link_count);
	} /* osi_list_foreach(tmp, list) */

	if (lf_addition) {
		if(!(ii = inode_hash_search(inode_hash,
									lf_dip->i_di.di_num.no_addr))) {
			log_crit( _("Unable to find lost+found inode in inode_hash!!\n"));
			return -1;
		} else {
			fix_inode_count(sbp, ii, lf_dip);
		}
	}

	return 0;
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
int pass4(struct gfs2_sbd *sbp)
{
	uint32_t i;
	osi_list_t *list;
	if(lf_dip)
		log_debug( _("At beginning of pass4, lost+found entries is %u\n"),
				  lf_dip->i_di.di_entries);
	log_info( _("Checking inode reference counts.\n"));
	for (i = 0; i < FSCK_HASH_SIZE; i++) {
		if (skip_this_pass || fsck_abort) /* if asked to skip the rest */
			return FSCK_OK;
		list = &inode_hash[i];
		if(scan_inode_list(sbp, list)) {
			stack;
			return FSCK_ERROR;
		}
	}

	if(lf_dip)
		log_debug( _("At end of pass4, lost+found entries is %u\n"),
				  lf_dip->i_di.di_entries);
	return FSCK_OK;
}
