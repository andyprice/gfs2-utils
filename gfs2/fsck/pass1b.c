#include "clusterautoconfig.h"

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <libintl.h>
#include <sys/stat.h>
#define _(String) gettext(String)

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
	struct duptree *b;
	struct inode_with_dups *id;
	int ref_inode_count;
	int ref_count;
};

static int check_metalist(struct gfs2_inode *ip, uint64_t block,
			  struct gfs2_buffer_head **bh, int h, void *private);
static int check_data(struct gfs2_inode *ip, uint64_t block, void *private);
static int check_eattr_indir(struct gfs2_inode *ip, uint64_t block,
			     uint64_t parent, struct gfs2_buffer_head **bh,
			     void *private);
static int check_eattr_leaf(struct gfs2_inode *ip, uint64_t block,
			    uint64_t parent, struct gfs2_buffer_head **bh,
			    void *private);
static int check_eattr_entry(struct gfs2_inode *ip,
			     struct gfs2_buffer_head *leaf_bh,
			     struct gfs2_ea_header *ea_hdr,
			     struct gfs2_ea_header *ea_hdr_prev,
			     void *private);
static int check_eattr_extentry(struct gfs2_inode *ip, uint64_t *ea_data_ptr,
				struct gfs2_buffer_head *leaf_bh,
				struct gfs2_ea_header *ea_hdr,
				struct gfs2_ea_header *ea_hdr_prev,
				void *private);
static int find_dentry(struct gfs2_inode *ip, struct gfs2_dirent *de,
		       struct gfs2_dirent *prev, struct gfs2_buffer_head *bh,
		       char *filename, uint16_t *count, void *priv);

struct metawalk_fxns find_refs = {
	.private = NULL,
	.check_leaf = NULL,
	.check_metalist = check_metalist,
	.check_data = check_data,
	.check_eattr_indir = check_eattr_indir,
	.check_eattr_leaf = check_eattr_leaf,
	.check_dentry = NULL,
	.check_eattr_entry = check_eattr_entry,
	.check_eattr_extentry = check_eattr_extentry,
};

struct metawalk_fxns find_dirents = {
	.private = NULL,
	.check_leaf = NULL,
	.check_metalist = NULL,
	.check_data = NULL,
	.check_eattr_indir = NULL,
	.check_eattr_leaf = NULL,
	.check_dentry = find_dentry,
	.check_eattr_entry = NULL,
	.check_eattr_extentry = NULL,
};

static int check_metalist(struct gfs2_inode *ip, uint64_t block,
			  struct gfs2_buffer_head **bh, int h, void *private)
{
	return add_duplicate_ref(ip, block, ref_as_meta, 1, INODE_VALID);
}

static int check_data(struct gfs2_inode *ip, uint64_t block, void *private)
{
	return add_duplicate_ref(ip, block, ref_as_data, 1, INODE_VALID);
}

static int check_eattr_indir(struct gfs2_inode *ip, uint64_t block,
			     uint64_t parent, struct gfs2_buffer_head **bh,
			     void *private)
{
	struct gfs2_sbd *sbp = ip->i_sbd;
	int error;

	error = add_duplicate_ref(ip, block, ref_as_ea, 1, INODE_VALID);
	if (!error)
		*bh = bread(sbp, block);

	return error;
}

static int check_eattr_leaf(struct gfs2_inode *ip, uint64_t block,
			    uint64_t parent, struct gfs2_buffer_head **bh,
			    void *private)
{
	struct gfs2_sbd *sbp = ip->i_sbd;
	int error;

	error = add_duplicate_ref(ip, block, ref_as_ea, 1, INODE_VALID);
	if (!error)
		*bh = bread(sbp, block);
	return error;
}

static int check_eattr_entry(struct gfs2_inode *ip,
			     struct gfs2_buffer_head *leaf_bh,
			     struct gfs2_ea_header *ea_hdr,
			     struct gfs2_ea_header *ea_hdr_prev, void *private)
{
	return 0;
}

static int check_eattr_extentry(struct gfs2_inode *ip, uint64_t *ea_data_ptr,
				struct gfs2_buffer_head *leaf_bh,
				struct gfs2_ea_header *ea_hdr,
				struct gfs2_ea_header *ea_hdr_prev,
				void *private)
{
	uint64_t block = be64_to_cpu(*ea_data_ptr);

	return add_duplicate_ref(ip, block, ref_as_ea, 1, INODE_VALID);
}

/*
 * check_dir_dup_ref - check for a directory entry duplicate reference
 *                     and if found, set the name into the id.
 * Returns: 1 if filename was found, otherwise 0
 */
