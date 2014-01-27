#include "clusterautoconfig.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <sys/stat.h>
#include <libintl.h>
#define _(String) gettext(String)

#include "libgfs2.h"
#include "fsck.h"
#include "util.h"
#include "metawalk.h"
#include "link.h"
#include "lost_n_found.h"
#include "inode_hash.h"

#define MAX_FILENAME 256

struct metawalk_fxns pass2_fxns;

struct metawalk_fxns delete_eattrs = {
	.check_eattr_indir = delete_eattr_indir,
	.check_eattr_leaf = delete_eattr_leaf,
	.check_eattr_entry = delete_eattr_entry,
	.check_eattr_extentry = delete_eattr_extentry,
};

/* Set children's parent inode in dir_info structure - ext2 does not set
 * dotdot inode here, but instead in pass3 - should we? */
static int set_parent_dir(struct gfs2_sbd *sdp, struct gfs2_inum child,
			  struct gfs2_inum parent)
{
	struct dir_info *di;

	di = dirtree_find(child.no_addr);
	if (!di) {
		log_err( _("Unable to find block %llu (0x%llx"
			   ") in dir_info list\n"),
			(unsigned long long)child.no_addr,
			(unsigned long long)child.no_addr);
		return -1;
	}

	if (di->dinode.no_addr == child.no_addr &&
	    di->dinode.no_formal_ino == child.no_formal_ino) {
		if (di->treewalk_parent) {
			log_err( _("Another directory at block %lld (0x%llx) "
				   "already contains this child %lld (0x%llx)"
				   " - checking parent %lld (0x%llx)\n"),
				 (unsigned long long)di->treewalk_parent,
				 (unsigned long long)di->treewalk_parent,
				 (unsigned long long)child.no_addr,
				 (unsigned long long)child.no_addr,
				 (unsigned long long)parent.no_addr,
				 (unsigned long long)parent.no_addr);
			return 1;
		}
		log_debug( _("Child %lld (0x%llx) has parent %lld (0x%llx)\n"),
			   (unsigned long long)child.no_addr,
			   (unsigned long long)child.no_addr,
			   (unsigned long long)parent.no_addr,
			   (unsigned long long)parent.no_addr);
		di->treewalk_parent = parent.no_addr;
	}

	return 0;
}

/* Set's the child's '..' directory inode number in dir_info structure */
static int set_dotdot_dir(struct gfs2_sbd *sdp, uint64_t childblock,
			  struct gfs2_inum parent)
{
	struct dir_info *di;

	di = dirtree_find(childblock);
	if (!di) {
		log_err( _("Unable to find block %"PRIu64" (0x%" PRIx64
			   ") in dir_info tree\n"), childblock, childblock);
		return -1;
	}
	if (di->dinode.no_addr != childblock) {
		log_debug("'..' doesn't point to what we found: childblock "
			  "(0x%llx) != dinode (0x%llx)\n",
			  (unsigned long long)childblock,
			  (unsigned long long)di->dinode.no_addr);
		return -1;
	}
	/* Special case for root inode because we set it earlier */
	if (di->dotdot_parent.no_addr &&
	    sdp->md.rooti->i_di.di_num.no_addr != di->dinode.no_addr) {
		/* This should never happen */
		log_crit( _("Dotdot parent already set for block %llu (0x%llx)"
			    "-> %llu (0x%llx)\n"),
			  (unsigned long long)childblock,
			  (unsigned long long)childblock,
			  (unsigned long long)di->dotdot_parent.no_addr,
			  (unsigned long long)di->dotdot_parent.no_addr);
		return -1;
	}
	log_debug("Setting '..' for directory block (0x%llx) to parent "
		  "(0x%llx)\n", (unsigned long long)childblock,
		  (unsigned long long)parent.no_addr);
	di->dotdot_parent.no_addr = parent.no_addr;
	di->dotdot_parent.no_formal_ino = parent.no_formal_ino;
	return 0;
}

static int check_eattr_indir(struct gfs2_inode *ip, uint64_t block,
			     uint64_t parent, struct gfs2_buffer_head **bh,
			     void *private)
{
	*bh = bread(ip->i_sbd, block);
	return 0;
}
static int check_eattr_leaf(struct gfs2_inode *ip, uint64_t block,
			    uint64_t parent, struct gfs2_buffer_head **bh,
			    void *private)
{
	*bh = bread(ip->i_sbd, block);
	return 0;
}

static const char *de_type_string(uint8_t de_type)
{
	const char *de_types[15] = {"unknown", "fifo", "chrdev", "invalid",
				    "directory", "invalid", "blkdev", "invalid",
				    "file", "invalid", "symlink", "invalid",
				    "socket", "invalid", "wht"};
	if (de_type < 15)
		return de_types[de_type];
	return de_types[3]; /* invalid */
}

static int check_file_type(uint8_t de_type, uint8_t blk_type, int gfs1)
{
	switch(blk_type) {
	case gfs2_inode_dir:
		if (de_type != (gfs1 ? GFS_FILE_DIR : DT_DIR))
			return 1;
		break;
	case gfs2_inode_file:
		if (de_type != (gfs1 ? GFS_FILE_REG : DT_REG))
			return 1;
		break;
	case gfs2_inode_lnk:
		if (de_type != (gfs1 ? GFS_FILE_LNK : DT_LNK))
			return 1;
		break;
	case gfs2_inode_device:
		if ((de_type != (gfs1 ? GFS_FILE_BLK : DT_BLK)) &&
		    (de_type != (gfs1 ? GFS_FILE_CHR : DT_CHR)))
			return 1;
		break;
	case gfs2_inode_fifo:
		if (de_type != (gfs1 ? GFS_FILE_FIFO : DT_FIFO))
			return 1;
		break;
	case gfs2_inode_sock:
		if (de_type != (gfs1 ? GFS_FILE_SOCK : DT_SOCK))
			return 1;
		break;
	default:
		log_err( _("Invalid block type\n"));
		return -1;
		break;
	}
	return 0;
}

struct metawalk_fxns pass2_fxns_delete = {
	.private = NULL,
	.check_metalist = delete_metadata,
	.check_data = delete_data,
	.check_leaf = delete_leaf,
	.check_eattr_indir = delete_eattr_indir,
	.check_eattr_leaf = delete_eattr_leaf,
	.check_eattr_entry = delete_eattr_entry,
	.check_eattr_extentry = delete_eattr_extentry,
};

/* bad_formal_ino - handle mismatches in formal inode number
 * Returns: 0 if the dirent was repaired
 *          1 if the caller should delete the dirent
 */
static int bad_formal_ino(struct gfs2_inode *ip, struct gfs2_dirent *dent,
			  struct gfs2_inum entry, const char *tmp_name,
			  uint8_t q, struct gfs2_dirent *de,
			  struct gfs2_buffer_head *bh)
{
	struct inode_info *ii;
	struct gfs2_inode *child_ip;
	struct gfs2_inum childs_dotdot;
	struct gfs2_sbd *sdp = ip->i_sbd;
	int error;

	ii = inodetree_find(entry.no_addr);
	log_err( _("Directory entry '%s' pointing to block %llu (0x%llx) in "
		   "directory %llu (0x%llx) has the wrong 'formal' inode "
		   "number.\n"), tmp_name, (unsigned long long)entry.no_addr,
		 (unsigned long long)entry.no_addr,
		 (unsigned long long)ip->i_di.di_num.no_addr,
		 (unsigned long long)ip->i_di.di_num.no_addr);
	log_err( _("The directory entry has %llu (0x%llx) but the inode has "
		   "%llu (0x%llx)\n"), (unsigned long long)entry.no_formal_ino,
		 (unsigned long long)entry.no_formal_ino,
		 (unsigned long long)ii->di_num.no_formal_ino,
		 (unsigned long long)ii->di_num.no_formal_ino);
	if (q != gfs2_inode_dir || !strcmp("..", tmp_name)) {
		if (query( _("Remove the corrupt directory entry? (y/n) ")))
			return 1;
		log_err( _("Corrupt directory entry not removed.\n"));
		return 0;
	}
	/* We have a directory pointing to another directory, but the
	   formal inode number still doesn't match. If that directory
	   has a '..' pointing back, just fix up the no_formal_ino. */
	child_ip = lgfs2_inode_read(sdp, entry.no_addr);
	error = dir_search(child_ip, "..", 2, NULL, &childs_dotdot);
	if (!error && childs_dotdot.no_addr == ip->i_di.di_num.no_addr) {
		log_err( _("The entry points to another directory with intact "
			   "linkage.\n"));
		if (query( _("Fix the bad directory entry? (y/n) "))) {
			log_err( _("Fixing the corrupt directory entry.\n"));
			entry.no_formal_ino = ii->di_num.no_formal_ino;
			de->de_inum.no_formal_ino = entry.no_formal_ino;
			gfs2_dirent_out(de, (char *)dent);
			bmodified(bh);
			incr_link_count(entry, ip, _("fixed reference"));
			set_parent_dir(sdp, entry, ip->i_di.di_num);
		} else {
			log_err( _("Directory entry not fixed.\n"));
		}
	} else {
		if (query( _("Remove the corrupt directory entry? (y/n) "))) {
			inode_put(&child_ip);
			return 1;
		}
		log_err( _("Corrupt directory entry not removed.\n"));
	}
	inode_put(&child_ip);
	return 0;
}

static int hash_table_index(uint32_t hash, struct gfs2_inode *ip)
{
	return hash >> (32 - ip->i_di.di_depth);
}

static int hash_table_max(int lindex, struct gfs2_inode *ip,
		   struct gfs2_buffer_head *bh)
{
	struct gfs2_leaf *leaf = (struct gfs2_leaf *)bh->b_data;
	return (1 << (ip->i_di.di_depth - be16_to_cpu(leaf->lf_depth))) +
		lindex - 1;
}

