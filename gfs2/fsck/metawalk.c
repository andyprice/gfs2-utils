#include "clusterautoconfig.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <libintl.h>
#include <ctype.h>
#include <fcntl.h>
#define _(String) gettext(String)

#include "libgfs2.h"
#include "osi_tree.h"
#include "fsck.h"
#include "util.h"
#include "metawalk.h"
#include "inode_hash.h"

#define COMFORTABLE_BLKS 5242880 /* 20GB in 4K blocks */

/* There are two bitmaps: (1) The "blockmap" that fsck uses to keep track of
   what block type has been discovered, and (2) The rgrp bitmap.  Function
   gfs2_blockmap_set is used to set the former and gfs2_set_bitmap
   is used to set the latter.  The two must be kept in sync, otherwise
   you'll get bitmap mismatches.  This function checks the status of the
   bitmap whenever the blockmap changes, and fixes it accordingly. */
int check_n_fix_bitmap(struct gfs2_sbd *sdp, uint64_t blk, int error_on_dinode,
		       enum gfs2_mark_block new_blockmap_state)
{
	int old_bitmap_state, new_bitmap_state;
	struct rgrp_tree *rgd;

	rgd = gfs2_blk2rgrpd(sdp, blk);

	old_bitmap_state = lgfs2_get_bitmap(sdp, blk, rgd);
	if (old_bitmap_state < 0) {
		log_err( _("Block %llu (0x%llx) is not represented in the "
			   "system bitmap; part of an rgrp or superblock.\n"),
			 (unsigned long long)blk, (unsigned long long)blk);
		return -1;
	}
	new_bitmap_state = blockmap_to_bitmap(new_blockmap_state, sdp->gfs1);
	if (old_bitmap_state != new_bitmap_state) {
		const char *allocdesc[2][5] = { /* gfs2 descriptions */
			{"free", "data", "unlinked", "inode", "reserved"},
			/* gfs1 descriptions: */
			{"free", "data", "free meta", "metadata", "reserved"}};

		if (error_on_dinode && old_bitmap_state == GFS2_BLKST_DINODE &&
		    new_bitmap_state != GFS2_BLKST_FREE) {
			log_debug(_("Reference as '%s' to block %llu (0x%llx) "
				    "which was marked as dinode. Needs "
				    "further investigation.\n"),
				  allocdesc[sdp->gfs1][new_bitmap_state],
				  (unsigned long long)blk,
				  (unsigned long long)blk);
			return 1;
		}
		/* Keep these messages as short as possible, or the output
		   gets to be huge and unmanageable. */
		log_err( _("Block %llu (0x%llx) was '%s', should be %s.\n"),
			 (unsigned long long)blk, (unsigned long long)blk,
			 allocdesc[sdp->gfs1][old_bitmap_state],
			 allocdesc[sdp->gfs1][new_bitmap_state]);
		if (query( _("Fix the bitmap? (y/n)"))) {
			/* If the new bitmap state is free (and therefore the
			   old state was not) we have to add to the free
			   space in the rgrp. If the old bitmap state was
			   free (and therefore it no longer is) we have to
			   subtract to the free space.  If the type changed
			   from dinode to data or data to dinode, no change in
			   free space. */
			gfs2_set_bitmap(sdp, blk, new_bitmap_state);
			if (new_bitmap_state == GFS2_BLKST_FREE) {
				/* If we're freeing a dinode, get rid of
				   the hash table entries for it. */
				if (old_bitmap_state == GFS2_BLKST_DINODE) {
					struct dir_info *dt;
					struct inode_info *ii;

					dt = dirtree_find(blk);
					if (dt)
						dirtree_delete(dt);
					ii = inodetree_find(blk);
					if (ii)
						inodetree_delete(ii);
				}
				rgd->rg.rg_free++;
				if (sdp->gfs1)
					gfs_rgrp_out((struct gfs_rgrp *)
						     &rgd->rg, rgd->bh[0]);
				else
					gfs2_rgrp_out_bh(&rgd->rg, rgd->bh[0]);
			} else if (old_bitmap_state == GFS2_BLKST_FREE) {
				rgd->rg.rg_free--;
				if (sdp->gfs1)
					gfs_rgrp_out((struct gfs_rgrp *)
						     &rgd->rg, rgd->bh[0]);
				else
					gfs2_rgrp_out_bh(&rgd->rg, rgd->bh[0]);
			}
			log_err( _("The bitmap was fixed.\n"));
		} else {
			log_err( _("The bitmap inconsistency was ignored.\n"));
		}
	}
	return 0;
}

/*
 * _fsck_blockmap_set - Mark a block in the 4-bit blockmap and the 2-bit
 *                      bitmap, and adjust free space accordingly.
 */
int _fsck_blockmap_set(struct gfs2_inode *ip, uint64_t bblock,
		       const char *btype, enum gfs2_mark_block mark,
		       int error_on_dinode,
		       const char *caller, int fline)
{
	int error;
	static int prev_ino_addr = 0;
	static enum gfs2_mark_block prev_mark = 0;
	static int prevcount = 0;

	if (print_level >= MSG_DEBUG) {
		if ((ip->i_di.di_num.no_addr == prev_ino_addr) &&
		    (mark == prev_mark)) {
			log_info("(0x%llx) ", (unsigned long long)bblock);
			prevcount++;
			if (prevcount > 10) {
				log_info("\n");
				prevcount = 0;
			}
		/* I'm circumventing the log levels here on purpose to make the
		   output easier to debug. */
		} else if (ip->i_di.di_num.no_addr == bblock) {
			if (prevcount) {
				log_info("\n");
				prevcount = 0;
			}
			printf( _("(%s:%d) %s inode found at block "
				  "(0x%llx): marking as '%s'\n"), caller, fline,
			       btype,
			       (unsigned long long)ip->i_di.di_num.no_addr,
			       block_type_string(mark));

		} else if (mark == gfs2_bad_block || mark == gfs2_meta_inval) {
			if (prevcount) {
				log_info("\n");
				prevcount = 0;
			}
			printf( _("(%s:%d) inode (0x%llx) references %s block"
				  " (0x%llx): marking as '%s'\n"),
			       caller, fline,
			       (unsigned long long)ip->i_di.di_num.no_addr,
			       btype, (unsigned long long)bblock,
			       block_type_string(mark));
		} else {
			if (prevcount) {
				log_info("\n");
				prevcount = 0;
			}
			printf( _("(%s:%d) inode (0x%llx) references %s block"
				  " (0x%llx): marking as '%s'\n"),
			       caller, fline,
			       (unsigned long long)ip->i_di.di_num.no_addr,
			       btype, (unsigned long long)bblock,
			       block_type_string(mark));
		}
		prev_ino_addr = ip->i_di.di_num.no_addr;
		prev_mark = mark;
	}

	/* First, check the rgrp bitmap against what we think it should be.
	   If that fails, it's an invalid block--part of an rgrp. */
	error = check_n_fix_bitmap(ip->i_sbd, bblock, error_on_dinode, mark);
	if (error) {
		if (error < 0)
			log_err( _("This block is not represented in the "
				   "bitmap.\n"));
		return error;
	}

	error = gfs2_blockmap_set(bl, bblock, mark);
	return error;
}

struct duptree *dupfind(uint64_t block)
{
	struct osi_node *node = dup_blocks.osi_node;

	while (node) {
		struct duptree *dt = (struct duptree *)node;

		if (block < dt->block)
			node = node->osi_left;
		else if (block > dt->block)
			node = node->osi_right;
		else
			return dt;
	}
	return NULL;
}

struct gfs2_inode *fsck_system_inode(struct gfs2_sbd *sdp, uint64_t block)
{
	int j;

	if (lf_dip && lf_dip->i_di.di_num.no_addr == block)
		return lf_dip;
	if (!sdp->gfs1)
		return is_system_inode(sdp, block);

	if (sdp->md.statfs && block == sdp->md.statfs->i_di.di_num.no_addr)
		return sdp->md.statfs;
	if (sdp->md.jiinode && block == sdp->md.jiinode->i_di.di_num.no_addr)
		return sdp->md.jiinode;
	if (sdp->md.riinode && block == sdp->md.riinode->i_di.di_num.no_addr)
		return sdp->md.riinode;
	if (sdp->md.qinode && block == sdp->md.qinode->i_di.di_num.no_addr)
		return sdp->md.qinode;
	if (sdp->md.rooti && block == sdp->md.rooti->i_di.di_num.no_addr)
		return sdp->md.rooti;
	for (j = 0; j < sdp->md.journals; j++)
		if (sdp->md.journal && sdp->md.journal[j] &&
		    block == sdp->md.journal[j]->i_di.di_num.no_addr)
			return sdp->md.journal[j];
	return NULL;
}

/* fsck_load_inode - same as gfs2_load_inode() in libgfs2 but system inodes
   get special treatment. */
struct gfs2_inode *fsck_load_inode(struct gfs2_sbd *sdp, uint64_t block)
{
	struct gfs2_inode *ip = NULL;

	ip = fsck_system_inode(sdp, block);
	if (ip)
		return ip;
	if (sdp->gfs1)
		return lgfs2_gfs_inode_read(sdp, block);
	return lgfs2_inode_read(sdp, block);
}

