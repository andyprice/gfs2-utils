#include "clusterautoconfig.h"

/* pass1 checks inodes for format & type, duplicate blocks, & incorrect
 * block count.
 *
 * It builds up tables that contains the state of each block (free,
 * block in use, metadata type, etc), as well as bad blocks and
 * duplicate blocks.  (See block_list.[ch] for more info)
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <unistd.h>
#include <string.h>
#include <time.h>
#include <sys/ioctl.h>
#include <inttypes.h>
#include <libintl.h>
#define _(String) gettext(String)

#include "libgfs2.h"
#include "fsck.h"
#include "inode_hash.h"
#include "util.h"
#include "link.h"
#include "metawalk.h"

struct block_count {
	uint64_t indir_count;
	uint64_t data_count;
	uint64_t ea_count;
};

static int leaf(struct gfs2_inode *ip, uint64_t block,
		struct gfs2_buffer_head *bh, void *private);
static int check_metalist(struct gfs2_inode *ip, uint64_t block,
			  struct gfs2_buffer_head **bh, void *private);
static int undo_check_metalist(struct gfs2_inode *ip, uint64_t block,
			       struct gfs2_buffer_head **bh, void *private);
static int check_data(struct gfs2_inode *ip, uint64_t block, void *private);
static int undo_check_data(struct gfs2_inode *ip, uint64_t block,
			   void *private);
static int check_eattr_indir(struct gfs2_inode *ip, uint64_t indirect,
			     uint64_t parent, struct gfs2_buffer_head **bh,
			     void *private);
static int check_eattr_leaf(struct gfs2_inode *ip, uint64_t block,
			    uint64_t parent, struct gfs2_buffer_head **bh,
			    void *private);
static int check_eattr_entries(struct gfs2_inode *ip,
			       struct gfs2_buffer_head *leaf_bh,
			       struct gfs2_ea_header *ea_hdr,
			       struct gfs2_ea_header *ea_hdr_prev,
			       void *private);
static int check_extended_leaf_eattr(struct gfs2_inode *ip, uint64_t *data_ptr,
				     struct gfs2_buffer_head *leaf_bh,
				     struct gfs2_ea_header *ea_hdr,
				     struct gfs2_ea_header *ea_hdr_prev,
				     void *private);
static int finish_eattr_indir(struct gfs2_inode *ip, int leaf_pointers,
			      int leaf_pointer_errors, void *private);

struct metawalk_fxns pass1_fxns = {
	.private = NULL,
	.check_leaf = leaf,
	.check_metalist = check_metalist,
	.check_data = check_data,
	.check_eattr_indir = check_eattr_indir,
	.check_eattr_leaf = check_eattr_leaf,
	.check_dentry = NULL,
	.check_eattr_entry = check_eattr_entries,
	.check_eattr_extentry = check_extended_leaf_eattr,
	.finish_eattr_indir = finish_eattr_indir,
};

struct metawalk_fxns undo_fxns = {
	.private = NULL,
	.check_metalist = undo_check_metalist,
	.check_data = undo_check_data,
};

static int leaf(struct gfs2_inode *ip, uint64_t block,
		struct gfs2_buffer_head *bh, void *private)
{
	struct block_count *bc = (struct block_count *) private;

	fsck_blockmap_set(ip, block, _("directory leaf"), gfs2_leaf_blk);
	bc->indir_count++;
	return 0;
}

static int check_metalist(struct gfs2_inode *ip, uint64_t block,
			  struct gfs2_buffer_head **bh, void *private)
{
	uint8_t q;
	int found_dup = 0, iblk_type;
	struct gfs2_buffer_head *nbh;
	struct block_count *bc = (struct block_count *)private;
	const char *blktypedesc;

	*bh = NULL;

	if (gfs2_check_range(ip->i_sbd, block)){ /* blk outside of FS */
		fsck_blockmap_set(ip, ip->i_di.di_num.no_addr,
				  _("itself"), gfs2_bad_block);
		log_debug( _("Bad indirect block pointer (out of range) "
			     "found in inode %lld (0x%llx).\n"),
			   (unsigned long long)ip->i_di.di_num.no_addr,
			   (unsigned long long)ip->i_di.di_num.no_addr);

		return 1;
	}
	if (S_ISDIR(ip->i_di.di_mode)) {
		iblk_type = GFS2_METATYPE_JD;
		blktypedesc = _("a directory hash table block");
	} else {
		iblk_type = GFS2_METATYPE_IN;
		blktypedesc = _("a journaled data block");
	}
	q = block_type(block);
	if(q != gfs2_block_free) {
		log_err( _("Found duplicate block %llu (0x%llx) referenced "
			   "as metadata in indirect block for dinode "
			   "%llu (0x%llx) - was marked %d (%s)\n"),
 			 (unsigned long long)block,
			 (unsigned long long)block,
			 (unsigned long long)ip->i_di.di_num.no_addr,
			 (unsigned long long)ip->i_di.di_num.no_addr, q,
			 block_type_string(q));
		gfs2_dup_set(block);
		found_dup = 1;
	}
	nbh = bread(ip->i_sbd, block);

	if (gfs2_check_meta(nbh, iblk_type)){
		log_debug( _("Inode %lld (0x%llx) has a bad indirect block "
			     "pointer %lld (0x%llx) (points to something "
			     "that is not %s).\n"),
			   (unsigned long long)ip->i_di.di_num.no_addr,
			   (unsigned long long)ip->i_di.di_num.no_addr,
			   (unsigned long long)block,
			   (unsigned long long)block, blktypedesc);
		if(!found_dup) {
			fsck_blockmap_set(ip, block, _("bad indirect"),
					  gfs2_meta_inval);
			brelse(nbh);
			return 1;
		}
		brelse(nbh);
	} else /* blk check ok */
		*bh = nbh;

	if (!found_dup)
		fsck_blockmap_set(ip, block, _("indirect"),
				  gfs2_indir_blk);
	bc->indir_count++;

	return 0;
}

