#include "clusterautoconfig.h"

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <libintl.h>
#include <sys/stat.h>
#define _(String) gettext(String)

#include <logging.h>
#include "libgfs2.h"
#include "fsck.h"
#include "osi_list.h"
#include "util.h"
#include "metawalk.h"
#include "inode_hash.h"

struct fxn_info {
	uint64_t block;
	int found;
	int ea_only;    /* The only dups were found in EAs */
};

struct dup_handler {
	struct duptree *dt;
	int ref_inode_count;
	int ref_count;
};

static void log_inode_reference(struct duptree *dt, osi_list_t *tmp, int inval)
{
	char reftypestring[32];
	struct inode_with_dups *id;

	id = osi_list_entry(tmp, struct inode_with_dups, list);
	if (id->dup_count == 1)
		sprintf(reftypestring, "as %s", reftypes[get_ref_type(id)]);
	else
		sprintf(reftypestring, "%d/%d/%d/%d",
			id->reftypecount[ref_is_inode],
			id->reftypecount[ref_as_data],
			id->reftypecount[ref_as_meta],
			id->reftypecount[ref_as_ea]);
	if (inval)
		log_warn( _("Invalid "));
	log_warn( _("Inode %s (%lld/0x%llx) has %d reference(s) to "
		    "block %llu (0x%llx) (%s)\n"), id->name,
		  (unsigned long long)id->block_no,
		  (unsigned long long)id->block_no, id->dup_count,
		  (unsigned long long)dt->block,
		  (unsigned long long)dt->block, reftypestring);
}

/*
 * resolve_dup_references - resolve all but the last dinode that has a
 *                          duplicate reference to a given block.
 *
 * @sdp - pointer to the superblock structure
 * @dt - pointer to the duplicate reference rbtree to use
 * @ref_list - list of duplicate references to be resolved (invalid or valid)
 * @dh - duplicate handler
 * inval - The references on this ref_list are invalid.  We prefer to delete
 *         these first before resorting to deleting valid dinodes.
 * acceptable_ref - Delete dinodes that reference the given block as anything
 *                  _but_ this type.  Try to save references as this type.
 */