/* fsck_inode_get - same as inode_get() in libgfs2 but system inodes
   get special treatment. */
struct gfs2_inode *fsck_inode_get(struct gfs2_sbd *sdp,
				  struct gfs2_buffer_head *bh)
{
	struct gfs2_inode *sysip;

	sysip = fsck_system_inode(sdp, bh->b_blocknr);
	if (sysip)
		return sysip;

	if (sdp->gfs1)
		return lgfs2_gfs_inode_get(sdp, bh);
	return lgfs2_inode_get(sdp, bh);
}

/* fsck_inode_put - same as inode_put() in libgfs2 but system inodes
   get special treatment. */
void fsck_inode_put(struct gfs2_inode **ip_in)
{
	struct gfs2_inode *ip = *ip_in;
	struct gfs2_inode *sysip;

	sysip = fsck_system_inode(ip->i_sbd, ip->i_di.di_num.no_addr);
	if (!sysip)
		inode_put(ip_in);
}

/**
 * dirent_repair - attempt to repair a corrupt directory entry.
 * @bh - The buffer header that contains the bad dirent
 * @de - The directory entry in native format
 * @dent - The directory entry in on-disk format
 * @type - Type of directory (DIR_LINEAR or DIR_EXHASH)
 * @first - TRUE if this is the first dirent in the buffer
 *
 * This function tries to repair a corrupt directory entry.  All we
 * know at this point is that the length field is wrong.
 */
static int dirent_repair(struct gfs2_inode *ip, struct gfs2_buffer_head *bh,
		  struct gfs2_dirent *de, struct gfs2_dirent *dent,
		  int type, int first)
{
	char *bh_end, *p;
	int calc_de_name_len = 0;
	
	/* If this is a sentinel, just fix the length and move on */
	if (first && !de->de_inum.no_formal_ino) { /* Is it a sentinel? */
		if (type == DIR_LINEAR)
			de->de_rec_len = ip->i_sbd->bsize -
				sizeof(struct gfs2_dinode);
		else
			de->de_rec_len = ip->i_sbd->bsize -
				sizeof(struct gfs2_leaf);
	} else {
		bh_end = bh->b_data + ip->i_sbd->bsize;
		/* first, figure out a probable name length */
		p = (char *)dent + sizeof(struct gfs2_dirent);
		while (*p &&         /* while there's a non-zero char and */
		       isprint(*p) && /* a printable character and */
		       p < bh_end) { /* not past end of buffer */
			calc_de_name_len++;
			p++;
		}
		if (!calc_de_name_len)
			return 1;
		/* There can often be noise at the end, so only          */
		/* Trust the shorter of the two in case we have too much */
		/* Or rather, only trust ours if it's shorter.           */
		if (!de->de_name_len || de->de_name_len > NAME_MAX ||
		    calc_de_name_len < de->de_name_len) /* if dent is hosed */
			de->de_name_len = calc_de_name_len; /* use ours */
		de->de_rec_len = GFS2_DIRENT_SIZE(de->de_name_len);
	}
	gfs2_dirent_out(de, (char *)dent);
	bmodified(bh);
	return 0;
}

/**
 * dirblk_truncate - truncate a directory block
 */
static void dirblk_truncate(struct gfs2_inode *ip, struct gfs2_dirent *fixb,
			    struct gfs2_buffer_head *bh)
{
	char *bh_end;
	struct gfs2_dirent de;

	bh_end = bh->b_data + ip->i_sbd->sd_sb.sb_bsize;
	/* truncate the block to save the most dentries.  To do this we
	   have to patch the previous dent. */
	gfs2_dirent_in(&de, (char *)fixb);
	de.de_rec_len = bh_end - (char *)fixb;
	gfs2_dirent_out(&de, (char *)fixb);
	bmodified(bh);
}

/*
 * check_entries - check directory entries for a given block
 *
 * @ip - dinode associated with this leaf block
 * bh - buffer for the leaf block
 * type - type of block this is (linear or exhash)
 * @count - set to the count entries
 * @lindex - the last inde
 * @pass - structure pointing to pass-specific functions
 *
 * returns: 0 - good block or it was repaired to be good
 *         -1 - error occurred
 */
static int check_entries(struct gfs2_inode *ip, struct gfs2_buffer_head *bh,
			 int type, uint32_t *count, int lindex,
			 struct metawalk_fxns *pass)
{
	struct gfs2_dirent *dent;
	struct gfs2_dirent de, *prev;
	int error = 0;
	char *bh_end;
	char *filename;
	int first = 1;

	bh_end = bh->b_data + ip->i_sbd->bsize;

	if (type == DIR_LINEAR) {
		dent = (struct gfs2_dirent *)(bh->b_data + sizeof(struct gfs2_dinode));
	} else if (type == DIR_EXHASH) {
		dent = (struct gfs2_dirent *)(bh->b_data + sizeof(struct gfs2_leaf));
		log_debug( _("Checking leaf %llu (0x%llx)\n"),
			  (unsigned long long)bh->b_blocknr,
			  (unsigned long long)bh->b_blocknr);
	} else {
		log_err( _("Invalid directory type %d specified\n"), type);
		return -1;
	}

	prev = NULL;
	if (!pass->check_dentry)
		return 0;

	while (1) {
		if (skip_this_pass || fsck_abort)
			return FSCK_OK;
		memset(&de, 0, sizeof(struct gfs2_dirent));
		gfs2_dirent_in(&de, (char *)dent);
		filename = (char *)dent + sizeof(struct gfs2_dirent);

		if (de.de_rec_len < sizeof(struct gfs2_dirent) +
		    de.de_name_len ||
		    (de.de_inum.no_formal_ino && !de.de_name_len && !first)) {
			log_err( _("Directory block %llu (0x%llx"
				"), entry %d of directory %llu "
				"(0x%llx) is corrupt.\n"),
				(unsigned long long)bh->b_blocknr,
				(unsigned long long)bh->b_blocknr,
				(*count) + 1,
				(unsigned long long)ip->i_di.di_num.no_addr,
				(unsigned long long)ip->i_di.di_num.no_addr);
			if (query( _("Attempt to repair it? (y/n) "))) {
				if (dirent_repair(ip, bh, &de, dent, type,
						  first)) {
					if (first) /* make a new sentinel */
						dirblk_truncate(ip, dent, bh);
					else
						dirblk_truncate(ip, prev, bh);
					log_err( _("Unable to repair corrupt "
						   "directory entry; the "
						   "entry was removed "
						   "instead.\n"));
					return 0;
				} else {
					log_err( _("Corrupt directory entry "
						   "repaired.\n"));
					/* keep looping through dentries */
				}
			} else {
				log_err( _("Corrupt directory entry ignored, "
					"stopped after checking %d entries.\n"),
					*count);
				return 0;
			}
		}
		if (!de.de_inum.no_formal_ino){
			if (first){
				log_debug( _("First dirent is a sentinel (place holder).\n"));
				first = 0;
			} else {
				log_err( _("Directory entry with inode number of "
					"zero in leaf %llu (0x%llx) of "
					"directory %llu (0x%llx)!\n"),
					(unsigned long long)bh->b_blocknr,
					(unsigned long long)bh->b_blocknr,
					(unsigned long long)ip->i_di.di_num.no_addr,
					(unsigned long long)ip->i_di.di_num.no_addr);
				if (query(_("Attempt to remove it? (y/n) "))) {
					dirblk_truncate(ip, prev, bh);
					log_err(_("The corrupt directory "
						  "entry was removed.\n"));
				} else {
					log_err( _("Corrupt directory entry "
						   "ignored, stopped after "
						   "checking %d entries.\n"),
						 *count);
				}
				return 0;
			}
		} else {
			if (!de.de_inum.no_addr && first) { /* reverse sentinel */
				log_debug( _("First dirent is a Sentinel (place holder).\n"));
				/* Swap the two to silently make it a proper sentinel */
				de.de_inum.no_addr = de.de_inum.no_formal_ino;
				de.de_inum.no_formal_ino = 0;
				gfs2_dirent_out(&de, (char *)dent);
				bmodified(bh);
				/* Mark dirent buffer as modified */
				first = 0;
			} else {
				error = pass->check_dentry(ip, dent, prev, bh,
							   filename, count,
							   lindex,
							   pass->private);
				if (error < 0) {
					stack;
					return -1;
				}
			}
		}

		if ((char *)dent + de.de_rec_len >= bh_end){
			log_debug( _("Last entry processed for %lld->%lld "
				     "(0x%llx->0x%llx), di_blocks=%llu.\n"),
				   (unsigned long long)ip->i_di.di_num.no_addr,
				   (unsigned long long)bh->b_blocknr,
				   (unsigned long long)ip->i_di.di_num.no_addr,
				   (unsigned long long)bh->b_blocknr,
				   (unsigned long long)ip->i_di.di_blocks);
			break;
		}

		/* If we didn't clear the dentry, or if we did, but it
		 * was the first dentry, set prev  */
		if (!error || first)
			prev = dent;
		first = 0;
		dent = (struct gfs2_dirent *)((char *)dent + de.de_rec_len);
	}
	return 0;
}

/**
 * check_leaf - check a leaf block for errors
 * Reads in the leaf block
 * Leaves the buffer around for further analysis (caller must brelse)
 */