static int check_leaf_depth(struct gfs2_inode *ip, uint64_t leaf_no,
			    int ref_count, struct gfs2_buffer_head *lbh)
{
	struct gfs2_leaf *leaf = (struct gfs2_leaf *)lbh->b_data;
	int cur_depth = be16_to_cpu(leaf->lf_depth);
	int exp_count = 1 << (ip->i_di.di_depth - cur_depth);
	int divisor;
	int factor, correct_depth;

	if (exp_count == ref_count)
		return 0;

	factor = 0;
	divisor = ref_count;
	while (divisor > 1) {
		factor++;
		divisor >>= 1;
	}
	if (ip->i_di.di_depth < factor) /* can't be fixed--leaf must be on the
					   wrong dinode. */
		return -1;
	correct_depth = ip->i_di.di_depth - factor;
	if (cur_depth == correct_depth)
		return 0;

	log_err(_("Leaf block %llu (0x%llx) in dinode %llu (0x%llx) has the "
		  "wrong depth: is %d (length %d), should be %d (length "
		  "%d).\n"),
		(unsigned long long)leaf_no, (unsigned long long)leaf_no,
		(unsigned long long)ip->i_di.di_num.no_addr,
		(unsigned long long)ip->i_di.di_num.no_addr,
		cur_depth, ref_count, correct_depth, exp_count);
	if (!query( _("Fix the leaf block? (y/n)"))) {
		log_err( _("The leaf block was not fixed.\n"));
		return 0;
	}

	leaf->lf_depth = cpu_to_be16(correct_depth);
	bmodified(lbh);
	log_err( _("The leaf block depth was fixed.\n"));
	return 1;
}

/* wrong_leaf: Deal with a dirent discovered to be on the wrong leaf block
 *
 * Returns: 1 if the dirent is to be removed, 0 if it needs to be kept,
 *          or -1 on error
 */
static int wrong_leaf(struct gfs2_inode *ip, struct gfs2_inum *entry,
		      const char *tmp_name, int lindex, int lindex_max,
		      int hash_index, struct gfs2_buffer_head *bh,
		      struct dir_status *ds, struct gfs2_dirent *dent,
		      struct gfs2_dirent *de, struct gfs2_dirent *prev_de,
		      uint32_t *count, uint8_t q)
{
	struct gfs2_sbd *sdp = ip->i_sbd;
	struct gfs2_buffer_head *dest_lbh;
	uint64_t planned_leaf, real_leaf;
	int li, dest_ref, error;
	uint64_t *tbl;

	log_err(_("Directory entry '%s' at block %lld (0x%llx) is on the "
		  "wrong leaf block.\n"), tmp_name,
		(unsigned long long)entry->no_addr,
		(unsigned long long)entry->no_addr);
	log_err(_("Leaf index is: 0x%x. The range for this leaf block is "
		  "0x%x - 0x%x\n"), hash_index, lindex, lindex_max);
	if (!query( _("Move the misplaced directory entry to "
		      "a valid leaf block? (y/n) "))) {
		log_err( _("Misplaced directory entry not moved.\n"));
		return 0;
	}

	/* check the destination leaf block's depth */
	tbl = get_dir_hash(ip);
	if (tbl == NULL) {
		perror("get_dir_hash");
		return -1;
	}
	planned_leaf = be64_to_cpu(tbl[hash_index]);
	log_err(_("Moving it from leaf %llu (0x%llx) to %llu (0x%llx)\n"),
		(unsigned long long)be64_to_cpu(tbl[lindex]),
		(unsigned long long)be64_to_cpu(tbl[lindex]),
		(unsigned long long)planned_leaf,
		(unsigned long long)planned_leaf);
	/* Can't trust lf_depth; we have to count */
	dest_ref = 0;
	for (li = 0; li < (1 << ip->i_di.di_depth); li++) {
		if (be64_to_cpu(tbl[li]) == planned_leaf)
			dest_ref++;
		else if (dest_ref)
			break;
	}
	dest_lbh = bread(sdp, planned_leaf);
	check_leaf_depth(ip, planned_leaf, dest_ref, dest_lbh);
	brelse(dest_lbh);
	free(tbl);

	/* check if it's already on the correct leaf block */
	error = dir_search(ip, tmp_name, de->de_name_len, NULL, &de->de_inum);
	if (!error) {
		log_err(_("The misplaced directory entry already appears on "
			  "the correct leaf block.\n"));
		log_err( _("The bad duplicate directory entry "
			   "'%s' was cleared.\n"), tmp_name);
		return 1; /* nuke the dent upon return */
	}

	if (dir_add(ip, tmp_name, de->de_name_len, &de->de_inum,
		    de->de_type) == 0) {
		log_err(_("The misplaced directory entry was moved to a "
			  "valid leaf block.\n"));
		gfs2_get_leaf_nr(ip, hash_index, &real_leaf);
		if (real_leaf != planned_leaf) {
			log_err(_("The planned leaf was split. The new leaf "
				  "is: %llu (0x%llx). di_blocks=%llu\n"),
				(unsigned long long)real_leaf,
				(unsigned long long)real_leaf,
				(unsigned long long)ip->i_di.di_blocks);
			fsck_blockmap_set(ip, real_leaf, _("split leaf"),
					  gfs2_indir_blk);
		}
		/* If the misplaced dirent was supposed to be earlier in the
		   hash table, we need to adjust our counts for the blocks
		   that have already been processed. If it's supposed to
		   appear later, we'll count it has part of our normal
		   processing when we get to that leaf block later on in the
		   hash table. */
		if (hash_index > lindex) {
			log_err(_("Accounting deferred.\n"));
			return 1; /* nuke the dent upon return */
		}
		/* If we get here, it's because we moved a dent to another
		   leaf, but that leaf has already been processed. So we have
		   to nuke the dent from this leaf when we return, but we
		   still need to do the "good dent" accounting. */
		if (de->de_type == (sdp->gfs1 ? GFS_FILE_DIR : DT_DIR)) {
			error = set_parent_dir(sdp, de->de_inum,
					       ip->i_di.di_num);
			if (error > 0)
				/* This is a bit of a kludge, but returning 0
				   in this case causes the caller to go through
				   function set_parent_dir a second time and
				   deal properly with the hard link. */
				return 0;
		}
		error = incr_link_count(*entry, ip,
					_("moved valid reference"));
		if (error > 0 &&
		    bad_formal_ino(ip, dent, *entry, tmp_name, q, de, bh) == 1)
			return 1; /* nuke it */

		/* You cannot do this:
		   (*count)++;
		   The reason is: *count is the count of dentries on the leaf,
		   and we moved the dentry to a previous leaf within the same
		   directory dinode. So the directory counts still get
		   incremented, but not leaf entries. When we called dir_add
		   above, it should have fixed that prev leaf's lf_entries. */
		ds->entry_count++;
		return 1;
	} else {
		log_err(_("Error moving directory entry.\n"));
		return 1; /* nuke it */
	}
}

/* basic_dentry_checks - fundamental checks for directory entries
 *
 * @ip: pointer to the incode inode structure
 * @entry: pointer to the inum info
 * @tmp_name: user-friendly file name
 * @count: pointer to the entry count
 * @de: pointer to the directory entry
 *
 * Returns: 1 means corruption, nuke the dentry, 0 means checks pass
 */
static int basic_dentry_checks(struct gfs2_inode *ip, struct gfs2_dirent *dent,
			       struct gfs2_inum *entry, const char *tmp_name,
			       uint32_t *count, struct gfs2_dirent *de,
			       struct dir_status *ds, uint8_t *q,
			       struct gfs2_buffer_head *bh)
{
	struct gfs2_sbd *sdp = ip->i_sbd;
	uint32_t calculated_hash;
	struct gfs2_inode *entry_ip = NULL;
	int error;
	struct inode_info *ii;

	if (!valid_block(ip->i_sbd, entry->no_addr)) {
		log_err( _("Block # referenced by directory entry %s in inode "
			   "%lld (0x%llx) is invalid\n"),
			 tmp_name, (unsigned long long)ip->i_di.di_num.no_addr,
			 (unsigned long long)ip->i_di.di_num.no_addr);
		if (query( _("Clear directory entry to out of range block? "
			    "(y/n) "))) {
			return 1;
		} else {
			log_err( _("Directory entry to out of range block remains\n"));
			(*count)++;
			ds->entry_count++;
			/* can't do this because the block is out of range:
			   incr_link_count(entry); */
			return 0;
		}
	}

	if (de->de_rec_len < GFS2_DIRENT_SIZE(de->de_name_len)) {
		log_err( _("Dir entry with bad record or name length\n"
			"\tRecord length = %u\n\tName length = %u\n"),
			de->de_rec_len, de->de_name_len);
		if (!query( _("Clear the directory entry? (y/n) "))) {
			log_err( _("Directory entry not fixed.\n"));
			return 0;
		}
		fsck_blockmap_set(ip, ip->i_di.di_num.no_addr,
				  _("corrupt directory entry"),
				  gfs2_inode_invalid);
		log_err( _("Bad directory entry deleted.\n"));
		return 1;
	}

	calculated_hash = gfs2_disk_hash(tmp_name, de->de_name_len);
	if (de->de_hash != calculated_hash){
	        log_err( _("Dir entry with bad hash or name length\n"
			   "\tHash found         = %u (0x%x)\n"
			   "\tFilename           = %s\n"),
			 de->de_hash, de->de_hash, tmp_name);
		log_err( _("\tName length found  = %u\n"
			   "\tHash expected      = %u (0x%x)\n"),
			 de->de_name_len, calculated_hash, calculated_hash);
		if (!query( _("Fix directory hash for %s? (y/n) "),
			   tmp_name)) {
			log_err( _("Directory entry hash for %s not "
				   "fixed.\n"), tmp_name);
			return 0;
		}
		de->de_hash = calculated_hash;
		gfs2_dirent_out(de, (char *)dent);
		bmodified(bh);
		log_err( _("Directory entry hash for %s fixed.\n"),
			 tmp_name);
	}