static void resolve_dup_references(struct gfs2_sbd *sdp, struct duptree *dt,
				   osi_list_t *ref_list,
				   struct dup_handler *dh,
				   int inval, int acceptable_ref)
{
	struct gfs2_inode *ip;
	struct inode_with_dups *id;
	osi_list_t *tmp, *x;
	struct metawalk_fxns pass1b_fxns_delete = {
		.private = NULL,
		.check_metalist = delete_metadata,
		.check_data = delete_data,
		.check_leaf = delete_leaf,
		.check_eattr_indir = delete_eattr_indir,
		.check_eattr_leaf = delete_eattr_leaf,
		.check_eattr_entry = delete_eattr_entry,
		.check_eattr_extentry = delete_eattr_extentry,
	};
	enum dup_ref_type this_ref;
	struct inode_info *ii;
	int found_good_ref = 0;
	uint8_t q;

	osi_list_foreach_safe(tmp, ref_list, x) {
		if (skip_this_pass || fsck_abort)
			return;

		id = osi_list_entry(tmp, struct inode_with_dups, list);
		dh->dt = dt;

		if (dh->ref_inode_count == 1) /* down to the last reference */
			return;

		this_ref = get_ref_type(id);
		q = block_type(id->block_no);
		if (inval)
			log_warn( _("Invalid "));
		/* FIXME: If we already found an acceptable reference to this
		 * block, we should really duplicate the block and fix all
		 * references to it in this inode.  Unfortunately, we would
		 * have to traverse the entire metadata tree to do that. */
		if (acceptable_ref != ref_types && /* If we're nuking all but
						      an acceptable reference
						      type and */
		    this_ref == acceptable_ref && /* this ref is acceptable */
		    !found_good_ref) { /* We haven't found a good reference */
			/* If this is an invalid inode, but not on the invalid
			   list, it's better to delete it. */
			if (q == GFS2_BLKST_DINODE) {
				found_good_ref = 1;
				log_warn( _("Inode %s (%lld/0x%llx)'s "
					    "reference to block %llu (0x%llx) "
					    "as '%s' is acceptable.\n"),
					  id->name,
					  (unsigned long long)id->block_no,
					  (unsigned long long)id->block_no,
					  (unsigned long long)dt->block,
					  (unsigned long long)dt->block,
					  reftypes[this_ref]);
				continue; /* don't delete the dinode */
			}
		}
		/* If this reference is from a system inode, for example, if
		   it's data or metadata inside a journal, the reference
		   should take priority over user dinodes that reference the
		   block. */
		if (!found_good_ref && fsck_system_inode(sdp, id->block_no)) {
			found_good_ref = 1;
			continue; /* don't delete the dinode */
		}
		log_warn( _("Inode %s (%lld/0x%llx) references block "
			    "%llu (0x%llx) as '%s', but the block is "
			    "really %s.\n"),
			  id->name, (unsigned long long)id->block_no,
			  (unsigned long long)id->block_no,
			  (unsigned long long)dt->block,
			  (unsigned long long)dt->block,
			  reftypes[this_ref], reftypes[acceptable_ref]);
		if (!(query( _("Okay to delete %s inode %lld (0x%llx)? "
			       "(y/n) "),
			     (inval ? _("invalidated") : ""),
			     (unsigned long long)id->block_no,
			     (unsigned long long)id->block_no))) {
			log_warn( _("The bad inode was not cleared."));
			/* delete the list entry so we don't leak memory but
			   leave the reference count. If we decrement the
			   ref count, we could get down to 1 and the dinode
			   would be changed without a 'Yes' answer. */
			/* (dh->ref_inode_count)--;*/
			dup_listent_delete(dt, id);
			continue;
		}
		if (q == GFS2_BLKST_FREE)
			log_warn( _("Inode %lld (0x%llx) was previously "
				    "deleted.\n"),
				  (unsigned long long)id->block_no,
				  (unsigned long long)id->block_no);
		else
			log_warn(_("Pass1b is deleting inode %lld (0x%llx).\n"),
				 (unsigned long long)id->block_no,
				 (unsigned long long)id->block_no);

		ip = fsck_load_inode(sdp, id->block_no);
		/* If we've already deleted this dinode, don't try to delete
		   it again. That could free blocks that used to be duplicate
		   references that are now resolved (and gone). */
		if (q != GFS2_BLKST_FREE) {
			/* Clear the EAs for the inode first */
			check_inode_eattr(ip, &pass1b_fxns_delete);
			/* If the reference was as metadata or data, we've got
			   a corrupt dinode that will be deleted. */
			if (inval || id->reftypecount[ref_as_data] ||
			    id->reftypecount[ref_as_meta]) {
				/* Remove the inode from the inode tree */
				ii = inodetree_find(ip->i_di.di_num.no_addr);
				if (ii)
					inodetree_delete(ii);
				fsck_blockmap_set(ip, ip->i_di.di_num.no_addr,
						_("duplicate referencing bad"),
						  GFS2_BLKST_UNLINKED);
				/* We delete the dup_handler inode count and
				   duplicate id BEFORE clearing the metadata,
				   because if this is the last reference to
				   this metadata block, we need to traverse the
				   tree and free the data blocks it references.
				   However, we don't want to delete other
				   duplicates that may be used by other
				   dinodes. */
				(dh->ref_inode_count)--;
				/* FIXME: other option should be to duplicate
				   the block for each duplicate and point the
				   metadata at the cloned blocks */
				check_metatree(ip, &pass1b_fxns_delete);
			}
		}
		/* Now we've got to go through an delete any other duplicate
		   references from this dinode we're deleting. If we don't,
		   pass1b will discover the other duplicate record, try to
		   delete this dinode a second time, and this time its earlier
		   duplicate references won't be seen as duplicates anymore
		   (because they were eliminated earlier in pass1b). And so
		   the blocks will be mistakenly freed, when, in fact, they're
		   still being referenced by a valid dinode. */
		delete_all_dups(ip);
		fsck_inode_put(&ip); /* out, brelse, free */
	}
	return;
}