int check_leaf(struct gfs2_inode *ip, int lindex, struct metawalk_fxns *pass,
	       uint64_t *leaf_no, struct gfs2_leaf *leaf, int *ref_count)
{
	int error = 0, fix;
	struct gfs2_buffer_head *lbh = NULL;
	uint32_t count = 0;
	struct gfs2_sbd *sdp = ip->i_sbd;
	const char *msg;

	/* Make sure the block number is in range. */
	if (!valid_block(ip->i_sbd, *leaf_no)) {
		log_err( _("Leaf block #%llu (0x%llx) is out of range for "
			   "directory #%llu (0x%llx) at index %d (0x%x).\n"),
			 (unsigned long long)*leaf_no,
			 (unsigned long long)*leaf_no,
			 (unsigned long long)ip->i_di.di_num.no_addr,
			 (unsigned long long)ip->i_di.di_num.no_addr,
			 lindex, lindex);
		msg = _("that is out of range");
		goto bad_leaf;
	}

	/* Try to read in the leaf block. */
	lbh = bread(sdp, *leaf_no);
	/* Make sure it's really a valid leaf block. */
	if (gfs2_check_meta(lbh, GFS2_METATYPE_LF)) {
		msg = _("that is not really a leaf");
		goto bad_leaf;
	}
	if (pass->check_leaf_depth)
		error = pass->check_leaf_depth(ip, *leaf_no, *ref_count, lbh);

	if (pass->check_leaf) {
		error = pass->check_leaf(ip, *leaf_no, pass->private);
		if (error == -EEXIST) {
			log_info(_("Previous reference to leaf %lld (0x%llx) "
				   "has already checked it; skipping.\n"),
				 (unsigned long long)*leaf_no,
				 (unsigned long long)*leaf_no);
			brelse(lbh);
			return error;
		}
	}
	/* Early versions of GFS2 had an endianess bug in the kernel that set
	   lf_dirent_format to cpu_to_be16(GFS2_FORMAT_DE).  This was fixed
	   to use cpu_to_be32(), but we should check for incorrect values and
	   replace them with the correct value. */

	gfs2_leaf_in(leaf, lbh);
	if (leaf->lf_dirent_format == (GFS2_FORMAT_DE << 16)) {
		log_debug( _("incorrect lf_dirent_format at leaf #%" PRIu64
			     "\n"), *leaf_no);
		leaf->lf_dirent_format = GFS2_FORMAT_DE;
		gfs2_leaf_out(leaf, lbh);
		log_debug( _("Fixing lf_dirent_format.\n"));
	}

	/* Make sure it's really a leaf. */
	if (leaf->lf_header.mh_type != GFS2_METATYPE_LF) {
		log_err( _("Inode %llu (0x%llx) points to bad leaf %llu"
			   " (0x%llx).\n"),
			 (unsigned long long)ip->i_di.di_num.no_addr,
			 (unsigned long long)ip->i_di.di_num.no_addr,
			 (unsigned long long)*leaf_no,
			 (unsigned long long)*leaf_no);
		msg = _("that is not a leaf");
		goto bad_leaf;
	}

	if (pass->check_dentry && is_dir(&ip->i_di, sdp->gfs1)) {
		error = check_entries(ip, lbh, DIR_EXHASH, &count, lindex,
				      pass);

		if (skip_this_pass || fsck_abort)
			goto out;

		if (error < 0) {
			stack;
			goto out; /* This seems wrong: needs investigation */
		}

		if (count == leaf->lf_entries)
			goto out;

		/* release and re-read the leaf in case check_entries
		   changed it. */
		brelse(lbh);
		lbh = bread(sdp, *leaf_no);
		gfs2_leaf_in(leaf, lbh);
		if (count != leaf->lf_entries) {
			log_err( _("Leaf %llu (0x%llx) entry count in "
				   "directory %llu (0x%llx) does not match "
				   "number of entries found - is %u, found %u\n"),
				 (unsigned long long)*leaf_no,
				 (unsigned long long)*leaf_no,
				 (unsigned long long)ip->i_di.di_num.no_addr,
				 (unsigned long long)ip->i_di.di_num.no_addr,
				 leaf->lf_entries, count);
			if (query( _("Update leaf entry count? (y/n) "))) {
				leaf->lf_entries = count;
				gfs2_leaf_out(leaf, lbh);
				log_warn( _("Leaf entry count updated\n"));
			} else
				log_err( _("Leaf entry count left in "
					   "inconsistant state\n"));
		}
	}
out:
	brelse(lbh);
	return 0;

bad_leaf:
	if (lbh)
		brelse(lbh);
	if (pass->repair_leaf) {
		/* The leaf we read in is bad so we need to repair it. */
		fix = pass->repair_leaf(ip, leaf_no, lindex, *ref_count, msg,
					pass->private);
		if (fix < 0)
			return fix;

	}
	return 1;
}

static int u64cmp(const void *p1, const void *p2)
{
	uint64_t a = *(uint64_t *)p1;
	uint64_t b = *(uint64_t *)p2;

	if (a > b)
		return 1;
	if (a < b)
		return -1;

	return 0;
}

static void dir_leaf_reada(struct gfs2_inode *ip, uint64_t *tbl, unsigned hsize)
{
	uint64_t *t = alloca(hsize * sizeof(uint64_t));
	uint64_t leaf_no;
	struct gfs2_sbd *sdp = ip->i_sbd;
	unsigned n = 0;
	unsigned i;

	for (i = 0; i < hsize; i++) {
		leaf_no = be64_to_cpu(tbl[i]);
		if (valid_block(ip->i_sbd, leaf_no))
			t[n++] = leaf_no * sdp->bsize;
	}
	qsort(t, n, sizeof(uint64_t), u64cmp);
	for (i = 0; i < n; i++)
		posix_fadvise(sdp->device_fd, t[i], sdp->bsize, POSIX_FADV_WILLNEED);
}

/* Checks exhash directory entries */
static int check_leaf_blks(struct gfs2_inode *ip, struct metawalk_fxns *pass)
{
	int error = 0;
	struct gfs2_leaf leaf;
	unsigned hsize = (1 << ip->i_di.di_depth);
	uint64_t leaf_no, leaf_next;
	uint64_t first_ok_leaf, orig_di_blocks;
	struct gfs2_buffer_head *lbh;
	int lindex;
	struct gfs2_sbd *sdp = ip->i_sbd;
	int ref_count, orig_ref_count, orig_di_depth, orig_di_height;
	uint64_t *tbl;
	int chained_leaf, tbl_valid;

	tbl = get_dir_hash(ip);
	if (tbl == NULL) {
		perror("get_dir_hash");
		return -1;
	}
	tbl_valid = 1;
	orig_di_depth = ip->i_di.di_depth;
	orig_di_height = ip->i_di.di_height;
	orig_di_blocks = ip->i_di.di_blocks;

	/* Turn off system readahead */
	posix_fadvise(sdp->device_fd, 0, 0, POSIX_FADV_RANDOM);

	/* Readahead */
	dir_leaf_reada(ip, tbl, hsize);

	if (pass->check_hash_tbl) {
		error = pass->check_hash_tbl(ip, tbl, hsize, pass->private);
		if (error < 0) {
			free(tbl);
			posix_fadvise(sdp->device_fd, 0, 0, POSIX_FADV_NORMAL);
			return error;
		}
		/* If hash table changes were made, read it in again. */
		if (error) {
			free(tbl);
			tbl = get_dir_hash(ip);
			if (tbl == NULL) {
				perror("get_dir_hash");
				return -1;
			}
		}
	}

	/* Find the first valid leaf pointer in range and use it as our "old"
	   leaf. That way, bad blocks at the beginning will be overwritten
	   with the first valid leaf. */
	first_ok_leaf = leaf_no = -1;
	for (lindex = 0; lindex < hsize; lindex++) {
		leaf_no = be64_to_cpu(tbl[lindex]);
		if (valid_block(ip->i_sbd, leaf_no)) {
			lbh = bread(sdp, leaf_no);
			/* Make sure it's really a valid leaf block. */
			if (gfs2_check_meta(lbh, GFS2_METATYPE_LF) == 0) {
				brelse(lbh);
				first_ok_leaf = leaf_no;
				break;
			}
			brelse(lbh);
		}
	}
	if (first_ok_leaf == -1) { /* no valid leaf found */
		log_err( _("Directory #%llu (0x%llx) has no valid leaf "
			   "blocks\n"),
			 (unsigned long long)ip->i_di.di_num.no_addr,
			 (unsigned long long)ip->i_di.di_num.no_addr);
		free(tbl);
		posix_fadvise(sdp->device_fd, 0, 0, POSIX_FADV_NORMAL);
		return 1;
	}
	lindex = 0;
	leaf_next = -1;
	while (lindex < hsize) {
		int l;

		if (fsck_abort)
			break;

		if (!tbl_valid) {
			free(tbl);
			log_debug(_("Re-reading 0x%llx hash table.\n"),
				  (unsigned long long)ip->i_di.di_num.no_addr);
			tbl = get_dir_hash(ip);
			if (tbl == NULL) {
				perror("get_dir_hash");
				return -1;
			}
			tbl_valid = 1;
			orig_di_depth = ip->i_di.di_depth;
			orig_di_height = ip->i_di.di_height;
			orig_di_blocks = ip->i_di.di_blocks;
		}
		leaf_no = be64_to_cpu(tbl[lindex]);

		/* count the number of block pointers to this leaf. We don't
		   need to count the current lindex, because we already know
		   it's a reference */
		ref_count = 1;

		for (l = lindex + 1; l < hsize; l++) {
			leaf_next = be64_to_cpu(tbl[l]);
			if (leaf_next != leaf_no)
				break;
			ref_count++;
		}
		orig_ref_count = ref_count;

		chained_leaf = 0;
		do {
			if (fsck_abort) {
				free(tbl);
				posix_fadvise(sdp->device_fd, 0, 0, POSIX_FADV_NORMAL);
				return 0;
			}
			error = check_leaf(ip, lindex, pass, &leaf_no, &leaf,
					   &ref_count);
			if (ref_count != orig_ref_count)
				tbl_valid = 0;
			if (!leaf.lf_next || error)
				break;
			leaf_no = leaf.lf_next;
			chained_leaf++;
			log_debug( _("Leaf chain #%d (0x%llx) detected.\n"),
				   chained_leaf, (unsigned long long)leaf_no);
		} while (1); /* while we have chained leaf blocks */
		if (orig_di_depth != ip->i_di.di_depth) {
			log_debug(_("Depth of 0x%llx changed from %d to %d\n"),
				  (unsigned long long)ip->i_di.di_num.no_addr,
				  orig_di_depth, ip->i_di.di_depth);
			tbl_valid = 0;
		}
		if (orig_di_height != ip->i_di.di_height) {
			log_debug(_("Height of 0x%llx changed from %d to "
				    "%d\n"),
				  (unsigned long long)ip->i_di.di_num.no_addr,
				  orig_di_height, ip->i_di.di_height);
			tbl_valid = 0;
		}
		if (orig_di_blocks != ip->i_di.di_blocks) {
			log_debug(_("Block count of 0x%llx changed from %llu "
				    "to %llu\n"),
				  (unsigned long long)ip->i_di.di_num.no_addr,
				  (unsigned long long)orig_di_blocks,
				  (unsigned long long)ip->i_di.di_blocks);
			tbl_valid = 0;
		}
		lindex += ref_count;
	} /* for every leaf block */
	free(tbl);
	posix_fadvise(sdp->device_fd, 0, 0, POSIX_FADV_NORMAL);
	return 0;
}