static int undo_check_metalist(struct gfs2_inode *ip, uint64_t block,
			       struct gfs2_buffer_head **bh, void *private)
{
	struct duptree *d;
	int found_dup = 0, iblk_type;
	struct gfs2_buffer_head *nbh;
	struct block_count *bc = (struct block_count *)private;

	*bh = NULL;

	if (gfs2_check_range(ip->i_sbd, block)){ /* blk outside of FS */
		fsck_blockmap_set(ip, ip->i_di.di_num.no_addr,
				  _("itself"), gfs2_block_free);
		return 1;
	}
	if (S_ISDIR(ip->i_di.di_mode))
		iblk_type = GFS2_METATYPE_JD;
	else
		iblk_type = GFS2_METATYPE_IN;

	d = dupfind(block);
	if (d) {
		log_err( _("Reversing duplicate status of block %llu (0x%llx) "
			   "referenced as metadata in indirect block for "
			   "dinode %llu (0x%llx)\n"),
 			 (unsigned long long)block,
			 (unsigned long long)block,
			 (unsigned long long)ip->i_di.di_num.no_addr,
			 (unsigned long long)ip->i_di.di_num.no_addr);
		d->refs--; /* one less reference */
		if (d->refs == 1)
			dup_delete(d);
		found_dup = 1;
	}
	nbh = bread(ip->i_sbd, block);

	if (gfs2_check_meta(nbh, iblk_type)) {
		if(!found_dup) {
			fsck_blockmap_set(ip, block, _("bad indirect"),
					  gfs2_block_free);
			brelse(nbh);
			return 1;
		}
		brelse(nbh);
	} else /* blk check ok */
		*bh = nbh;

	bc->indir_count--;
	if (found_dup)
		return 1; /* don't process the metadata again */
	else
		fsck_blockmap_set(ip, block, _("bad indirect"),
				  gfs2_block_free);
	return 0;
}

static int check_data(struct gfs2_inode *ip, uint64_t block, void *private)
{
	uint8_t q;
	struct block_count *bc = (struct block_count *) private;
	int error = 0;

	if (gfs2_check_range(ip->i_sbd, block)) {
		log_err( _("inode %lld (0x%llx) has a bad data block pointer "
			   "%lld (out of range)\n"),
			 (unsigned long long)ip->i_di.di_num.no_addr,
			 (unsigned long long)ip->i_di.di_num.no_addr,
			 (unsigned long long)block);
		/* Mark the owner of this block with the bad_block
		 * designator so we know to check it for out of range
		 * blocks later */
		fsck_blockmap_set(ip, ip->i_di.di_num.no_addr,
				  _("bad (out of range) data"),
				  gfs2_bad_block);
		return 1;
	}
	q = block_type(block);
	if (q != gfs2_block_free) {
		log_err( _("Found duplicate block referenced as data at %"
			   PRIu64 " (0x%"PRIx64 ")\n"), block, block);
		if (q != gfs2_meta_inval) {
			gfs2_dup_set(block);
			/* If the prev ref was as data, this is likely a data
			   block, so keep the block count for both refs. */
			if (q == gfs2_block_used)
				bc->data_count++;
			return 1;
		}
		/* An explanation is in order here.  At this point we found
		   a duplicate block, a block that was already referenced
		   somewhere else.  We'll resolve those duplicates in pass1b.
		   However, if the block is marked "invalid" that's a special
		   case.  It's likely that the block was discovered to be
		   invalid metadata--i.e. doesn't have a metadata header.
		   However, it still may be a valid data block, since they
		   won't have metadata headers.  In that case, the block is
		   marked as duplicate, but also as a data block. */
		error = 1;
		gfs2_block_unmark(ip->i_sbd, bl, block, gfs2_meta_inval);
		gfs2_dup_set(block);
	}
	fsck_blockmap_set(ip, block, _("data"), gfs2_block_used);
	bc->data_count++;
	return error;
}