	*q = block_type(entry->no_addr);
	/* Get the status of the directory inode */
	/**
	 * 1. Blocks marked "invalid" were invalidated due to duplicate
	 * block references.  Pass1b should have already taken care of deleting
	 * their metadata, so here we only need to delete the directory entries
	 * pointing to them.  We delete the metadata in pass1b because we need
	 * to eliminate the inode referencing the duplicate-referenced block
	 * from the list of candidates to keep.  So we have a delete-as-we-go
	 * policy.
	 *
	 * 2. Blocks marked "bad" need to have their entire
	 * metadata tree deleted.
	*/
	if (*q == gfs2_inode_invalid || *q == gfs2_bad_block) {
		/* This entry's inode has bad blocks in it */

		/* Handle bad blocks */
		log_err( _("Found directory entry '%s' pointing to invalid "
			   "block %lld (0x%llx)\n"), tmp_name,
			 (unsigned long long)entry->no_addr,
			 (unsigned long long)entry->no_addr);

		if (!query( _("Delete inode containing bad blocks? (y/n)"))) {
			log_warn( _("Entry to inode containing bad blocks remains\n"));
			return 0;
		}

		if (*q == gfs2_bad_block) {
			if (ip->i_di.di_num.no_addr == entry->no_addr)
				entry_ip = ip;
			else
				entry_ip = fsck_load_inode(sdp, entry->no_addr);
			if (ip->i_di.di_eattr) {
				check_inode_eattr(entry_ip,
						  &pass2_fxns_delete);
			}
			check_metatree(entry_ip, &pass2_fxns_delete);
			if (entry_ip != ip)
				fsck_inode_put(&entry_ip);
		}
		fsck_blockmap_set(ip, entry->no_addr,
				  _("bad directory entry"), gfs2_block_free);
		log_err( _("Inode %lld (0x%llx) was deleted.\n"),
			 (unsigned long long)entry->no_addr,
			 (unsigned long long)entry->no_addr);
		return 1;
	}
	if (*q < gfs2_inode_dir || *q > gfs2_inode_sock) {
		log_err( _("Directory entry '%s' referencing inode %llu "
			   "(0x%llx) in dir inode %llu (0x%llx) block type "
			   "%d: %s.\n"), tmp_name,
			 (unsigned long long)entry->no_addr,
			 (unsigned long long)entry->no_addr,
			 (unsigned long long)ip->i_di.di_num.no_addr,
			 (unsigned long long)ip->i_di.di_num.no_addr,
			 *q, *q == gfs2_inode_invalid ?
			 _("was previously marked invalid") :
			 _("was deleted or is not an inode"));

		if (!query( _("Clear directory entry to non-inode block? "
			     "(y/n) "))) {
			log_err( _("Directory entry to non-inode block remains\n"));
			return 0;
		}

		/* Don't decrement the link here: Here in pass2, we increment
		   only when we know it's okay.
		   decr_link_count(ip->i_di.di_num.no_addr); */
		/* If it was previously marked invalid (i.e. known
		   to be bad, not just a free block, etc.) then the temptation
		   would be to delete any metadata it holds.  The trouble is:
		   if it's invalid, we may or _may_not_ have traversed its
		   metadata tree, and therefore may or may not have marked the
		   blocks it points to as a metadata type, or as a duplicate.
		   If there is really a duplicate reference, but we didn't
		   process the metadata tree because it's invalid, some other
		   inode has a reference to the metadata block, in which case
		   freeing it would do more harm than good.  IOW we cannot
		   count on "delete_block_if_notdup" knowing whether it's
		   really a duplicate block if we never traversed the metadata
		   tree for the invalid inode. */
		return 1;
	}

	error = check_file_type(de->de_type, *q, sdp->gfs1);
	if (error < 0) {
		log_err( _("Error: directory entry type is "
			   "incompatible with block type at block %lld "
			   "(0x%llx) in directory inode %llu (0x%llx).\n"),
			 (unsigned long long)entry->no_addr,
			 (unsigned long long)entry->no_addr,
			 (unsigned long long)ip->i_di.di_num.no_addr,
			 (unsigned long long)ip->i_di.di_num.no_addr);
		log_err( _("Directory entry type is %d, block type is %d.\n"),
			 de->de_type, *q);
		stack;
		return -1;
	}
	if (error > 0) {
		log_err( _("Type '%s' in dir entry (%s, %llu/0x%llx) conflicts"
			 " with type '%s' in dinode. (Dir entry is stale.)\n"),
			 de_type_string(de->de_type), tmp_name,
			 (unsigned long long)entry->no_addr,
			 (unsigned long long)entry->no_addr,
			 block_type_string(*q));
		if (!query( _("Clear stale directory entry? (y/n) "))) {
			log_err( _("Stale directory entry remains\n"));
			return 0;
		}
		if (ip->i_di.di_num.no_addr == entry->no_addr)
			entry_ip = ip;
		else
			entry_ip = fsck_load_inode(sdp, entry->no_addr);
		check_inode_eattr(entry_ip, &delete_eattrs);
		if (entry_ip != ip)
			fsck_inode_put(&entry_ip);
		return 1;
	}
	/* We need to verify the formal inode number matches. If it doesn't,
	   it needs to be deleted. */
	ii = inodetree_find(entry->no_addr);
	if (ii && ii->di_num.no_formal_ino != entry->no_formal_ino) {
		log_err( _("Directory entry '%s' pointing to block %llu "
			   "(0x%llx) in directory %llu (0x%llx) has the "
			   "wrong 'formal' inode number.\n"), tmp_name,
			 (unsigned long long)entry->no_addr,
			 (unsigned long long)entry->no_addr,
			 (unsigned long long)ip->i_di.di_num.no_addr,
			 (unsigned long long)ip->i_di.di_num.no_addr);
		log_err( _("The directory entry has %llu (0x%llx) but the "
			   "inode has %llu (0x%llx)\n"),
			 (unsigned long long)entry->no_formal_ino,
			 (unsigned long long)entry->no_formal_ino,
			 (unsigned long long)ii->di_num.no_formal_ino,
			 (unsigned long long)ii->di_num.no_formal_ino);
		return 1;
	}
	return 0;
}

/* FIXME: should maybe refactor this a bit - but need to deal with
 * FIXMEs internally first */
static int check_dentry(struct gfs2_inode *ip, struct gfs2_dirent *dent,
			struct gfs2_dirent *prev_de,
			struct gfs2_buffer_head *bh, char *filename,
			uint32_t *count, int lindex, void *priv)
{
	struct gfs2_sbd *sdp = ip->i_sbd;
	uint8_t q = 0;
	char tmp_name[MAX_FILENAME];
	struct gfs2_inum entry;
	struct dir_status *ds = (struct dir_status *) priv;
	int error;
	struct gfs2_inode *entry_ip = NULL;
	struct gfs2_dirent dentry, *de;
	int hash_index; /* index into the hash table based on the hash */
	int lindex_max; /* largest acceptable hash table index for hash */

	memset(&dentry, 0, sizeof(struct gfs2_dirent));
	gfs2_dirent_in(&dentry, (char *)dent);
	de = &dentry;

	entry.no_addr = de->de_inum.no_addr;
	entry.no_formal_ino = de->de_inum.no_formal_ino;

	/* Start of checks */
	memset(tmp_name, 0, MAX_FILENAME);
	if (de->de_name_len < MAX_FILENAME)
		strncpy(tmp_name, filename, de->de_name_len);
	else
		strncpy(tmp_name, filename, MAX_FILENAME - 1);

	error = basic_dentry_checks(ip, dent, &entry, tmp_name, count, de,
				    ds, &q, bh);
	if (error)
		goto nuke_dentry;

	if (!strcmp(".", tmp_name)) {
		log_debug( _("Found . dentry in directory %lld (0x%llx)\n"),
			     (unsigned long long)ip->i_di.di_num.no_addr,
			     (unsigned long long)ip->i_di.di_num.no_addr);

		if (ds->dotdir) {
			log_err( _("Already found '.' entry in directory %llu"
				" (0x%llx)\n"),
				(unsigned long long)ip->i_di.di_num.no_addr,
				(unsigned long long)ip->i_di.di_num.no_addr);
			if (!query( _("Clear duplicate '.' entry? (y/n) "))) {
				log_err( _("Duplicate '.' entry remains\n"));
				/* FIXME: Should we continue on here
				 * and check the rest of the '.' entry? */
				goto dentry_is_valid;
			}
			if (ip->i_di.di_num.no_addr == entry.no_addr)
				entry_ip = ip;
			else
				entry_ip = fsck_load_inode(sdp, entry.no_addr);
			check_inode_eattr(entry_ip, &delete_eattrs);
			if (entry_ip != ip)
				fsck_inode_put(&entry_ip);
			goto nuke_dentry;
		}

		/* GFS2 does not rely on '.' being in a certain
		 * location */

		/* check that '.' refers to this inode */
		if (entry.no_addr != ip->i_di.di_num.no_addr) {
			log_err( _("'.' entry's value incorrect in directory %llu"
				" (0x%llx).  Points to %llu"
				" (0x%llx) when it should point to %llu"
				" (0x%llx).\n"),
				(unsigned long long)entry.no_addr,
				(unsigned long long)entry.no_addr,
				(unsigned long long)entry.no_addr,
				(unsigned long long)entry.no_addr,
				(unsigned long long)ip->i_di.di_num.no_addr,
				(unsigned long long)ip->i_di.di_num.no_addr);
			if (!query( _("Remove '.' reference? (y/n) "))) {
				log_err( _("Invalid '.' reference remains\n"));
				/* Not setting ds->dotdir here since
				 * this '.' entry is invalid */
				goto dentry_is_valid;
			}
			if (ip->i_di.di_num.no_addr == entry.no_addr)
				entry_ip = ip;
			else
				entry_ip = fsck_load_inode(sdp, entry.no_addr);
			check_inode_eattr(entry_ip, &delete_eattrs);
			if (entry_ip != ip)
				fsck_inode_put(&entry_ip);
			goto nuke_dentry;
		}

		ds->dotdir = 1;
		goto dentry_is_valid;
	}
	if (!strcmp("..", tmp_name)) {
		log_debug( _("Found '..' dentry in directory %lld (0x%llx)\n"),
			     (unsigned long long)ip->i_di.di_num.no_addr,
			     (unsigned long long)ip->i_di.di_num.no_addr);
		if (ds->dotdotdir) {
			log_err( _("Already had a '..' entry in directory %llu"
				"(0x%llx)\n"),
				(unsigned long long)ip->i_di.di_num.no_addr,
				(unsigned long long)ip->i_di.di_num.no_addr);
			if (!query( _("Clear duplicate '..' entry? (y/n) "))) {
				log_err( _("Duplicate '..' entry remains\n"));
				/* FIXME: Should we continue on here
				 * and check the rest of the '..'
				 * entry? */
				goto dentry_is_valid;
			}

			if (ip->i_di.di_num.no_addr == entry.no_addr)
				entry_ip = ip;
			else
				entry_ip = fsck_load_inode(sdp, entry.no_addr);
			check_inode_eattr(entry_ip, &delete_eattrs);
			if (entry_ip != ip)
				fsck_inode_put(&entry_ip);

			goto nuke_dentry;
		}

		if (q != gfs2_inode_dir) {
			log_err( _("Found '..' entry in directory %llu (0x%llx) "
				"pointing to something that's not a directory"),
				(unsigned long long)ip->i_di.di_num.no_addr,
				(unsigned long long)ip->i_di.di_num.no_addr);
			if (!query( _("Clear bad '..' directory entry? (y/n) "))) {
				log_err( _("Bad '..' directory entry remains\n"));
				goto dentry_is_valid;
			}
			if (ip->i_di.di_num.no_addr == entry.no_addr)
				entry_ip = ip;
			else
				entry_ip = fsck_load_inode(sdp, entry.no_addr);
			check_inode_eattr(entry_ip, &delete_eattrs);
			if (entry_ip != ip)
				fsck_inode_put(&entry_ip);

			goto nuke_dentry;
		}
		/* GFS2 does not rely on '..' being in a certain location */

		/* Add the address this entry is pointing to
		 * to this inode's dotdot_parent in
		 * dir_info */
		if (set_dotdot_dir(sdp, ip->i_di.di_num.no_addr, entry)) {
			stack;
			return -1;
		}

		ds->dotdotdir = 1;
		goto dentry_is_valid;
	}
	/* If this is an exhash directory, make sure the dentries in the leaf
	   block have a hash table index that fits */
	if (ip->i_di.di_flags & GFS2_DIF_EXHASH) {
		hash_index = hash_table_index(de->de_hash, ip);
		lindex_max = hash_table_max(lindex, ip, bh);
		if (hash_index < lindex || hash_index > lindex_max) {
			int nuke_dent;

			nuke_dent = wrong_leaf(ip, &entry, tmp_name, lindex,
					       lindex_max, hash_index, bh, ds,
					       dent, de, prev_de, count, q);
			if (nuke_dent)
				goto nuke_dentry;
		}
	}

	/* After this point we're only concerned with directories */
	if (q != gfs2_inode_dir) {
		log_debug( _("Found non-dir inode dentry pointing to %lld "
			     "(0x%llx)\n"),
			   (unsigned long long)entry.no_addr,
			   (unsigned long long)entry.no_addr);
		goto dentry_is_valid;
	}

	/*log_debug( _("Found plain directory dentry\n"));*/
	error = set_parent_dir(sdp, entry, ip->i_di.di_num);
	if (error > 0) {
		log_err( _("%s: Hard link to block %llu (0x%llx"
			   ") detected.\n"), tmp_name,
			(unsigned long long)entry.no_addr,
			(unsigned long long)entry.no_addr);

		if (query( _("Clear hard link to directory? (y/n) ")))
			goto nuke_dentry;
		else {
			log_err( _("Hard link to directory remains\n"));
			goto dentry_is_valid;
		}
	} else if (error < 0) {
		stack;
		return -1;
	}
dentry_is_valid:
	/* This directory inode links to this inode via this dentry */
	error = incr_link_count(entry, ip, _("valid reference"));
	if (error > 0 &&
	    bad_formal_ino(ip, dent, entry, tmp_name, q, de, bh) == 1)
		goto nuke_dentry;

	(*count)++;
	ds->entry_count++;
	/* End of checks */
	return 0;

nuke_dentry:
	dirent2_del(ip, bh, prev_de, dent);
	log_err( _("Bad directory entry '%s' cleared.\n"), tmp_name);
	return 1;
}