static int check_dir_dup_ref(struct gfs2_inode *ip,  struct gfs2_dirent *de,
			     osi_list_t *tmp2, char *filename)
{
	struct inode_with_dups *id;

	id = osi_list_entry(tmp2, struct inode_with_dups, list);
	if(id->name)
		/* We can only have one parent of inodes that contain duplicate
		 * blocks...no need to keep looking for this one. */
		return 1;
	if(id->block_no == de->de_inum.no_addr) {
		id->name = strdup(filename);
		id->parent = ip->i_di.di_num.no_addr;
		log_debug( _("Duplicate block %llu (0x%llx"
			     ") is in file or directory %llu"
			     " (0x%llx) named %s\n"),
			   (unsigned long long)id->block_no,
			   (unsigned long long)id->block_no,
			   (unsigned long long)ip->i_di.di_num.no_addr,
			   (unsigned long long)ip->i_di.di_num.no_addr,
			   filename);
		/* If there are duplicates of duplicates, I guess we'll miss
		   them here. */
		return 1;
	}
	return 0;
}

static int find_dentry(struct gfs2_inode *ip, struct gfs2_dirent *de,
		       struct gfs2_dirent *prev,
		       struct gfs2_buffer_head *bh, char *filename,
		       uint16_t *count, void *priv)
{
	struct osi_node *n, *next = NULL;
	osi_list_t *tmp2;
	struct duptree *b;
	int found;

	for (n = osi_first(&dup_blocks); n; n = next) {
		next = osi_next(n);
		b = (struct duptree *)n;
		found = 0;
		osi_list_foreach(tmp2, &b->ref_invinode_list) {
			if (check_dir_dup_ref(ip, de, tmp2, filename)) {
				found = 1;
				break;
			}
		}
		if (!found) {
			osi_list_foreach(tmp2, &b->ref_inode_list) {
				if (check_dir_dup_ref(ip, de, tmp2, filename))
					break;
			}
		}
	}
	/* Return the number of leaf entries so metawalk doesn't flag this
	   leaf as having none. */
	*count = be16_to_cpu(((struct gfs2_leaf *)bh->b_data)->lf_entries);
	return 0;
}

static int clear_dup_metalist(struct gfs2_inode *ip, uint64_t block,
			      struct gfs2_buffer_head **bh, int h,
			      void *private)
{
	struct dup_handler *dh = (struct dup_handler *) private;
	struct duptree *d;

	if (gfs2_check_range(ip->i_sbd, block) != 0)
		return 0;

	/* This gets tricky. We're traversing a metadata tree trying to
	   delete an inode based on it having a duplicate block reference
	   somewhere in its metadata.  We know this block is listed as data
	   or metadata for this inode, but it may or may not be one of the
	   actual duplicate references that caused the problem.  If it's not
	   a duplicate, it's normal metadata that isn't referenced anywhere
	   else, but we're deleting the inode out from under it, so we need
	   to delete it altogether. If the block is a duplicate referenced
	   block, we need to keep its type intact and let the caller sort
	   it out once we're down to a single reference. */
	d = dupfind(block);
	if (!d) {
		fsck_blockmap_set(ip, block, _("no longer valid"),
				  gfs2_block_free);
		return 0;
	}
	/* This block, having failed the above test, is duplicated somewhere */
	if(block == dh->b->block) {
		log_err( _("Not clearing duplicate reference in inode \"%s\" "
			   "at block #%llu (0x%llx) to block #%llu (0x%llx) "
			   "because it's valid for another inode.\n"),
			 dh->id->name ? dh->id->name : _("unknown name"),
			 (unsigned long long)ip->i_di.di_num.no_addr,
			 (unsigned long long)ip->i_di.di_num.no_addr,
			 (unsigned long long)block, (unsigned long long)block);
		log_err( _("Inode %s is in directory %llu (0x%llx)\n"),
			 dh->id->name ? dh->id->name : "",
			 (unsigned long long)dh->id->parent,
			 (unsigned long long)dh->id->parent);
	}
	/* We return 1 not 0 because we need build_and_check_metalist to
	   bypass adding the metadata below it to the metalist.  If that
	   were to happen, all the indirect blocks pointed to by the
	   duplicate block would be processed twice, which means it might
	   be mistakenly freed as "no longer valid" (in this function above)
	   even though it's valid metadata for a different inode. Returning
	   1 ensures that the metadata isn't processed again. */
	return 1;
}

static int clear_dup_data(struct gfs2_inode *ip, uint64_t block, void *private)
{
	return clear_dup_metalist(ip, block, NULL, 0, private);
}