static int undo_check_data(struct gfs2_inode *ip, uint64_t block,
			   void *private)
{
	struct duptree *d;
	struct block_count *bc = (struct block_count *) private;

	if (gfs2_check_range(ip->i_sbd, block)) {
		/* Mark the owner of this block with the bad_block
		 * designator so we know to check it for out of range
		 * blocks later */
		fsck_blockmap_set(ip, ip->i_di.di_num.no_addr,
				  _("bad (out of range) data"),
				  gfs2_block_free);
		return 1;
	}
	d = dupfind(block);
	if (d) {
		log_err( _("Reversing duplicate status of block %llu (0x%llx) "
			   "referenced as data by dinode %llu (0x%llx)\n"),
			 (unsigned long long)block,
			 (unsigned long long)block,
			 (unsigned long long)ip->i_di.di_num.no_addr,
			 (unsigned long long)ip->i_di.di_num.no_addr);
		d->refs--; /* one less reference */
		if (d->refs == 1)
			dup_delete(d);
		bc->data_count--;
		return 1;
	}
	fsck_blockmap_set(ip, block, _("data"), gfs2_block_free);
	bc->data_count--;
	return 0;
}

static int remove_inode_eattr(struct gfs2_inode *ip, struct block_count *bc)
{
	struct duptree *dt;
	struct inode_with_dups *id;
	osi_list_t *ref;
	int moved = 0;

	/* If it's a duplicate reference to the block, we need to check
	   if the reference is on the valid or invalid inodes list.
	   If it's on the valid inode's list, move it to the invalid
	   inodes list.  The reason is simple: This inode, although
	   valid, has an now-invalid reference, so we should not give
	   this reference preferential treatment over others. */
	dt = dupfind(ip->i_di.di_eattr);
	if (dt) {
		osi_list_foreach(ref, &dt->ref_inode_list) {
			id = osi_list_entry(ref, struct inode_with_dups, list);
			if (id->block_no == ip->i_di.di_num.no_addr) {
				log_debug( _("Moving inode %lld (0x%llx)'s "
					     "duplicate reference to %lld "
					     "(0x%llx) from the valid to the "
					     "invalid reference list.\n"),
					   (unsigned long long)
					   ip->i_di.di_num.no_addr,
					   (unsigned long long)
					   ip->i_di.di_num.no_addr,
					   (unsigned long long)
					   ip->i_di.di_eattr,
					   (unsigned long long)
					   ip->i_di.di_eattr);
				/* Move from the normal to the invalid list */
				osi_list_del(&id->list);
				osi_list_add_prev(&id->list,
						  &dt->ref_invinode_list);
				moved = 1;
				break;
			}
		}
		if (!moved)
			log_debug( _("Duplicate reference to %lld "
				     "(0x%llx) not moved.\n"),
				   (unsigned long long)ip->i_di.di_eattr,
				   (unsigned long long)ip->i_di.di_eattr);
	} else {
		delete_block(ip, ip->i_di.di_eattr, NULL,
			     "extended attribute", NULL);
	}
	ip->i_di.di_eattr = 0;
	bc->ea_count = 0;
	ip->i_di.di_blocks = 1 + bc->indir_count + bc->data_count;
	ip->i_di.di_flags &= ~GFS2_DIF_EA_INDIRECT;
	bmodified(ip->i_bh);
	return 0;
}

static int ask_remove_inode_eattr(struct gfs2_inode *ip,
				  struct block_count *bc)
{
	log_err( _("Inode %lld (0x%llx) has unrecoverable Extended Attribute "
		   "errors.\n"), (unsigned long long)ip->i_di.di_num.no_addr,
		 (unsigned long long)ip->i_di.di_num.no_addr);
	if (query( _("Clear all Extended Attributes from the inode? (y/n) "))){
		if (!remove_inode_eattr(ip, bc))
			log_err( _("Extended attributes were removed.\n"));
		else
			log_err( _("Unable to remove inode eattr pointer; "
				   "the error remains.\n"));
	} else {
		log_err( _("Extended attributes were not removed.\n"));
	}
	return 0;
}

/* clear_eas - clear the extended attributes for an inode
 *
 * @ip       - in core inode pointer
 * @bc       - pointer to a block count structure
 * block     - the block that had the problem
 * duplicate - if this is a duplicate block, don't set it "free"
 * emsg      - what to tell the user about the eas being checked
 * Returns: 1 if the EA is fixed, else 0 if it was not fixed.
 */
static int clear_eas(struct gfs2_inode *ip, struct block_count *bc,
		     uint64_t block, int duplicate, const char *emsg)
{
	log_err( _("Inode #%llu (0x%llx): %s"),
		(unsigned long long)ip->i_di.di_num.no_addr,
		(unsigned long long)ip->i_di.di_num.no_addr, emsg);
	log_err( _(" at block #%lld (0x%llx).\n"),
		 (unsigned long long)block, (unsigned long long)block);
	if (query( _("Clear the bad Extended Attribute? (y/n) "))) {
		if (block == ip->i_di.di_eattr) {
			remove_inode_eattr(ip, bc);
			log_err( _("The bad extended attribute was "
				   "removed.\n"));
		} else if (!duplicate) {
			delete_block(ip, block, NULL,
				     _("bad extended attribute"), NULL);
		}
		return 1;
	} else {
		log_err( _("The bad Extended Attribute was not fixed.\n"));
		bc->ea_count++;
		return 0;
	}
}