static int check_eattr_entries(struct gfs2_inode *ip,
			       struct gfs2_buffer_head *bh,
			       struct metawalk_fxns *pass)
{
	struct gfs2_ea_header *ea_hdr, *ea_hdr_prev = NULL;
	uint64_t *ea_data_ptr = NULL;
	int i;
	int error = 0;
	uint32_t offset = (uint32_t)sizeof(struct gfs2_meta_header);

	if (!pass->check_eattr_entry)
		return 0;

	ea_hdr = (struct gfs2_ea_header *)(bh->b_data +
					  sizeof(struct gfs2_meta_header));

	while (1){
		if (ea_hdr->ea_type == GFS2_EATYPE_UNUSED)
			error = 0;
		else
			error = pass->check_eattr_entry(ip, bh, ea_hdr,
							ea_hdr_prev,
							pass->private);
		if (error < 0) {
			stack;
			return -1;
		}
		if (error == 0 && pass->check_eattr_extentry &&
		   ea_hdr->ea_num_ptrs) {
			uint32_t tot_ealen = 0;
			struct gfs2_sbd *sdp = ip->i_sbd;

			ea_data_ptr = ((uint64_t *)((char *)ea_hdr +
						    sizeof(struct gfs2_ea_header) +
						    ((ea_hdr->ea_name_len + 7) & ~7)));

			/* It is possible when a EA is shrunk
			** to have ea_num_ptrs be greater than
			** the number required for ** data.
			** In this case, the EA ** code leaves
			** the blocks ** there for **
			** reuse...........  */

			for(i = 0; i < ea_hdr->ea_num_ptrs; i++){
				if (pass->check_eattr_extentry(ip,
							      ea_data_ptr,
							      bh, ea_hdr,
							      ea_hdr_prev,
							      pass->private)) {
					log_err(_("Bad extended attribute "
						  "found at block %lld "
						  "(0x%llx)"),
						(unsigned long long)
						be64_to_cpu(*ea_data_ptr),
						(unsigned long long)
						be64_to_cpu(*ea_data_ptr));
					if (query( _("Repair the bad Extended "
						     "Attribute? (y/n) "))) {
						ea_hdr->ea_num_ptrs = i;
						ea_hdr->ea_data_len =
							cpu_to_be32(tot_ealen);
						*ea_data_ptr = 0;
						bmodified(bh);
						/* Endianness doesn't matter
						   in this case because it's
						   a single byte. */
						fsck_blockmap_set(ip,
						       ip->i_di.di_eattr,
						       _("extended attribute"),
						       gfs2_meta_eattr);
						log_err( _("The EA was "
							   "fixed.\n"));
					} else {
						error = 1;
						log_err( _("The bad EA was "
							   "not fixed.\n"));
					}
				}
				tot_ealen += sdp->sd_sb.sb_bsize -
					sizeof(struct gfs2_meta_header);
				ea_data_ptr++;
			}
		}
		offset += be32_to_cpu(ea_hdr->ea_rec_len);
		if (ea_hdr->ea_flags & GFS2_EAFLAG_LAST ||
		   offset >= ip->i_sbd->sd_sb.sb_bsize || ea_hdr->ea_rec_len == 0){
			break;
		}
		ea_hdr_prev = ea_hdr;
		ea_hdr = (struct gfs2_ea_header *)
			((char *)(ea_hdr) +
			 be32_to_cpu(ea_hdr->ea_rec_len));
	}

	return error;
}

/**
 * check_leaf_eattr
 * @ip: the inode the eattr comes from
 * @block: block number of the leaf
 *
 * Returns: 0 on success, 1 if removal is needed, -1 on error
 */
static int check_leaf_eattr(struct gfs2_inode *ip, uint64_t block,
			    uint64_t parent, struct metawalk_fxns *pass)
{
	struct gfs2_buffer_head *bh = NULL;

	if (pass->check_eattr_leaf) {
		int error = 0;

		log_debug( _("Checking EA leaf block #%llu (0x%llx).\n"),
			   (unsigned long long)block,
			   (unsigned long long)block);

		error = pass->check_eattr_leaf(ip, block, parent, &bh,
					       pass->private);
		if (error < 0) {
			stack;
			return -1;
		}
		if (error > 0) {
			if (bh)
				brelse(bh);
			return 1;
		}
		if (bh) {
			error = check_eattr_entries(ip, bh, pass);
			brelse(bh);
		}
		return error;
	}

	return 0;
}

/**
 * delete_block - delete a block associated with an inode
 */
int delete_block(struct gfs2_inode *ip, uint64_t block,
		 struct gfs2_buffer_head **bh, const char *btype,
		 void *private)
{
	if (valid_block(ip->i_sbd, block)) {
		fsck_blockmap_set(ip, block, btype, gfs2_block_free);
		return 0;
	}
	return -1;
}

/**
 * find_remove_dup - find out if this is a duplicate ref.  If so, remove it.
 *
 * Returns: 1 if there are any remaining references to this block, else 0.
 */
int find_remove_dup(struct gfs2_inode *ip, uint64_t block, const char *btype)
{
	struct duptree *dt;
	struct inode_with_dups *id;

	dt = dupfind(block);
	if (!dt)
		return 0;

	/* remove the inode reference id structure for this reference. */
	id = find_dup_ref_inode(dt, ip);
	if (!id)
		goto more_refs;

	dup_listent_delete(dt, id);
	if (dt->refs == 0) {
		log_info( _("This was the last reference: it's no longer a "
			    "duplicate.\n"));
		dup_delete(dt); /* not duplicate now */
		return 0;
	}
more_refs:
	log_info( _("%d block reference(s) remain.\n"), dt->refs);
	return 1; /* references still exist so do not free the block. */
}

/**
 * delete_block_if_notdup - delete blocks associated with an inode
 *
 * Ignore blocks that are already marked free.
 * If it has been identified as duplicate, remove the duplicate reference.
 * If all duplicate references have been removed, delete the block.
 */
static int delete_block_if_notdup(struct gfs2_inode *ip, uint64_t block,
				  struct gfs2_buffer_head **bh,
				  const char *btype, int *was_duplicate,
				  void *private)
{
	uint8_t q;

	if (!valid_block(ip->i_sbd, block))
		return meta_error;

	q = block_type(block);
	if (q == gfs2_block_free) {
		log_info( _("%s block %lld (0x%llx), part of inode "
			    "%lld (0x%llx), was already free.\n"),
			  btype, (unsigned long long)block,
			  (unsigned long long)block,
			  (unsigned long long)ip->i_di.di_num.no_addr,
			  (unsigned long long)ip->i_di.di_num.no_addr);
		return meta_is_good;
	}
	if (find_remove_dup(ip, block, btype)) { /* a dup */
		if (was_duplicate)
			*was_duplicate = 1;
		log_err( _("Not clearing duplicate reference in inode "
			   "at block #%llu (0x%llx) to block #%llu (0x%llx) "
			   "because it's referenced by another inode.\n"),
			 (unsigned long long)ip->i_di.di_num.no_addr,
			 (unsigned long long)ip->i_di.di_num.no_addr,
			 (unsigned long long)block, (unsigned long long)block);
	} else {
		fsck_blockmap_set(ip, block, btype, gfs2_block_free);
	}
	return meta_is_good;
}