static int clear_dup_eattr_indir(struct gfs2_inode *ip, uint64_t block,
				 uint64_t parent, struct gfs2_buffer_head **bh,
				 void *private)
{
	return clear_dup_metalist(ip, block, NULL, 0, private);
}

static int clear_dup_eattr_leaf(struct gfs2_inode *ip, uint64_t block,
				uint64_t parent, struct gfs2_buffer_head **bh,
				void *private)
{
	return clear_dup_metalist(ip, block, NULL, 0, private);
}

static int clear_eattr_entry (struct gfs2_inode *ip,
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
		max_ptrs = (be32_to_cpu(ea_hdr->ea_data_len) + avail_size - 1) /
			avail_size;

		if(max_ptrs > ea_hdr->ea_num_ptrs)
			return 1;
		else {
			log_debug( _("  Pointers Required: %d\n  Pointers Reported: %d\n"),
					  max_ptrs, ea_hdr->ea_num_ptrs);
		}
	}
	return 0;
}

static int clear_eattr_extentry(struct gfs2_inode *ip, uint64_t *ea_data_ptr,
				struct gfs2_buffer_head *leaf_bh,
				struct gfs2_ea_header *ea_hdr,
				struct gfs2_ea_header *ea_hdr_prev,
				void *private)
{
	uint64_t block = be64_to_cpu(*ea_data_ptr);

	return clear_dup_metalist(ip, block, NULL, 0, private);
}

/* Finds all references to duplicate blocks in the metadata */
static int find_block_ref(struct gfs2_sbd *sbp, uint64_t inode)
{
	struct gfs2_inode *ip;
	int error = 0;

	ip = fsck_load_inode(sbp, inode); /* bread, inode_get */
	/* double-check the meta header just to be sure it's metadata */
	if (ip->i_di.di_header.mh_magic != GFS2_MAGIC ||
	    ip->i_di.di_header.mh_type != GFS2_METATYPE_DI) {
		log_debug( _("Block %lld (0x%llx) is not gfs2 metadata.\n"),
			     (unsigned long long)inode,
			     (unsigned long long)inode);
		return 1;
	}
	error = check_metatree(ip, &find_refs);
	if (error < 0) {
		stack;
		fsck_inode_put(&ip); /* out, brelse, free */
		return error;
	}

	/* Exhash dir leafs will be checked by check_metatree (right after
	   the "end:" label.)  But if this is a linear directory we need to
	   check the dir with check_linear_dir. */
	if(S_ISDIR(ip->i_di.di_mode) && !(ip->i_di.di_flags & GFS2_DIF_EXHASH))
		error = check_linear_dir(ip, ip->i_bh, &find_dirents);

	/* Check for ea references in the inode */
	if(!error)
		error = check_inode_eattr(ip, &find_refs);

	fsck_inode_put(&ip); /* out, brelse, free */

	return error;
}

static void log_inode_reference(struct duptree *b, osi_list_t *tmp, int inval)
{
	char reftypestring[32];
	struct inode_with_dups *id;

	id = osi_list_entry(tmp, struct inode_with_dups, list);
	if (id->dup_count == 1) {
		if (id->reftypecount[ref_as_data])
			strcpy(reftypestring, "as data");
		else if (id->reftypecount[ref_as_meta])
			strcpy(reftypestring, "as metadata");
		else
			strcpy(reftypestring, "as extended attribute");
	} else {
		sprintf(reftypestring, "%d/%d/%d",
			id->reftypecount[ref_as_data],
			id->reftypecount[ref_as_meta],
			id->reftypecount[ref_as_ea]);
	}
	if (inval)
		log_warn( _("Invalid "));
	log_warn( _("Inode %s (%lld/0x%llx) has %d reference(s) to "
		    "block %llu (0x%llx) (%s)\n"), id->name,
		  (unsigned long long)id->block_no,
		  (unsigned long long)id->block_no, id->dup_count,
		  (unsigned long long)b->block,
		  (unsigned long long)b->block, reftypestring);
}