static int check_eattr_indir(struct gfs2_inode *ip, uint64_t indirect,
			     uint64_t parent, struct gfs2_buffer_head **bh,
			     void *private)
{
	struct gfs2_sbd *sdp = ip->i_sbd;
	int ret = 0;
	uint8_t q;
	struct block_count *bc = (struct block_count *) private;

	/* This inode contains an eattr - it may be invalid, but the
	 * eattr attributes points to a non-zero block */
	if(gfs2_check_range(sdp, indirect)) {
		/*log_warn("EA indirect block #%"PRIu64" is out of range.\n",
			indirect);
			gfs2_blockmap_set(bl, parent, bad_block);*/
		/* Doesn't help to mark this here - this gets checked
		 * in pass1c */
		return 1;
	}
	q = block_type(indirect);

	/* Special duplicate processing:  If we have an EA block,
	   check if it really is an EA.  If it is, let duplicate
	   handling sort it out.  If it isn't, clear it but don't
	   count it as a duplicate. */
	*bh = bread(sdp, indirect);
	if(gfs2_check_meta(*bh, GFS2_METATYPE_IN)) {
		if(q != gfs2_block_free) { /* Duplicate? */
			if (!clear_eas(ip, bc, indirect, 1,
				       _("Bad indirect Extended Attribute "
					 "duplicate found"))) {
				gfs2_dup_set(indirect);
				bc->ea_count++;
			}
			return 1;
		}
		clear_eas(ip, bc, indirect, 0,
			  _("Extended Attribute indirect block has incorrect "
			    "type"));
		return 1;
	}
	if(q != gfs2_block_free) { /* Duplicate? */
		log_err( _("Inode #%llu (0x%llx): Duplicate Extended "
			   "Attribute indirect block found at #%llu "
			   "(0x%llx).\n"),
			 (unsigned long long)ip->i_di.di_num.no_addr,
			 (unsigned long long)ip->i_di.di_num.no_addr,
			 (unsigned long long)indirect,
			 (unsigned long long)indirect);
		gfs2_dup_set(indirect);
		bc->ea_count++;
		ret = 1;
	} else {
		fsck_blockmap_set(ip, indirect,
				  _("indirect Extended Attribute"),
				  gfs2_indir_blk);
		bc->ea_count++;
	}
	return ret;
}

static int finish_eattr_indir(struct gfs2_inode *ip, int leaf_pointers,
			      int leaf_pointer_errors, void *private)
{
	struct block_count *bc = (struct block_count *) private;

	if (leaf_pointer_errors == leaf_pointers) /* All eas were bad */
		return ask_remove_inode_eattr(ip, bc);
	log_debug( _("Marking inode #%llu (0x%llx) with extended "
		     "attribute block\n"),
		   (unsigned long long)ip->i_di.di_num.no_addr,
		   (unsigned long long)ip->i_di.di_num.no_addr);
	/* Mark the inode as having an eattr in the block map
	   so pass1c can check it. */
	gfs2_special_add(&ip->i_sbd->eattr_blocks, ip->i_di.di_num.no_addr);
	if (!leaf_pointer_errors)
		return 0;
	log_err( _("Inode %lld (0x%llx) has recoverable indirect "
		   "Extended Attribute errors.\n"),
		   (unsigned long long)ip->i_di.di_num.no_addr,
		   (unsigned long long)ip->i_di.di_num.no_addr);
	if (query( _("Okay to fix the block count for the inode? (y/n) "))) {
		ip->i_di.di_blocks = 1 + bc->indir_count +
			bc->data_count + bc->ea_count;
		bmodified(ip->i_bh);
		log_err( _("Block count fixed.\n"));
		return 1;
	}
	log_err( _("Block count not fixed.\n"));
	return 1;
}

static int check_leaf_block(struct gfs2_inode *ip, uint64_t block, int btype,
			    struct gfs2_buffer_head **bh, void *private)
{
	struct gfs2_buffer_head *leaf_bh = NULL;
	struct gfs2_sbd *sdp = ip->i_sbd;
	uint8_t q;
	struct block_count *bc = (struct block_count *) private;

	q = block_type(block);
	/* Special duplicate processing:  If we have an EA block, check if it
	   really is an EA.  If it is, let duplicate handling sort it out.
	   If it isn't, clear it but don't count it as a duplicate. */
	leaf_bh = bread(sdp, block);
	if(gfs2_check_meta(leaf_bh, btype)) {
		if(q != gfs2_block_free) { /* Duplicate? */
			clear_eas(ip, bc, block, 1,
				  _("Bad Extended Attribute duplicate found"));
		} else {
			clear_eas(ip, bc, block, 0,
				  _("Extended Attribute leaf block "
				    "has incorrect type"));
		}
		brelse(leaf_bh);
		return 1;
	}
	if(q != gfs2_block_free) { /* Duplicate? */
		log_debug( _("Duplicate block found at #%lld (0x%llx).\n"),
			   (unsigned long long)block,
			   (unsigned long long)block);
		gfs2_dup_set(block);
		bc->ea_count++;
		brelse(leaf_bh);
		return 1;
	}
	if (ip->i_di.di_eattr == 0) {
		/* Can only get in here if there were unrecoverable ea
		   errors that caused clear_eas to be called.  What we
		   need to do here is remove the subsequent ea blocks. */
		clear_eas(ip, bc, block, 0,
			  _("Extended Attribute block removed due to "
			    "previous errors.\n"));
		brelse(leaf_bh);
		return 1;
	}
	/* Point of confusion: We've got to set the ea block itself to
	   gfs2_meta_eattr here.  Elsewhere we mark the inode with
	   gfs2_eattr_block meaning it contains an eattr for pass1c. */
	fsck_blockmap_set(ip, block, _("Extended Attribute"), gfs2_meta_eattr);
	bc->ea_count++;
	*bh = leaf_bh;
	return 0;
}