/* pad_with_leafblks - pad a hash table with pointers to new leaf blocks
 *
 * @ip: pointer to the dinode structure
 * @tbl: pointer to the hash table in memory
 * @lindex: index location within the hash table to pad
 * @len: number of pointers to be padded
 */
static void pad_with_leafblks(struct gfs2_inode *ip, uint64_t *tbl,
			      int lindex, int len)
{
	int new_len, i;
	uint32_t proper_start = lindex;
	uint64_t new_leaf_blk;

	log_err(_("Padding inode %llu (0x%llx) hash table at offset %d (0x%x) "
		  "for %d pointers.\n"),
		(unsigned long long)ip->i_di.di_num.no_addr,
		(unsigned long long)ip->i_di.di_num.no_addr, lindex, lindex,
		len);
	while (len) {
		new_len = 1;
		/* Determine the next factor of 2 down from extras. We can't
		   just write out a leaf block on a power-of-two boundary.
		   We also need to make sure it has a length that will
		   ensure a "proper start" block as well. */
		while ((new_len << 1) <= len) {
			/* Translation: If doubling the size of the new leaf
			   will make its start boundary wrong, we have to
			   settle for a smaller length (and iterate more). */
			proper_start = (lindex & ~((new_len << 1) - 1));
			if (lindex != proper_start)
				break;
			new_len <<= 1;
		}
		write_new_leaf(ip, lindex, new_len, "after", &new_leaf_blk);
		log_err(_("New leaf block was allocated at %llu (0x%llx) for "
			  "index %d (0x%x), length %d\n"),
			(unsigned long long)new_leaf_blk,
			(unsigned long long)new_leaf_blk,
			lindex, lindex, new_len);
		fsck_blockmap_set(ip, new_leaf_blk, _("pad leaf"),
				  gfs2_leaf_blk);
		/* Fix the hash table in memory to have the new leaf */
		for (i = 0; i < new_len; i++)
			tbl[lindex + i] = cpu_to_be64(new_leaf_blk);
		len -= new_len;
		lindex += new_len;
	}
}

/* lost_leaf - repair a leaf block that's on the wrong directory inode
 *
 * If the correct index is less than the starting index, we have a problem.
 * Since we process the index sequentially, the previous index has already
 * been processed, fixed, and is now correct. But this leaf wants to overwrite
 * a previously written good leaf. The only thing we can do is move all the
 * directory entries to lost+found so we don't overwrite the good leaf. Then
 * we need to pad the gap we leave.
 */
static int lost_leaf(struct gfs2_inode *ip, uint64_t *tbl, uint64_t leafno,
		     int ref_count, int lindex, struct gfs2_buffer_head *bh)
{
	char *filename;
	char *bh_end = bh->b_data + ip->i_sbd->bsize;
	struct gfs2_dirent de, *dent;
	int error;

	log_err(_("Leaf block %llu (0x%llx) seems to be out of place and its "
		  "contents need to be moved to lost+found.\n"),
		(unsigned long long)leafno, (unsigned long long)leafno);
	if (!query( _("Attempt to fix it? (y/n) "))) {
		log_err( _("Directory leaf was not fixed.\n"));
		return 0;
	}
	make_sure_lf_exists(ip);

	dent = (struct gfs2_dirent *)(bh->b_data + sizeof(struct gfs2_leaf));
	while (1) {
		char tmp_name[PATH_MAX];

		memset(&de, 0, sizeof(struct gfs2_dirent));
		gfs2_dirent_in(&de, (char *)dent);
		filename = (char *)dent + sizeof(struct gfs2_dirent);
		memset(tmp_name, 0, sizeof(tmp_name));
		if (de.de_name_len > sizeof(filename)) {
			log_debug(_("Encountered bad filename length; "
				    "stopped processing.\n"));
			break;
		}
		memcpy(tmp_name, filename, de.de_name_len);
		if ((de.de_name_len == 1 && filename[0] == '.')) {
			log_debug(_("Skipping entry '.'\n"));
		} else if (de.de_name_len == 2 && filename[0] == '.' &&
			   filename[1] == '.') {
			log_debug(_("Skipping entry '..'\n"));
		} else if (!de.de_inum.no_formal_ino) { /* sentinel */
			log_debug(_("Skipping sentinel '%s'\n"), tmp_name);
		} else {
			uint32_t count;
			struct dir_status ds = {0};
			uint8_t q = 0;

			error = basic_dentry_checks(ip, dent, &de.de_inum,
						    tmp_name, &count, &de,
						    &ds, &q, bh);
			if (error) {
				log_err(_("Not relocating corrupt entry "
					  "\"%s\".\n"), tmp_name);
			} else {
				error = dir_add(lf_dip, filename,
						de.de_name_len, &de.de_inum,
						de.de_type);
				if (error && error != -EEXIST) {
					log_err(_("Error %d encountered while "
						  "trying to relocate \"%s\" "
						  "to lost+found.\n"), error,
						tmp_name);
					return error;
				}
				/* This inode is linked from lost+found */
				incr_link_count(de.de_inum, lf_dip,
						_("from lost+found"));
				/* If it's a directory, lost+found is
				   back-linked to it via .. */
				if (q == gfs2_inode_dir)
					incr_link_count(lf_dip->i_di.di_num,
							NULL,
							_("to lost+found"));
				log_err(_("Relocated \"%s\", block %llu "
					  "(0x%llx) to lost+found.\n"),
					tmp_name,
					(unsigned long long)de.de_inum.no_addr,
					(unsigned long long)de.de_inum.no_addr);
			}
		}
		if ((char *)dent + de.de_rec_len >= bh_end)
			break;
		dent = (struct gfs2_dirent *)((char *)dent + de.de_rec_len);
	}
	log_err(_("Directory entries from misplaced leaf block were relocated "
		  "to lost+found.\n"));
	/* Free the lost leaf. */
	fsck_blockmap_set(ip, leafno, _("lost leaf"), gfs2_block_free);
	ip->i_di.di_blocks--;
	bmodified(ip->i_bh);
	/* Now we have to deal with the bad hash table entries pointing to the
	   misplaced leaf block. But we can't just fill the gap with a single
	   leaf. We have to write on nice power-of-two boundaries, and we have
	   to pad out any extra pointers. */
	pad_with_leafblks(ip, tbl, lindex, ref_count);
	return 1;
}

