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
#include "eattr.h"
#include "metawalk.h"
#include "link.h"

#define MAX_FILENAME 256

/* Set children's parent inode in dir_info structure - ext2 does not set
 * dotdot inode here, but instead in pass3 - should we? */
static int set_parent_dir(struct gfs2_sbd *sbp, uint64_t childblock,
				   uint64_t parentblock)
{
	struct dir_info *di;

	if(!find_di(sbp, childblock, &di)) {
		if(di->dinode == childblock) {
			if (di->treewalk_parent) {
				log_err( _("Another directory at block %" PRIu64
						" (0x%" PRIx64 ") already contains"
						" this child - checking %" PRIu64 " (0x%" PRIx64 ")\n"),
						di->treewalk_parent, di->treewalk_parent,
						parentblock, parentblock);
				return 1;
			}
			di->treewalk_parent = parentblock;
		}
	} else {
		log_err( _("Unable to find block %"PRIu64" (0x%" PRIx64
				") in dir_info list\n"),	childblock,	childblock);
		return -1;
	}

	return 0;
}

/* Set's the child's '..' directory inode number in dir_info structure */
static int set_dotdot_dir(struct gfs2_sbd *sbp, uint64_t childblock,
				   uint64_t parentblock)
{
	struct dir_info *di;

	if(!find_di(sbp, childblock, &di)) {
		if(di->dinode == childblock) {
			/* Special case for root inode because we set
			 * it earlier */
			if(di->dotdot_parent && sbp->md.rooti->i_di.di_num.no_addr
			   != di->dinode) {
				/* This should never happen */
				log_crit( _("Dotdot parent already set for"
						 " block %"PRIu64" (0x%" PRIx64 ") -> %" PRIu64
						 " (0x%" PRIx64 ")\n"), childblock, childblock,
						 di->dotdot_parent, di->dotdot_parent);
				return -1;
			}
			di->dotdot_parent = parentblock;
		}
	} else {
		log_err( _("Unable to find block %"PRIu64" (0x%" PRIx64
				") in dir_info list\n"), childblock, childblock);
		return -1;
	}

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

static int check_file_type(uint8_t de_type, uint8_t block_type)
{
	switch(block_type) {
	case gfs2_inode_dir:
		if(de_type != DT_DIR)
			return 1;
		break;
	case gfs2_inode_file:
		if(de_type != DT_REG)
			return 1;
		break;
	case gfs2_inode_lnk:
		if(de_type != DT_LNK)
			return 1;
		break;
	case gfs2_inode_blk:
		if(de_type != DT_BLK)
			return 1;
		break;
	case gfs2_inode_chr:
		if(de_type != DT_CHR)
			return 1;
		break;
	case gfs2_inode_fifo:
		if(de_type != DT_FIFO)
			return 1;
		break;
	case gfs2_inode_sock:
		if(de_type != DT_SOCK)
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
	.check_eattr_indir = delete_eattr_indir,
	.check_eattr_leaf = delete_eattr_leaf,
};

/* FIXME: should maybe refactor this a bit - but need to deal with
 * FIXMEs internally first */
static int check_dentry(struct gfs2_inode *ip, struct gfs2_dirent *dent,
		 struct gfs2_dirent *prev_de,
		 struct gfs2_buffer_head *bh, char *filename,
		 uint16_t *count, void *priv)
{
	struct gfs2_sbd *sbp = ip->i_sbd;
	struct gfs2_block_query q = {0};
	char tmp_name[MAX_FILENAME];
	uint64_t entryblock;
	struct dir_status *ds = (struct dir_status *) priv;
	int error;
	struct gfs2_inode *entry_ip = NULL;
	struct metawalk_fxns clear_eattrs = {0};
	struct gfs2_dirent dentry, *de;
	uint32_t calculated_hash;

	memset(&dentry, 0, sizeof(struct gfs2_dirent));
	gfs2_dirent_in(&dentry, (char *)dent);
	de = &dentry;

	clear_eattrs.check_eattr_indir = clear_eattr_indir;
	clear_eattrs.check_eattr_leaf = clear_eattr_leaf;
	clear_eattrs.check_eattr_entry = clear_eattr_entry;
	clear_eattrs.check_eattr_extentry = clear_eattr_extentry;

	entryblock = de->de_inum.no_addr;

	/* Start of checks */
	if (de->de_rec_len < GFS2_DIRENT_SIZE(de->de_name_len)){
		log_err( _("Dir entry with bad record or name length\n"
			"\tRecord length = %u\n"
			"\tName length = %u\n"),
			de->de_rec_len,
			de->de_name_len);
		gfs2_blockmap_set(sbp, bl, ip->i_di.di_num.no_addr,
			       gfs2_meta_inval);
		return 1;
		/* FIXME: should probably delete the entry here at the
		 * very least - maybe look at attempting to fix it */
	}
	
	calculated_hash = gfs2_disk_hash(filename, de->de_name_len);
	if (de->de_hash != calculated_hash){
	        log_err( _("Dir entry with bad hash or name length\n"
					"\tHash found         = %u (0x%x)\n"
					"\tFilename           = %s\n"), de->de_hash, de->de_hash,
					filename);
			log_err( _("\tName length found  = %u\n"
					"\tHash expected      = %u (0x%x)\n"),
					de->de_name_len, calculated_hash, calculated_hash);
			if(query( _("Fix directory hash for %s? (y/n) "),
				  filename)) {
				de->de_hash = calculated_hash;
				gfs2_dirent_out(de, (char *)dent);
				log_err( _("Directory entry hash for %s fixed.\n"), filename);
			}
			else {
				log_err( _("Directory entry hash for %s not fixed.\n"), filename);
				return 1;
			}
	}
	/* FIXME: This should probably go to the top of the fxn, and
	 * references to filename should be replaced with tmp_name */
	memset(tmp_name, 0, MAX_FILENAME);
	if(de->de_name_len < MAX_FILENAME)
		strncpy(tmp_name, filename, de->de_name_len);
	else
		strncpy(tmp_name, filename, MAX_FILENAME - 1);

	if(gfs2_check_range(ip->i_sbd, entryblock)) {
		log_err( _("Block # referenced by directory entry %s is out of range\n"),
				tmp_name);
		if(query( _("Clear directory entry tp out of range block? (y/n) "))) {
			log_err( _("Clearing %s\n"), tmp_name);
			dirent2_del(ip, bh, prev_de, dent);
			bmodified(bh);
			return 1;
		} else {
			log_err( _("Directory entry to out of range block remains\n"));
			(*count)++;
			ds->entry_count++;
			return 0;
		}
	}
	if(gfs2_block_check(sbp, bl, de->de_inum.no_addr, &q)) {
		stack;
		return -1;
	}
	/* Get the status of the directory inode */
	if(q.block_type == gfs2_bad_block) {
		/* This entry's inode has bad blocks in it */

		/* Handle bad blocks */
		log_err( _("Found a bad directory entry: %s\n"), filename);

		if(query( _("Delete inode containing bad blocks? (y/n)"))) {
			entry_ip = fsck_load_inode(sbp, de->de_inum.no_addr);
			check_inode_eattr(entry_ip, &pass2_fxns_delete);
			check_metatree(entry_ip, &pass2_fxns_delete);
			bmodified(entry_ip->i_bh);
			fsck_inode_put(&entry_ip);
			dirent2_del(ip, bh, prev_de, dent);
			gfs2_blockmap_set(sbp, bl, de->de_inum.no_addr,
				       gfs2_block_free);
			bmodified(bh);
			log_warn( _("The inode containing bad blocks was "
				    "deleted.\n"));
			return 1;
		} else {
			log_warn( _("Entry to inode containing bad blocks remains\n"));
			(*count)++;
			ds->entry_count++;
			return 0;
		}

	}
	if(q.block_type != gfs2_inode_dir && q.block_type != gfs2_inode_file &&
	   q.block_type != gfs2_inode_lnk && q.block_type != gfs2_inode_blk &&
	   q.block_type != gfs2_inode_chr && q.block_type != gfs2_inode_fifo &&
	   q.block_type != gfs2_inode_sock) {
		log_err( _("Directory entry '%s' at block %llu (0x%llx"
			   ") in dir inode %llu (0x%llx"
			   ") block type %d: %s.\n"), tmp_name,
			 (unsigned long long)de->de_inum.no_addr,
			 (unsigned long long)de->de_inum.no_addr,
			 (unsigned long long)ip->i_di.di_num.no_addr,
			 (unsigned long long)ip->i_di.di_num.no_addr,
			 q.block_type, q.block_type == gfs2_meta_inval ?
			 _("previously marked invalid") :
			 _("is not an inode"));

		if(query( _("Clear directory entry to non-inode block? "
			    "(y/n) "))) {
			struct gfs2_buffer_head *bhi;

			dirent2_del(ip, bh, prev_de, dent);
			bmodified(bh);
			log_warn( _("Directory entry '%s' cleared\n"), tmp_name);
			/* If it was previously marked invalid (i.e. known
			   to be bad, not just a free block, etc.) then
			   delete any metadata it holds.  If not, return. */
			if (q.block_type != gfs2_meta_inval)
				return 1;

			/* Now try to clear the dinode, if it is an dinode */
			bhi = bread(sbp, de->de_inum.no_addr);
			error = gfs2_check_meta(bhi, GFS2_METATYPE_DI);
			bmodified(bhi);
			brelse(bhi);
			if (error)
				return 1; /* not a dinode: nothing to delete */

			entry_ip = fsck_load_inode(sbp, de->de_inum.no_addr);
			check_inode_eattr(entry_ip, &pass2_fxns_delete);
			check_metatree(entry_ip, &pass2_fxns_delete);
			bmodified(entry_ip->i_bh);
			fsck_inode_put(&entry_ip);
			gfs2_blockmap_set(sbp, bl, de->de_inum.no_addr,
				       gfs2_block_free);

			return 1;
		} else {
			log_err( _("Directory entry to non-inode block remains\n"));
			(*count)++;
			ds->entry_count++;
			return 0;
		}
	}

	error = check_file_type(de->de_type, q.block_type);
	if(error < 0) {
		stack;
		return -1;
	}
	if(error > 0) {
		log_warn( _("Type '%s' in dir entry (%s, %llu/0x%llx) conflicts"
			 " with type '%s' in dinode. (Dir entry is stale.)\n"),
			 de_type_string(de->de_type), tmp_name,
			 (unsigned long long)de->de_inum.no_addr,
			 (unsigned long long)de->de_inum.no_addr,
			 block_type_string(&q));
		if(query( _("Clear stale directory entry? (y/n) "))) {
			entry_ip = fsck_load_inode(sbp, de->de_inum.no_addr);
			check_inode_eattr(entry_ip, &clear_eattrs);
			fsck_inode_put(&entry_ip);

			dirent2_del(ip, bh, prev_de, dent);
			bmodified(bh);
			log_err( _("Stale directory entry deleted\n"));
			return 1;
		} else {
			log_err( _("Stale directory entry remains\n"));
			(*count)++;
			ds->entry_count++;
			return 0;
		}
	}

	if(!strcmp(".", tmp_name)) {
		log_debug( _("Found . dentry\n"));

		if(ds->dotdir) {
			log_err( _("Already found '.' entry in directory %llu"
				" (0x%llx)\n"),
				(unsigned long long)ip->i_di.di_num.no_addr,
				(unsigned long long)ip->i_di.di_num.no_addr);
			if(query( _("Clear duplicate '.' entry? (y/n) "))) {
				entry_ip = fsck_load_inode(sbp, de->de_inum.no_addr);
				check_inode_eattr(entry_ip, &clear_eattrs);
				fsck_inode_put(&entry_ip);

				dirent2_del(ip, bh, prev_de, dent);
				bmodified(bh);
				return 1;
			} else {
				log_err( _("Duplicate '.' entry remains\n"));
				/* FIXME: Should we continue on here
				 * and check the rest of the '.'
				 * entry? */
				increment_link(sbp, de->de_inum.no_addr);
				(*count)++;
				ds->entry_count++;
				return 0;
			}
		}

		/* GFS2 does not rely on '.' being in a certain
		 * location */

		/* check that '.' refers to this inode */
		if(de->de_inum.no_addr != ip->i_di.di_num.no_addr) {
			log_err( _("'.' entry's value incorrect in directory %llu"
				" (0x%llx).  Points to %llu"
				" (0x%llx) when it should point to %llu"
				" (0x%llx).\n"),
				(unsigned long long)de->de_inum.no_addr,
				(unsigned long long)de->de_inum.no_addr,
				(unsigned long long)de->de_inum.no_addr,
				(unsigned long long)de->de_inum.no_addr,
				(unsigned long long)ip->i_di.di_num.no_addr,
				(unsigned long long)ip->i_di.di_num.no_addr);
			if(query( _("Remove '.' reference? (y/n) "))) {
				entry_ip = fsck_load_inode(sbp, de->de_inum.no_addr);
				check_inode_eattr(entry_ip, &clear_eattrs);
				fsck_inode_put(&entry_ip);

				dirent2_del(ip, bh, prev_de, dent);
				bmodified(bh);
				return 1;

			} else {
				log_err( _("Invalid '.' reference remains\n"));
				/* Not setting ds->dotdir here since
				 * this '.' entry is invalid */
				increment_link(sbp, de->de_inum.no_addr);
				(*count)++;
				ds->entry_count++;
				return 0;
			}
		}

		ds->dotdir = 1;
		increment_link(sbp, de->de_inum.no_addr);
		(*count)++;
		ds->entry_count++;

		return 0;
	}
	if(!strcmp("..", tmp_name)) {
		log_debug( _("Found .. dentry\n"));
		if(ds->dotdotdir) {
			log_err( _("Already found '..' entry in directory %llu"
				"(0x%llx)\n"),
				(unsigned long long)ip->i_di.di_num.no_addr,
				(unsigned long long)ip->i_di.di_num.no_addr);
			if(query( _("Clear duplicate '..' entry? (y/n) "))) {

				entry_ip = fsck_load_inode(sbp, de->de_inum.no_addr);
				check_inode_eattr(entry_ip, &clear_eattrs);
				fsck_inode_put(&entry_ip);

				dirent2_del(ip, bh, prev_de, dent);
				bmodified(bh);
				return 1;
			} else {
				log_err( _("Duplicate '..' entry remains\n"));
				/* FIXME: Should we continue on here
				 * and check the rest of the '..'
				 * entry? */
				increment_link(sbp, de->de_inum.no_addr);
				(*count)++;
				ds->entry_count++;
				return 0;
			}
		}

		if(q.block_type != gfs2_inode_dir) {
			log_err( _("Found '..' entry in directory %llu (0x%llx) "
				"pointing to something that's not a directory"),
				(unsigned long long)ip->i_di.di_num.no_addr,
				(unsigned long long)ip->i_di.di_num.no_addr);
			if(query( _("Clear bad '..' directory entry? (y/n) "))) {
				entry_ip = fsck_load_inode(sbp, de->de_inum.no_addr);
				check_inode_eattr(entry_ip, &clear_eattrs);
				fsck_inode_put(&entry_ip);

				dirent2_del(ip, bh, prev_de, dent);
				bmodified(bh);
				return 1;
			} else {
				log_err( _("Bad '..' directory entry remains\n"));
				increment_link(sbp, de->de_inum.no_addr);
				(*count)++;
				ds->entry_count++;
				return 0;
			}
		}
		/* GFS2 does not rely on '..' being in a
		 * certain location */

		/* Add the address this entry is pointing to
		 * to this inode's dotdot_parent in
		 * dir_info */
		if(set_dotdot_dir(sbp, ip->i_di.di_num.no_addr, entryblock)) {
			stack;
			return -1;
		}

		ds->dotdotdir = 1;
		increment_link(sbp, de->de_inum.no_addr);
		(*count)++;
		ds->entry_count++;
		return 0;
	}

	/* After this point we're only concerned with
	 * directories */
	if(q.block_type != gfs2_inode_dir) {
		log_debug( _("Found non-dir inode dentry\n"));
		increment_link(sbp, de->de_inum.no_addr);
		(*count)++;
		ds->entry_count++;
		return 0;
	}

	log_debug( _("Found plain directory dentry\n"));
	error = set_parent_dir(sbp, entryblock, ip->i_di.di_num.no_addr);
	if(error > 0) {
		log_err( _("%s: Hard link to block %" PRIu64" (0x%" PRIx64
				") detected.\n"), filename, entryblock, entryblock);

		if(query( _("Clear hard link to directory? (y/n) "))) {
			bmodified(bh);
			dirent2_del(ip, bh, prev_de, dent);
			log_warn( _("Directory entry %s cleared\n"), filename);

			return 1;
		} else {
			log_err( _("Hard link to directory remains\n"));
			(*count)++;
			ds->entry_count++;
			return 0;
		}
	}
	else if (error < 0) {
		stack;
		return -1;
	}
	increment_link(sbp, de->de_inum.no_addr);
	(*count)++;
	ds->entry_count++;
	/* End of checks */
	return 0;
}


struct metawalk_fxns pass2_fxns = {
	.private = NULL,
	.check_leaf = NULL,
	.check_metalist = NULL,
	.check_data = NULL,
	.check_eattr_indir = check_eattr_indir,
	.check_eattr_leaf = check_eattr_leaf,
	.check_dentry = check_dentry,
	.check_eattr_entry = NULL,
};

/* Check system directory inode                                           */
/* Should work for all system directories: root, master, jindex, per_node */
static int check_system_dir(struct gfs2_inode *sysinode, const char *dirname,
		     int builder(struct gfs2_sbd *sbp))
{
	uint64_t iblock = 0;
	struct dir_status ds = {0};
	char *filename;
	int filename_len;
	char tmp_name[256];
	int error = 0;

	log_info( _("Checking system directory inode '%s'\n"), dirname);

	if (sysinode) {
		iblock = sysinode->i_di.di_num.no_addr;
		if(gfs2_block_check(sysinode->i_sbd, bl, iblock, &ds.q)) {
			iblock = sysinode->i_di.di_num.no_addr;
		}
	}
	pass2_fxns.private = (void *) &ds;
	if(ds.q.block_type == gfs2_bad_block) {
		/* First check that the directory's metatree is valid */
		if(check_metatree(sysinode, &pass2_fxns)) {
			stack;
			return -1;
		}
	}
	error = check_dir(sysinode->i_sbd, iblock, &pass2_fxns);
	if(error < 0) {
		stack;
		return -1;
	}
	if (error > 0)
		gfs2_blockmap_set(sysinode->i_sbd, bl, iblock, gfs2_meta_inval);

	if(check_inode_eattr(sysinode, &pass2_fxns)) {
		stack;
		return -1;
	}
	if(!ds.dotdir) {
		log_err( _("No '.' entry found for %s directory.\n"), dirname);
		if (query( _("Is it okay to add '.' entry? (y/n) "))) {
			sprintf(tmp_name, ".");
			filename_len = strlen(tmp_name); /* no trailing NULL */
			if(!(filename = malloc(sizeof(char) * filename_len))) {
				log_err( _("Unable to allocate name string\n"));
				stack;
				return -1;
			}
			if(!(memset(filename, 0, sizeof(char) *
				    filename_len))) {
				log_err( _("Unable to zero name string\n"));
				stack;
				return -1;
			}
			memcpy(filename, tmp_name, filename_len);
			log_warn( _("Adding '.' entry\n"));
			dir_add(sysinode, filename, filename_len,
				&(sysinode->i_di.di_num), DT_DIR);
			increment_link(sysinode->i_sbd,
				       sysinode->i_di.di_num.no_addr);
			ds.entry_count++;
			free(filename);
		} else
			log_err( _("The directory was not fixed.\n"));
	}
	if(sysinode->i_di.di_entries != ds.entry_count) {
		log_err( _("%s inode %llu (0x%llx"
			"): Entries is %d - should be %d\n"), dirname,
			(unsigned long long)sysinode->i_di.di_num.no_addr,
			(unsigned long long)sysinode->i_di.di_num.no_addr,
			sysinode->i_di.di_entries, ds.entry_count);
		if(query( _("Fix entries for %s inode %llu (0x%llx)? (y/n) "),
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
	return 0;
}

/**
 * is_system_dir - determine if a given block is for a system directory.
 */
static inline int is_system_dir(struct gfs2_sbd *sbp, uint64_t block)
{
	if (block == sbp->md.rooti->i_di.di_num.no_addr ||
	    block == sbp->md.jiinode->i_di.di_num.no_addr ||
	    block == sbp->md.pinode->i_di.di_num.no_addr ||
	    block == sbp->master_dir->i_di.di_num.no_addr)
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
int pass2(struct gfs2_sbd *sbp)
{
	uint64_t i;
	struct gfs2_block_query q;
	struct dir_status ds = {0};
	struct gfs2_inode *ip;
	char *filename;
	int filename_len;
	char tmp_name[256];
	int error = 0;
	struct dup_blocks *b;

	/* Check all the system directory inodes. */
	if (check_system_dir(sbp->md.jiinode, "jindex", build_jindex)) {
		stack;
		return FSCK_ERROR;
	}
	if (check_system_dir(sbp->md.pinode, "per_node", build_per_node)) {
		stack;
		return FSCK_ERROR;
	}
	if (check_system_dir(sbp->master_dir, "master", build_master)) {
		stack;
		return FSCK_ERROR;
	}
	if (check_system_dir(sbp->md.rooti, "root", build_root)) {
		stack;
		return FSCK_ERROR;
	}
	log_info( _("Checking directory inodes.\n"));
	/* Grab each directory inode, and run checks on it */
	for(i = 0; i < last_fs_block; i++) {
		warm_fuzzy_stuff(i);
		if (skip_this_pass || fsck_abort) /* if asked to skip the rest */
			return FSCK_OK;

		/* Skip the system inodes - they're checked above */
		if (is_system_dir(sbp, i))
			continue;

		if(gfs2_block_check(sbp, bl, i, &q)) {
			log_err( _("Can't get block %"PRIu64 " (0x%" PRIx64
					") from block list\n"), i, i);
			return FSCK_ERROR;
		}

		if(q.block_type != gfs2_inode_dir)
			continue;

		log_debug( _("Checking directory inode at block %"PRIu64" (0x%"
				  PRIx64 ")\n"), i, i);

		memset(&ds, 0, sizeof(ds));
		pass2_fxns.private = (void *) &ds;
		if(ds.q.block_type == gfs2_bad_block) {
			/* First check that the directory's metatree
			 * is valid */
			ip = fsck_load_inode(sbp, i);
			if(check_metatree(ip, &pass2_fxns)) {
				fsck_inode_put(&ip);
				stack;
				return FSCK_ERROR;
			}
		}
		error = check_dir(sbp, i, &pass2_fxns);
		if(error < 0) {
			stack;
			return FSCK_ERROR;
		}
		if (error > 0) {
			struct dir_info *di = NULL;
			error = find_di(sbp, i, &di);
			if(error < 0) {
				stack;
				return FSCK_ERROR;
			}
			if(error == 0) {
				/* FIXME: factor */
				if(query( _("Remove directory entry for bad"
					    " inode %"PRIu64" (0x%" PRIx64 ") in %"PRIu64
					    " (0x%" PRIx64 ")? (y/n)"), i, i, di->treewalk_parent,
					  di->treewalk_parent)) {
					error = remove_dentry_from_dir(sbp, di->treewalk_parent,
												   i);
					if(error < 0) {
						stack;
						return FSCK_ERROR;
					}
					if(error > 0) {
						log_warn( _("Unable to find dentry for %"
								 PRIu64 " (0x%" PRIx64 ") in %" PRIu64
								 " (0x%" PRIx64 ")\n"), i, i,
								 di->treewalk_parent, di->treewalk_parent);
					}
					log_warn( _("Directory entry removed\n"));
				} else
					log_err( _("Directory entry to invalid inode remains.\n"));
			}
			gfs2_blockmap_set(sbp, bl, i, gfs2_meta_inval);
		}
		ip = fsck_load_inode(sbp, i);
		if(!ds.dotdir) {
			log_err(_("No '.' entry found for directory inode at "
				  "block %"PRIu64" (0x%" PRIx64 ")\n"), i, i);

			if (query( _("Is it okay to add '.' entry? (y/n) "))) {
				sprintf(tmp_name, ".");
				filename_len = strlen(tmp_name); /* no trailing
								    NULL */
				if(!(filename = malloc(sizeof(char) *
						       filename_len))) {
					log_err(_("Unable to allocate name\n"));
					stack;
					return FSCK_ERROR;
				}
				if(!memset(filename, 0, sizeof(char) *
					   filename_len)) {
					log_err( _("Unable to zero name\n"));
					stack;
					return FSCK_ERROR;
				}
				memcpy(filename, tmp_name, filename_len);

				dir_add(ip, filename, filename_len,
					&(ip->i_di.di_num), DT_DIR);
				increment_link(ip->i_sbd,
					       ip->i_di.di_num.no_addr);
				ds.entry_count++;
				free(filename);
				log_err( _("The directory was fixed.\n"));
			} else {
				log_err( _("The directory was not fixed.\n"));
			}
		}

		if(ip->i_di.di_entries != ds.entry_count) {
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
	/* Now that we've deleted the inodes marked "bad" we can safely
	   get rid of the duplicate block list.  If we do it any sooner,
	   we won't discover that a given block is a duplicate and avoid
	   deleting it from both inodes referencing it. Note: The other
	   returns from this function are premature exits of the program
	   and gfs2_block_list_destroy should get rid of the list for us. */
	while (!osi_list_empty(&sbp->dup_blocks.list)) {
		b = osi_list_entry(sbp->dup_blocks.list.next,
				   struct dup_blocks, list);
		osi_list_del(&b->list);
		free(b);
	}
	return FSCK_OK;
}