/**
 * check_extended_leaf_eattr
 * @ip
 * @el_blk: block number of the extended leaf
 *
 * An EA leaf block can contain EA's with pointers to blocks
 * where the data for that EA is kept.  Those blocks still
 * have the gfs2 meta header of type GFS2_METATYPE_EA
 *
 * Returns: 0 if correct[able], -1 if removal is needed
 */
static int check_extended_leaf_eattr(struct gfs2_inode *ip, uint64_t *data_ptr,
				     struct gfs2_buffer_head *leaf_bh,
				     struct gfs2_ea_header *ea_hdr,
				     struct gfs2_ea_header *ea_hdr_prev,
				     void *private)
{
	uint64_t el_blk = be64_to_cpu(*data_ptr);
	struct gfs2_sbd *sdp = ip->i_sbd;
	struct gfs2_buffer_head *bh = NULL;
	int error;

	if(gfs2_check_range(sdp, el_blk)){
		log_err( _("Inode #%llu (0x%llx): Extended Attribute block "
			   "%llu (0x%llx) has an extended leaf block #%llu "
			   "(0x%llx) that is out of range.\n"),
			 (unsigned long long)ip->i_di.di_num.no_addr,
			 (unsigned long long)ip->i_di.di_num.no_addr,
			 (unsigned long long)ip->i_di.di_eattr,
			 (unsigned long long)ip->i_di.di_eattr,
			 (unsigned long long)el_blk,
			 (unsigned long long)el_blk);
		fsck_blockmap_set(ip, ip->i_di.di_eattr,
				  _("bad (out of range) Extended Attribute "),
				  gfs2_bad_block);
		return 1;
	}
	error = check_leaf_block(ip, el_blk, GFS2_METATYPE_ED, &bh, private);
	if (bh)
		brelse(bh);
	return error;
}

static int check_eattr_leaf(struct gfs2_inode *ip, uint64_t block,
			    uint64_t parent, struct gfs2_buffer_head **bh,
			    void *private)
{
	struct gfs2_sbd *sdp = ip->i_sbd;

	/* This inode contains an eattr - it may be invalid, but the
	 * eattr attributes points to a non-zero block.
	 * Clarification: If we're here we're checking a leaf block, and the
	 * source dinode needs to be marked as having extended attributes.
	 * That instructs pass1c to check the contents of the ea blocks. */
	log_debug( _("Setting inode %lld (0x%llx) as having eattr "
		     "block(s) attached.\n"),
		   (unsigned long long)ip->i_di.di_num.no_addr,
		   (unsigned long long)ip->i_di.di_num.no_addr);
	gfs2_special_add(&sdp->eattr_blocks, ip->i_di.di_num.no_addr);
	if(gfs2_check_range(sdp, block)) {
		log_warn( _("Inode #%llu (0x%llx): Extended Attribute leaf "
			    "block #%llu (0x%llx) is out of range.\n"),
			 (unsigned long long)ip->i_di.di_num.no_addr,
			 (unsigned long long)ip->i_di.di_num.no_addr,
			 (unsigned long long)block, (unsigned long long)block);
		fsck_blockmap_set(ip, ip->i_di.di_eattr,
				  _("bad (out of range) Extended "
				    "Attribute leaf"), gfs2_bad_block);
		return 1;
	}
	return check_leaf_block(ip, block, GFS2_METATYPE_EA, bh, private);
}

static int check_eattr_entries(struct gfs2_inode *ip,
			       struct gfs2_buffer_head *leaf_bh,
			       struct gfs2_ea_header *ea_hdr,
			       struct gfs2_ea_header *ea_hdr_prev,
			       void *private)
{
	struct gfs2_sbd *sdp = ip->i_sbd;
	char ea_name[256];

	if(!ea_hdr->ea_name_len){
		/* Skip this entry for now */
		return 1;
	}

	memset(ea_name, 0, sizeof(ea_name));
	strncpy(ea_name, (char *)ea_hdr + sizeof(struct gfs2_ea_header),
		ea_hdr->ea_name_len);

	if(!GFS2_EATYPE_VALID(ea_hdr->ea_type) &&
	   ((ea_hdr_prev) || (!ea_hdr_prev && ea_hdr->ea_type))){
		/* Skip invalid entry */
		return 1;
	}

	if(ea_hdr->ea_num_ptrs){
		uint32_t avail_size;
		int max_ptrs;

		avail_size = sdp->sd_sb.sb_bsize - sizeof(struct gfs2_meta_header);
		max_ptrs = (be32_to_cpu(ea_hdr->ea_data_len)+avail_size-1)/avail_size;

		if(max_ptrs > ea_hdr->ea_num_ptrs) {
			return 1;
		} else {
			log_debug( _("  Pointers Required: %d\n  Pointers Reported: %d\n"),
				  max_ptrs, ea_hdr->ea_num_ptrs);
		}
	}
	return 0;
}

