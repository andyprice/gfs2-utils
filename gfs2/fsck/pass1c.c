#include "clusterautoconfig.h"

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <libintl.h>
#define _(String) gettext(String)

#include <logging.h>
#include "libgfs2.h"
#include "fsck.h"
#include "util.h"
#include "metawalk.h"

struct metawalk_fxns pass1c_fxns_delete = {
	.private = NULL,
	.check_eattr_indir = delete_eattr_indir,
	.check_eattr_leaf = delete_eattr_leaf,
};

static int remove_eattr_entry(struct gfs2_sbd *sdp,
			      struct gfs2_buffer_head *leaf_bh,
			      struct gfs2_ea_header *curr,
			      struct gfs2_ea_header *prev)
{
	if (!prev)
		curr->ea_type = GFS2_EATYPE_UNUSED;
	else {
		uint32_t tmp32 = be32_to_cpu(curr->ea_rec_len) +
			be32_to_cpu(prev->ea_rec_len);
		prev->ea_rec_len = cpu_to_be32(tmp32);
		if (curr->ea_flags & GFS2_EAFLAG_LAST)
			prev->ea_flags |= GFS2_EAFLAG_LAST;	
	}
	log_err( _("Bad Extended Attribute at block #%llu"
		   " (0x%llx) removed.\n"),
		 (unsigned long long)leaf_bh->b_blocknr,
		 (unsigned long long)leaf_bh->b_blocknr);
	bmodified(leaf_bh);
	return 0;
}

static int ask_remove_eattr_entry(struct gfs2_sbd *sdp,
				  struct gfs2_buffer_head *leaf_bh,
				  struct gfs2_ea_header *curr,
				  struct gfs2_ea_header *prev,
				  int fix_curr, int fix_curr_len)
{
	if (query( _("Remove the bad Extended Attribute entry? (y/n) "))) {
		if (fix_curr)
			curr->ea_flags |= GFS2_EAFLAG_LAST;
		if (fix_curr_len) {
			uint32_t max_size = sdp->sd_sb.sb_bsize;
			uint32_t offset = (uint32_t)(((unsigned long)curr) -
					     ((unsigned long)leaf_bh->b_data));
			curr->ea_rec_len = cpu_to_be32(max_size - offset);
		}
		if (remove_eattr_entry(sdp, leaf_bh, curr, prev)) {
			stack;
			return -1;
		}
	} else {
		log_err( _("Bad Extended Attribute not removed.\n"));
	}
	return 1;
}

static int ask_remove_eattr(struct gfs2_inode *ip)
{
	if (query( _("Remove the bad Extended Attribute? (y/n) "))) {
		check_inode_eattr(ip, &pass1c_fxns_delete);
		bmodified(ip->i_bh);
		log_err( _("Bad Extended Attribute removed.\n"));
		return 1;
	}
	log_err( _("Bad Extended Attribute not removed.\n"));
	return 0;
}

static int check_eattr_indir(struct gfs2_inode *ip, uint64_t block,
		      uint64_t parent, struct gfs2_buffer_head **bh,
		      void *private)
{
	struct gfs2_sbd *sdp = ip->i_sbd;
	uint8_t q;
	struct gfs2_buffer_head *indir_bh = NULL;

	if (!valid_block(sdp, block)) {
		log_err( _("Extended attributes indirect block #%llu"
			" (0x%llx) for inode #%llu"
			" (0x%llx) is invalid...removing\n"),
			(unsigned long long)block,
			(unsigned long long)block,
			(unsigned long long)ip->i_di.di_num.no_addr,
			(unsigned long long)ip->i_di.di_num.no_addr);
		return ask_remove_eattr(ip);
	}
	q = block_type(block);
	if (q != gfs2_indir_blk) {
		log_err( _("Extended attributes indirect block #%llu"
			" (0x%llx) for inode #%llu"
			" (0x%llx) is invalid.\n"),
			(unsigned long long)block,
			(unsigned long long)block,
			(unsigned long long)ip->i_di.di_num.no_addr,
			(unsigned long long)ip->i_di.di_num.no_addr);
		return ask_remove_eattr(ip);
	}
	else
		indir_bh = bread(sdp, block);

	*bh = indir_bh;
	return 0;
}

static int check_eattr_leaf(struct gfs2_inode *ip, uint64_t block,
		     uint64_t parent, struct gfs2_buffer_head **bh,
		     void *private)
{
	struct gfs2_sbd *sdp = ip->i_sbd;
	uint8_t q;

	if (!valid_block(sdp, block)) {
		log_err( _("Extended attributes block for inode #%llu"
			" (0x%llx) is invalid.\n"),
			(unsigned long long)ip->i_di.di_num.no_addr,
			(unsigned long long)ip->i_di.di_num.no_addr);
		return ask_remove_eattr(ip);
	}
	q = block_type(block);
	if (q != gfs2_meta_eattr) {
		log_err( _("Extended attributes block for inode #%llu"
			   " (0x%llx) invalid.\n"),
			 (unsigned long long)ip->i_di.di_num.no_addr,
			 (unsigned long long)ip->i_di.di_num.no_addr);
		return ask_remove_eattr(ip);
	}
	else 
		*bh = bread(sdp, block);

	return 0;
}