/* revise_dup_handler - get current information about a duplicate reference
 *
 * Function resolve_dup_references can delete dinodes that reference blocks
 * which may have duplicate references. Therefore, the duplicate tree is
 * constantly being changed. This function revises the duplicate handler so
 * that it accurately matches what's in the duplicate tree regarding this block
 */
static void revise_dup_handler(uint64_t dup_blk, struct dup_handler *dh)
{
	osi_list_t *tmp;
	struct duptree *dt;
	struct inode_with_dups *id;

	dh->ref_inode_count = 0;
	dh->ref_count = 0;
	dh->dt = NULL;

	dt = dupfind(dup_blk);
	if (!dt)
		return;

	dh->dt = dt;
	/* Count the duplicate references, both valid and invalid */
	osi_list_foreach(tmp, &dt->ref_invinode_list) {
		id = osi_list_entry(tmp, struct inode_with_dups, list);
		dh->ref_inode_count++;
		dh->ref_count += id->dup_count;
	}
	osi_list_foreach(tmp, &dt->ref_inode_list) {
		id = osi_list_entry(tmp, struct inode_with_dups, list);
		dh->ref_inode_count++;
		dh->ref_count += id->dup_count;
	}
}

/* handle_dup_blk - handle a duplicate block reference.
 *
 * This function should resolve and delete the duplicate block reference given,
 * iow dt.
 */