/**
 * check_indirect_eattr
 * @ip: the inode the eattr comes from
 * @indirect_block
 *
 * Returns: 0 on success -1 on error
 */
static int check_indirect_eattr(struct gfs2_inode *ip, uint64_t indirect,
				struct metawalk_fxns *pass)
{
	int error = 0;
	uint64_t *ea_leaf_ptr, *end;
	uint64_t block;
	struct gfs2_buffer_head *indirect_buf = NULL;
	struct gfs2_sbd *sdp = ip->i_sbd;
	int first_ea_is_bad = 0;
	uint64_t di_eattr_save = ip->i_di.di_eattr;
	uint64_t offset = ip->i_sbd->gfs1 ? sizeof(struct gfs_indirect) : sizeof(struct gfs2_meta_header);

	log_debug( _("Checking EA indirect block #%llu (0x%llx).\n"),
		  (unsigned long long)indirect,
		  (unsigned long long)indirect);

	if (!pass->check_eattr_indir)
		return 0;
	error = pass->check_eattr_indir(ip, indirect, ip->i_di.di_num.no_addr,
					&indirect_buf, pass->private);
	if (!error) {
		int leaf_pointers = 0, leaf_pointer_errors = 0;

		ea_leaf_ptr = (uint64_t *)(indirect_buf->b_data + offset);
		end = ea_leaf_ptr + ((sdp->sd_sb.sb_bsize - offset) / 8);

		while (*ea_leaf_ptr && (ea_leaf_ptr < end)){
			block = be64_to_cpu(*ea_leaf_ptr);
			leaf_pointers++;
			error = check_leaf_eattr(ip, block, indirect, pass);
			if (error) {
				leaf_pointer_errors++;
				if (query( _("Fix the indirect "
					     "block too? (y/n) ")))
					*ea_leaf_ptr = 0;
			}
			/* If the first eattr lead is bad, we can't have
			   a hole, so we have to treat this as an unrecoverable
			   eattr error and delete all eattr info. Calling
			   finish_eattr_indir here causes ip->i_di.di_eattr = 0
			   and that ensures that subsequent calls to
			   check_leaf_eattr result in the eattr
			   check_leaf_block nuking them all "due to previous
			   errors" */
			if (leaf_pointers == 1 && leaf_pointer_errors == 1) {
				first_ea_is_bad = 1;
				if (pass->finish_eattr_indir)
					pass->finish_eattr_indir(ip,
							leaf_pointers,
							leaf_pointer_errors,
							pass->private);
			} else if (leaf_pointer_errors) {
				/* This is a bit tricky.  We can't have eattr
				   holes. So if we have 4 good eattrs, 1 bad
				   eattr and 5 more good ones: GGGGBGGGGG,
				   we need to tell check_leaf_eattr to delete
				   all eattrs after the bad one.  So we want:
				   GGGG when we finish.  To do that, we set
				   di_eattr to 0 temporarily. */
				ip->i_di.di_eattr = 0;
				bmodified(ip->i_bh);
			}
			ea_leaf_ptr++;
		}
		if (pass->finish_eattr_indir) {
			if (!first_ea_is_bad) {
				/* If the first ea is good but subsequent ones
				   were bad and deleted, we need to restore
				   the saved di_eattr block. */
				if (leaf_pointer_errors)
					ip->i_di.di_eattr = di_eattr_save;
				pass->finish_eattr_indir(ip, leaf_pointers,
							 leaf_pointer_errors,
							 pass->private);
			}
			if (leaf_pointer_errors &&
			    leaf_pointer_errors == leaf_pointers) {
				delete_block(ip, indirect, NULL, "leaf", NULL);
				error = 1;
			}
		}
	}
	if (indirect_buf)
		brelse(indirect_buf);

	return error;
}

/**
 * check_inode_eattr - check the EA's for a single inode
 * @ip: the inode whose EA to check
 *
 * Returns: 0 on success, -1 on error
 */
int check_inode_eattr(struct gfs2_inode *ip, struct metawalk_fxns *pass)
{
	int error = 0;

	if (!ip->i_di.di_eattr)
		return 0;

	log_debug( _("Extended attributes exist for inode #%llu (0x%llx).\n"),
		  (unsigned long long)ip->i_di.di_num.no_addr,
		  (unsigned long long)ip->i_di.di_num.no_addr);

	if (ip->i_di.di_flags & GFS2_DIF_EA_INDIRECT){
		if ((error = check_indirect_eattr(ip, ip->i_di.di_eattr, pass)))
			stack;
	} else {
		error = check_leaf_eattr(ip, ip->i_di.di_eattr,
					 ip->i_di.di_num.no_addr, pass);
		if (error)
			stack;
	}

	return error;
}

/**
 * free_metalist - free all metadata on a multi-level metadata list
 */
static void free_metalist(struct gfs2_inode *ip, osi_list_t *mlp)
{
	int i;
	struct gfs2_buffer_head *nbh;

	for (i = 0; i < GFS2_MAX_META_HEIGHT; i++) {
		osi_list_t *list;

		list = &mlp[i];
		while (!osi_list_empty(list)) {
			nbh = osi_list_entry(list->next,
					     struct gfs2_buffer_head, b_altlist);
			if (nbh == ip->i_bh)
				osi_list_del(&nbh->b_altlist);
			else
				brelse(nbh);
		}
	}
}

/**
 * build_and_check_metalist - check a bunch of indirect blocks
 *                            This includes hash table blocks for directories
 *                            which are technically "data" in the bitmap.
 *
 * Returns: 0 - all is well, process the blocks this metadata references
 *          1 - something went wrong, but process the sub-blocks anyway
 *         -1 - something went wrong, so don't process the sub-blocks
 * @ip:
 * @mlp:
 */
static int build_and_check_metalist(struct gfs2_inode *ip, osi_list_t *mlp,
				    struct metawalk_fxns *pass)
{
	uint32_t height = ip->i_di.di_height;
	struct gfs2_buffer_head *bh, *nbh, *metabh = ip->i_bh;
	osi_list_t *prev_list, *cur_list, *tmp;
	int h, head_size, iblk_type;
	uint64_t *ptr, block;
	int error, was_duplicate, is_valid;

	osi_list_add(&metabh->b_altlist, &mlp[0]);

	/* Directories are special.  Their 'data' is the hash table, which is
	   basically an indirect block list. Their height is not important
	   because it checks everything through the hash table using
	   "depth" field calculations. However, we still have to check the
	   indirect blocks, even if the height == 1.  */
	if (is_dir(&ip->i_di, ip->i_sbd->gfs1))
		height++;

	/* if (<there are no indirect blocks to check>) */
	if (height < 2)
		return meta_is_good;
	for (h = 1; h < height; h++) {
		if (h > 1) {
			if (is_dir(&ip->i_di, ip->i_sbd->gfs1) &&
			    h == ip->i_di.di_height + 1)
				iblk_type = GFS2_METATYPE_JD;
			else
				iblk_type = GFS2_METATYPE_IN;
			if (ip->i_sbd->gfs1)
				head_size = sizeof(struct gfs_indirect);
			else
				head_size = sizeof(struct gfs2_meta_header);
		} else {
			iblk_type = GFS2_METATYPE_DI;
			head_size = sizeof(struct gfs2_dinode);
		}
		prev_list = &mlp[h - 1];
		cur_list = &mlp[h];

		for (tmp = prev_list->next; tmp != prev_list; tmp = tmp->next){
			bh = osi_list_entry(tmp, struct gfs2_buffer_head,
					    b_altlist);
			if (gfs2_check_meta(bh, iblk_type)) {
				if (pass->invalid_meta_is_fatal)
					return meta_error;

				continue;
			}

			/* Now check the metadata itself */
			for (ptr = (uint64_t *)(bh->b_data + head_size);
			     (char *)ptr < (bh->b_data + ip->i_sbd->bsize);
			     ptr++) {
				if (skip_this_pass || fsck_abort) {
					free_metalist(ip, mlp);
					return meta_is_good;
				}
				nbh = NULL;

				if (!*ptr)
					continue;

				block = be64_to_cpu(*ptr);
				was_duplicate = 0;
				error = pass->check_metalist(ip, block, &nbh,
							     h, &is_valid,
							     &was_duplicate,
							     pass->private);
				/* check_metalist should hold any buffers
				   it gets with "bread". */
				if (error == meta_error) {
					stack;
					log_info(_("\nSerious metadata "
						   "error on block %llu "
						   "(0x%llx).\n"),
						 (unsigned long long)block,
						 (unsigned long long)block);
					return error;
				}
				if (error == meta_skip_further) {
					log_info(_("\nUnrecoverable metadata "
						   "error on block %llu "
						   "(0x%llx). Further metadata"
						   " will be skipped.\n"),
						 (unsigned long long)block,
						 (unsigned long long)block);
					return error;
				}
				if (!is_valid) {
					log_debug( _("Skipping rejected block "
						     "%llu (0x%llx)\n"),
						   (unsigned long long)block,
						   (unsigned long long)block);
					if (pass->invalid_meta_is_fatal)
						return meta_error;

					continue;
				}
				if (was_duplicate) {
					log_debug( _("Skipping duplicate %llu "
						     "(0x%llx)\n"),
						   (unsigned long long)block,
						   (unsigned long long)block);
					continue;
				}
				if (!valid_block(ip->i_sbd, block)) {
					log_debug( _("Skipping invalid block "
						     "%lld (0x%llx)\n"),
						   (unsigned long long)block,
						   (unsigned long long)block);
					if (pass->invalid_meta_is_fatal)
						return meta_error;

					continue;
				}
				if (!nbh)
					nbh = bread(ip->i_sbd, block);
				osi_list_add_prev(&nbh->b_altlist, cur_list);
			} /* for all data on the indirect block */
		} /* for blocks at that height */
	} /* for height */
	return 0;
}