static int basic_check_dentry(struct gfs2_inode *ip, struct gfs2_dirent *dent,
			      struct gfs2_dirent *prev_de,
			      struct gfs2_buffer_head *bh, char *filename,
			      uint32_t *count, int lindex, void *priv)
{
	uint8_t q = 0;
	char tmp_name[MAX_FILENAME];
	struct gfs2_inum entry;
	struct dir_status *ds = (struct dir_status *) priv;
	struct gfs2_dirent dentry, *de;
	int error;

	memset(&dentry, 0, sizeof(struct gfs2_dirent));
	gfs2_dirent_in(&dentry, (char *)dent);
	de = &dentry;

	entry.no_addr = de->de_inum.no_addr;
	entry.no_formal_ino = de->de_inum.no_formal_ino;

	/* Start of checks */
	memset(tmp_name, 0, MAX_FILENAME);
	if (de->de_name_len < MAX_FILENAME)
		strncpy(tmp_name, filename, de->de_name_len);
	else
		strncpy(tmp_name, filename, MAX_FILENAME - 1);

	error = basic_dentry_checks(ip, dent, &entry, tmp_name, count, de,
				    ds, &q, bh);
	if (error) {
		dirent2_del(ip, bh, prev_de, dent);
		log_err( _("Bad directory entry '%s' cleared.\n"), tmp_name);
		return 1;
	} else {
		(*count)++;
		return 0;
	}
}

static int pass2_repair_leaf(struct gfs2_inode *ip, uint64_t *leaf_no,
			     int lindex, int ref_count, const char *msg,
			     void *private)
{
	return repair_leaf(ip, leaf_no, lindex, ref_count, msg, 1);
}

/* The purpose of leafck_fxns is to provide a means for function fix_hashtable
 * to do basic sanity checks on leaf blocks before manipulating them, for
 * example, splitting them. If they're corrupt, splitting them or trying to
 * move their contents can cause a segfault. We can't really use the standard
 * pass2_fxns because that will do things we don't want. For example, it will
 * find '.' and '..' and increment the directory link count, which would be
 * done a second time when the dirent is really checked in pass2_fxns.
 * We don't want it to do the "wrong leaf" thing, or set_parent_dir either.
 * We just want a basic sanity check on pointers and lengths.
 */
struct metawalk_fxns leafck_fxns = {
	.check_leaf_depth = check_leaf_depth,
	.check_dentry = basic_check_dentry,
	.repair_leaf = pass2_repair_leaf,
};

/* fix_hashtable - fix a corrupt hash table
 *
 * The main intent of this function is to sort out hash table problems.
 * That is, it needs to determine if leaf blocks are in the wrong place,
 * if the count of pointers is wrong, and if there are extra pointers.
 * Everything should be placed on correct power-of-two boundaries appropriate
 * to their leaf depth, and extra pointers should be correctly padded with new
 * leaf blocks.
 *
 * @ip: the directory dinode structure pointer
 * @tbl: hash table that's already read into memory
 * @hsize: hash table size, as dictated by the dinode's di_depth
 * @leafblk: the leaf block number that appears at this lindex in the tbl
 * @lindex: leaf index that has a problem
 * @proper_start: where this leaf's pointers should start, as far as the
 *                hash table is concerned (sight unseen; trusting the leaf
 *                really belongs here).
 * @len: count of pointers in the hash table to this leafblk
 * @proper_len: pointer to return the proper number of pointers, as the kernel
 *              calculates it, based on the leaf depth.
 * @factor: the proper depth, given this number of pointers (rounded down).
 *
 * Returns: 0 - no changes made, or X if changes were made
 */
static int fix_hashtable(struct gfs2_inode *ip, uint64_t *tbl, unsigned hsize,
			 uint64_t leafblk, int lindex, uint32_t proper_start,
			 int len, int *proper_len, int factor)
{
	struct gfs2_buffer_head *lbh;
	struct gfs2_leaf leaf;
	struct gfs2_dirent dentry, *de;
	int changes = 0, error, i, extras, hash_index;
	uint64_t new_leaf_blk;
	uint64_t leaf_no;
	uint32_t leaf_proper_start;

	*proper_len = len;
	log_err(_("Dinode %llu (0x%llx) has a hash table error at index "
		  "0x%x, length 0x%x: leaf block %llu (0x%llx)\n"),
		(unsigned long long)ip->i_di.di_num.no_addr,
		(unsigned long long)ip->i_di.di_num.no_addr, lindex, len,
		(unsigned long long)leafblk, (unsigned long long)leafblk);
	if (!query( _("Fix the hash table? (y/n) "))) {
		log_err(_("Hash table not fixed.\n"));
		return 0;
	}

	memset(&leaf, 0, sizeof(leaf));
	leaf_no = leafblk;
	error = check_leaf(ip, lindex, &leafck_fxns, &leaf_no, &leaf, &len);
	if (error) {
		log_debug("Leaf repaired while fixing the hash table.\n");
		error = 0;
	}
	lbh = bread(ip->i_sbd, leafblk);
	/* If the leaf's depth is out of range for this dinode, it's obviously
	   attached to the wrong dinode. Move the dirents to lost+found. */
	if (leaf.lf_depth > ip->i_di.di_depth) {
		log_err(_("This leaf block's depth (%d) is too big for this "
			  "dinode's depth (%d)\n"),
			leaf.lf_depth, ip->i_di.di_depth);
		error = lost_leaf(ip, tbl, leafblk, len, lindex, lbh);
		brelse(lbh);
		return error;
	}

	memset(&dentry, 0, sizeof(struct gfs2_dirent));
	de = (struct gfs2_dirent *)(lbh->b_data + sizeof(struct gfs2_leaf));
	gfs2_dirent_in(&dentry, (char *)de);

	/* If this is an empty leaf, we can just delete it and pad. */
	if ((dentry.de_rec_len == cpu_to_be16(ip->i_sbd->bsize -
					      sizeof(struct gfs2_leaf))) &&
	    (dentry.de_inum.no_formal_ino == 0)) {
		brelse(lbh);
		gfs2_free_block(ip->i_sbd, leafblk);
		log_err(_("Out of place leaf block %llu (0x%llx) had no "
			"entries, so it was deleted.\n"),
			(unsigned long long)leafblk,
			(unsigned long long)leafblk);
		pad_with_leafblks(ip, tbl, lindex, len);
		log_err(_("Reprocessing index 0x%x (case 1).\n"), lindex);
		return 1;
	}

	/* Calculate the proper number of pointers based on the leaf depth. */
	*proper_len = 1 << (ip->i_di.di_depth - leaf.lf_depth);

	/* Look at the first dirent and check its hash value to see if it's
	   at the proper starting offset. */
	hash_index = hash_table_index(dentry.de_hash, ip);
	/* Need to use len here, not *proper_len because the leaf block may
	   be valid within the range, but starts too soon in the hash table. */
	if (hash_index < lindex ||  hash_index > lindex + len) {
		log_err(_("This leaf block has hash index %d, which is out of "
			  "bounds for where it appears in the hash table "
			  "(%d - %d)\n"),
			hash_index, lindex, lindex + *proper_len);
		error = lost_leaf(ip, tbl, leafblk, len, lindex, lbh);
		brelse(lbh);
		return error;
	}

	/* Now figure out where this leaf should start, and pad any pointers
	   up to that point with new leaf blocks. */
	leaf_proper_start = (hash_index & ~(*proper_len - 1));
	if (lindex < leaf_proper_start) {
		log_err(_("Leaf pointers start at %d (0x%x), should be %d "
			  "(%x).\n"), lindex, lindex,
			leaf_proper_start, leaf_proper_start);
		pad_with_leafblks(ip, tbl, lindex, leaf_proper_start - lindex);
		brelse(lbh);
		return 1; /* reprocess the starting lindex */
	}
	/* If the proper start according to the leaf's hash index is later
	   than the proper start according to the hash table, it's once
	   again lost and we have to relocate it. The same applies if the
	   leaf's hash index is prior to the proper state, but the leaf is
	   already at its maximum depth. */
	if ((leaf_proper_start < proper_start) ||
	    ((*proper_len > len || lindex > leaf_proper_start) &&
	     leaf.lf_depth == ip->i_di.di_depth)) {
		log_err(_("Leaf block should start at 0x%x, but it appears at "
			  "0x%x in the hash table.\n"), leaf_proper_start,
			proper_start);
		error = lost_leaf(ip, tbl, leafblk, len, lindex, lbh);
		brelse(lbh);
		return error;
	}

	/* If we SHOULD have more pointers than we do, we can solve the
	   problem by splitting the block to a lower depth. Then we may have
	   the right number of pointers. If the leaf block pointers start
	   later than they should, we can split the leaf to give it a smaller
	   footprint in the hash table. */
	if ((*proper_len > len || lindex > leaf_proper_start) &&
	    ip->i_di.di_depth > leaf.lf_depth) {
		log_err(_("For depth %d, length %d, the proper start is: "
			  "0x%x.\n"), factor, len, proper_start);
		changes++;
		new_leaf_blk = find_free_blk(ip->i_sbd);
		dir_split_leaf(ip, lindex, leafblk, lbh);
		/* re-read the leaf to pick up dir_split_leaf's changes */
		gfs2_leaf_in(&leaf, lbh);
		*proper_len = 1 << (ip->i_di.di_depth - leaf.lf_depth);
		log_err(_("Leaf block %llu (0x%llx) was split from length "
			  "%d to %d\n"), (unsigned long long)leafblk,
			(unsigned long long)leafblk, len, *proper_len);
		if (*proper_len < 0) {
			log_err(_("Programming error: proper_len=%d, "
				  "di_depth = %d, lf_depth = %d.\n"),
				*proper_len, ip->i_di.di_depth, leaf.lf_depth);
			exit(FSCK_ERROR);
		}
		log_err(_("New split-off leaf block was allocated at %lld "
			  "(0x%llx) for index %d (0x%x)\n"),
			(unsigned long long)new_leaf_blk,
			(unsigned long long)new_leaf_blk, lindex, lindex);
		fsck_blockmap_set(ip, new_leaf_blk, _("split leaf"),
				  gfs2_leaf_blk);
		log_err(_("Hash table repaired.\n"));
		/* Fix up the hash table in memory to include the new leaf */
		for (i = 0; i < *proper_len; i++)
			tbl[lindex + i] = cpu_to_be64(new_leaf_blk);
		if (*proper_len < (len >> 1)) {
			log_err(_("One leaf split is not enough. The hash "
				  "table will need to be reprocessed.\n"));
			brelse(lbh);
			return changes;
		}
		lindex += (*proper_len); /* skip the new leaf from the split */
		len -= (*proper_len);
	}
	if (*proper_len < len) {
		log_err(_("There are %d pointers, but leaf 0x%llx's "
			  "depth, %d, only allows %d\n"),
			len, (unsigned long long)leafblk, leaf.lf_depth,
			*proper_len);
	}
	brelse(lbh);
	/* At this point, lindex should be at the proper end of the pointers.
	   Now we need to replace any extra duplicate pointers to the old
	   (original) leafblk (that ran off the end) with new leaf blocks. */
	lindex += (*proper_len); /* Skip past the normal good pointers */
	len -= (*proper_len);
	extras = 0;
	for (i = 0; i < len; i++) {
		if (be64_to_cpu(tbl[lindex + i]) == leafblk)
			extras++;
		else
			break;
	}
	if (extras) {
		log_err(_("Found %d extra pointers to leaf %llu (0x%llx)\n"),
			extras, (unsigned long long)leafblk,
			(unsigned long long)leafblk);
		pad_with_leafblks(ip, tbl, lindex, extras);
		log_err(_("Reprocessing index 0x%x (case 2).\n"), lindex);
		return 1;
	}
	return changes;
}