static int clear_a_reference(struct gfs2_sbd *sbp, struct duptree *b,
			     osi_list_t *ref_list, struct dup_handler *dh,
			     int inval)
{
	struct gfs2_inode *ip;
	struct inode_with_dups *id;
	osi_list_t *tmp, *x;
	struct metawalk_fxns clear_dup_fxns = {
		.private = NULL,
		.check_leaf = NULL,
		.check_metalist = clear_dup_metalist,
		.check_data = clear_dup_data,
		.check_eattr_indir = clear_dup_eattr_indir,
		.check_eattr_leaf = clear_dup_eattr_leaf,
		.check_dentry = NULL,
		.check_eattr_entry = clear_eattr_entry,
		.check_eattr_extentry = clear_eattr_extentry,
	};

	osi_list_foreach_safe(tmp, ref_list, x) {
		id = osi_list_entry(tmp, struct inode_with_dups, list);
		dh->b = b;
		dh->id = id;
		if(dh->ref_inode_count == 1) /* down to the last reference */
			return 1;
		if (!(query( _("Okay to clear %s inode %lld (0x%llx)? (y/n) "),
			     (inval ? _("invalidated") : ""),
			     (unsigned long long)id->block_no,
			     (unsigned long long)id->block_no))) {
			log_warn( _("The bad inode was not cleared...\n"));
			continue;
		}
		log_warn( _("Clearing inode %lld (0x%llx)....\n"),
			  (unsigned long long)id->block_no,
			  (unsigned long long)id->block_no);
		clear_dup_fxns.private = (void *) dh;
		/* Clear the EAs for the inode first */
		ip = fsck_load_inode(sbp, id->block_no);
		check_inode_eattr(ip, &clear_dup_fxns);
		/* If the dup wasn't only in the EA, clear the inode */
		if (id->reftypecount[ref_as_data] ||
		    id->reftypecount[ref_as_meta])
			check_metatree(ip, &clear_dup_fxns);

		fsck_blockmap_set(ip, ip->i_di.di_num.no_addr,
				  _("bad"), gfs2_inode_invalid);
		fsck_inode_put(&ip); /* out, brelse, free */
		(dh->ref_inode_count)--;
		/* Inode is marked invalid and is removed in pass2 */
		/* FIXME: other option should be to duplicate the
		 * block for each duplicate and point the metadata at
		 * the cloned blocks */
	}
	if(dh->ref_inode_count == 1) /* down to the last reference */
		return 1;
	return 0;
}

static int handle_dup_blk(struct gfs2_sbd *sbp, struct duptree *b)
{
	struct gfs2_inode *ip;
	osi_list_t *tmp;
	struct inode_with_dups *id;
	struct dup_handler dh = {0};
	int last_reference, ref_in_invalid_inode = 0;

	osi_list_foreach(tmp, &b->ref_invinode_list) {
		id = osi_list_entry(tmp, struct inode_with_dups, list);
		dh.ref_inode_count++;
		dh.ref_count += id->dup_count;
		ref_in_invalid_inode = 1;
	}
	osi_list_foreach(tmp, &b->ref_inode_list) {
		id = osi_list_entry(tmp, struct inode_with_dups, list);
		dh.ref_inode_count++;
		dh.ref_count += id->dup_count;
	}
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
	if (dh.ref_count == 1) {
		struct gfs2_buffer_head *bh;
		uint32_t cmagic;

		bh = bread(sbp, b->block);
		cmagic = ((struct gfs2_meta_header *)(bh->b_data))->mh_magic;
		brelse(bh);
		if (be32_to_cpu(cmagic) == GFS2_MAGIC) {
			if (ref_in_invalid_inode)
				tmp = b->ref_invinode_list.next;
			else
				tmp = b->ref_inode_list.next;
			id = osi_list_entry(tmp, struct inode_with_dups, list);
			log_warn( _("Inode %s (%lld/0x%llx) has a reference to"
				    " data block %llu (0x%llx), "
				    "but the block is really metadata.\n"),
				  id->name, (unsigned long long)id->block_no,
				  (unsigned long long)id->block_no,
				  (unsigned long long)b->block,
				  (unsigned long long)b->block);
			if (query( _("Clear the inode? (y/n) "))) {
				struct inode_info *ii;

				log_warn( _("Clearing inode %lld (0x%llx)...\n"),
					 (unsigned long long)id->block_no,
					 (unsigned long long)id->block_no);
				ip = fsck_load_inode(sbp, id->block_no);
				ii = inodetree_find(ip->i_di.di_num.no_addr);
				if (ii)
					inodetree_delete(ii);
				/* Setting the block to invalid means the inode
				   is cleared in pass2 */
				fsck_blockmap_set(ip, ip->i_di.di_num.no_addr,
						 _("inode with bad duplicate"),
						 gfs2_inode_invalid);
				fsck_inode_put(&ip);
			} else {
				log_warn( _("The bad inode was not cleared."));
			}
			return 0;
		}
		/* The other references may have been discredited due to
		   invalid metadata or something.  Use the last remaining. */
		log_notice( _("Block %llu (0x%llx) has only one remaining "
			      "reference.\n"),
			    (unsigned long long)b->block,
			    (unsigned long long)b->block);
		return 0;
	}

	log_notice( _("Block %llu (0x%llx) has %d inodes referencing it"
		   " for a total of %d duplicate references\n"),
		   (unsigned long long)b->block, (unsigned long long)b->block,
		   dh.ref_inode_count, dh.ref_count);

	osi_list_foreach(tmp, &b->ref_invinode_list)
		log_inode_reference(b, tmp, 1);
	osi_list_foreach(tmp, &b->ref_inode_list)
		log_inode_reference(b, tmp, 0);

	last_reference = clear_a_reference(sbp, b, &b->ref_invinode_list,
					   &dh, 1);
	if (!last_reference)
		last_reference = clear_a_reference(sbp, b, &b->ref_inode_list,
						   &dh, 0);

	if (last_reference && !osi_list_empty(&b->ref_inode_list)) {
		uint8_t q;

		/* If we're down to a single reference (and not all references
		   deleted, which may be the case of an inode that has only
		   itself and a reference), we need to reset the block type
		   from invalid to data or metadata. Start at the first one
		   in the list, not the structure's place holder. */
		tmp = (&b->ref_inode_list)->next;
		id = osi_list_entry(tmp, struct inode_with_dups, list);
		log_debug( _("Resetting the type based on the remaining "
			     "reference in inode %lld (0x%llx).\n"),
			   (unsigned long long)id->block_no,
			   (unsigned long long)id->block_no);
		ip = fsck_load_inode(sbp, id->block_no);

		q = block_type(id->block_no);
		if (q == gfs2_inode_invalid) {
			log_debug( _("The remaining reference inode %lld "
				     "(0x%llx) is marked invalid: Marking "
				     "the block as free.\n"),
				   (unsigned long long)id->block_no,
				   (unsigned long long)id->block_no);
			fsck_blockmap_set(ip, b->block,
					  _("reference-repaired leaf"),
					  gfs2_block_free);
		} else if (id->reftypecount[ref_as_data]) {
			fsck_blockmap_set(ip, b->block,
					  _("reference-repaired data"),
					  gfs2_block_used);
		} else if (id->reftypecount[ref_as_meta]) {
			if (S_ISDIR(ip->i_di.di_mode))
				fsck_blockmap_set(ip, b->block,
						  _("reference-repaired leaf"),
						  gfs2_leaf_blk);
			else
				fsck_blockmap_set(ip, b->block,
						  _("reference-repaired "
						    "indirect"),
						  gfs2_indir_blk);
		} else
			fsck_blockmap_set(ip, b->block,
					  _("reference-repaired extended "
					    "attribute"),
					  gfs2_meta_eattr);
		fsck_inode_put(&ip); /* out, brelse, free */
	} else {
		log_debug( _("All duplicate references were resolved.\n"));
	}
	return 0;
}