/**
 * check_data - check all data pointers for a given buffer
 *              This does not include "data" blocks that are really
 *              hash table blocks for directories.
 *
 * @ip:
 *
 * returns: +ENOENT if there are too many bad pointers
 *          -1 if a more serious error occurred.
 *          0 if no errors occurred
 *          1 if errors were found and corrected
 *          2 (ENOENT) is there were too many bad pointers
 */
static int check_data(struct gfs2_inode *ip, struct metawalk_fxns *pass,
		      struct gfs2_buffer_head *bh, int head_size,
		      uint64_t *blks_checked, uint64_t *error_blk)
{
	int error = 0, rc = 0;
	uint64_t block, *ptr;
	uint64_t *ptr_start = (uint64_t *)(bh->b_data + head_size);
	char *ptr_end = (bh->b_data + ip->i_sbd->bsize);
	uint64_t metablock = bh->b_blocknr;

	/* If there isn't much pointer corruption check the pointers */
	for (ptr = ptr_start ; (char *)ptr < ptr_end && !fsck_abort; ptr++) {
		if (!*ptr)
			continue;

		if (skip_this_pass || fsck_abort)
			return error;
		block =  be64_to_cpu(*ptr);
		/* It's important that we don't call valid_block() and
		   bypass calling check_data on invalid blocks because that
		   would defeat the rangecheck_block related functions in
		   pass1. Therefore the individual check_data functions
		   should do a range check. */
		rc = pass->check_data(ip, metablock, block, pass->private);
		if (!error && rc) {
			error = rc;
			log_info("\n");
			if (rc < 0) {
				*error_blk = block;
				log_info(_("Unrecoverable "));
			}
			log_info(_("data block error %d on block %llu "
				   "(0x%llx).\n"), rc,
				 (unsigned long long)block,
				 (unsigned long long)block);
		}
		if (rc < 0)
			return rc;
		(*blks_checked)++;
	}
	return error;
}

static int undo_check_data(struct gfs2_inode *ip, struct metawalk_fxns *pass,
			   uint64_t *ptr_start, char *ptr_end,
			   uint64_t error_blk)
{
	int rc = 0;
	uint64_t block, *ptr;

	/* If there isn't much pointer corruption check the pointers */
	for (ptr = ptr_start ; (char *)ptr < ptr_end && !fsck_abort; ptr++) {
		if (!*ptr)
			continue;

		if (skip_this_pass || fsck_abort)
			return 1;
		block =  be64_to_cpu(*ptr);
		if (block == error_blk)
			return 1;
		rc = pass->undo_check_data(ip, block, pass->private);
		if (rc < 0)
			return rc;
	}
	return 0;
}

static int hdr_size(struct gfs2_buffer_head *bh, int height)
{
	if (height > 1) {
		if (gfs2_check_meta(bh, GFS2_METATYPE_IN))
			return 0;
		if (bh->sdp->gfs1)
			return sizeof(struct gfs_indirect);
		else
			return sizeof(struct gfs2_meta_header);
	}
	/* if this isn't really a dinode, skip it */
	if (gfs2_check_meta(bh, GFS2_METATYPE_DI))
		return 0;

	return sizeof(struct gfs2_dinode);
}

/**
 * check_metatree
 * @ip: inode structure in memory
 * @pass: structure passed in from caller to determine the sub-functions
 *
 */
int check_metatree(struct gfs2_inode *ip, struct metawalk_fxns *pass)
{
	osi_list_t metalist[GFS2_MAX_META_HEIGHT];
	osi_list_t *list, *tmp;
	struct gfs2_buffer_head *bh;
	uint32_t height = ip->i_di.di_height;
	int  i, head_size;
	uint64_t blks_checked = 0;
	int error, rc;
	int metadata_clean = 0;
	uint64_t error_blk = 0;
	int hit_error_blk = 0;

	if (!height && !is_dir(&ip->i_di, ip->i_sbd->gfs1))
		return 0;

	for (i = 0; i < GFS2_MAX_META_HEIGHT; i++)
		osi_list_init(&metalist[i]);

	/* create and check the metadata list for each height */
	error = build_and_check_metalist(ip, &metalist[0], pass);
	if (error) {
		stack;
		goto undo_metalist;
	}

	metadata_clean = 1;
	/* For directories, we've already checked the "data" blocks which
	 * comprise the directory hash table, so we perform the directory
	 * checks and exit. */
        if (is_dir(&ip->i_di, ip->i_sbd->gfs1)) {
		if (!(ip->i_di.di_flags & GFS2_DIF_EXHASH))
			goto out;
		/* check validity of leaf blocks and leaf chains */
		error = check_leaf_blks(ip, pass);
		if (error)
			goto undo_metalist;
		goto out;
	}

	/* check data blocks */
	list = &metalist[height - 1];
	if (ip->i_di.di_blocks > COMFORTABLE_BLKS)
		last_reported_fblock = -10000000;

	for (tmp = list->next; !error && tmp != list; tmp = tmp->next) {
		if (fsck_abort) {
			free_metalist(ip, &metalist[0]);
			return 0;
		}
		bh = osi_list_entry(tmp, struct gfs2_buffer_head, b_altlist);
		head_size = hdr_size(bh, height);
		if (!head_size)
			continue;

		if (pass->check_data)
			error = check_data(ip, pass, bh, head_size,
					   &blks_checked, &error_blk);
		if (pass->big_file_msg && ip->i_di.di_blocks > COMFORTABLE_BLKS)
			pass->big_file_msg(ip, blks_checked);
	}
	if (pass->big_file_msg && ip->i_di.di_blocks > COMFORTABLE_BLKS) {
		log_notice( _("\rLarge file at %lld (0x%llx) - 100 percent "
			      "complete.                                   "
			      "\n"),
			    (unsigned long long)ip->i_di.di_num.no_addr,
			    (unsigned long long)ip->i_di.di_num.no_addr);
		fflush(stdout);
	}
undo_metalist:
	if (!error)
		goto out;
	log_err( _("Error: inode %llu (0x%llx) had unrecoverable errors.\n"),
		 (unsigned long long)ip->i_di.di_num.no_addr,
		 (unsigned long long)ip->i_di.di_num.no_addr);
	if (!query( _("Remove the invalid inode? (y/n) "))) {
		free_metalist(ip, &metalist[0]);
		log_err(_("Invalid inode not deleted.\n"));
		return error;
	}
	for (i = 0; pass->undo_check_meta && i < height; i++) {
		while (!osi_list_empty(&metalist[i])) {
			list = &metalist[i];
			bh = osi_list_entry(list->next,
					    struct gfs2_buffer_head,
					    b_altlist);
			log_err(_("Undoing metadata work for block %llu "
				  "(0x%llx)\n"),
				(unsigned long long)bh->b_blocknr,
				(unsigned long long)bh->b_blocknr);
			if (i)
				rc = pass->undo_check_meta(ip, bh->b_blocknr,
							   i, pass->private);
			else
				rc = 0;
			if (metadata_clean && rc == 0 && i == height - 1 &&
			    !hit_error_blk) {
				head_size = hdr_size(bh, height);
				if (head_size) {
					rc = undo_check_data(ip, pass,
							     (uint64_t *)
					      (bh->b_data + head_size),
					      (bh->b_data + ip->i_sbd->bsize),
							     error_blk);
					if (rc > 0) {
						hit_error_blk = 1;
						rc = 0;
					}
				}
			}
			if (bh == ip->i_bh)
				osi_list_del(&bh->b_altlist);
			else
				brelse(bh);
		}
	}
	/* There may be leftover duplicate records, so we need to delete them.
	   For example, if a metadata block was found to be a duplicate, we
	   may not have added it to the metalist, which means it's not there
	   to undo. */
	delete_all_dups(ip);
	/* Set the dinode as "bad" so it gets deleted */
	fsck_blockmap_set(ip, ip->i_di.di_num.no_addr,
			  _("corrupt"), gfs2_block_free);
	log_err(_("The corrupt inode was invalidated.\n"));
out:
	free_metalist(ip, &metalist[0]);
	return error;
}

/* Checks stuffed inode directories */
int check_linear_dir(struct gfs2_inode *ip, struct gfs2_buffer_head *bh,
		     struct metawalk_fxns *pass)
{
	int error = 0;
	uint32_t count = 0;