/* check_hash_tbl_dups - check for the same leaf in multiple places */
static int check_hash_tbl_dups(struct gfs2_inode *ip, uint64_t *tbl,
			       unsigned hsize, int lindex, int len)
{
	int l, len2;
	uint64_t leafblk, leaf_no;
	struct gfs2_buffer_head *lbh;
	struct gfs2_leaf leaf;
	struct gfs2_dirent dentry, *de;
	int hash_index; /* index into the hash table based on the hash */

	leafblk = be64_to_cpu(tbl[lindex]);
	for (l = 0; l < hsize; l++) {
		if (l == lindex) { /* skip the valid reference */
			l += len - 1;
			continue;
		}
		if (be64_to_cpu(tbl[l]) != leafblk)
			continue;

		for (len2 = 0; l + len2 < hsize; len2++) {
			if (l + len2 == lindex)
				break;
			if (be64_to_cpu(tbl[l + len2]) != leafblk)
				break;
		}
		log_err(_("Dinode %llu (0x%llx) has duplicate leaf pointers "
			  "to block %llu (0x%llx) at offsets %u (0x%x) "
			  "(for 0x%x) and %u (0x%x) (for 0x%x)\n"),
			(unsigned long long)ip->i_di.di_num.no_addr,
			(unsigned long long)ip->i_di.di_num.no_addr,
			(unsigned long long)leafblk,
			(unsigned long long)leafblk, lindex, lindex, len,
			l, l, len2);

		/* See which set of references is valid: the one passed in
		   or the duplicate we found. */
		memset(&leaf, 0, sizeof(leaf));
		leaf_no = leafblk;
		if (!valid_block(ip->i_sbd, leaf_no)) /* Checked later */
			continue;

		lbh = bread(ip->i_sbd, leafblk);
		if (gfs2_check_meta(lbh, GFS2_METATYPE_LF)) { /* Chked later */
			brelse(lbh);
			continue;
		}

		memset(&dentry, 0, sizeof(struct gfs2_dirent));
		de = (struct gfs2_dirent *)(lbh->b_data +
					    sizeof(struct gfs2_leaf));
		gfs2_dirent_in(&dentry, (char *)de);
		hash_index = hash_table_index(dentry.de_hash, ip);
		brelse(lbh);
		/* check the duplicate ref first */
		if (hash_index < l ||  hash_index > l + len2) {
			log_err(_("This leaf block has hash index %d, which "
				  "is out of bounds for lindex (%d - %d)\n"),
				hash_index, l, l + len2);
			if (!query( _("Fix the hash table? (y/n) "))) {
				log_err(_("Hash table not fixed.\n"));
				return 0;
			}
			/* Adjust the ondisk block count. The original value
			   may have been correct without the duplicates but
			   pass1 would have counted them and adjusted the
			   count to include them. So we must subtract them. */
			ip->i_di.di_blocks--;
			bmodified(ip->i_bh);
			pad_with_leafblks(ip, tbl, l, len2);
		} else {
			log_debug(_("Hash index 0x%x is the proper "
				    "reference to leaf 0x%llx.\n"),
				  l, (unsigned long long)leafblk);
		}
		/* Check the original ref: both references might be bad.
		   If both were bad, just return and if we encounter it
		   again, we'll treat it as new. If the original ref is not
		   bad, keep looking for (and fixing) other instances. */
		if (hash_index < lindex ||  hash_index > lindex + len) {
			log_err(_("This leaf block has hash index %d, which "
				  "is out of bounds for lindex (%d - %d).\n"),
				hash_index, lindex, lindex + len);
			if (!query( _("Fix the hash table? (y/n) "))) {
				log_err(_("Hash table not fixed.\n"));
				return 0;
			}
			ip->i_di.di_blocks--;
			bmodified(ip->i_bh);
			pad_with_leafblks(ip, tbl, lindex, len);
			/* At this point we know both copies are bad, so we
			   return to start fresh */
			return -EFAULT;
		} else {
			log_debug(_("Hash index 0x%x is the proper "
				    "reference to leaf 0x%llx.\n"),
				  lindex, (unsigned long long)leafblk);
		}
	}
	return 0;
}

/* check_hash_tbl - check that the hash table is sane
 *
 * We've got to make sure the hash table is sane. Each leaf needs to
 * be counted a proper power of 2. We can't just have 3 pointers to a leaf.
 * The number of pointers must correspond to the proper leaf depth, and they
 * must all fall on power-of-two boundaries. The leaf block pointers all need
 * to fall properly on these boundaries, otherwise the kernel code's
 * calculations will land it on the wrong leaf block while it's searching,
 * and the result will be files you can see with ls, but can't open, delete
 * or use them.
 *
 * The goal of this function is to check the hash table to make sure the
 * boundaries and lengths all line up properly, and if not, to fix it.
 *
 * Note: There's a delicate balance here, because this function gets called
 *       BEFORE leaf blocks are checked by function check_leaf from function
 *       check_leaf_blks: the hash table has to be sane before we can start
 *       checking all the leaf blocks. And yet if there's hash table corruption
 *       we may need to reference leaf blocks to fix it, which means we need
 *       to check and/or fix a leaf block along the way.
 */
static int check_hash_tbl(struct gfs2_inode *ip, uint64_t *tbl,
			  unsigned hsize, void *private)
{
	int error = 0;
	int lindex, len, proper_len, i, changes = 0;
	uint64_t leafblk;
	struct gfs2_leaf leaf;
	struct gfs2_buffer_head *lbh;
	int factor;
	uint32_t proper_start;
	uint32_t next_proper_start;
	int anomaly;

	lindex = 0;
	while (lindex < hsize) {
		if (fsck_abort)
			return changes;
		len = 1;
		factor = 0;
		leafblk = be64_to_cpu(tbl[lindex]);
		next_proper_start = lindex;
		anomaly = 0;
		while (lindex + (len << 1) - 1 < hsize) {
			if (be64_to_cpu(tbl[lindex + (len << 1) - 1]) !=
			    leafblk)
				break;
			next_proper_start = (lindex & ~((len << 1) - 1));
			if (lindex != next_proper_start)
				anomaly = 1;
			/* Check if there are other values written between
			   here and the next factor. */
			for (i = len; !anomaly && i + lindex < hsize &&
				     i < (len << 1); i++)
				if (be64_to_cpu(tbl[lindex + i]) != leafblk)
					anomaly = 1;
			if (anomaly)
				break;
			len <<= 1;
			factor++;
		}

		/* Check for leftover pointers after the factor of two: */
		proper_len = len; /* A factor of 2 that fits nicely */
		while (lindex + len < hsize &&
		       be64_to_cpu(tbl[lindex + len]) == leafblk)
			len++;

		/* See if that leaf block is valid. If not, write a new one
		   that falls on a proper boundary. If it doesn't naturally,
		   we may need more. */
		if (!valid_block(ip->i_sbd, leafblk)) {
			uint64_t new_leafblk;

			log_err(_("Dinode %llu (0x%llx) has bad leaf pointers "
				  "at offset %d for %d\n"),
				(unsigned long long)ip->i_di.di_num.no_addr,
				(unsigned long long)ip->i_di.di_num.no_addr,
				lindex, len);
			if (!query( _("Fix the hash table? (y/n) "))) {
				log_err(_("Hash table not fixed.\n"));
				lindex += len;
				continue;
			}
			error = write_new_leaf(ip, lindex, proper_len,
					       _("replacing"), &new_leafblk);
			if (error)
				return error;

			for (i = lindex; i < lindex + proper_len; i++)
				tbl[i] = cpu_to_be64(new_leafblk);
			lindex += proper_len;
			continue;
		}

		if (check_hash_tbl_dups(ip, tbl, hsize, lindex, len))
			continue;

		/* Make sure they call on proper leaf-split boundaries. This
		   is the calculation used by the kernel, and dir_split_leaf */
		proper_start = (lindex & ~(proper_len - 1));
		if (lindex != proper_start) {
			log_debug(_("lindex 0x%llx is not a proper starting "
				    "point for leaf %llu (0x%llx): 0x%llx\n"),
				  (unsigned long long)lindex,
				  (unsigned long long)leafblk,
				  (unsigned long long)leafblk,
				  (unsigned long long)proper_start);
			changes = fix_hashtable(ip, tbl, hsize, leafblk,
						lindex, proper_start, len,
						&proper_len, factor);
			/* Check if we need to split more leaf blocks */
			if (changes) {
				if (proper_len < (len >> 1))
					log_err(_("More leaf splits are "
						  "needed; "));
				log_err(_("Reprocessing index 0x%x (case 3).\n"),
					lindex);
				continue; /* Make it reprocess the lindex */
			}
		}
		/* Check for extra pointers to this leaf. At this point, len
		   is the number of pointers we have. proper_len is the proper
		   number of pointers if the hash table is assumed correct.
		   Function fix_hashtable will read in the leaf block and
		   determine the "actual" proper length based on the leaf
		   depth, and adjust the hash table accordingly. */
		if (len != proper_len) {
			log_err(_("Length %d (0x%x) is not a proper length "
				  "for leaf %llu (0x%llx). Valid boundary "
				  "assumed to be %d (0x%x).\n"), len, len,
				(unsigned long long)leafblk,
				(unsigned long long)leafblk,
				proper_len, proper_len);
			lbh = bread(ip->i_sbd, leafblk);
			gfs2_leaf_in(&leaf, lbh);
			if (gfs2_check_meta(lbh, GFS2_METATYPE_LF) ||
			    leaf.lf_depth > ip->i_di.di_depth)
				leaf.lf_depth = factor;
			brelse(lbh);
			changes = fix_hashtable(ip, tbl, hsize, leafblk,
						lindex, lindex, len,
						&proper_len, leaf.lf_depth);
			/* If fixing the hash table made changes, we can no
			   longer count on the leaf block pointers all pointing
			   to the same leaf (which is checked below). To avoid
			   flagging another error, reprocess the offset. */
			if (changes) {
				log_err(_("Reprocessing index 0x%x (case 4).\n"),
					lindex);
				continue; /* Make it reprocess the lindex */
			}
		}

		/* Now make sure they're all the same pointer */
		for (i = lindex; i < lindex + proper_len; i++) {
			if (fsck_abort)
				return changes;

			if (be64_to_cpu(tbl[i]) == leafblk) /* No problem */
				continue;

			log_err(_("Dinode %llu (0x%llx) has a hash table "
				  "inconsistency at index %d (0x%x) for %d\n"),
				(unsigned long long)ip->i_di.di_num.no_addr,
				(unsigned long long)ip->i_di.di_num.no_addr,
				i, i, len);
			if (!query( _("Fix the hash table? (y/n) "))) {
				log_err(_("Hash table not fixed.\n"));
				continue;
			}
			changes++;
			/* Now we have to determine if the hash table is
			   corrupt, or if the leaf has the wrong depth. */
			lbh = bread(ip->i_sbd, leafblk);
			gfs2_leaf_in(&leaf, lbh);
			brelse(lbh);
			/* Calculate the expected pointer count based on the
			   leaf depth. */
			proper_len = 1 << (ip->i_di.di_depth - leaf.lf_depth);
			if (proper_len != len) {
				log_debug(_("Length 0x%x is not proper for "
					    "leaf %llu (0x%llx): 0x%x\n"),
					  len, (unsigned long long)leafblk,
					  (unsigned long long)leafblk,
					  proper_len);
				changes = fix_hashtable(ip, tbl, hsize,
							leafblk, lindex,
							lindex, len,
							&proper_len,
							leaf.lf_depth);
				break;
			}
		}
		lindex += proper_len;
	}
	if (!error && changes)
		error = 1;
	return error;
}

