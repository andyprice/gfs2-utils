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

#include <logging.h>
#include "libgfs2.h"
#include "link.h"
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
int check_n_fix_bitmap(struct lgfs2_sbd *sdp, struct rgrp_tree *rgd,
		       uint64_t blk, int error_on_dinode, int new_state)
{
	int old_state;
	int treat_as_inode = 0;
	int rewrite_rgrp = 0;
	const char *allocdesc[2][5] = { /* gfs2 descriptions */
		{"free", "data", "unlinked", "inode", "reserved"},
		/* gfs1 descriptions: */
		{"free", "data", "free meta", "metadata", "reserved"}};
	static struct rgrp_tree *prevrgd = NULL;

	if (prevrgd && rgrp_contains_block(prevrgd, blk)) {
		rgd = prevrgd;
	} else if (rgd == NULL || !rgrp_contains_block(rgd, blk)) {
		rgd = lgfs2_blk2rgrpd(sdp, blk);
		prevrgd = rgd;
	}
	old_state = lgfs2_get_bitmap(sdp, blk, rgd);
	if (old_state < 0) {
		log_err(_("Block %"PRIu64" (0x%"PRIx64") is not represented in the "
			   "system bitmap; part of an rgrp or superblock.\n"),
		        blk, blk);
		return -1;
	}
	if (old_state == new_state)
		return 0;

	if (error_on_dinode && old_state == GFS2_BLKST_DINODE &&
	    new_state != GFS2_BLKST_FREE) {
		log_debug(_("Reference as '%s' to block %"PRIu64" (0x%"PRIx64") which "
			    "was marked as dinode. Needs further investigation.\n"),
		          allocdesc[sdp->gfs1][new_state], blk, blk);
		return 1;
	}
	/* Keep these messages as short as possible, or the output gets to be
	   huge and unmanageable. */
	log_err(_("Block %"PRIu64" (0x%"PRIx64") was '%s', should be %s.\n"),
	        blk, blk, allocdesc[sdp->gfs1][old_state], allocdesc[sdp->gfs1][new_state]);
	if (!query( _("Fix the bitmap? (y/n)"))) {
		log_err( _("The bitmap inconsistency was ignored.\n"));
		return 0;
	}
	/* If the new bitmap state is free (and therefore the old state was
	   not) we have to add to the free space in the rgrp. If the old
	   bitmap state was free (and therefore it no longer is) we have to
	   subtract to the free space.  If the type changed from dinode to 
	   data or data to dinode, no change in free space. */
	lgfs2_set_bitmap(rgd, blk, new_state);
	if (new_state == GFS2_BLKST_FREE) {
		rgd->rt_free++;
		rewrite_rgrp = 1;
	} else if (old_state == GFS2_BLKST_FREE) {
		rgd->rt_free--;
		rewrite_rgrp = 1;
	}
	/* If we're freeing a dinode, get rid of the data structs for it. */
	if (old_state == GFS2_BLKST_DINODE ||
	    old_state == GFS2_BLKST_UNLINKED) {
		struct dir_info *dt;
		struct inode_info *ii;

		dt = dirtree_find(blk);
		if (dt) {
			dirtree_delete(dt);
			treat_as_inode = 1;
		}
		ii = inodetree_find(blk);
		if (ii) {
			inodetree_delete(ii);
			treat_as_inode = 1;
		} else if (!sdp->gfs1) {
			treat_as_inode = 1;
		} else if (link1_type(&nlink1map, blk) == 1) {
			/* This is a GFS1 fs (so all metadata is marked inode).
			   We need to verify it is an inode before we can decr
			   the rgrp inode count. */
			treat_as_inode = 1;
		}
		if (old_state == GFS2_BLKST_DINODE) {
			if (treat_as_inode && rgd->rt_dinodes > 0)
				rgd->rt_dinodes--;
			else if (sdp->gfs1 && rgd->rt_usedmeta > 0)
				rgd->rt_usedmeta--;
			rewrite_rgrp = 1;
		}
		link1_set(&nlink1map, blk, 0);
	} else if (new_state == GFS2_BLKST_DINODE) {
		if (!sdp->gfs1) {
			treat_as_inode = 1;
		} else {
			/* This is GFS1 (so all metadata is marked inode). We
			   need to verify it is an inode before we can decr
			   the rgrp inode count. */
			if (link1_type(&nlink1map, blk) == 1)
				treat_as_inode = 1;
			else {
				struct dir_info *dt;
				struct inode_info *ii;

				dt = dirtree_find(blk);
				if (dt)
					treat_as_inode = 1;
				else {
					ii = inodetree_find(blk);
					if (ii)
						treat_as_inode = 1;
				}
			}
		}
		if (treat_as_inode)
			rgd->rt_dinodes++;
		else if (sdp->gfs1)
			rgd->rt_usedmeta++;
		rewrite_rgrp = 1;
	}
	if (rewrite_rgrp) {
		if (sdp->gfs1)
			lgfs2_gfs_rgrp_out(rgd, rgd->bits[0].bi_data);
		else
			lgfs2_rgrp_out(rgd, rgd->bits[0].bi_data);
		rgd->bits[0].bi_modified = 1;
	}
	log_err( _("The bitmap was fixed.\n"));
	return 0;
}

/*
 * _fsck_bitmap_set - Mark a block in the bitmap, and adjust free space.
 */