static int handle_dup_blk(struct gfs2_sbd *sdp, struct duptree *dt)
{
	osi_list_t *tmp;
	struct gfs2_inode *ip;
	struct inode_with_dups *id;
	struct dup_handler dh = {0};
	struct gfs2_buffer_head *bh;
	uint32_t cmagic, ctype;
	enum dup_ref_type acceptable_ref;
	uint64_t dup_blk;

	dup_blk = dt->block;
	revise_dup_handler(dup_blk, &dh);

	/* Log the duplicate references */
	log_notice( _("Block %llu (0x%llx) has %d inodes referencing it"
		   " for a total of %d duplicate references:\n"),
		    (unsigned long long)dt->block,
		    (unsigned long long)dt->block,
		    dh.ref_inode_count, dh.ref_count);

	osi_list_foreach(tmp, &dt->ref_invinode_list)
		log_inode_reference(dt, tmp, 1);
	osi_list_foreach(tmp, &dt->ref_inode_list)
		log_inode_reference(dt, tmp, 0);

	/* Figure out the block type to see if we can eliminate references
	   to a different type. In other words, if the duplicate block looks
	   like metadata, we can delete dinodes that reference it as data.
	   If the block doesn't look like metadata, we can eliminate any
	   references to it as metadata.  Dinodes with such references are
	   clearly corrupt and need to be deleted.
	   And if we're left with a single reference, problem solved. */
	bh = bread(sdp, dt->block);
	cmagic = ((struct gfs2_meta_header *)(bh->b_data))->mh_magic;
	ctype = ((struct gfs2_meta_header *)(bh->b_data))->mh_type;
	brelse(bh);

	/* If this is a dinode, any references to it (except in directory
	   entries) are invalid and should be deleted. */
	if (be32_to_cpu(cmagic) == GFS2_MAGIC &&
	    be32_to_cpu(ctype) == GFS2_METATYPE_DI)
		acceptable_ref = ref_is_inode;
	else if (be32_to_cpu(cmagic) == GFS2_MAGIC &&
	    (be32_to_cpu(ctype) == GFS2_METATYPE_EA ||
	     be32_to_cpu(ctype) == GFS2_METATYPE_ED))
		acceptable_ref = ref_as_ea;
	else if (be32_to_cpu(cmagic) == GFS2_MAGIC &&
		 be32_to_cpu(ctype) <= GFS2_METATYPE_QC)
		acceptable_ref = ref_as_meta;
	else
		acceptable_ref = ref_as_data;

	/* A single reference to the block implies a possible situation where
	   a data pointer points to a metadata block.  In other words, the
	   duplicate reference in the file system is (1) Metadata block X and
	   (2) A dinode reference such as a data pointer pointing to block X.
	   We can't really check for that in pass1 because user data might
	   just _look_ like metadata by coincidence, and at the time we're
	   checking, we might not have processed the referenced block.
	   Here in pass1b we're sure. */
	/* Another possibility here is that there is a single reference
	   because all the other metadata references were in inodes that got
	   invalidated for other reasons, such as bad pointers.  So we need to
	   make sure at this point that any inode deletes reverse out any
	   duplicate reference before we get to this point. */

	/* Step 1 - eliminate references from inodes that are not valid.
	 *          This may be because they were deleted due to corruption.
	 *          All block types are unacceptable, so we use ref_types.
	 */
	if (dh.ref_count > 1) {
		log_debug( _("----------------------------------------------\n"
			     "Step 1: Eliminate references to block %llu "
			     "(0x%llx) that were previously marked "
			     "invalid.\n"),
			   (unsigned long long)dt->block,
			   (unsigned long long)dt->block);
		resolve_dup_references(sdp, dt, &dt->ref_invinode_list,
				       &dh, 1, ref_types);
		revise_dup_handler(dup_blk, &dh);
	}
	/* Step 2 - eliminate reference from inodes that reference it as the
	 *          wrong type.  For example, a data file referencing it as
	 *          a data block, but it's really a metadata block.  Or a
	 *          directory inode referencing a data block as a leaf block.
	 */
	if (dh.ref_count > 1) {
		log_debug( _("----------------------------------------------\n"
			     "Step 2: Eliminate references to block %llu "
			     "(0x%llx) that need the wrong block type.\n"),
			   (unsigned long long)dt->block,
			   (unsigned long long)dt->block);
		resolve_dup_references(sdp, dt, &dt->ref_inode_list, &dh, 0,
				       acceptable_ref);
		revise_dup_handler(dup_blk, &dh);
	}
	/* Step 3 - We have multiple dinodes referencing it as the correct
	 *          type.  Just blast one of them.
	 *          All block types are fair game, so we use ref_types.
	 */
	if (dh.ref_count > 1) {
		log_debug( _("----------------------------------------------\n"
			     "Step 3: Choose one reference to block %llu "
			     "(0x%llx) to keep.\n"),
			   (unsigned long long)dt->block,
			   (unsigned long long)dt->block);
		resolve_dup_references(sdp, dt, &dt->ref_inode_list, &dh, 0,
				       ref_types);
		revise_dup_handler(dup_blk, &dh);
	}
	/* If there's still a last remaining reference, and it's a valid
	   reference, use it to determine the correct block type for our
	   blockmap and bitmap. */
	if (dh.ref_inode_count == 1 && !osi_list_empty(&dt->ref_inode_list)) {
		uint8_t q;

		log_notice( _("Block %llu (0x%llx) has only one remaining "
			      "valid inode referencing it.\n"),
			    (unsigned long long)dup_blk,
			    (unsigned long long)dup_blk);
		/* If we're down to a single reference (and not all references
		   deleted, which may be the case of an inode that has only
		   itself and a reference), we need to reset the block type
		   from invalid to data or metadata. Start at the first one
		   in the list, not the structure's place holder. */
		tmp = dt->ref_inode_list.next;
		id = osi_list_entry(tmp, struct inode_with_dups, list);
		log_debug( _("----------------------------------------------\n"
			     "Step 4. Set block type based on the remaining "
			     "reference in inode %lld (0x%llx).\n"),
			   (unsigned long long)id->block_no,
			   (unsigned long long)id->block_no);
		ip = fsck_load_inode(sdp, id->block_no);

		q = block_type(id->block_no);
		if (q == GFS2_BLKST_UNLINKED) {
			log_debug( _("The remaining reference inode %lld "
				     "(0x%llx) is marked invalid: Marking "
				     "the block as free.\n"),
				   (unsigned long long)id->block_no,
				   (unsigned long long)id->block_no);
			fsck_blockmap_set(ip, dt->block,
					  _("reference-repaired leaf"),
					  GFS2_BLKST_FREE);
		} else if (id->reftypecount[ref_is_inode]) {
			set_ip_blockmap(ip, 0); /* 0=do not add to dirtree */
		} else if (id->reftypecount[ref_as_data]) {
			fsck_blockmap_set(ip, dt->block,
					  _("reference-repaired data"),
					  GFS2_BLKST_USED);
		} else if (id->reftypecount[ref_as_meta]) {
			if (is_dir(&ip->i_di, sdp->gfs1))
				fsck_blockmap_set(ip, dt->block,
						  _("reference-repaired leaf"),
						  sdp->gfs1 ?
						  GFS2_BLKST_DINODE :
						  GFS2_BLKST_USED);
			else
				fsck_blockmap_set(ip, dt->block,
						  _("reference-repaired "
						    "indirect"), sdp->gfs1 ?
						  GFS2_BLKST_DINODE :
						  GFS2_BLKST_USED);
		} else
			fsck_blockmap_set(ip, dt->block,
					  _("reference-repaired extended "
					    "attribute"),
					  sdp->gfs1 ? GFS2_BLKST_DINODE :
					  GFS2_BLKST_USED);
		fsck_inode_put(&ip); /* out, brelse, free */
		log_debug(_("Done with duplicate reference to block 0x%llx\n"),
			  (unsigned long long)dt->block);
		dup_delete(dt);
	} else {
		/* They may have answered no and not fixed all references. */
		log_debug( _("All duplicate references to block 0x%llx were "
			     "processed.\n"), (unsigned long long)dup_blk);
		if (dh.ref_count) {
			log_debug(_("Done with duplicate reference to block "
				    "0x%llx, but %d references remain.\n"),
				  (unsigned long long)dup_blk, dh.ref_count);
		} else {
			log_notice( _("Block %llu (0x%llx) has no more "
				      "references; Marking as 'free'.\n"),
				    (unsigned long long)dup_blk,
				    (unsigned long long)dup_blk);
			if (dh.dt)
				dup_delete(dh.dt);
			/* Now fix the block type of the block in question. */
			gfs2_blockmap_set(bl, dup_blk, GFS2_BLKST_FREE);
			check_n_fix_bitmap(sdp, dup_blk, 0, GFS2_BLKST_FREE);
		}
	}
	return 0;
}