/**
 * Check for massive amounts of pointer corruption.  If the block has
 * lots of out-of-range pointers, we can't trust any of the pointers.
 * For example, a stray pointer with a value of 0x1d might be
 * corruption/nonsense, and if so, we don't want to delete an
 * important file (like master or the root directory) because of it.
 * We need to check for a large number of bad pointers BEFORE we start
 * messing with them because we don't want to mark a block as a
 * duplicate (for example) until we know if the pointers in general can
 * be trusted. Thus it needs to be in a separate loop.
 */
static int rangecheck_block(struct gfs2_inode *ip, uint64_t block,
			    struct gfs2_buffer_head **bh,
			    const char *btype, void *private)
{
	long *bad_pointers = (long *)private;
	uint8_t q;

	if (gfs2_check_range(ip->i_sbd, block) != 0) {
		(*bad_pointers)++;
		log_debug( _("Bad %s block pointer (out of range #%ld) "
			     "found in inode %lld (0x%llx).\n"), btype,
			   *bad_pointers,
			   (unsigned long long)ip->i_di.di_num.no_addr,
			   (unsigned long long)ip->i_di.di_num.no_addr);
		if ((*bad_pointers) <= BAD_POINTER_TOLERANCE)
			return ENOENT;
		else
			return -ENOENT; /* Exits check_metatree quicker */
	}
	/* See how many duplicate blocks it has */
	q = block_type(block);
	if (q != gfs2_block_free) {
		(*bad_pointers)++;
		log_debug( _("Duplicated %s block pointer (violation #%ld) "
			     "found in inode %lld (0x%llx).\n"), btype,
			   *bad_pointers,
			   (unsigned long long)ip->i_di.di_num.no_addr,
			   (unsigned long long)ip->i_di.di_num.no_addr);
		if ((*bad_pointers) <= BAD_POINTER_TOLERANCE)
			return ENOENT;
		else
			return -ENOENT; /* Exits check_metatree quicker */
	}
	return 0;
}

static int rangecheck_metadata(struct gfs2_inode *ip, uint64_t block,
			       struct gfs2_buffer_head **bh, void *private)
{
	return rangecheck_block(ip, block, bh, _("metadata"), private);
}

static int rangecheck_leaf(struct gfs2_inode *ip, uint64_t block,
			   struct gfs2_buffer_head *bh, void *private)
{
	return rangecheck_block(ip, block, &bh, _("leaf"), private);
}

static int rangecheck_data(struct gfs2_inode *ip, uint64_t block,
			   void *private)
{
	return rangecheck_block(ip, block, NULL, _("data"), private);
}

static int rangecheck_eattr_indir(struct gfs2_inode *ip, uint64_t block,
				  uint64_t parent,
				  struct gfs2_buffer_head **bh, void *private)
{
	return rangecheck_block(ip, block, NULL,
				_("indirect extended attribute"),
				private);
}

static int rangecheck_eattr_leaf(struct gfs2_inode *ip, uint64_t block,
				 uint64_t parent, struct gfs2_buffer_head **bh,
				 void *private)
{
	return rangecheck_block(ip, block, NULL, _("extended attribute"),
				private);
}

struct metawalk_fxns rangecheck_fxns = {
        .private = NULL,
        .check_metalist = rangecheck_metadata,
        .check_data = rangecheck_data,
        .check_leaf = rangecheck_leaf,
        .check_eattr_indir = rangecheck_eattr_indir,
        .check_eattr_leaf = rangecheck_eattr_leaf,
};