int _fsck_bitmap_set(struct lgfs2_inode *ip, uint64_t bblock,
		     const char *btype, int mark,
		     int error_on_dinode, const char *caller, int fline)
{
	int error;
	static int prev_ino_addr = 0;
	static int prev_mark = 0;
	static int prevcount = 0;
	static const char *prev_caller = NULL;

	if (print_level >= MSG_DEBUG) {
		if ((ip->i_num.in_addr == prev_ino_addr) &&
		    (mark == prev_mark) && caller == prev_caller) {
			log_info("(0x%"PRIx64") ", bblock);
			prevcount++;
			if (prevcount > 10) {
				log_info("\n");
				prevcount = 0;
			}
		/* I'm circumventing the log levels here on purpose to make the
		   output easier to debug. */
		} else if (ip->i_num.in_addr == bblock) {
			if (prevcount) {
				log_info("\n");
				prevcount = 0;
			}
			printf(_("(%s:%d) %s inode found at block (0x%"PRIx64"): marking as '%s'\n"),
			       caller, fline, btype, ip->i_num.in_addr, block_type_string(mark));
		} else {
			if (prevcount) {
				log_info("\n");
				prevcount = 0;
			}
			printf(_("(%s:%d) inode (0x%"PRIx64") references %s block"
			         " (0x%"PRIx64"): marking as '%s'\n"),
			      caller, fline, ip->i_num.in_addr, btype, bblock, block_type_string(mark));
		}
		prev_ino_addr = ip->i_num.in_addr;
		prev_mark = mark;
		prev_caller = caller;
	}
	error = check_n_fix_bitmap(ip->i_sbd, ip->i_rgd, bblock,
				   error_on_dinode, mark);
	if (error < 0)
		log_err(_("This block is not represented in the bitmap.\n"));
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

struct lgfs2_inode *fsck_system_inode(struct lgfs2_sbd *sdp, uint64_t block)
{
	int j;

	if (lf_dip && lf_dip->i_num.in_addr == block)
		return lf_dip;
	if (!sdp->gfs1)
		return lgfs2_is_system_inode(sdp, block);

	if (sdp->md.statfs && block == sdp->md.statfs->i_num.in_addr)
		return sdp->md.statfs;
	if (sdp->md.jiinode && block == sdp->md.jiinode->i_num.in_addr)
		return sdp->md.jiinode;
	if (sdp->md.riinode && block == sdp->md.riinode->i_num.in_addr)
		return sdp->md.riinode;
	if (sdp->md.qinode && block == sdp->md.qinode->i_num.in_addr)
		return sdp->md.qinode;
	if (sdp->md.rooti && block == sdp->md.rooti->i_num.in_addr)
		return sdp->md.rooti;
	for (j = 0; j < sdp->md.journals; j++)
		if (sdp->md.journal && sdp->md.journal[j] &&
		    block == sdp->md.journal[j]->i_num.in_addr)
			return sdp->md.journal[j];
	return NULL;
}

/* fsck_load_inode - same as gfs2_load_inode() in libgfs2 but system inodes
   get special treatment. */
struct lgfs2_inode *fsck_load_inode(struct lgfs2_sbd *sdp, uint64_t block)
{
	struct lgfs2_inode *ip = NULL;

	ip = fsck_system_inode(sdp, block);
	if (ip)
		return ip;
	if (sdp->gfs1)
		return lgfs2_gfs_inode_read(sdp, block);
	return lgfs2_inode_read(sdp, block);
}

/* fsck_inode_get - same as inode_get() in libgfs2 but system inodes
   get special treatment. */
struct lgfs2_inode *fsck_inode_get(struct lgfs2_sbd *sdp, struct rgrp_tree *rgd,
				  struct lgfs2_buffer_head *bh)
{
	struct lgfs2_inode *sysip;
	struct lgfs2_inode *ip;

	sysip = fsck_system_inode(sdp, bh->b_blocknr);
	if (sysip)
		return sysip;

	if (sdp->gfs1)
		ip = lgfs2_gfs_inode_get(sdp, bh->b_data);
	else
		ip = lgfs2_inode_get(sdp, bh);
	if (ip) {
		ip->i_rgd = rgd;
		ip->i_bh = bh;
	}
	return ip;
}

/* fsck_inode_put - same as lgfs2_inode_put() in libgfs2 but system inodes
   get special treatment. */
void fsck_inode_put(struct lgfs2_inode **ip_in)
{
	struct lgfs2_inode *ip = *ip_in;
	struct lgfs2_inode *sysip;

	sysip = fsck_system_inode(ip->i_sbd, ip->i_num.in_addr);
	if (!sysip)
		lgfs2_inode_put(ip_in);
}

/**
 * dirent_repair - attempt to repair a corrupt directory entry.
 * @bh - The buffer header that contains the bad dirent
 * @dh - The directory entry in native format
 * @dent - The directory entry in on-disk format
 * @type - Type of directory (DIR_LINEAR or DIR_EXHASH)
 * @first - TRUE if this is the first dirent in the buffer
 *
 * This function tries to repair a corrupt directory entry.  All we
 * know at this point is that the length field is wrong.
 */
static int dirent_repair(struct lgfs2_inode *ip, struct lgfs2_buffer_head *bh,
		  struct lgfs2_dirent *d, struct gfs2_dirent *dent,
		  int type, int first)
{
	char *bh_end, *p;
	int calc_de_name_len = 0;

	/* If this is a sentinel, just fix the length and move on */
	if (first && !d->dr_inum.in_formal_ino) { /* Is it a sentinel? */
		if (type == DIR_LINEAR)
			d->dr_rec_len = ip->i_sbd->sd_bsize -
				sizeof(struct gfs2_dinode);
		else
			d->dr_rec_len = ip->i_sbd->sd_bsize -
				sizeof(struct gfs2_leaf);
	} else {
		bh_end = bh->b_data + ip->i_sbd->sd_bsize;
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
		if (!d->dr_name_len || d->dr_name_len > NAME_MAX ||
		    calc_de_name_len < d->dr_name_len) /* if dent is hosed */
			d->dr_name_len = calc_de_name_len; /* use ours */
		d->dr_rec_len = GFS2_DIRENT_SIZE(d->dr_name_len);
	}
	lgfs2_dirent_out(d, dent);
	lgfs2_bmodified(bh);
	return 0;
}

/**
 * dirblk_truncate - truncate a directory block
 */
static void dirblk_truncate(struct lgfs2_inode *ip, struct gfs2_dirent *fixb,
			    struct lgfs2_buffer_head *bh)
{
	char *bh_end;
	struct lgfs2_dirent d;

	bh_end = bh->b_data + ip->i_sbd->sd_bsize;
	/* truncate the block to save the most dentries.  To do this we
	   have to patch the previous dent. */
	lgfs2_dirent_in(&d, fixb);
	d.dr_rec_len = bh_end - (char *)fixb;
	lgfs2_dirent_out(&d, fixb);
	lgfs2_bmodified(bh);
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
static int check_entries(struct lgfs2_inode *ip, struct lgfs2_buffer_head *bh,
			 int type, uint32_t *count, int lindex,
			 struct metawalk_fxns *pass)
{
	struct gfs2_dirent *dent, *prev;
	struct lgfs2_dirent d;
	int error = 0;
	char *bh_end;
	char *filename;
	int first = 1;

	bh_end = bh->b_data + ip->i_sbd->sd_bsize;

	if (type == DIR_LINEAR) {
		dent = (struct gfs2_dirent *)(bh->b_data + sizeof(struct gfs2_dinode));
	} else {
		dent = (struct gfs2_dirent *)(bh->b_data + sizeof(struct gfs2_leaf));
		log_debug(_("Checking leaf %"PRIu64" (0x%"PRIx64")\n"),
		          bh->b_blocknr, bh->b_blocknr);
	}

	prev = NULL;
	if (!pass->check_dentry)
		return 0;

	while (1) {
		if (skip_this_pass || fsck_abort)
			return FSCK_OK;
		lgfs2_dirent_in(&d, dent);
		filename = (char *)dent + sizeof(struct gfs2_dirent);

		if (d.dr_rec_len < sizeof(struct gfs2_dirent) +
		    d.dr_name_len ||
		    (d.dr_inum.in_formal_ino && !d.dr_name_len && !first)) {
			log_err(_("Directory block %"PRIu64" (0x%"PRIx64"), "
			          "entry %d of directory %"PRIu64" (0x%"PRIx64") "
			          "is corrupt.\n"),
				bh->b_blocknr, bh->b_blocknr, (*count) + 1,
				ip->i_num.in_addr, ip->i_num.in_addr);
			if (query( _("Attempt to repair it? (y/n) "))) {
				if (dirent_repair(ip, bh, &d, dent, type,
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
		if (!d.dr_inum.in_formal_ino) {
			if (first) {
				log_debug( _("First dirent is a sentinel (place holder).\n"));
				first = 0;
			} else {
				log_err(_("Directory entry with inode number of "
					"zero in leaf %"PRIu64" (0x%"PRIx64") of "
					"directory %"PRIu64" (0x%"PRIx64")!\n"),
					bh->b_blocknr, bh->b_blocknr,
					ip->i_num.in_addr, ip->i_num.in_addr);
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
			if (!d.dr_inum.in_addr && first) { /* reverse sentinel */
				log_debug( _("First dirent is a Sentinel (place holder).\n"));
				/* Swap the two to silently make it a proper sentinel */
				d.dr_inum.in_addr = d.dr_inum.in_formal_ino;
				d.dr_inum.in_formal_ino = 0;
				lgfs2_dirent_out(&d, dent);
				lgfs2_bmodified(bh);
				/* Mark dirent buffer as modified */
				first = 0;
			} else {
				error = pass->check_dentry(ip, dent, prev, bh,
							   filename, count,
							   &lindex,
							   pass->private);
				if (error < 0) {
					stack;
					return error;
				}
			}
		}

		if ((char *)dent + d.dr_rec_len >= bh_end){
			log_debug(_("Last entry processed for %"PRIu64"->%"PRIu64
			            "(0x%"PRIx64"->0x%"PRIx64"), di_blocks=%"PRIu64".\n"),
			            ip->i_num.in_addr, bh->b_blocknr, ip->i_num.in_addr,
			            bh->b_blocknr, ip->i_blocks);
			break;
		}

		/* If we didn't clear the dentry, or if we did, but it
		 * was the first dentry, set prev  */
		if (!error || first)
			prev = dent;
		first = 0;
		dent = (struct gfs2_dirent *)((char *)dent + d.dr_rec_len);
	}
	return 0;
}

/**
 * check_leaf - check a leaf block for errors
 * Reads in the leaf block
 * Leaves the buffer around for further analysis (caller must lgfs2_brelse)
 */
int check_leaf(struct lgfs2_inode *ip, int lindex, struct metawalk_fxns *pass,
	       uint64_t *leaf_no, struct lgfs2_leaf *leaf, int *ref_count)
{
	int error = 0, fix;
	struct lgfs2_buffer_head *lbh = NULL;
	struct gfs2_leaf *lfp;
	uint32_t count = 0;
	struct lgfs2_sbd *sdp = ip->i_sbd;
	const char *msg;
	int di_depth = ip->i_depth;

	/* Make sure the block number is in range. */
	if (!valid_block_ip(ip, *leaf_no)) {
		log_err( _("Leaf block #%"PRIu64" (0x%"PRIx64") is out of range for "
			   "directory #%"PRIu64" (0x%"PRIx64") at index %d (0x%x).\n"),
			 *leaf_no, *leaf_no, ip->i_num.in_addr, ip->i_num.in_addr,
			 lindex, lindex);
		msg = _("that is out of range");
		goto bad_leaf;
	}

	/* Try to read in the leaf block. */
	lbh = lgfs2_bread(sdp, *leaf_no);
	/* Make sure it's really a valid leaf block. */
	if (lgfs2_check_meta(lbh->b_data, GFS2_METATYPE_LF)) {
		msg = _("that is not really a leaf");
		goto bad_leaf;
	}
	if (pass->check_leaf_depth)
		error = pass->check_leaf_depth(ip, *leaf_no, *ref_count, lbh);

	if (error >= 0 && pass->check_leaf) {
		error = pass->check_leaf(ip, *leaf_no, pass->private);
		if (error == -EEXIST) {
			log_info(_("Previous reference to leaf %"PRIu64" (0x%"PRIx64") "
				   "has already checked it; skipping.\n"),
			         *leaf_no, *leaf_no);
			lgfs2_brelse(lbh);
			return error;
		}
	}
	/* Early versions of GFS2 had an endianess bug in the kernel that set
	   lf_dirent_format to cpu_to_be16(GFS2_FORMAT_DE).  This was fixed
	   to use cpu_to_be32(), but we should check for incorrect values and
	   replace them with the correct value. */

	lgfs2_leaf_in(leaf, lbh->b_data);
	if (leaf->lf_dirent_format == (GFS2_FORMAT_DE << 16)) {
		log_debug( _("incorrect lf_dirent_format at leaf #%" PRIu64
			     "\n"), *leaf_no);
		leaf->lf_dirent_format = GFS2_FORMAT_DE;
		lgfs2_leaf_out(leaf, lbh->b_data);
		lgfs2_bmodified(lbh);
		log_debug( _("Fixing lf_dirent_format.\n"));
	}

	lfp = (struct gfs2_leaf *)lbh->b_data;
	/* Make sure it's really a leaf. */
	if (be32_to_cpu(lfp->lf_header.mh_type) != GFS2_METATYPE_LF) {
		log_err(_("Inode %"PRIu64" (0x%"PRIx64") points to bad leaf %"PRIu64
			  " (0x%"PRIx64").\n"),
		        ip->i_num.in_addr, ip->i_num.in_addr, *leaf_no, *leaf_no);
		msg = _("that is not a leaf");
		goto bad_leaf;
	}

	if (pass->check_dentry && is_dir(ip, sdp->gfs1)) {
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
		lgfs2_brelse(lbh);
		lbh = lgfs2_bread(sdp, *leaf_no);
		lgfs2_leaf_in(leaf, lbh->b_data);
		if (count != leaf->lf_entries) {
			log_err(_("Leaf %"PRIu64" (0x%"PRIx64") entry count in "
				   "directory %"PRIu64" (0x%"PRIx64") does not match "
				   "number of entries found - is %u, found %u\n"),
			        *leaf_no, *leaf_no, ip->i_num.in_addr, ip->i_num.in_addr,
			        leaf->lf_entries, count);
			if (query( _("Update leaf entry count? (y/n) "))) {
				leaf->lf_entries = count;
				lgfs2_leaf_out(leaf, lbh->b_data);
				lgfs2_bmodified(lbh);
				log_warn( _("Leaf entry count updated\n"));
			} else
				log_err( _("Leaf entry count left in "
					   "inconsistent state\n"));
		}
	}
out:
	if (di_depth < ip->i_depth) {
		log_debug(_("Depth of directory %"PRIu64" (0x%"PRIx64") changed from "
			    "%d to %d; adjusting ref_count from %d to %d\n"),
		          ip->i_num.in_addr, ip->i_num.in_addr, di_depth, ip->i_depth,
			  *ref_count, (*ref_count) << (ip->i_depth - di_depth));
		(*ref_count) <<= (ip->i_depth - di_depth);
	}
	lgfs2_brelse(lbh);
	if (error < 0)
		return error;
	return 0;

bad_leaf:
	if (lbh)
		lgfs2_brelse(lbh);
	if (pass->repair_leaf) {
		/* The leaf we read in is bad so we need to repair it. */
		fix = pass->repair_leaf(ip, leaf_no, lindex, *ref_count, msg);
		if (fix < 0)
			return fix;

	}
	if (di_depth < ip->i_depth) {
		log_debug(_("Depth of directory %"PRIu64" (0x%"PRIx64") changed from "
			    "%d to %d. Adjusting ref_count from %d to %d\n"),
		          ip->i_num.in_addr, ip->i_num.in_addr, di_depth, ip->i_depth,
			  *ref_count, (*ref_count) << (ip->i_depth - di_depth));
		(*ref_count) <<= (ip->i_depth - di_depth);
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

static void dir_leaf_reada(struct lgfs2_inode *ip, __be64 *tbl, unsigned hsize)
{
	uint64_t *t = alloca(hsize * sizeof(uint64_t));
	uint64_t leaf_no;
	struct lgfs2_sbd *sdp = ip->i_sbd;
	unsigned n = 0;
	unsigned i;

	for (i = 0; i < hsize; i++) {
		leaf_no = be64_to_cpu(tbl[i]);
		if (valid_block_ip(ip, leaf_no))
			t[n++] = leaf_no * sdp->sd_bsize;
	}
	qsort(t, n, sizeof(uint64_t), u64cmp);
	for (i = 0; i < n; i++)
		posix_fadvise(sdp->device_fd, t[i], sdp->sd_bsize, POSIX_FADV_WILLNEED);
}

/* Checks exhash directory entries */
int check_leaf_blks(struct lgfs2_inode *ip, struct metawalk_fxns *pass)
{
	int error = 0;
	unsigned hsize = (1 << ip->i_depth);
	uint64_t leaf_no, leaf_next;
	uint64_t first_ok_leaf, orig_di_blocks;
	struct lgfs2_buffer_head *lbh;
	int lindex;
	struct lgfs2_sbd *sdp = ip->i_sbd;
	int ref_count, orig_ref_count, orig_di_depth, orig_di_height;
	__be64 *tbl;
	int chained_leaf, tbl_valid;

	tbl = get_dir_hash(ip);
	if (tbl == NULL) {
		perror("get_dir_hash");
		return -1;
	}
	tbl_valid = 1;
	orig_di_depth = ip->i_depth;
	orig_di_height = ip->i_height;
	orig_di_blocks = ip->i_blocks;

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
		if (valid_block_ip(ip, leaf_no)) {
			lbh = lgfs2_bread(sdp, leaf_no);
			/* Make sure it's really a valid leaf block. */
			if (lgfs2_check_meta(lbh->b_data, GFS2_METATYPE_LF) == 0) {
				lgfs2_brelse(lbh);
				first_ok_leaf = leaf_no;
				break;
			}
			lgfs2_brelse(lbh);
		}
	}
	if (first_ok_leaf == -1) { /* no valid leaf found */
		log_err(_("Directory #%"PRIu64" (0x%"PRIx64") has no valid leaf blocks\n"),
		        ip->i_num.in_addr, ip->i_num.in_addr);
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
			log_debug(_("Re-reading 0x%"PRIx64" hash table.\n"), ip->i_num.in_addr);
			tbl = get_dir_hash(ip);
			if (tbl == NULL) {
				perror("get_dir_hash");
				return -1;
			}
			tbl_valid = 1;
			orig_di_depth = ip->i_depth;
			orig_di_height = ip->i_height;
			orig_di_blocks = ip->i_blocks;
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
			struct lgfs2_leaf leaf;
			if (fsck_abort) {
				free(tbl);
				posix_fadvise(sdp->device_fd, 0, 0, POSIX_FADV_NORMAL);
				return 0;
			}
			error = check_leaf(ip, lindex, pass, &leaf_no, &leaf,
					   &ref_count);
			if (ref_count != orig_ref_count) {
				log_debug(_("Ref count of leaf 0x%"PRIx64
				            " changed from %d to %d.\n"),
				          leaf_no, orig_ref_count, ref_count);
				tbl_valid = 0;
			}
			if (error < 0) {
				free(tbl);
				return error;
			}
			if (!leaf.lf_next || error)
				break;
			leaf_no = leaf.lf_next;
			chained_leaf++;
			log_debug(_("Leaf chain #%d (0x%"PRIx64") detected.\n"),
			          chained_leaf, leaf_no);
		} while (1); /* while we have chained leaf blocks */
		if (orig_di_depth != ip->i_depth) {
			log_debug(_("Depth of 0x%"PRIx64" changed from %d to %d\n"),
			          ip->i_num.in_addr, orig_di_depth, ip->i_depth);
			tbl_valid = 0;
			lindex <<= (ip->i_depth - orig_di_depth);
			hsize = (1 << ip->i_depth);
		}
		if (orig_di_height != ip->i_height) {
			log_debug(_("Height of 0x%"PRIx64" changed from %d to %d\n"),
			          ip->i_num.in_addr, orig_di_height, ip->i_height);
			tbl_valid = 0;
		}
		if (orig_di_blocks != ip->i_blocks) {
			log_debug(_("Block count of 0x%"PRIx64" changed from %"PRIu64" to %"PRIu64"\n"),
			          ip->i_num.in_addr, orig_di_blocks, ip->i_blocks);
			tbl_valid = 0;
		}
		lindex += ref_count;
	} /* for every leaf block */
	free(tbl);
	posix_fadvise(sdp->device_fd, 0, 0, POSIX_FADV_NORMAL);
	return 0;
}

static int check_eattr_entries(struct lgfs2_inode *ip,
			       struct lgfs2_buffer_head *bh,
			       struct metawalk_fxns *pass)
{
	struct gfs2_ea_header *ea_hdr, *ea_hdr_prev = NULL;
	__be64 *ea_data_ptr = NULL;
	int i;
	int error = 0, err;
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
			struct lgfs2_sbd *sdp = ip->i_sbd;

			ea_data_ptr = ((__be64 *)((char *)ea_hdr +
						    sizeof(struct gfs2_ea_header) +
						    ((ea_hdr->ea_name_len + 7) & ~7)));

			/* It is possible when a EA is shrunk
			** to have ea_num_ptrs be greater than
			** the number required for ** data.
			** In this case, the EA ** code leaves
			** the blocks ** there for **
			** reuse...........  */

			for(i = 0; i < ea_hdr->ea_num_ptrs; i++){
				err = pass->check_eattr_extentry(ip, i,
						ea_data_ptr, bh, tot_ealen,
						ea_hdr, ea_hdr_prev,
						pass->private);
				if (err)
					error = err;
				tot_ealen += sdp->sd_bsize -
					sizeof(struct gfs2_meta_header);
				ea_data_ptr++;
			}
		}
		offset += be32_to_cpu(ea_hdr->ea_rec_len);
		if (ea_hdr->ea_flags & GFS2_EAFLAG_LAST ||
		   offset >= ip->i_sbd->sd_bsize || ea_hdr->ea_rec_len == 0){
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
static int check_leaf_eattr(struct lgfs2_inode *ip, uint64_t block,
			    uint64_t parent, struct metawalk_fxns *pass)
{
	struct lgfs2_buffer_head *bh = NULL;

	if (pass->check_eattr_leaf) {
		int error = 0;

		log_debug(_("Checking EA leaf block #%"PRIu64" (0x%"PRIx64") for "
			     "inode #%"PRIu64" (0x%"PRIx64").\n"),
		          block, block, ip->i_num.in_addr, ip->i_num.in_addr);

		error = pass->check_eattr_leaf(ip, block, parent, &bh,
					       pass->private);
		if (error < 0) {
			stack;
			return -1;
		}
		if (error > 0) {
			if (bh)
				lgfs2_brelse(bh);
			return 1;
		}
		if (bh) {
			error = check_eattr_entries(ip, bh, pass);
			lgfs2_brelse(bh);
		}
		return error;
	}

	return 0;
}

/**
 * check_indirect_eattr
 * @ip: the inode the eattr comes from
 * @indirect_block
 *
 * Returns: 0 on success -1 on error
 */
static int check_indirect_eattr(struct lgfs2_inode *ip, uint64_t indirect,
				struct lgfs2_buffer_head *indirect_buf,
				struct metawalk_fxns *pass)
{
	int error = 0, err;
	__be64 *ea_leaf_ptr, *end;
	uint64_t block;
	struct lgfs2_sbd *sdp = ip->i_sbd;
	int first_ea_is_bad = 0;
	uint64_t di_eattr_save = ip->i_eattr;
	uint64_t offset = ip->i_sbd->gfs1 ? sizeof(struct gfs_indirect) : sizeof(struct gfs2_meta_header);
	int leaf_pointers = 0, leaf_pointer_errors = 0;

	ea_leaf_ptr = (__be64 *)(indirect_buf->b_data + offset);
	end = ea_leaf_ptr + ((sdp->sd_bsize - offset) / 8);

	while (*ea_leaf_ptr && (ea_leaf_ptr < end)){
		block = be64_to_cpu(*ea_leaf_ptr);
		leaf_pointers++;
		err = check_leaf_eattr(ip, block, indirect, pass);
		if (err) {
			error = err;
			log_err(_("Error detected in leaf block %"PRIu64" (0x%"PRIx64") "
				  "referenced by indirect block %"PRIu64" (0x%"PRIx64").\n"),
			        block, block, indirect, indirect);
			log_err(_("Subsequent leaf block pointers should be "
				  "cleared.\n"));
		}
		if (error) { /* leaf blocks following an error must also be
				treated as error blocks and cleared. */
			leaf_pointer_errors++;
			log_err(_("Pointer to EA leaf block %"PRIu64" (0x%"PRIx64") in "
				  "indirect block %"PRIu64" (0x%"PRIx64") should be cleared.\n"),
			        block, block, indirect, indirect);
		}
		/* If the first eattr lead is bad, we can't have a hole, so we
		   have to treat this as an unrecoverable eattr error and
		   delete all eattr info. Calling finish_eattr_indir here
		   causes ip->i_di.di_eattr = 0 and that ensures that
		   subsequent calls to check_leaf_eattr result in the eattr
		   check_leaf_block nuking them all "due to previous errors" */
		if (leaf_pointers == 1 && leaf_pointer_errors == 1) {
			first_ea_is_bad = 1;
			if (pass->finish_eattr_indir)
				pass->finish_eattr_indir(ip, leaf_pointers,
							 leaf_pointer_errors,
							 pass->private);
		} else if (leaf_pointer_errors) {
			/* This is a bit tricky.  We can't have eattr holes.
			   So if we have 4 good eattrs, 1 bad eattr and 5 more
			   good ones: GGGGBGGGGG, we need to tell
			   check_leaf_eattr to delete all eattrs after the bad
			   one. So we want: GGGG when we finish. To do that,
			   we set di_eattr to 0 temporarily. */
			ip->i_eattr = 0;
			lgfs2_bmodified(ip->i_bh);
		}
		ea_leaf_ptr++;
	}
	/* If we temporarily nuked the ea block to prevent checking past
	   a corrupt ea leaf, we need to restore the saved di_eattr block. */
	if (di_eattr_save != 0)
		ip->i_eattr = di_eattr_save;
	if (pass->finish_eattr_indir) {
		if (!first_ea_is_bad) {
			pass->finish_eattr_indir(ip, leaf_pointers,
						 leaf_pointer_errors,
						 pass->private);
		}
		if (pass->delete_block && leaf_pointer_errors &&
		    leaf_pointer_errors == leaf_pointers) {
			pass->delete_block(ip, indirect, NULL, "leaf", NULL);
			error = 1;
		}
	}

	return error;
}

/**
 * check_inode_eattr - check the EA's for a single inode
 * @ip: the inode whose EA to check
 *
 * Returns: 0 on success, -1 on error
 */
int check_inode_eattr(struct lgfs2_inode *ip, struct metawalk_fxns *pass)
{
	int error = 0;
	struct lgfs2_buffer_head *indirect_buf = NULL;

	if (!ip->i_eattr)
		return 0;

	if (ip->i_flags & GFS2_DIF_EA_INDIRECT){
		if (!pass->check_eattr_indir)
			return 0;

		log_debug(_("Checking EA indirect block #%"PRIu64" (0x%"PRIx64") for "
			     "inode #%"PRIu64" (0x%"PRIx64")..\n"),
		          ip->i_eattr, ip->i_eattr, ip->i_num.in_addr, ip->i_num.in_addr);
		error = pass->check_eattr_indir(ip, ip->i_eattr, ip->i_num.in_addr,
						&indirect_buf, pass->private);
		if (!error) {
			error = check_indirect_eattr(ip, ip->i_eattr,
						     indirect_buf, pass);
			if (error)
				stack;
		}
		if (indirect_buf)
			lgfs2_brelse(indirect_buf);
		return error;
	}
	error = check_leaf_eattr(ip, ip->i_eattr, ip->i_num.in_addr, pass);
	if (error)
		stack;

	return error;
}

/**
 * free_metalist - free all metadata on a multi-level metadata list
 */
static void free_metalist(struct lgfs2_inode *ip, osi_list_t *mlp)
{
	unsigned int height = ip->i_height;
	unsigned int i;
	struct lgfs2_buffer_head *nbh;

	for (i = 0; i <= height; i++) {
		osi_list_t *list;

		list = &mlp[i];
		while (!osi_list_empty(list)) {
			nbh = osi_list_entry(list->next,
					     struct lgfs2_buffer_head, b_altlist);
			if (nbh == ip->i_bh)
				osi_list_del_init(&nbh->b_altlist);
			else
				lgfs2_brelse(nbh);
		}
	}
}

static void file_ra(struct lgfs2_inode *ip, struct lgfs2_buffer_head *bh,
		    int head_size, int maxptrs, int h)
{
	struct lgfs2_sbd *sdp = ip->i_sbd;
	uint64_t sblock = 0, block;
	int extlen = 0;
	__be64 *p;

	if (h + 2 == ip->i_height) {
		p = (__be64 *)(bh->b_data + head_size);
		if (*p && *(p + 1)) {
			sblock = be64_to_cpu(*p);
			p++;
			block = be64_to_cpu(*p);
			extlen = block - sblock;
			if (extlen > 1 && extlen <= maxptrs) {
				posix_fadvise(sdp->device_fd,
					      sblock * sdp->sd_bsize,
					      (extlen + 1) * sdp->sd_bsize,
					      POSIX_FADV_WILLNEED);
				return;
			}
		}
		extlen = 0;
	}
	for (p = (__be64 *)(bh->b_data + head_size);
	     p < (__be64 *)(bh->b_data + sdp->sd_bsize); p++) {
		if (*p) {
			if (!sblock) {
				sblock = be64_to_cpu(*p);
				extlen = 1;
				continue;
			}
			block = be64_to_cpu(*p);
			if (block == sblock + extlen) {
				extlen++;
				continue;
			}
		}
		if (extlen && sblock) {
			if (extlen > 1)
				extlen--;
			posix_fadvise(sdp->device_fd, sblock * sdp->sd_bsize,
				      extlen * sdp->sd_bsize,
				      POSIX_FADV_WILLNEED);
			extlen = 0;
			p--;
		}
	}
	if (extlen)
		posix_fadvise(sdp->device_fd, sblock * sdp->sd_bsize,
			      extlen * sdp->sd_bsize, POSIX_FADV_WILLNEED);
}

static int do_check_metalist(struct iptr iptr, int height, struct lgfs2_buffer_head **bhp,
                             struct metawalk_fxns *pass)
{
	struct lgfs2_inode *ip = iptr.ipt_ip;
	uint64_t block = iptr_block(iptr);
	int was_duplicate = 0;
	int is_valid = 1;
	int error;

	if (pass->check_metalist == NULL)
		return 0;

	error = pass->check_metalist(iptr, bhp, height, &is_valid,
				     &was_duplicate, pass->private);
	if (error == META_ERROR) {
		stack;
		log_info("\n");
		log_info(_("Serious metadata error on block %"PRIu64" (0x%"PRIx64").\n"),
		         block, block);
		return error;
	}
	if (error == META_SKIP_FURTHER) {
		log_info("\n");
		log_info(_("Unrecoverable metadata error on block %"PRIu64" (0x%"PRIx64")\n"),
		         block, block);
		log_info(_("Further metadata will be skipped.\n"));
		return error;
	}
	if (!is_valid) {
		log_debug("Skipping rejected block %"PRIu64" (0x%"PRIx64")\n", block, block);
		if (pass->invalid_meta_is_fatal)
			return META_ERROR;
		return META_SKIP_ONE;
	}
	if (was_duplicate) {
		log_debug("Skipping duplicate %"PRIu64" (0x%"PRIx64")\n", block, block);
		return META_SKIP_ONE;
	}
	if (!valid_block_ip(ip, block)) {
		log_debug("Skipping invalid block %"PRIu64" (0x%"PRIx64")\n", block, block);
		if (pass->invalid_meta_is_fatal)
			return META_ERROR;
		return META_SKIP_ONE;
	}
	return error;
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
static int build_and_check_metalist(struct lgfs2_inode *ip, osi_list_t *mlp,
				    struct metawalk_fxns *pass)
{
	uint32_t height = ip->i_height;
	struct lgfs2_buffer_head *metabh = ip->i_bh;
	osi_list_t *prev_list, *cur_list, *tmp;
	struct iptr iptr = { .ipt_ip = ip, NULL, 0};
	int h, head_size, iblk_type;
	__be64 *undoptr;
	int maxptrs;
	int error;

	osi_list_add(&metabh->b_altlist, &mlp[0]);

	/* Directories are special.  Their 'data' is the hash table, which is
	   basically an indirect block list. Their height is not important
	   because it checks everything through the hash table using
	   "depth" field calculations. However, we still have to check the
	   indirect blocks, even if the height == 1.  */
	if (is_dir(ip, ip->i_sbd->gfs1))
		height++;

	/* if (<there are no indirect blocks to check>) */
	if (height < 2)
		return META_IS_GOOD;
	for (h = 1; h < height; h++) {
		if (h > 1) {
			if (is_dir(ip, ip->i_sbd->gfs1) &&
			    h == ip->i_height + 1)
				iblk_type = GFS2_METATYPE_JD;
			else
				iblk_type = GFS2_METATYPE_IN;
			if (ip->i_sbd->gfs1) {
				head_size = sizeof(struct gfs_indirect);
				maxptrs = (ip->i_sbd->sd_bsize - head_size) /
					sizeof(uint64_t);
			} else {
				head_size = sizeof(struct gfs2_meta_header);
				maxptrs = ip->i_sbd->sd_inptrs;
			}
		} else {
			iblk_type = GFS2_METATYPE_DI;
			head_size = sizeof(struct gfs2_dinode);
			maxptrs = ip->i_sbd->sd_diptrs;
		}
		prev_list = &mlp[h - 1];
		cur_list = &mlp[h];

		for (tmp = prev_list->next; tmp != prev_list; tmp = tmp->next) {
			iptr.ipt_off = head_size;
			iptr.ipt_bh = osi_list_entry(tmp, struct lgfs2_buffer_head, b_altlist);

			if (lgfs2_check_meta(iptr_buf(iptr), iblk_type)) {
				if (pass->invalid_meta_is_fatal)
					return META_ERROR;

				continue;
			}
			if (pass->readahead)
				file_ra(ip, iptr.ipt_bh, head_size, maxptrs, h);

			/* Now check the metadata itself */
			for (; iptr.ipt_off < ip->i_sbd->sd_bsize; iptr.ipt_off += sizeof(uint64_t)) {
				struct lgfs2_buffer_head *nbh = NULL;

				if (skip_this_pass || fsck_abort)
					return META_IS_GOOD;
				if (!iptr_block(iptr))
					continue;

				error = do_check_metalist(iptr, h, &nbh, pass);
				if (error == META_ERROR || error == META_SKIP_FURTHER)
					goto error_undo;
				if (error == META_SKIP_ONE)
					continue;
				if (!nbh)
					nbh = lgfs2_bread(ip->i_sbd, iptr_block(iptr));
				osi_list_add_prev(&nbh->b_altlist, cur_list);
			} /* for all data on the indirect block */
		} /* for blocks at that height */
	} /* for height */
	return 0;

error_undo: /* undo what we've done so far for this block */
	if (pass->undo_check_meta == NULL)
		return error;

	log_info(_("Undoing the work we did before the error on block %"PRIu64" (0x%"PRIx64").\n"),
	         iptr.ipt_bh->b_blocknr, iptr.ipt_bh->b_blocknr);
	for (undoptr = (__be64 *)(iptr_buf(iptr) + head_size);
	     undoptr < iptr_ptr(iptr) && undoptr < iptr_endptr(iptr);
	     undoptr++) {
		uint64_t block = be64_to_cpu(*undoptr);

		if (block == 0)
			continue;

		pass->undo_check_meta(ip, block, h, pass->private);
	}
	return error;
}

static unsigned int hdr_size(struct lgfs2_buffer_head *bh, unsigned int height)
{
	if (height > 1) {
		if (bh->sdp->gfs1)
			return sizeof(struct gfs_indirect);
		else
			return sizeof(struct gfs2_meta_header);
	}
	return sizeof(struct gfs2_dinode);
}

struct error_block {
	uint64_t metablk; /* metadata block where error was found */
	int metaoff; /* offset in that metadata block where error found */
	uint64_t errblk; /* error block */
};

static void report_data_error(uint64_t metablock, int offset, uint64_t block,
			      struct error_block *error_blk,
			      int rc, int error)
{
	log_info("\n");
	if (rc < 0) {
		/* A fatal error trumps a non-fatal one. */
		if ((error_blk->errblk == 0) ||
		    (rc < error)) {
			log_debug(_("Fatal error on metadata "
				    "block 0x%"PRIx64", "
				    "offset 0x%x, referencing data "
				    "block 0x%"PRIx64" "
				    "preempts non-fatal error on "
				    "block 0x%"PRIx64"\n"),
				  metablock,
				  offset,
				  block,
				  error_blk->errblk);
			error_blk->metablk = metablock;
			error_blk->metaoff = offset;
			error_blk->errblk = block;
		}
		log_info(_("Unrecoverable "));
	} else { /* nonfatal error */
		if (error_blk->errblk == 0) {
			error_blk->metablk = metablock;
			error_blk->metaoff = offset;
			error_blk->errblk = block;
		}
	}
	log_info(_("data block error %d on metadata "
		   "block %"PRId64" (0x%"PRIx64"), "
		   "offset %d (0x%x), referencing "
		   "data block %"PRId64" (0x%"PRIx64").\n"),
		 rc,
		 metablock, metablock,
		 offset, offset,
		 block, block);
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
static int metawalk_check_data(struct lgfs2_inode *ip, struct metawalk_fxns *pass,
		      struct lgfs2_buffer_head *bh, unsigned int height,
		      uint64_t *blks_checked, struct error_block *error_blk)
{
	int error = 0, rc = 0;
	uint64_t block;
	__be64 *ptr_start = (__be64 *)(bh->b_data + hdr_size(bh, height));
	__be64 *ptr_end = (__be64 *)(bh->b_data + ip->i_sbd->sd_bsize);
	__be64 *ptr;
	uint64_t metablock = bh->b_blocknr;

	/* If there isn't much pointer corruption check the pointers */
	log_debug("Processing data blocks for inode 0x%"PRIx64", metadata block 0x%"PRIx64".\n",
	          ip->i_num.in_addr, metablock);
	for (ptr = ptr_start ; ptr < ptr_end && !fsck_abort; ptr++) {
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
		rc = pass->check_data(ip, metablock, block, pass->private,
				      bh, ptr);
		if (rc && (!error || (rc < error))) {
			report_data_error(metablock, (char *)ptr - bh->b_data, block, error_blk, rc, error);
			error = rc;
		}
		if (rc < 0)
			return rc;
		(*blks_checked)++;
	}
	return error;
}

static int report_undo_data_error(uint64_t metablock, int offset, uint64_t block,
				  struct error_block *error_blk,
				  int *found_error_blk, int error)
{
	if (metablock == error_blk->metablk &&
	    offset == error_blk->metaoff &&
	    block == error_blk->errblk) {
		if (error < 0) { /* A fatal error that stopped it? */
			log_debug(_("Stopping the undo process: "
				    "fatal error block 0x%"PRIx64" was "
				    "found at metadata block 0x%"PRIx64","
				    "offset 0x%x.\n"),
				  error_blk->errblk,
				  error_blk->metablk,
				  error_blk->metaoff);
			return 1;
		}
		*found_error_blk = 1;
		log_debug(_("The non-fatal error block 0x%"PRIx64" was "
			    "found at metadata block 0x%"PRIx64", offset "
			    "0x%d, but undo processing will continue "
			    "until the end of this metadata block.\n"),
			  error_blk->errblk,
			  error_blk->metablk,
			  error_blk->metaoff);
	}
	return 0;
}

static int undo_check_data(struct lgfs2_inode *ip, struct metawalk_fxns *pass,
			   struct lgfs2_buffer_head *bh, unsigned int height,
			   struct error_block *error_blk, int error)
{
	__be64 *ptr_start = (__be64 *)(bh->b_data + hdr_size(bh, height));
	__be64 *ptr_end = (__be64 *)(bh->b_data + ip->i_sbd->sd_bsize);
	__be64 *ptr;
	uint64_t metablock = bh->b_blocknr;
	int rc = 0;
	uint64_t block;
	int found_error_blk = 0;

	/* If there isn't much pointer corruption check the pointers */
	for (ptr = ptr_start ; ptr < ptr_end && !fsck_abort; ptr++) {
		if (!*ptr)
			continue;

		if (skip_this_pass || fsck_abort)
			return 1;
		block =  be64_to_cpu(*ptr);
		if (report_undo_data_error(metablock, (char *)ptr - bh->b_data,
					   block, error_blk, &found_error_blk, error))
			return 1;
		rc = pass->undo_check_data(ip, block, pass->private);
		if (rc < 0)
			return rc;
	}
	return found_error_blk;
}

static unsigned int should_check(struct lgfs2_buffer_head *bh, unsigned int height)
{
	int iblk_type = height > 1 ? GFS2_METATYPE_IN : GFS2_METATYPE_DI;

	return lgfs2_check_meta(bh->b_data, iblk_type) == 0;
}

/**
 * check_metatree
 * @ip: inode structure in memory
 * @pass: structure passed in from caller to determine the sub-functions
 *
 */
int check_metatree(struct lgfs2_inode *ip, struct metawalk_fxns *pass)
{
	unsigned int height = ip->i_height;
	osi_list_t *metalist = alloca((height + 1) * sizeof(*metalist));
	osi_list_t *list, *tmp;
	struct lgfs2_buffer_head *bh;
	unsigned int i;
	uint64_t blks_checked = 0;
	int error, rc;
	int metadata_clean = 0;
	struct error_block error_blk = {0, 0, 0};
	int hit_error_blk = 0;

	if (!height && !is_dir(ip, ip->i_sbd->gfs1))
		return 0;

	/* metalist has one extra element for directories (see build_and_check_metalist). */
	for (i = 0; i <= height; i++)
		osi_list_init(&metalist[i]);

	/* create and check the metadata list for each height */
	error = build_and_check_metalist(ip, metalist, pass);
	if (error) {
		stack;
		goto undo_metalist;
	}

	metadata_clean = 1;
	/* For directories, we've already checked the "data" blocks which
	 * comprise the directory hash table, so we perform the directory
	 * checks and exit. */
        if (is_dir(ip, ip->i_sbd->gfs1)) {
		if (!(ip->i_flags & GFS2_DIF_EXHASH))
			goto out;
		/* check validity of leaf blocks and leaf chains */
		error = check_leaf_blks(ip, pass);
		if (error)
			goto undo_metalist;
		goto out;
	}

	/* check data blocks */
	list = &metalist[height - 1];
	if (ip->i_blocks > COMFORTABLE_BLKS)
		last_reported_fblock = -10000000;

	for (tmp = list->next; !error && tmp != list; tmp = tmp->next) {
		if (fsck_abort) {
			free_metalist(ip, metalist);
			return 0;
		}
		bh = osi_list_entry(tmp, struct lgfs2_buffer_head, b_altlist);
		if (!should_check(bh, height))
			continue;

		if (pass->check_data)
			error = metawalk_check_data(ip, pass, bh, height,
					   &blks_checked, &error_blk);
		if (pass->big_file_msg && ip->i_blocks > COMFORTABLE_BLKS)
			pass->big_file_msg(ip, blks_checked);
	}
	if (pass->big_file_msg && ip->i_blocks > COMFORTABLE_BLKS) {
		log_notice( _("\rLarge file at %"PRIu64" (0x%"PRIx64") - 100 percent "
			      "complete.                                   "
			      "\n"),
			    ip->i_num.in_addr, ip->i_num.in_addr);
		fflush(stdout);
	}
undo_metalist:
	if (!error)
		goto out;
	log_err(_("Error: inode %"PRIu64" (0x%"PRIx64") had unrecoverable errors at "
	          "metadata block %"PRIu64" (0x%"PRIx64"), offset %d (0x%x), block "
	          "%"PRIu64" (0x%"PRIx64").\n"),
	        ip->i_num.in_addr, ip->i_num.in_addr, error_blk.metablk, error_blk.metablk,
		error_blk.metaoff, error_blk.metaoff, error_blk.errblk, error_blk.errblk);
	if (!query( _("Remove the invalid inode? (y/n) "))) {
		free_metalist(ip, metalist);
		log_err(_("Invalid inode not deleted.\n"));
		return error;
	}
	for (i = 0; pass->undo_check_meta && i < height; i++) {
		while (!osi_list_empty(&metalist[i])) {
			list = &metalist[i];
			bh = osi_list_entry(list->next,
					    struct lgfs2_buffer_head,
					    b_altlist);
			log_err(_("Undoing metadata work for block %"PRIu64" (0x%"PRIx64")\n"),
			        bh->b_blocknr, bh->b_blocknr);
			if (i)
				rc = pass->undo_check_meta(ip, bh->b_blocknr,
							   i, pass->private);
			else
				rc = 0;
			if (metadata_clean && rc == 0 && i == height - 1 &&
			    !hit_error_blk) {
				if (should_check(bh, height)) {
					rc = undo_check_data(ip, pass,
							     bh,
							     height,
							     &error_blk,
							     error);
					if (rc > 0) {
						hit_error_blk = 1;
						log_err("Reached the error "
							"block undoing work "
							"for inode %"PRIu64" "
							"(0x%"PRIx64").\n",
							ip->i_num.in_addr, ip->i_num.in_addr);
						rc = 0;
					}
				}
			}
			if (bh == ip->i_bh)
				osi_list_del(&bh->b_altlist);
			else
				lgfs2_brelse(bh);
		}
	}
	/* There may be leftover duplicate records, so we need to delete them.
	   For example, if a metadata block was found to be a duplicate, we
	   may not have added it to the metalist, which means it's not there
	   to undo. */
	delete_all_dups(ip);
	/* Set the dinode as "bad" so it gets deleted */
	fsck_bitmap_set(ip, ip->i_num.in_addr, "corrupt", GFS2_BLKST_FREE);
	log_err(_("The corrupt inode was invalidated.\n"));
out:
	free_metalist(ip, metalist);
	return error;
}

/* Checks stuffed inode directories */
int check_linear_dir(struct lgfs2_inode *ip, struct lgfs2_buffer_head *bh,
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

int check_dir(struct lgfs2_sbd *sdp, struct lgfs2_inode *ip, struct metawalk_fxns *pass)
{
	int error = 0;

	if (ip->i_flags & GFS2_DIF_EXHASH)
		error = check_leaf_blks(ip, pass);
	else
		error = check_linear_dir(ip, ip->i_bh, pass);

	if (error < 0)
		stack;

	return error;
}