	error = check_entries(ip, bh, DIR_LINEAR, &count, 0, pass);
	if (error < 0) {
		stack;
		return -1;
	}

	return error;
}

int check_dir(struct gfs2_sbd *sdp, uint64_t block, struct metawalk_fxns *pass)
{
	struct gfs2_inode *ip;
	int error = 0;
	uint64_t cur_blks;

	ip = fsck_load_inode(sdp, block);

	cur_blks = ip->i_di.di_blocks;

	if (ip->i_di.di_flags & GFS2_DIF_EXHASH)
		error = check_leaf_blks(ip, pass);
	else
		error = check_linear_dir(ip, ip->i_bh, pass);

	if (error < 0)
		stack;

	if (ip->i_di.di_blocks != cur_blks)
		reprocess_inode(ip, _("Current"));

	fsck_inode_put(&ip); /* does a brelse */
	return error;
}

static int remove_dentry(struct gfs2_inode *ip, struct gfs2_dirent *dent,
			 struct gfs2_dirent *prev_de,
			 struct gfs2_buffer_head *bh,
			 char *filename, uint32_t *count, int lindex,
			 void *private)
{
	/* the metawalk_fxn's private field must be set to the dentry
	 * block we want to clear */
	uint64_t *dentryblock = (uint64_t *) private;
	struct gfs2_dirent dentry, *de;

	memset(&dentry, 0, sizeof(struct gfs2_dirent));
	gfs2_dirent_in(&dentry, (char *)dent);
	de = &dentry;

	if (de->de_inum.no_addr == *dentryblock)
		dirent2_del(ip, bh, prev_de, dent);
	else
		(*count)++;

	return 0;

}

int remove_dentry_from_dir(struct gfs2_sbd *sdp, uint64_t dir,
			   uint64_t dentryblock)
{
	struct metawalk_fxns remove_dentry_fxns = {0};
	uint8_t q;
	int error;

	log_debug( _("Removing dentry %llu (0x%llx) from directory %llu"
		     " (0x%llx)\n"), (unsigned long long)dentryblock,
		  (unsigned long long)dentryblock,
		  (unsigned long long)dir, (unsigned long long)dir);
	if (!valid_block(sdp, dir)) {
		log_err( _("Parent directory is invalid\n"));
		return 1;
	}
	remove_dentry_fxns.private = &dentryblock;
	remove_dentry_fxns.check_dentry = remove_dentry;

	q = block_type(dir);
	if (q != gfs2_inode_dir) {
		log_info( _("Parent block is not a directory...ignoring\n"));
		return 1;
	}
	/* Need to run check_dir with a private var of dentryblock,
	 * and fxns that remove that dentry if found */
	error = check_dir(sdp, dir, &remove_dentry_fxns);

	return error;
}

int delete_metadata(struct gfs2_inode *ip, uint64_t block,
		    struct gfs2_buffer_head **bh, int h, int *is_valid,
		    int *was_duplicate, void *private)
{
	*is_valid = 1;
	*was_duplicate = 0;
	return delete_block_if_notdup(ip, block, bh, _("metadata"),
				      was_duplicate, private);
}

int delete_leaf(struct gfs2_inode *ip, uint64_t block, void *private)
{
	return delete_block_if_notdup(ip, block, NULL, _("leaf"), NULL,
				      private);
}

int delete_data(struct gfs2_inode *ip, uint64_t metablock,
		uint64_t block, void *private)
{
	return delete_block_if_notdup(ip, block, NULL, _("data"), NULL,
				      private);
}

static int del_eattr_generic(struct gfs2_inode *ip, uint64_t block,
			     uint64_t parent, struct gfs2_buffer_head **bh,
			     void *private, const char *eatype)
{
	int ret = 0;
	int was_free = 0;
	uint8_t q;

	if (valid_block(ip->i_sbd, block)) {
		q = block_type(block);
		if (q == gfs2_block_free)
			was_free = 1;
		ret = delete_block_if_notdup(ip, block, NULL, eatype,
					     NULL, private);
		if (!ret) {
			*bh = bread(ip->i_sbd, block);
			if (!was_free)
				ip->i_di.di_blocks--;
			bmodified(ip->i_bh);
		}
	}
	/* Even if it's a duplicate reference, we want to eliminate the
	   reference itself, and adjust di_blocks accordingly. */
	if (ip->i_di.di_eattr) {
		if (block == ip->i_di.di_eattr)
			ip->i_di.di_eattr = 0;
		bmodified(ip->i_bh);
	}
	return ret;
}

int delete_eattr_indir(struct gfs2_inode *ip, uint64_t block, uint64_t parent,
		       struct gfs2_buffer_head **bh, void *private)
{
	return del_eattr_generic(ip, block, parent, bh, private,
				 _("extended attribute"));
}

int delete_eattr_leaf(struct gfs2_inode *ip, uint64_t block, uint64_t parent,
		      struct gfs2_buffer_head **bh, void *private)
{
	return del_eattr_generic(ip, block, parent, bh, private,
				 _("indirect extended attribute"));
}

int delete_eattr_entry(struct gfs2_inode *ip, struct gfs2_buffer_head *leaf_bh,
		       struct gfs2_ea_header *ea_hdr,
		       struct gfs2_ea_header *ea_hdr_prev, void *private)
{
	struct gfs2_sbd *sdp = ip->i_sbd;
	char ea_name[256];
	uint32_t avail_size;
	int max_ptrs;

	if (!ea_hdr->ea_name_len){
		/* Skip this entry for now */
		return 1;
	}

	memset(ea_name, 0, sizeof(ea_name));
	strncpy(ea_name, (char *)ea_hdr + sizeof(struct gfs2_ea_header),
		ea_hdr->ea_name_len);

	if (!GFS2_EATYPE_VALID(ea_hdr->ea_type) &&
	   ((ea_hdr_prev) || (!ea_hdr_prev && ea_hdr->ea_type))){
		/* Skip invalid entry */
		return 1;
	}

	if (!ea_hdr->ea_num_ptrs)
		return 0;

	avail_size = sdp->sd_sb.sb_bsize - sizeof(struct gfs2_meta_header);
	max_ptrs = (be32_to_cpu(ea_hdr->ea_data_len) + avail_size - 1) /
		avail_size;

	if (max_ptrs > ea_hdr->ea_num_ptrs)
		return 1;

	log_debug( _("  Pointers Required: %d\n  Pointers Reported: %d\n"),
		   max_ptrs, ea_hdr->ea_num_ptrs);

	return 0;
}

int delete_eattr_extentry(struct gfs2_inode *ip, uint64_t *ea_data_ptr,
			  struct gfs2_buffer_head *leaf_bh,
			  struct gfs2_ea_header *ea_hdr,
			  struct gfs2_ea_header *ea_hdr_prev, void *private)
{
	uint64_t block = be64_to_cpu(*ea_data_ptr);

	return delete_block_if_notdup(ip, block, NULL, _("extended attribute"),
				      NULL, private);
}

static int alloc_metalist(struct gfs2_inode *ip, uint64_t block,
			  struct gfs2_buffer_head **bh, int h, int *is_valid,
			  int *was_duplicate, void *private)
{
	uint8_t q;
	const char *desc = (const char *)private;

	/* No need to range_check here--if it was added, it's in range. */
	/* We can't check the bitmap here because this function is called
	   after the bitmap has been set but before the blockmap has. */
	*is_valid = 1;
	*was_duplicate = 0;
	*bh = bread(ip->i_sbd, block);
	q = block_type(block);
	if (blockmap_to_bitmap(q, ip->i_sbd->gfs1) == GFS2_BLKST_FREE) {
		log_debug(_("%s reference to new metadata block "
			    "%lld (0x%llx) is now marked as indirect.\n"),
			  desc, (unsigned long long)block,
			  (unsigned long long)block);
		gfs2_blockmap_set(bl, block, gfs2_indir_blk);
	}
	return meta_is_good;
}

static int alloc_data(struct gfs2_inode *ip, uint64_t metablock,
		      uint64_t block, void *private)
{
	uint8_t q;
	const char *desc = (const char *)private;

	/* No need to range_check here--if it was added, it's in range. */
	/* We can't check the bitmap here because this function is called
	   after the bitmap has been set but before the blockmap has. */
	q = block_type(block);
	if (blockmap_to_bitmap(q, ip->i_sbd->gfs1) == GFS2_BLKST_FREE) {
		log_debug(_("%s reference to new data block "
			    "%lld (0x%llx) is now marked as data.\n"),
			  desc, (unsigned long long)block,
			  (unsigned long long)block);
		gfs2_blockmap_set(bl, block, gfs2_block_used);
	}
	return 0;
}

static int alloc_leaf(struct gfs2_inode *ip, uint64_t block, void *private)
{
	uint8_t q;

	/* No need to range_check here--if it was added, it's in range. */
	/* We can't check the bitmap here because this function is called
	   after the bitmap has been set but before the blockmap has. */
	q = block_type(block);
	if (blockmap_to_bitmap(q, ip->i_sbd->gfs1) == GFS2_BLKST_FREE)
		fsck_blockmap_set(ip, block, _("newly allocated leaf"),
				  gfs2_leaf_blk);
	return 0;
}