static int check_leaf_refs(struct gfs2_inode *ip, uint64_t block,
			   void *private)
{
	return add_duplicate_ref(ip, block, ref_as_meta, 1, INODE_VALID);
}

static int check_metalist_refs(struct gfs2_inode *ip, uint64_t block,
			       struct gfs2_buffer_head **bh, int h,
			       int *is_valid, int *was_duplicate,
			       void *private)
{
	*was_duplicate = 0;
	*is_valid = 1;
	return add_duplicate_ref(ip, block, ref_as_meta, 1, INODE_VALID);
}

static int check_data_refs(struct gfs2_inode *ip, uint64_t metablock,
			   uint64_t block, void *private)
{
	return add_duplicate_ref(ip, block, ref_as_data, 1, INODE_VALID);
}

static int check_eattr_indir_refs(struct gfs2_inode *ip, uint64_t block,
				  uint64_t parent,
				  struct gfs2_buffer_head **bh, void *private)
{
	struct gfs2_sbd *sdp = ip->i_sbd;
	int error;

	error = add_duplicate_ref(ip, block, ref_as_ea, 1, INODE_VALID);
	if (!error)
		*bh = bread(sdp, block);

	return error;
}

static int check_eattr_leaf_refs(struct gfs2_inode *ip, uint64_t block,
				 uint64_t parent, struct gfs2_buffer_head **bh,
				 void *private)
{
	struct gfs2_sbd *sdp = ip->i_sbd;
	int error;

	error = add_duplicate_ref(ip, block, ref_as_ea, 1, INODE_VALID);
	if (!error)
		*bh = bread(sdp, block);
	return error;
}

static int check_eattr_entry_refs(struct gfs2_inode *ip,
				  struct gfs2_buffer_head *leaf_bh,
				  struct gfs2_ea_header *ea_hdr,
				  struct gfs2_ea_header *ea_hdr_prev,
				  void *private)
{
	return 0;
}

static int check_eattr_extentry_refs(struct gfs2_inode *ip,
				     uint64_t *ea_data_ptr,
				     struct gfs2_buffer_head *leaf_bh,
				     struct gfs2_ea_header *ea_hdr,
				     struct gfs2_ea_header *ea_hdr_prev,
				     void *private)
{
	uint64_t block = be64_to_cpu(*ea_data_ptr);

	return add_duplicate_ref(ip, block, ref_as_ea, 1, INODE_VALID);
}