struct metawalk_fxns pass2_fxns = {
	.private = NULL,
	.check_leaf_depth = check_leaf_depth,
	.check_leaf = NULL,
	.check_metalist = NULL,
	.check_data = NULL,
	.check_eattr_indir = check_eattr_indir,
	.check_eattr_leaf = check_eattr_leaf,
	.check_dentry = check_dentry,
	.check_eattr_entry = NULL,
	.check_hash_tbl = check_hash_tbl,
	.repair_leaf = pass2_repair_leaf,
};

static int check_metalist_qc(struct gfs2_inode *ip, uint64_t block,
			     struct gfs2_buffer_head **bh, int h,
			     int *is_valid, int *was_duplicate, void *private)
{
	*was_duplicate = 0;
	*is_valid = 1;
	*bh = bread(ip->i_sbd, block);
	return meta_is_good;
}

static int check_data_qc(struct gfs2_inode *ip, uint64_t metablock,
			 uint64_t block, void *private)
{
	struct gfs2_buffer_head *bh;

	/* At this point, basic data block checks have already been done,
	   so we only need to make sure they're QC blocks. */
	if (!valid_block(ip->i_sbd, block))
		return -1;

	bh = bread(ip->i_sbd, block);
	if (gfs2_check_meta(bh, GFS2_METATYPE_QC) != 0) {
		log_crit(_("Error: quota_change block at %lld (0x%llx) is "
			   "the wrong metadata type.\n"),
			 (unsigned long long)block, (unsigned long long)block);
		brelse(bh);
		return -1;
	}
	brelse(bh);
	return 0;
}

struct metawalk_fxns quota_change_fxns = {
	.check_metalist = check_metalist_qc,
	.check_data = check_data_qc,
};

/* check_pernode_for - verify a file within the system per_node directory
 * @x - index number X
 * @per_node - pointer to the per_node inode
 * @fn - system file name
 * @filelen - the file length the system file needs to be
 * @multiple - the file length must be a multiple (versus the exact value)
 * @pass - a metawalk function for checking the data blocks (if any)
 * @builder - a rebuild function for the file
 *
 * Returns: 0 if all went well, else error. */
static int check_pernode_for(int x, struct gfs2_inode *pernode, const char *fn,
			     unsigned long long filelen, int multiple,
			     struct metawalk_fxns *pass,
			     int builder(struct gfs2_inode *per_node,
					 unsigned int j))
{
	struct gfs2_inode *ip;
	int error, valid_size = 1;

	log_debug(_("Checking system file %s\n"), fn);
	error = gfs2_lookupi(pernode, fn, strlen(fn), &ip);
	if (error) {
		log_err(_("System file %s is missing.\n"), fn);
		if (!query( _("Rebuild the system file? (y/n) ")))
			return 0;
		goto build_it;
	}
	if (!ip->i_di.di_size)
		valid_size = 0;
	else if (!multiple && ip->i_di.di_size != filelen)
		valid_size = 0;
	else if (multiple && (ip->i_di.di_size % filelen))
		valid_size = 0;
	if (!valid_size) {
		log_err(_("System file %s has an invalid size. Is %llu, "
			  "should be %llu.\n"), fn, ip->i_di.di_size, filelen);
		if (!query( _("Rebuild the system file? (y/n) ")))
			goto out_good;
		fsck_inode_put(&ip);
		goto build_it;
	}
	if (pass) {
		error = check_metatree(ip, pass);
		if (!error)
			goto out_good;
		log_err(_("System file %s has bad contents.\n"), fn);
		if (!query( _("Delete and rebuild the system file? (y/n) ")))
			goto out_good;
		check_metatree(ip, &pass2_fxns_delete);
		fsck_inode_put(&ip);
		gfs2_dirent_del(pernode, fn, strlen(fn));
		goto build_it;
	}
out_good:
	fsck_inode_put(&ip);
	return 0;

build_it:
	if (builder(pernode, x)) {
		log_err(_("Error building %s\n"), fn);
		return -1;
	}
	error = gfs2_lookupi(pernode, fn, strlen(fn), &ip);
	if (error) {
		log_err(_("Error rebuilding %s.\n"), fn);
		return -1;
	}
	fsck_blockmap_set(ip, ip->i_di.di_num.no_addr, fn, gfs2_inode_file);
	reprocess_inode(ip, fn);
	log_err(_("System file %s rebuilt.\n"), fn);
	goto out_good;
}

/* Check system directory inode                                           */
/* Should work for all system directories: root, master, jindex, per_node */
static int check_system_dir(struct gfs2_inode *sysinode, const char *dirname,
		     int builder(struct gfs2_sbd *sdp))
{
	uint64_t iblock = 0, cur_blks;
	struct dir_status ds = {0};
	char *filename;
	int filename_len;
	char tmp_name[256];
	int error = 0;

	log_info( _("Checking system directory inode '%s'\n"), dirname);

	if (!sysinode) {
		log_err( _("Failed to check '%s': sysinode is null\n"), dirname);
		stack;
		return -1;
	}

	iblock = sysinode->i_di.di_num.no_addr;
	ds.q = block_type(iblock);

	pass2_fxns.private = (void *) &ds;
	if (ds.q == gfs2_bad_block) {
		cur_blks = sysinode->i_di.di_blocks;
		/* First check that the directory's metatree is valid */
		error = check_metatree(sysinode, &pass2_fxns);
		if (error < 0) {
			stack;
			return error;
		}
		if (sysinode->i_di.di_blocks != cur_blks)
			reprocess_inode(sysinode, _("System inode"));
	}
	error = check_dir(sysinode->i_sbd, iblock, &pass2_fxns);
	if (skip_this_pass || fsck_abort) /* if asked to skip the rest */
		return FSCK_OK;
	if (error < 0) {
		stack;
		return -1;
	}
	if (error > 0)
		fsck_blockmap_set(sysinode, iblock, dirname,
				  gfs2_inode_invalid);

	if (check_inode_eattr(sysinode, &pass2_fxns)) {
		stack;
		return -1;
	}
	if (!ds.dotdir) {
		log_err( _("No '.' entry found for %s directory.\n"), dirname);
		if (query( _("Is it okay to add '.' entry? (y/n) "))) {
			cur_blks = sysinode->i_di.di_blocks;
			sprintf(tmp_name, ".");
			filename_len = strlen(tmp_name); /* no trailing NULL */
			if (!(filename = malloc(sizeof(char) * filename_len))) {
				log_err( _("Unable to allocate name string\n"));
				stack;
				return -1;
			}
			if (!(memset(filename, 0, sizeof(char) *
				    filename_len))) {
				log_err( _("Unable to zero name string\n"));
				stack;
				free(filename);
				return -1;
			}
			memcpy(filename, tmp_name, filename_len);
			log_warn( _("Adding '.' entry\n"));
			error = dir_add(sysinode, filename, filename_len,
					&(sysinode->i_di.di_num),
					(sysinode->i_sbd->gfs1 ?
					 GFS_FILE_DIR : DT_DIR));
			if (error) {
				log_err(_("Error adding directory %s: %s\n"),
				        filename, strerror(errno));
				free(filename);
				return -errno;
			}
			if (cur_blks != sysinode->i_di.di_blocks)
				reprocess_inode(sysinode, dirname);
			/* This system inode is linked to itself via '.' */
			incr_link_count(sysinode->i_di.di_num, sysinode,
					"sysinode \".\"");
			ds.entry_count++;
			free(filename);
		} else
			log_err( _("The directory was not fixed.\n"));
	}
	if (sysinode->i_di.di_entries != ds.entry_count) {
		log_err( _("%s inode %llu (0x%llx"
			"): Entries is %d - should be %d\n"), dirname,
			(unsigned long long)sysinode->i_di.di_num.no_addr,
			(unsigned long long)sysinode->i_di.di_num.no_addr,
			sysinode->i_di.di_entries, ds.entry_count);
		if (query( _("Fix entries for %s inode %llu (0x%llx)? (y/n) "),
			  dirname,
			  (unsigned long long)sysinode->i_di.di_num.no_addr,
			  (unsigned long long)sysinode->i_di.di_num.no_addr)) {
			sysinode->i_di.di_entries = ds.entry_count;
			bmodified(sysinode->i_bh);
			log_warn( _("Entries updated\n"));
		} else {
			log_err( _("Entries for inode %llu (0x%llx"
				") left out of sync\n"),
				(unsigned long long)
				sysinode->i_di.di_num.no_addr,
				(unsigned long long)
				sysinode->i_di.di_num.no_addr);
		}
	}
	error = 0;
	if (sysinode == sysinode->i_sbd->md.pinode) {
		int j;
		char fn[64];

		/* Make sure all the per_node files are there, and valid */
		for (j = 0; j < sysinode->i_sbd->md.journals; j++) {
			sprintf(fn, "inum_range%d", j);
			error += check_pernode_for(j, sysinode, fn, 16, 0,
						   NULL, build_inum_range);
			sprintf(fn, "statfs_change%d", j);
			error += check_pernode_for(j, sysinode, fn, 24, 0,
						   NULL, build_statfs_change);
			sprintf(fn, "quota_change%d", j);
			error += check_pernode_for(j, sysinode, fn, 1048576, 1,
						   &quota_change_fxns,
						   build_quota_change);
		}
	}
	return error;
}