static int handle_di(struct gfs2_sbd *sdp, struct gfs2_buffer_head *bh,
			  uint64_t block)
{
	uint8_t q;
	struct gfs2_inode *ip;
	int error;
	struct block_count bc = {0};
	long bad_pointers;

	q = block_type(block);
	if(q != gfs2_block_free) {
		log_err( _("Found duplicate block referenced as an inode at "
			   "#%" PRIu64 " (0x%" PRIx64 ")\n"), block, block);
		gfs2_dup_set(block);
		fsck_inode_put(&ip);
		return 0;
	}

	ip = fsck_inode_get(sdp, bh);
	if (ip->i_di.di_num.no_addr != block) {
		log_err( _("Inode #%llu (0x%llx): Bad inode address found: %llu "
			"(0x%llx)\n"), (unsigned long long)block,
			(unsigned long long)block,
			(unsigned long long)ip->i_di.di_num.no_addr,
			(unsigned long long)ip->i_di.di_num.no_addr);
		if(query( _("Fix address in inode at block #%"
			    PRIu64 " (0x%" PRIx64 ")? (y/n) "),
			 block, block)) {
			ip->i_di.di_num.no_addr = ip->i_di.di_num.no_formal_ino = block;
			bmodified(ip->i_bh);
		} else
			log_err( _("Address in inode at block #%" PRIu64
				 " (0x%" PRIx64 ") not fixed\n"), block, block);
	}

	bad_pointers = 0L;

	/* First, check the metadata for massive amounts of pointer corruption.
	   Such corruption can only lead us to ruin trying to clean it up,
	   so it's better to check it up front and delete the inode if
	   there is corruption. */
	rangecheck_fxns.private = &bad_pointers;
	error = check_metatree(ip, &rangecheck_fxns);
	if (bad_pointers > BAD_POINTER_TOLERANCE) {
		log_err( _("Error: inode %llu (0x%llx) has more than "
			   "%d bad pointers.\n"),
			 (unsigned long long)ip->i_di.di_num.no_addr,
			 (unsigned long long)ip->i_di.di_num.no_addr,
			 BAD_POINTER_TOLERANCE);
		fsck_blockmap_set(ip, ip->i_di.di_num.no_addr,
				  _("badly corrupt"), gfs2_block_free);
		fsck_inode_put(&ip);
		return 0;
	}

	switch(ip->i_di.di_mode & S_IFMT) {

	case S_IFDIR:
		if (fsck_blockmap_set(ip, block, _("directory"),
				      gfs2_inode_dir)) {
			stack;
			fsck_inode_put(&ip);
			return -1;
		}
		if(!dirtree_insert(block)) {
			stack;
			fsck_inode_put(&ip);
			return -1;
		}
		break;
	case S_IFREG:
		if (fsck_blockmap_set(ip, block, _("file"),
				      gfs2_inode_file)) {
			stack;
			fsck_inode_put(&ip);
			return -1;
		}
		break;
	case S_IFLNK:
		if (fsck_blockmap_set(ip, block, _("symlink"),
				      gfs2_inode_lnk)) {
			stack;
			fsck_inode_put(&ip);
			return -1;
		}
		break;
	case S_IFBLK:
		if (fsck_blockmap_set(ip, block, _("block device"),
				      gfs2_inode_blk)) {
			stack;
			fsck_inode_put(&ip);
			return -1;
		}
		break;
	case S_IFCHR:
		if (fsck_blockmap_set(ip, block, _("character device"),
				      gfs2_inode_chr)) {
			stack;
			fsck_inode_put(&ip);
			return -1;
		}
		break;
	case S_IFIFO:
		if (fsck_blockmap_set(ip, block, _("fifo"),
				      gfs2_inode_fifo)) {
			stack;
			fsck_inode_put(&ip);
			return -1;
		}
		break;
	case S_IFSOCK:
		if (fsck_blockmap_set(ip, block, _("socket"),
				      gfs2_inode_sock)) {
			stack;
			fsck_inode_put(&ip);
			return -1;
		}
		break;
	default:
		if (fsck_blockmap_set(ip, block, _("invalid mode"),
				      gfs2_inode_invalid)) {
			stack;
			fsck_inode_put(&ip);
			return -1;
		}
		fsck_inode_put(&ip);
		return 0;
	}
	if(set_link_count(ip->i_di.di_num.no_addr, ip->i_di.di_nlink)) {
		stack;
		fsck_inode_put(&ip);
		return -1;
	}

	if (S_ISDIR(ip->i_di.di_mode) &&
	    (ip->i_di.di_flags & GFS2_DIF_EXHASH)) {
		if (((1 << ip->i_di.di_depth) * sizeof(uint64_t)) != ip->i_di.di_size){
			log_warn( _("Directory dinode #%llu (0x%llx"
				 ") has bad depth.  Found %u, Expected %u\n"),
				 (unsigned long long)ip->i_di.di_num.no_addr,
				 (unsigned long long)ip->i_di.di_num.no_addr,
				 ip->i_di.di_depth,
				 (1 >> (ip->i_di.di_size/sizeof(uint64_t))));
			if(fsck_blockmap_set(ip, block, _("bad depth"),
					     gfs2_meta_inval)) {
				stack;
				fsck_inode_put(&ip);
				return -1;
			}
			fsck_inode_put(&ip);
			return 0;
		}
	}

	pass1_fxns.private = &bc;
	error = check_metatree(ip, &pass1_fxns);
	if (fsck_abort || error < 0) {
		fsck_inode_put(&ip);
		return 0;
	}
	if (error > 0) {
		log_err( _("Error: inode %llu (0x%llx) has unrecoverable "
			   "errors; invalidating.\n"),
			 (unsigned long long)ip->i_di.di_num.no_addr,
			 (unsigned long long)ip->i_di.di_num.no_addr);
		undo_fxns.private = &bc;
		check_metatree(ip, &undo_fxns);
		/* If we undo the metadata accounting, including metadatas
		   duplicate block status, we need to make sure later passes
		   don't try to free up the metadata referenced by this inode.
		   Therefore we mark the inode as free space. */
		fsck_blockmap_set(ip, ip->i_di.di_num.no_addr,
				  _("corrupt"), gfs2_block_free);
		fsck_inode_put(&ip);
		return 0;
	}

	error = check_inode_eattr(ip, &pass1_fxns);

	if (error &&
	    !(ip->i_di.di_flags & GFS2_DIF_EA_INDIRECT))
		ask_remove_inode_eattr(ip, &bc);

	if (ip->i_di.di_blocks != 
		(1 + bc.indir_count + bc.data_count + bc.ea_count)) {
		log_err( _("Inode #%llu (0x%llx): Ondisk block count (%llu"
			") does not match what fsck found (%llu)\n"),
			(unsigned long long)ip->i_di.di_num.no_addr,
			(unsigned long long)ip->i_di.di_num.no_addr,
			(unsigned long long)ip->i_di.di_blocks,
			(unsigned long long)1 + bc.indir_count +
			bc.data_count + bc.ea_count);
		log_info( _("inode has: %lld, but fsck counts: Dinode:1 + "
			    "indir:%lld + data: %lld + ea: %lld\n"),
			  (unsigned long long)ip->i_di.di_blocks,
			  (unsigned long long)bc.indir_count,
			  (unsigned long long)bc.data_count,
			  (unsigned long long)bc.ea_count);
		if (query( _("Fix ondisk block count? (y/n) "))) {
			ip->i_di.di_blocks = 1 + bc.indir_count + bc.data_count +
				bc.ea_count;
			bmodified(ip->i_bh);
			log_err( _("Block count for #%llu (0x%llx) fixed\n"),
				(unsigned long long)ip->i_di.di_num.no_addr,
				(unsigned long long)ip->i_di.di_num.no_addr);
		} else
			log_err( _("Bad block count for #%llu (0x%llx"
				") not fixed\n"),
				(unsigned long long)ip->i_di.di_num.no_addr,
				(unsigned long long)ip->i_di.di_num.no_addr);
	}

	fsck_inode_put(&ip);
	return 0;
}