static int check_eattr_entry(struct gfs2_inode *ip,
			     struct gfs2_buffer_head *leaf_bh,
			     struct gfs2_ea_header *ea_hdr,
			     struct gfs2_ea_header *ea_hdr_prev,
			     void *private)
{
	struct gfs2_sbd *sdp = ip->i_sbd;
	char ea_name[256];
	uint32_t offset = (uint32_t)(((unsigned long)ea_hdr) -
			                  ((unsigned long)leaf_bh->b_data));
	uint32_t max_size = sdp->sd_sb.sb_bsize;

	if (!ea_hdr->ea_name_len){
		log_err( _("EA has name length of zero\n"));
		return ask_remove_eattr_entry(sdp, leaf_bh, ea_hdr,
					      ea_hdr_prev, 1, 1);
	}
	if (offset + be32_to_cpu(ea_hdr->ea_rec_len) > max_size){
		log_err( _("EA rec length too long\n"));
		return ask_remove_eattr_entry(sdp, leaf_bh, ea_hdr,
					      ea_hdr_prev, 1, 1);
	}
	if (offset + be32_to_cpu(ea_hdr->ea_rec_len) == max_size &&
	   (ea_hdr->ea_flags & GFS2_EAFLAG_LAST) == 0){
		log_err( _("last EA has no last entry flag\n"));
		return ask_remove_eattr_entry(sdp, leaf_bh, ea_hdr,
					      ea_hdr_prev, 0, 0);
	}
	if (!ea_hdr->ea_name_len){
		log_err( _("EA has name length of zero\n"));
		return ask_remove_eattr_entry(sdp, leaf_bh, ea_hdr,
					      ea_hdr_prev, 0, 0);
	}

	memset(ea_name, 0, sizeof(ea_name));
	strncpy(ea_name, (char *)ea_hdr + sizeof(struct gfs2_ea_header),
		ea_hdr->ea_name_len);

	if (!GFS2_EATYPE_VALID(ea_hdr->ea_type) &&
	   ((ea_hdr_prev) || (!ea_hdr_prev && ea_hdr->ea_type))){
		log_err( _("EA (%s) type is invalid (%d > %d).\n"),
			ea_name, ea_hdr->ea_type, GFS2_EATYPE_LAST);
		return ask_remove_eattr_entry(sdp, leaf_bh, ea_hdr,
					      ea_hdr_prev, 0, 0);
	}

	if (ea_hdr->ea_num_ptrs){
		uint32_t avail_size;
		int max_ptrs;

		avail_size = sdp->sd_sb.sb_bsize - sizeof(struct gfs2_meta_header);
		max_ptrs = (be32_to_cpu(ea_hdr->ea_data_len)+avail_size-1)/avail_size;

		if (max_ptrs > ea_hdr->ea_num_ptrs){
			log_err( _("EA (%s) has incorrect number of pointers.\n"), ea_name);
			log_err( _("  Required:  %d\n  Reported:  %d\n"),
				 max_ptrs, ea_hdr->ea_num_ptrs);
			return ask_remove_eattr_entry(sdp, leaf_bh, ea_hdr,
						      ea_hdr_prev, 0, 0);
		} else {
			log_debug( _(" Pointers Required: %d\n  Pointers Reported: %d\n"),
					  max_ptrs, ea_hdr->ea_num_ptrs);
		}
	}
	return 0;
}

static int check_eattr_extentry(struct gfs2_inode *ip, uint64_t *ea_ptr,
			 struct gfs2_buffer_head *leaf_bh,
			 struct gfs2_ea_header *ea_hdr,
			 struct gfs2_ea_header *ea_hdr_prev, void *private)
{
	uint8_t q;
	struct gfs2_sbd *sdp = ip->i_sbd;

	q = block_type(be64_to_cpu(*ea_ptr));
	if (q != gfs2_meta_eattr) {
		if (remove_eattr_entry(sdp, leaf_bh, ea_hdr, ea_hdr_prev)){
			stack;
			return -1;
		}
		return 1;
	}
	return 0;
}

/* Go over all inodes with extended attributes and verify the EAs are
 * valid */
int pass1c(struct gfs2_sbd *sdp)
{
	uint64_t block_no = 0;
	struct gfs2_buffer_head *bh;
	struct gfs2_inode *ip = NULL;
	struct metawalk_fxns pass1c_fxns = { 0 };
	int error = 0;
	osi_list_t *tmp, *x;
	struct special_blocks *ea_block;

	pass1c_fxns.check_eattr_indir = &check_eattr_indir;
	pass1c_fxns.check_eattr_leaf = &check_eattr_leaf;
	pass1c_fxns.check_eattr_entry = &check_eattr_entry;
	pass1c_fxns.check_eattr_extentry = &check_eattr_extentry;
	pass1c_fxns.private = NULL;

	log_info( _("Looking for inodes containing ea blocks...\n"));
	osi_list_foreach_safe(tmp, &sdp->eattr_blocks.list, x) {
		ea_block = osi_list_entry(tmp, struct special_blocks, list);
		block_no = ea_block->block;
		warm_fuzzy_stuff(block_no);

		if (skip_this_pass || fsck_abort) /* if asked to skip the rest */
			return FSCK_OK;
		bh = bread(sdp, block_no);
		if (!gfs2_check_meta(bh, GFS2_METATYPE_DI)) { /* if a dinode */
			log_info( _("EA in inode %llu (0x%llx)\n"),
				 (unsigned long long)block_no,
				 (unsigned long long)block_no);
			gfs2_special_clear(&sdp->eattr_blocks, block_no);
			ip = fsck_inode_get(sdp, bh);
			ip->bh_owned = 1;

			log_debug( _("Found eattr at %llu (0x%llx)\n"),
				  (unsigned long long)ip->i_di.di_eattr,
				  (unsigned long long)ip->i_di.di_eattr);
			/* FIXME: Handle walking the eattr here */
			error = check_inode_eattr(ip, &pass1c_fxns);
			if (error < 0) {
				stack;
				brelse(bh);
				return FSCK_ERROR;
			}

			fsck_inode_put(&ip); /* dinode_out, brelse, free */
		} else {
			brelse(bh);
		}
	}
	return FSCK_OK;
}