struct metawalk_fxns alloc_fxns = {
	.private = NULL,
	.check_leaf = alloc_leaf,
	.check_metalist = alloc_metalist,
	.check_data = alloc_data,
	.check_eattr_indir = NULL,
	.check_eattr_leaf = NULL,
	.check_dentry = NULL,
	.check_eattr_entry = NULL,
	.check_eattr_extentry = NULL,
	.finish_eattr_indir = NULL,
};

/*
 * reprocess_inode - fixes the blockmap to match the bitmap due to an
 *                   unexpected block allocation via libgfs2.
 *
 * The problem we're trying to overcome here is when a new block must be
 * added to a dinode because of a write.  This will happen when lost+found
 * needs a new indirect block for its hash table.  In that case, the write
 * causes a new block to be assigned in the bitmap but that block is not yet
 * accurately reflected in the fsck blockmap.  We need to compensate here.
 *
 * We can't really use fsck_blockmap_set here because the new block
 * was already allocated by libgfs2 and therefore it took care of
 * the rgrp free space variable.  fsck_blockmap_set adjusts the free space
 * in the rgrp according to the change, which has already been done.
 * So it's only our blockmap that now disagrees with the rgrp bitmap, so we
 * need to fix only that.
 */
void reprocess_inode(struct gfs2_inode *ip, const char *desc)
{
	int error;

	alloc_fxns.private = (void *)desc;
	log_info( _("%s inode %llu (0x%llx) had blocks added; reprocessing "
		    "its metadata tree at height=%d.\n"), desc,
		  (unsigned long long)ip->i_di.di_num.no_addr,
		  (unsigned long long)ip->i_di.di_num.no_addr,
		  ip->i_di.di_height);
	error = check_metatree(ip, &alloc_fxns);
	if (error)
		log_err( _("Error %d reprocessing the %s metadata tree.\n"),
			 error, desc);
}

/*
 * write_new_leaf - allocate and write a new leaf to cover a gap in hash table
 * @dip: the directory inode
 * @start_lindex: where in the hash table to start writing
 * @num_copies: number of copies of the pointer to write into hash table
 * @before_or_after: desc. of whether this is being added before/after/etc.
 * @bn: pointer to return the newly allocated leaf's block number
 */
int write_new_leaf(struct gfs2_inode *dip, int start_lindex, int num_copies,
		   const char *before_or_after, uint64_t *bn)
{
	struct gfs2_buffer_head *nbh;
	struct gfs2_leaf *leaf;
	struct gfs2_dirent *dent;
	int count, i;
	int factor = 0, pad_size;
	uint64_t *cpyptr;
	char *padbuf;
	int divisor = num_copies;
	int end_lindex = start_lindex + num_copies;

	padbuf = malloc(num_copies * sizeof(uint64_t));
	/* calculate the depth needed for the new leaf */
	while (divisor > 1) {
		factor++;
		divisor /= 2;
	}
	/* Make sure the number of copies is properly a factor of 2 */
	if ((1 << factor) != num_copies) {
		log_err(_("Program error: num_copies not a factor of 2.\n"));
		log_err(_("num_copies=%d, dinode = %lld (0x%llx)\n"),
			num_copies,
			(unsigned long long)dip->i_di.di_num.no_addr,
			(unsigned long long)dip->i_di.di_num.no_addr);
		log_err(_("lindex = %d (0x%x)\n"), start_lindex, start_lindex);
		stack;
		free(padbuf);
		return -1;
	}

	/* allocate and write out a new leaf block */
	if (lgfs2_meta_alloc(dip, bn)) {
		log_err( _("Error: allocation failed while fixing directory leaf "
			   "pointers.\n"));
		free(padbuf);
		return -1;
	}
	fsck_blockmap_set(dip, *bn, _("directory leaf"), gfs2_leaf_blk);
	log_err(_("A new directory leaf was allocated at block %lld "
		  "(0x%llx) to fill the %d (0x%x) pointer gap %s the existing "
		  "pointer at index %d (0x%x).\n"), (unsigned long long)*bn,
		(unsigned long long)*bn, num_copies, num_copies,
		before_or_after, start_lindex, start_lindex);
	dip->i_di.di_blocks++;
	bmodified(dip->i_bh);
	nbh = bget(dip->i_sbd, *bn);
	memset(nbh->b_data, 0, dip->i_sbd->bsize);
	leaf = (struct gfs2_leaf *)nbh->b_data;
	leaf->lf_header.mh_magic = cpu_to_be32(GFS2_MAGIC);
	leaf->lf_header.mh_type = cpu_to_be32(GFS2_METATYPE_LF);
	leaf->lf_header.mh_format = cpu_to_be32(GFS2_FORMAT_LF);
	leaf->lf_depth = cpu_to_be16(dip->i_di.di_depth - factor);

	/* initialize the first dirent on the new leaf block */
	dent = (struct gfs2_dirent *)(nbh->b_data + sizeof(struct gfs2_leaf));
	dent->de_rec_len = cpu_to_be16(dip->i_sbd->bsize -
				       sizeof(struct gfs2_leaf));
	bmodified(nbh);
	brelse(nbh);

	/* pad the hash table with the new leaf block */
	cpyptr = (uint64_t *)padbuf;
	for (i = start_lindex; i < end_lindex; i++) {
		*cpyptr = cpu_to_be64(*bn);
		cpyptr++;
	}
	pad_size = num_copies * sizeof(uint64_t);
	log_err(_("Writing to the hash table of directory %lld "
		  "(0x%llx) at index: 0x%x for 0x%lx pointers.\n"),
		(unsigned long long)dip->i_di.di_num.no_addr,
		(unsigned long long)dip->i_di.di_num.no_addr,
		start_lindex, pad_size / sizeof(uint64_t));
	if (dip->i_sbd->gfs1)
		count = gfs1_writei(dip, padbuf, start_lindex *
				    sizeof(uint64_t), pad_size);
	else
		count = gfs2_writei(dip, padbuf, start_lindex *
				    sizeof(uint64_t), pad_size);
	free(padbuf);
	if (count != pad_size) {
		log_err( _("Error: bad write while fixing directory leaf "
			   "pointers.\n"));
		return -1;
	}
	return 0;
}

/* repair_leaf - Warn the user of an error and ask permission to fix it
 * Process a bad leaf pointer and ask to repair the first time.
 * The repair process involves extending the previous leaf's entries
 * so that they replace the bad ones.  We have to hack up the old
 * leaf a bit, but it's better than deleting the whole directory,
 * which is what used to happen before. */
int repair_leaf(struct gfs2_inode *ip, uint64_t *leaf_no, int lindex,
		int ref_count, const char *msg, int allow_alloc)
{
	int new_leaf_blks = 0, error, refs;
	uint64_t bn = 0;

	log_err( _("Directory Inode %llu (0x%llx) points to leaf %llu"
		   " (0x%llx) %s.\n"),
		 (unsigned long long)ip->i_di.di_num.no_addr,
		 (unsigned long long)ip->i_di.di_num.no_addr,
		 (unsigned long long)*leaf_no,
		 (unsigned long long)*leaf_no, msg);
	if (!query( _("Attempt to patch around it? (y/n) "))) {
		log_err( _("Bad leaf left in place.\n"));
		goto out;
	}
	if (!allow_alloc) {
		uint64_t *cpyptr;
		char *padbuf;
		int pad_size, i;

		padbuf = malloc(ref_count * sizeof(uint64_t));
		cpyptr = (uint64_t *)padbuf;
		for (i = 0; i < ref_count; i++) {
			*cpyptr = 0;
			cpyptr++;
		}
		pad_size = ref_count * sizeof(uint64_t);
		log_err(_("Writing zeros to the hash table of directory %lld "
			  "(0x%llx) at index: 0x%x for 0x%x pointers.\n"),
			(unsigned long long)ip->i_di.di_num.no_addr,
			(unsigned long long)ip->i_di.di_num.no_addr,
			lindex, ref_count);
		if (ip->i_sbd->gfs1)
			gfs1_writei(ip, padbuf, lindex * sizeof(uint64_t),
				    pad_size);
		else
			gfs2_writei(ip, padbuf, lindex * sizeof(uint64_t),
				    pad_size);
		free(padbuf);
		log_err( _("Directory Inode %llu (0x%llx) patched.\n"),
			 (unsigned long long)ip->i_di.di_num.no_addr,
			 (unsigned long long)ip->i_di.di_num.no_addr);
		goto out;
	}
	/* We can only write leafs in quantities that are factors of
	   two, since leaves are doubled, not added sequentially.
	   So if we have a hole that's not a factor of 2, we have to
	   break it down into separate leaf blocks that are. */
	while (ref_count) {
		refs = 1;
		while (refs <= ref_count) {
			if (refs * 2 > ref_count)
				break;
			refs *= 2;
		}
		error = write_new_leaf(ip, lindex, refs, _("replacing"), &bn);
		if (error)
			return error;

		new_leaf_blks++;
		lindex += refs;
		ref_count -= refs;
	}
	log_err( _("Directory Inode %llu (0x%llx) repaired.\n"),
		 (unsigned long long)ip->i_di.di_num.no_addr,
		 (unsigned long long)ip->i_di.di_num.no_addr);
out:
	*leaf_no = bn;
	return new_leaf_blks;
}