/* Finds all references to duplicate blocks in the metadata */
/* Finds all references to duplicate blocks in the metadata */
static int find_block_ref(struct gfs2_sbd *sdp, uint64_t inode)
{
	struct gfs2_inode *ip;
	int error = 0;
	struct metawalk_fxns find_refs = {
		.private = NULL,
		.check_leaf = check_leaf_refs,
		.check_metalist = check_metalist_refs,
		.check_data = check_data_refs,
		.check_eattr_indir = check_eattr_indir_refs,
		.check_eattr_leaf = check_eattr_leaf_refs,
		.check_eattr_entry = check_eattr_entry_refs,
		.check_eattr_extentry = check_eattr_extentry_refs,
	};

	ip = fsck_load_inode(sdp, inode); /* bread, inode_get */

	/* double-check the meta header just to be sure it's metadata */
	if (ip->i_di.di_header.mh_magic != GFS2_MAGIC ||
	    ip->i_di.di_header.mh_type != GFS2_METATYPE_DI) {
		log_debug( _("Block %lld (0x%llx) is not gfs2 metadata.\n"),
			     (unsigned long long)inode,
			     (unsigned long long)inode);
		error = 1;
		goto out;
	}
	/* Check to see if this inode was referenced by another by mistake */
	add_duplicate_ref(ip, inode, ref_is_inode, 1, INODE_VALID);

	/* Check this dinode's metadata for references to known duplicates */
	error = check_metatree(ip, &find_refs);
	if (error < 0)
		stack;

	/* Check for ea references in the inode */
	if (!error)
		error = check_inode_eattr(ip, &find_refs);

out:
	fsck_inode_put(&ip); /* out, brelse, free */
	return error;
}

/* Pass 1b handles finding the previous inode for a duplicate block
 * When found, store the inodes pointing to the duplicate block for
 * use in pass2 */
int pass1b(struct gfs2_sbd *sdp)
{
	struct duptree *dt;
	uint64_t i;
	uint8_t q;
	struct osi_node *n;
	int rc = FSCK_OK;

	log_info( _("Looking for duplicate blocks...\n"));

	/* If there were no dups in the bitmap, we don't need to do anymore */
	if (dup_blocks.osi_node == NULL) {
		log_info( _("No duplicate blocks found\n"));
		return FSCK_OK;
	}

	/* Rescan the fs looking for pointers to blocks that are in
	 * the duplicate block map */
	log_info( _("Scanning filesystem for inodes containing duplicate blocks...\n"));
	log_debug( _("Filesystem has %llu (0x%llx) blocks total\n"),
		  (unsigned long long)last_fs_block,
		  (unsigned long long)last_fs_block);
	for (i = 0; i < last_fs_block; i++) {
		if (skip_this_pass || fsck_abort) /* if asked to skip the rest */
			goto out;

		if (dups_found_first == dups_found) {
			log_debug(_("Found all %d original references to "
				    "duplicates.\n"), dups_found);
			break;
		}
		q = block_type(i);

		if (q == GFS2_BLKST_FREE || q == GFS2_BLKST_USED)
			continue;

		if (q == GFS2_BLKST_UNLINKED)
			log_debug( _("Checking invalidated duplicate block "
				     "%lld (0x%llx)\n"),
				   (unsigned long long)i,
				   (unsigned long long)i);

		warm_fuzzy_stuff(i);
		if (find_block_ref(sdp, i) < 0) {
			stack;
			rc = FSCK_ERROR;
			goto out;
		}
	}

	/* Fix dups here - it's going to slow things down a lot to fix
	 * it later */
	log_info( _("Handling duplicate blocks\n"));
out:
	/* Resolve all duplicates by clearing out the dup tree */
        while ((n = osi_first(&dup_blocks))) {
                dt = (struct duptree *)n;
		if (!skip_this_pass && !rc) /* no error & not asked to skip the rest */
			handle_dup_blk(sdp, dt);
	}
	return rc;
}