/* Pass 1b handles finding the previous inode for a duplicate block
 * When found, store the inodes pointing to the duplicate block for
 * use in pass2 */
int pass1b(struct gfs2_sbd *sbp)
{
	struct duptree *b;
	uint64_t i;
	uint8_t q;
	struct osi_node *n, *next = NULL;
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
	for(i = 0; i < last_fs_block; i++) {
		if (skip_this_pass || fsck_abort) /* if asked to skip the rest */
			goto out;

		if (dups_found_first == dups_found) {
			log_debug(_("Found all %d original references to "
				    "duplicates.\n"), dups_found);
			break;
		}
		q = block_type(i);

		if (q < gfs2_inode_dir)
			continue;
		if (q > gfs2_inode_invalid)
			continue;

		if (q == gfs2_inode_invalid)
			log_debug( _("Checking invalidated duplicate dinode "
				     "%lld (0x%llx)\n"),
				   (unsigned long long)i,
				   (unsigned long long)i);

		warm_fuzzy_stuff(i);
		if (find_block_ref(sbp, i) < 0) {
			stack;
			rc = FSCK_ERROR;
			goto out;
		}
	}

	/* Fix dups here - it's going to slow things down a lot to fix
	 * it later */
	log_info( _("Handling duplicate blocks\n"));
out:
        for (n = osi_first(&dup_blocks); n; n = next) {
		next = osi_next(n);
                b = (struct duptree *)n;
		if (!skip_this_pass && !rc) /* no error & not asked to skip the rest */
			handle_dup_blk(sbp, b);
		/* Do not attempt to free the dup_blocks list or its parts
		   here because any func that calls check_metatree needs
		   to check duplicate status based on this linked list.
		   This is especially true for pass2 where it may delete "bad"
		   inodes, and we can't delete an inode's indirect block if
		   it was a duplicate (therefore in use by another dinode). */
	}
	return rc;
}