/**
 * is_system_dir - determine if a given block is for a system directory.
 */
static inline int is_system_dir(struct gfs2_sbd *sdp, uint64_t block)
{
	if (block == sdp->md.rooti->i_di.di_num.no_addr)
		return TRUE;
	if (sdp->gfs1)
		return FALSE;
	if (block == sdp->md.jiinode->i_di.di_num.no_addr ||
	    block == sdp->md.pinode->i_di.di_num.no_addr ||
	    block == sdp->master_dir->i_di.di_num.no_addr)
		return TRUE;
	return FALSE;
}

/* What i need to do in this pass is check that the dentries aren't
 * pointing to invalid blocks...and verify the contents of each
 * directory. and start filling in the directory info structure*/

/**
 * pass2 - check pathnames
 *
 * verify root inode
 * directory name length
 * entries in range
 */
int pass2(struct gfs2_sbd *sdp)
{
	uint64_t dirblk, cur_blks;
	uint8_t q;
	struct dir_status ds = {0};
	struct gfs2_inode *ip;
	char *filename;
	int filename_len;
	char tmp_name[256];
	int error = 0;

	/* Check all the system directory inodes. */
	if (!sdp->gfs1 &&
	    check_system_dir(sdp->md.jiinode, "jindex", build_jindex)) {
		stack;
		return FSCK_ERROR;
	}
	if (skip_this_pass || fsck_abort) /* if asked to skip the rest */
		return FSCK_OK;
	if (!sdp->gfs1 &&
	    check_system_dir(sdp->md.pinode, "per_node", build_per_node)) {
		stack;
		return FSCK_ERROR;
	}
	if (skip_this_pass || fsck_abort) /* if asked to skip the rest */
		return FSCK_OK;
	if (!sdp->gfs1 &&
	    check_system_dir(sdp->master_dir, "master", build_master)) {
		stack;
		return FSCK_ERROR;
	}
	if (skip_this_pass || fsck_abort) /* if asked to skip the rest */
		return FSCK_OK;
	if (check_system_dir(sdp->md.rooti, "root", build_root)) {
		stack;
		return FSCK_ERROR;
	}
	if (skip_this_pass || fsck_abort) /* if asked to skip the rest */
		return FSCK_OK;
	log_info( _("Checking directory inodes.\n"));
	/* Grab each directory inode, and run checks on it */
	for (dirblk = 0; dirblk < last_fs_block; dirblk++) {
		warm_fuzzy_stuff(dirblk);
		if (skip_this_pass || fsck_abort) /* if asked to skip the rest */
			return FSCK_OK;

		/* Skip the system inodes - they're checked above */
		if (is_system_dir(sdp, dirblk))
			continue;

		q = block_type(dirblk);

		if (q != gfs2_inode_dir)
			continue;

		/* If we created lost+found, its links should have been
		   properly adjusted, so don't check it. */
		if (lf_was_created &&
		    (dirblk == lf_dip->i_di.di_num.no_addr)) {
			log_debug(_("Pass2 skipping the new lost+found.\n"));
			continue;
		}

		log_debug( _("Checking directory inode at block %llu (0x%llx)\n"),
			  (unsigned long long)dirblk, (unsigned long long)dirblk);

		memset(&ds, 0, sizeof(ds));
		pass2_fxns.private = (void *) &ds;
		if (ds.q == gfs2_bad_block) {
			/* First check that the directory's metatree
			 * is valid */
			ip = fsck_load_inode(sdp, dirblk);
			cur_blks = ip->i_di.di_blocks;
			error = check_metatree(ip, &pass2_fxns);
			fsck_inode_put(&ip);
			if (error < 0) {
				stack;
				return error;
			}
			if (ip->i_di.di_blocks != cur_blks)
				reprocess_inode(ip, "current");
		}
		error = check_dir(sdp, dirblk, &pass2_fxns);
		if (skip_this_pass || fsck_abort) /* if asked to skip the rest */
			return FSCK_OK;
		if (error < 0) {
			stack;
			return FSCK_ERROR;
		}
		if (error > 0) {
			struct dir_info *di;

			di = dirtree_find(dirblk);
			if (!di) {
				stack;
				return FSCK_ERROR;
			}
			if (query( _("Remove directory entry for bad"
				    " inode %llu (0x%llx) in %llu"
				    " (0x%llx)? (y/n)"),
				  (unsigned long long)dirblk,
				  (unsigned long long)dirblk,
				  (unsigned long long)di->treewalk_parent,
				  (unsigned long long)di->treewalk_parent)) {
				error = remove_dentry_from_dir(sdp, di->treewalk_parent,
							       dirblk);
				if (error < 0) {
					stack;
					return FSCK_ERROR;
				}
				if (error > 0) {
					log_warn( _("Unable to find dentry for %llu"
						    " (0x%llx) in %llu"
						    " (0x%llx)\n"),
						  (unsigned long long)dirblk,
						  (unsigned long long)dirblk,
						  (unsigned long long)di->treewalk_parent,
						  (unsigned long long)di->treewalk_parent);
				}
				log_warn( _("Directory entry removed\n"));
			} else
				log_err( _("Directory entry to invalid inode remains.\n"));
			log_debug( _("Directory block %lld (0x%llx) "
				     "is now marked as 'invalid'\n"),
				   (unsigned long long)dirblk,
				   (unsigned long long)dirblk);
			/* Can't use fsck_blockmap_set here because we don't
			   have an inode in memory. */
			gfs2_blockmap_set(bl, dirblk, gfs2_inode_invalid);
			check_n_fix_bitmap(sdp, dirblk, 0, gfs2_inode_invalid);
		}
		ip = fsck_load_inode(sdp, dirblk);
		if (!ds.dotdir) {
			log_err(_("No '.' entry found for directory inode at "
				  "block %llu (0x%llx)\n"),
				(unsigned long long)dirblk,
				(unsigned long long)dirblk);

			if (query( _("Is it okay to add '.' entry? (y/n) "))) {
				sprintf(tmp_name, ".");
				filename_len = strlen(tmp_name); /* no trailing
								    NULL */
				if (!(filename = malloc(sizeof(char) *
						       filename_len))) {
					log_err(_("Unable to allocate name\n"));
					stack;
					return FSCK_ERROR;
				}
				if (!memset(filename, 0, sizeof(char) *
					   filename_len)) {
					log_err( _("Unable to zero name\n"));
					stack;
					return FSCK_ERROR;
				}
				memcpy(filename, tmp_name, filename_len);

				cur_blks = ip->i_di.di_blocks;
				error = dir_add(ip, filename, filename_len,
						&(ip->i_di.di_num),
						(sdp->gfs1 ? GFS_FILE_DIR :
						 DT_DIR));
				if (error) {
					log_err(_("Error adding directory %s: %s\n"),
					        filename, strerror(errno));
					return -errno;
				}
				if (cur_blks != ip->i_di.di_blocks) {
					char dirname[80];

					sprintf(dirname, _("Directory at %lld "
							   "(0x%llx)"),
						(unsigned long long)dirblk,
						(unsigned long long)dirblk);
					reprocess_inode(ip, dirname);
				}
				/* directory links to itself via '.' */
				incr_link_count(ip->i_di.di_num, ip,
						_("\". (itself)\""));
				ds.entry_count++;
				free(filename);
				log_err( _("The directory was fixed.\n"));
			} else {
				log_err( _("The directory was not fixed.\n"));
			}
		}

		if (!fsck_abort && ip->i_di.di_entries != ds.entry_count) {
			log_err( _("Entries is %d - should be %d for inode "
				"block %llu (0x%llx)\n"),
				ip->i_di.di_entries, ds.entry_count,
				(unsigned long long)ip->i_di.di_num.no_addr,
				(unsigned long long)ip->i_di.di_num.no_addr);
			if (query( _("Fix the entry count? (y/n) "))) {
				ip->i_di.di_entries = ds.entry_count;
				bmodified(ip->i_bh);
			} else {
				log_err( _("The entry count was not fixed.\n"));
			}
		}
		fsck_inode_put(&ip); /* does a gfs2_dinode_out, brelse */
	}
	return FSCK_OK;
}