/**
 * pass1 - walk through inodes and check inode state
 *
 * this walk can be done using root inode and depth first search,
 * watching for repeat inode numbers
 *
 * format & type
 * link count
 * duplicate blocks
 * bad blocks
 * inodes size
 * dir info
 */
int pass1(struct gfs2_sbd *sbp)
{
	struct gfs2_buffer_head *bh;
	osi_list_t *tmp;
	uint64_t block;
	struct rgrp_list *rgd;
	int first;
	uint64_t i;
	uint64_t blk_count;
	uint64_t offset;
	uint64_t rg_count = 0;

	/* FIXME: In the gfs fsck, we had to mark things like the
	 * journals and indices and such as 'other_meta' - in gfs2,
	 * the journals are files and are found in the normal file
	 * sweep - is there any metadata we need to mark here before
	 * the sweeps start that we won't find otherwise? */

	/* So, do we do a depth first search starting at the root
	 * inode, or use the rg bitmaps, or just read every fs block
	 * to find the inodes?  If we use the depth first search, why
	 * have pass3 at all - if we use the rg bitmaps, pass5 is at
	 * least partially invalidated - if we read every fs block,
	 * things will probably be intolerably slow.  The current fsck
	 * uses the rg bitmaps, so maybe that's the best way to start
	 * things - we can change the method later if necessary.
	 */
	for (tmp = sbp->rglist.next; tmp != &sbp->rglist;
	     tmp = tmp->next, rg_count++) {
		log_debug( _("Checking metadata in Resource Group #%" PRIu64 "\n"),
				 rg_count);
		rgd = osi_list_entry(tmp, struct rgrp_list, list);
		for (i = 0; i < rgd->ri.ri_length; i++) {
			log_debug( _("rgrp block %lld (0x%llx) "
				     "is now marked as 'rgrp data'\n"),
				   rgd->ri.ri_addr + i, rgd->ri.ri_addr + i);
			if(gfs2_blockmap_set(bl, rgd->ri.ri_addr + i,
					     gfs2_meta_rgrp)){
				stack;
				return FSCK_ERROR;
			}
			/* rgrps and bitmaps don't have bits to represent
			   their blocks, so don't do this:
			check_n_fix_bitmap(sbp, rgd->ri.ri_addr + i,
			gfs2_meta_rgrp);*/
		}

		offset = sizeof(struct gfs2_rgrp);
		blk_count = 1;
		first = 1;

		while (1) {
			/* "block" is relative to the entire file system */
			/* Get the next dinode in the file system, according
			   to the bitmap.  This should ONLY be dinodes. */
			if (gfs2_next_rg_meta(rgd, &block, first))
				break;
			warm_fuzzy_stuff(block);

			if (fsck_abort) /* if asked to abort */
				return FSCK_OK;
			if (skip_this_pass) {
				printf( _("Skipping pass 1 is not a good idea.\n"));
				skip_this_pass = FALSE;
				fflush(stdout);
			}
			bh = bread(sbp, block);

			/*log_debug( _("Checking metadata block #%" PRIu64
			  " (0x%" PRIx64 ")\n"), block, block);*/

			if (gfs2_check_meta(bh, GFS2_METATYPE_DI)) {
				log_err( _("Found invalid inode at block #"
					   "%llu (0x%llx)\n"),
					 (unsigned long long)block,
					 (unsigned long long)block);
				if (gfs2_blockmap_set(bl, block,
						      gfs2_block_free)) {
					stack;
					brelse(bh);
					return FSCK_ERROR;
				}
				check_n_fix_bitmap(sbp, block,
						   gfs2_block_free);
			} else if (handle_di(sbp, bh, block) < 0) {
				stack;
				brelse(bh);
				return FSCK_ERROR;
			}
			/* Ignore everything else - they should be hit by the
			   handle_di step.  Don't check NONE either, because
			   check_meta passes everything if GFS2_METATYPE_NONE
			   is specified.  Hopefully, other metadata types such
			   as indirect blocks will be handled when the inode
			   itself is processed, and if it's not, it should be
			   caught in pass5. */
			brelse(bh);
			first = 0;
		}
	}
	return FSCK_OK;
}
