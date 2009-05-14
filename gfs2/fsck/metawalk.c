#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <libintl.h>
#define _(String) gettext(String)

#include "libgfs2.h"
#include "fsck.h"
#include "util.h"
#include "metawalk.h"
#include "hash.h"

struct gfs2_inode *get_system_inode(struct gfs2_sbd *sbp, uint64_t block)
{
	int j;

	if (block == sbp->md.inum->i_di.di_num.no_addr)
		return sbp->md.inum;
	if (block == sbp->md.statfs->i_di.di_num.no_addr)
		return sbp->md.statfs;
	if (block == sbp->md.jiinode->i_di.di_num.no_addr)
		return sbp->md.jiinode;
	if (block == sbp->md.riinode->i_di.di_num.no_addr)
		return sbp->md.riinode;
	if (block == sbp->md.qinode->i_di.di_num.no_addr)
		return sbp->md.qinode;
	if (block == sbp->md.pinode->i_di.di_num.no_addr)
		return sbp->md.pinode;
	if (block == sbp->md.rooti->i_di.di_num.no_addr)
		return sbp->md.rooti;
	if (block == sbp->master_dir->i_di.di_num.no_addr)
		return sbp->master_dir;
	if (lf_dip && block == lf_dip->i_di.di_num.no_addr)
		return lf_dip;
	for (j = 0; j < sbp->md.journals; j++)
		if (block == sbp->md.journal[j]->i_di.di_num.no_addr)
			return sbp->md.journal[j];
	return NULL;
}

/* fsck_load_inode - same as gfs2_load_inode() in libgfs2 but system inodes
   get special treatment. */
struct gfs2_inode *fsck_load_inode(struct gfs2_sbd *sbp, uint64_t block)
{
	struct gfs2_inode *ip = NULL;

	ip = get_system_inode(sbp, block);
	if (ip) {
		bhold(ip->i_bh);
		return ip;
	}
	return gfs2_load_inode(sbp, block);
}

/* fsck_inode_get - same as inode_get() in libgfs2 but system inodes
   get special treatment. */
struct gfs2_inode *fsck_inode_get(struct gfs2_sbd *sdp,
				  struct gfs2_buffer_head *bh)
{
	struct gfs2_inode *ip, *sysip;

	ip = calloc(1, sizeof(struct gfs2_inode));
	if (ip == NULL) {
		fprintf(stderr, _("Out of memory in %s\n"), __FUNCTION__);
		exit(-1);
	}
	gfs2_dinode_in(&ip->i_di, bh->b_data);
	ip->i_bh = bh;
	ip->i_sbd = sdp;

	sysip = get_system_inode(sdp, ip->i_di.di_num.no_addr);
	if (sysip) {
		free(ip);
		return sysip;
	}
	return ip;
}

/* fsck_inode_put - same as inode_put() in libgfs2 but system inodes
   get special treatment. */
void fsck_inode_put(struct gfs2_inode *ip, enum update_flags update)
{
	struct gfs2_inode *sysip;

	sysip = get_system_inode(ip->i_sbd, ip->i_di.di_num.no_addr);
	if (sysip) {
		if (update)
			gfs2_dinode_out(&ip->i_di, ip->i_bh->b_data);
		brelse(ip->i_bh, update);
	} else {
		inode_put(ip, update);
	}
}

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
	}
	else {
		bh_end = bh->b_data + ip->i_sbd->bsize;
		/* first, figure out a probable name length */
		p = (char *)dent + sizeof(struct gfs2_dirent);
		while (*p &&         /* while there's a non-zero char and */
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
	return 0;
}

static int check_entries(struct gfs2_inode *ip, struct gfs2_buffer_head *bh,
		  int eindex, int type, enum update_flags *update,
		  uint16_t *count, struct metawalk_fxns *pass)
{
	struct gfs2_leaf *leaf = NULL;
	struct gfs2_dirent *dent;
	struct gfs2_dirent de, *prev;
	int error = 0;
	char *bh_end;
	char *filename;
	int first = 1;

	bh_end = bh->b_data + ip->i_sbd->bsize;

	if(type == DIR_LINEAR) {
		dent = (struct gfs2_dirent *)(bh->b_data + sizeof(struct gfs2_dinode));
	}
	else if (type == DIR_EXHASH) {
		dent = (struct gfs2_dirent *)(bh->b_data + sizeof(struct gfs2_leaf));
		leaf = (struct gfs2_leaf *)bh->b_data;
		log_debug( _("Checking leaf %" PRIu64 " (0x%" PRIx64 ")\n"),
				  bh->b_blocknr, bh->b_blocknr);
	}
	else {
		log_err( _("Invalid directory type %d specified\n"), type);
		return -1;
	}

	prev = NULL;
	if(!pass->check_dentry)
		return 0;

	while(1) {
		memset(&de, 0, sizeof(struct gfs2_dirent));
		gfs2_dirent_in(&de, (char *)dent);
		filename = (char *)dent + sizeof(struct gfs2_dirent);

		if (de.de_rec_len < sizeof(struct gfs2_dirent) +
		    de.de_name_len || !de.de_name_len) {
			log_err( _("Directory block %llu (0x%llx"
				"), entry %d of directory %llu"
				"(0x%llx) is corrupt.\n"),
				(unsigned long long)bh->b_blocknr,
				(unsigned long long)bh->b_blocknr,
				(*count) + 1,
				(unsigned long long)ip->i_di.di_num.no_addr,
				(unsigned long long)ip->i_di.di_num.no_addr);
			errors_found++;
			if (query(&opts, _("Attempt to repair it? (y/n) "))) {
				if (dirent_repair(ip, bh, &de, dent, type,
						  first))
					break;
				else {
					errors_corrected++;
					*update = updated;
				}
			}
			else {
				log_err( _("Corrupt directory entry ignored, "
					"stopped after checking %d entries.\n"),
					*count);
				break;
			}
		}
		if (!de.de_inum.no_formal_ino){
			if(first){
				log_debug( _("First dirent is a sentinel (place holder).\n"));
				first = 0;
			} else {
				/* FIXME: Do something about this */
				log_err( _("Directory entry with inode number of "
					"zero in leaf %llu (0x%llx) of "
					"directory %llu (0x%llx)!\n"),
					(unsigned long long)bh->b_blocknr,
					(unsigned long long)bh->b_blocknr,
					(unsigned long long)ip->i_di.di_num.no_addr,
					(unsigned long long)ip->i_di.di_num.no_addr);
				return 1;
			}
		} else {
			if (!de.de_inum.no_addr && first) { /* reverse sentinel */
				log_debug( _("First dirent is a Sentinel (place holder).\n"));
				/* Swap the two to silently make it a proper sentinel */
				de.de_inum.no_addr = de.de_inum.no_formal_ino;
				de.de_inum.no_formal_ino = 0;
				gfs2_dirent_out(&de, (char *)dent);
				*update = (opts.no ? not_updated : updated);
				/* Mark dirent buffer as modified */
				first = 0;
			}
			else {
				error = pass->check_dentry(ip, dent, prev, bh,
							   filename, update,
							   count,
							   pass->private);
				if(error < 0) {
					stack;
					return -1;
				}
				/*if(error > 0) {
				  return 1;
				  }*/
			}
		}

		if ((char *)dent + de.de_rec_len >= bh_end){
			log_debug( _("Last entry processed.\n"));
			break;
		}

		/* If we didn't clear the dentry, or if we did, but it
		 * was the first dentry, set prev  */
		if(!error || first)
			prev = dent;
		first = 0;
		dent = (struct gfs2_dirent *)((char *)dent + de.de_rec_len);
	}
	return 0;
}

/* Process a bad leaf pointer and ask to repair the first time.      */
/* The repair process involves extending the previous leaf's entries */
/* so that they replace the bad ones.  We have to hack up the old    */
/* leaf a bit, but it's better than deleting the whole directory,    */
/* which is what used to happen before.                              */
static void warn_and_patch(struct gfs2_inode *ip, uint64_t *leaf_no, 
		    uint64_t *bad_leaf, uint64_t old_leaf, int pindex,
		    const char *msg)
{
	if (*bad_leaf != *leaf_no) {
		log_err( _("Directory Inode %llu (0x%llx) points to leaf %llu"
			" (0x%llx) %s.\n"),
			(unsigned long long)ip->i_di.di_num.no_addr,
			(unsigned long long)ip->i_di.di_num.no_addr,
			(unsigned long long)*leaf_no,
			(unsigned long long)*leaf_no, msg);
	}
	errors_found++;
	if (*leaf_no == *bad_leaf ||
	    query(&opts, _("Attempt to patch around it? (y/n) "))) {
		errors_corrected++;
		gfs2_put_leaf_nr(ip, pindex, old_leaf);
	}
	else
		log_err( _("Bad leaf left in place.\n"));
	*bad_leaf = *leaf_no;
	*leaf_no = old_leaf;
}

/* Checks exhash directory entries */
static int check_leaf(struct gfs2_inode *ip, enum update_flags *update,
	       struct metawalk_fxns *pass)
{
	int error;
	struct gfs2_leaf leaf, oldleaf;
	uint64_t leaf_no, old_leaf, bad_leaf = -1;
	struct gfs2_buffer_head *lbh;
	int lindex;
	struct gfs2_sbd *sbp = ip->i_sbd;
	uint16_t count;
	int ref_count = 0, exp_count = 0;

	old_leaf = 0;
	memset(&oldleaf, 0, sizeof(oldleaf));
	for(lindex = 0; lindex < (1 << ip->i_di.di_depth); lindex++) {
		gfs2_get_leaf_nr(ip, lindex, &leaf_no);

		/* GFS has multiple indirect pointers to the same leaf
		 * until those extra pointers are needed, so skip the
		 * dups */
		if (leaf_no == bad_leaf) {
			gfs2_put_leaf_nr(ip, lindex, old_leaf); /* fill w/old
								  leaf info */
			ref_count++;
			continue;
		}
		else if(old_leaf == leaf_no) {
			ref_count++;
			continue;
		} else {
			if(ref_count != exp_count){
				log_err( _("Dir #%llu (0x%llx) has an incorrect "
					"number of pointers to leaf #%llu "
					" (0x%llx)\n\tFound: %u,  Expected: "
					"%u\n"), (unsigned long long)
					ip->i_di.di_num.no_addr,
					(unsigned long long)
					ip->i_di.di_num.no_addr,
					(unsigned long long)old_leaf,
					(unsigned long long)old_leaf,
					ref_count, exp_count);
				errors_found++;
				if (query(&opts, _("Attempt to fix it? (y/n) ")))
				{
					int factor = 0, divisor = ref_count;

					errors_corrected++;
					lbh = bread(&sbp->buf_list, old_leaf);
					while (divisor > 1) {
						factor++;
						divisor /= 2;
					}
					oldleaf.lf_depth = ip->i_di.di_depth -
						factor;
					gfs2_leaf_out(&oldleaf, lbh->b_data);
					brelse(lbh, updated);
				}
				else
					return 1;
			}
			ref_count = 1;
		}

		count = 0;
		do {
			/* Make sure the block number is in range. */
			if(gfs2_check_range(ip->i_sbd, leaf_no)){
				log_err( _("Leaf block #%llu (0x%llx) is out "
					"of range for directory #%llu (0x%llx"
					").\n"), (unsigned long long)leaf_no,
					(unsigned long long)leaf_no,
					(unsigned long long)
					ip->i_di.di_num.no_addr,
					(unsigned long long)
					ip->i_di.di_num.no_addr);
				warn_and_patch(ip, &leaf_no, &bad_leaf,
					       old_leaf, lindex,
					       _("that is out of range"));
				memcpy(&leaf, &oldleaf, sizeof(oldleaf));
				break;
			}

			*update = not_updated;
			/* Try to read in the leaf block. */
			lbh = bread(&sbp->buf_list, leaf_no);
			/* Make sure it's really a valid leaf block. */
			if (gfs2_check_meta(lbh, GFS2_METATYPE_LF)) {
				warn_and_patch(ip, &leaf_no, &bad_leaf,
					       old_leaf, lindex,
					       _("that is not really a leaf"));
				memcpy(&leaf, &oldleaf, sizeof(oldleaf));
				brelse(lbh, (opts.no ? not_updated : updated));
				break;
			}
			gfs2_leaf_in(&leaf, lbh->b_data);
			if(pass->check_leaf) {
				error = pass->check_leaf(ip, leaf_no, lbh,
							 pass->private);
			}

			/*
			 * Early versions of GFS2 had an endianess bug in the
			 * kernel that set lf_dirent_format to
			 * cpu_to_be16(GFS2_FORMAT_DE).  This was fixed to use
			 * cpu_to_be32(), but we should check for incorrect 
			 * values and replace them with the correct value. */

			if (leaf.lf_dirent_format == (GFS2_FORMAT_DE << 16)) {
				log_debug( _("incorrect lf_dirent_format at leaf #%" PRIu64 "\n"), leaf_no);
				leaf.lf_dirent_format = GFS2_FORMAT_DE;
				gfs2_leaf_out(&leaf, lbh->b_data);
				log_debug( _("Fixing lf_dirent_format.\n"));
				*update = (opts.no ? not_updated : updated);
			}

			/* Make sure it's really a leaf. */
			if (leaf.lf_header.mh_type != GFS2_METATYPE_LF) {
				log_err( _("Inode %llu (0x%llx"
					") points to bad leaf %llu"
					" (0x%llx).\n"),
					(unsigned long long)
					ip->i_di.di_num.no_addr,
					(unsigned long long)
					ip->i_di.di_num.no_addr,
					(unsigned long long)leaf_no,
					(unsigned long long)leaf_no);
				brelse(lbh, *update);
				break;
			}
			exp_count = (1 << (ip->i_di.di_depth - leaf.lf_depth));
			log_debug( _("expected count %u - di_depth %u, leaf depth %u\n"),
					  exp_count, ip->i_di.di_depth, leaf.lf_depth);

			if(pass->check_dentry &&
			   S_ISDIR(ip->i_di.di_mode)) {
				error = check_entries(ip, lbh, lindex,
						      DIR_EXHASH, update,
						      &count, pass);

				/* Since the buffer possibly got
				 * updated directly, release it now,
				 * and grab it again later if we need it. */

				brelse(lbh, *update);

				if(error < 0) {
					stack;
					return -1;
				}

				if(error > 0)
					return 1;

				if(update && (count != leaf.lf_entries)) {
					enum update_flags f = not_updated;

					lbh = bread(&sbp->buf_list, leaf_no);
					gfs2_leaf_in(&leaf, lbh->b_data);

					log_err( _("Leaf %llu (0x%llx) entry "
						"count in directory %llu"
						" (0x%llx) doesn't match "
						"number of entries found "
						"- is %u, found %u\n"),
						(unsigned long long)leaf_no,
						(unsigned long long)leaf_no,
						(unsigned long long)
						ip->i_di.di_num.no_addr,
						(unsigned long long)
						ip->i_di.di_num.no_addr,
						leaf.lf_entries, count);
					errors_found++;
					if(query(&opts, _("Update leaf entry count? (y/n) "))) {
						errors_corrected++;
						leaf.lf_entries = count;
						gfs2_leaf_out(&leaf, lbh->b_data);
						log_warn( _("Leaf entry count updated\n"));
						f = updated;
					} else
						log_err( _("Leaf entry count left in inconsistant state\n"));
					brelse(lbh, f);
				}
				/* FIXME: Need to get entry count and
				 * compare it against leaf->lf_entries */
				break;
			} else {
				brelse(lbh, *update);
				if(!leaf.lf_next)
					break;
				leaf_no = leaf.lf_next;
				log_debug( _("Leaf chain detected.\n"));
			}
		} while(1);
		old_leaf = leaf_no;
		memcpy(&oldleaf, &leaf, sizeof(oldleaf));
	}
	return 0;
}

static int check_eattr_entries(struct gfs2_inode *ip,
			       struct gfs2_buffer_head *bh,
			       struct metawalk_fxns *pass,
			       enum update_flags *update_it)
{
	struct gfs2_ea_header *ea_hdr, *ea_hdr_prev = NULL;
	uint64_t *ea_data_ptr = NULL;
	int i;
	int error = 0;
	uint32_t offset = (uint32_t)sizeof(struct gfs2_meta_header);

	*update_it = 0;
	if(!pass->check_eattr_entry) {
		return 0;
	}

	ea_hdr = (struct gfs2_ea_header *)(bh->b_data +
					  sizeof(struct gfs2_meta_header));

	while(1){
		error = pass->check_eattr_entry(ip, bh, ea_hdr, ea_hdr_prev,
						pass->private);
		if(error < 0) {
			stack;
			return -1;
		}
		if(error == 0 && pass->check_eattr_extentry &&
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
				if(pass->check_eattr_extentry(ip,
							      ea_data_ptr,
							      bh, ea_hdr,
							      ea_hdr_prev,
							      update_it,
							      pass->private)) {
					errors_found++;
					if (query(&opts, _("Repair the bad EA? "
						  "(y/n) "))) {
						errors_corrected++;
						ea_hdr->ea_num_ptrs = i;
						ea_hdr->ea_data_len =
							cpu_to_be32(tot_ealen);
						*ea_data_ptr = 0;
						*update_it = 1;
						/* Endianness doesn't matter
						   in this case because it's
						   a single byte. */
						return -1;
					}
					log_err( _("The bad EA was not fixed.\n"));
				}
				tot_ealen += sdp->sd_sb.sb_bsize -
					sizeof(struct gfs2_meta_header);
				ea_data_ptr++;
			}
		}
		offset += be32_to_cpu(ea_hdr->ea_rec_len);
		if(ea_hdr->ea_flags & GFS2_EAFLAG_LAST ||
		   offset >= ip->i_sbd->sd_sb.sb_bsize || ea_hdr->ea_rec_len == 0){
			break;
		}
		ea_hdr_prev = ea_hdr;
		ea_hdr = (struct gfs2_ea_header *)
			((char *)(ea_hdr) +
			 be32_to_cpu(ea_hdr->ea_rec_len));
	}

	return 0;
}

/**
 * check_leaf_eattr
 * @ip: the inode the eattr comes from
 * @block: block number of the leaf
 *
 * Returns: 0 on success, -1 if removal is needed
 */
static int check_leaf_eattr(struct gfs2_inode *ip, uint64_t block,
			    uint64_t parent, enum update_flags *want_updated,
			    struct metawalk_fxns *pass)
{
	struct gfs2_buffer_head *bh = NULL;
	int error = 0;

	log_debug( _("Checking EA leaf block #%"PRIu64" (0x%" PRIx64 ").\n"),
			  block, block);

	if(pass->check_eattr_leaf) {
		error = pass->check_eattr_leaf(ip, block, parent, &bh,
					       want_updated, pass->private);
		if(error < 0) {
			stack;
			return -1;
		}
		if(error > 0) {
			return 1;
		}
		check_eattr_entries(ip, bh, pass, want_updated);
		if (bh)
			brelse(bh, *want_updated);
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
static int check_indirect_eattr(struct gfs2_inode *ip, uint64_t indirect,
				enum update_flags *want_updated,
				struct metawalk_fxns *pass){
	int error = 0;
	uint64_t *ea_leaf_ptr, *end;
	uint64_t block;
	struct gfs2_buffer_head *indirect_buf = NULL;
	struct gfs2_sbd *sdp = ip->i_sbd;

	*want_updated = not_updated;
	log_debug( _("Checking EA indirect block #%"PRIu64" (0x%" PRIx64 ").\n"),
			  indirect, indirect);

	if (!pass->check_eattr_indir)
		return 0;
	error = pass->check_eattr_indir(ip, indirect, ip->i_di.di_num.no_addr,
					&indirect_buf, want_updated,
					pass->private);
	if (!error) {
		int leaf_pointers = 0, leaf_pointer_errors = 0;

		ea_leaf_ptr = (uint64_t *)(indirect_buf->b_data
								   + sizeof(struct gfs2_meta_header));
		end = ea_leaf_ptr + ((sdp->sd_sb.sb_bsize
							  - sizeof(struct gfs2_meta_header)) / 8);

		while(*ea_leaf_ptr && (ea_leaf_ptr < end)){
			block = be64_to_cpu(*ea_leaf_ptr);
			leaf_pointers++;
			error = check_leaf_eattr(ip, block, indirect,
						 want_updated, pass);
			if (error)
				leaf_pointer_errors++;
			ea_leaf_ptr++;
		}
		if (pass->finish_eattr_indir) {
			int indir_ok = 1;

			if (leaf_pointer_errors == leaf_pointers)
				indir_ok = 0;
			pass->finish_eattr_indir(ip, indir_ok, want_updated,
						 pass->private);
			if (!indir_ok) {
				if (*want_updated)
					gfs2_set_bitmap(sdp, indirect,
							GFS2_BLKST_FREE);
				gfs2_block_clear(sdp, bl, indirect,
						 gfs2_indir_blk);
				gfs2_block_set(sdp, bl, indirect,
					       gfs2_block_free);
				error = 1;
			}
		}
	}
	if (indirect_buf)
		brelse(indirect_buf, not_updated);

	return error;
}

/**
 * check_inode_eattr - check the EA's for a single inode
 * @ip: the inode whose EA to check
 *
 * Returns: 0 on success, -1 on error
 */
int check_inode_eattr(struct gfs2_inode *ip, enum update_flags *want_updated,
		      struct metawalk_fxns *pass)
{
	int error = 0;

	if(!ip->i_di.di_eattr){
		return 0;
	}

	log_debug( _("Extended attributes exist for inode #%llu (0x%llx).\n"),
		  (unsigned long long)ip->i_di.di_num.no_addr,
		  (unsigned long long)ip->i_di.di_num.no_addr);

	if(ip->i_di.di_flags & GFS2_DIF_EA_INDIRECT){
		if((error = check_indirect_eattr(ip, ip->i_di.di_eattr,
						 want_updated, pass)))
			stack;
	} else {
		if((error = check_leaf_eattr(ip, ip->i_di.di_eattr,
					     ip->i_di.di_num.no_addr,
					     want_updated, pass)))
			stack;
	}

	return error;
}

/**
 * build_and_check_metalist - check a bunch of indirect blocks
 * Note: Every buffer put on the metalist should be "held".
 * @ip:
 * @mlp:
 */
static int build_and_check_metalist(struct gfs2_inode *ip,
				    osi_list_t *mlp,
				    struct metawalk_fxns *pass)
{
	uint32_t height = ip->i_di.di_height;
	struct gfs2_buffer_head *bh, *nbh, *metabh;
	osi_list_t *prev_list, *cur_list, *tmp;
	int i, head_size;
	uint64_t *ptr, block;
	int err;

	metabh = bread(&ip->i_sbd->buf_list, ip->i_di.di_num.no_addr);

	osi_list_add(&metabh->b_altlist, &mlp[0]);

	/* if(<there are no indirect blocks to check>) */
	if (height < 2)
		return 0;
	for (i = 1; i < height; i++){
		prev_list = &mlp[i - 1];
		cur_list = &mlp[i];

		for (tmp = prev_list->next; tmp != prev_list; tmp = tmp->next){
			bh = osi_list_entry(tmp, struct gfs2_buffer_head,
					    b_altlist);

			head_size = (i > 1 ?
				     sizeof(struct gfs2_meta_header) :
				     sizeof(struct gfs2_dinode));

			for (ptr = (uint64_t *)(bh->b_data + head_size);
			     (char *)ptr < (bh->b_data + ip->i_sbd->bsize);
			     ptr++) {
				nbh = NULL;
		
				if (!*ptr)
					continue;

				block = be64_to_cpu(*ptr);
				err = pass->check_metalist(ip, block, &nbh,
							   pass->private);
				/* check_metalist should hold any buffers
				   it gets with "bread". */
				if(err < 0) {
					stack;
					goto fail;
				}
				if(err > 0) {
					log_debug( _("Skipping block %" PRIu64
						  " (0x%" PRIx64 ")\n"),
						  block, block);
					continue;
				}
				if(!nbh)
					nbh = bread(&ip->i_sbd->buf_list,
						    block);

				osi_list_add(&nbh->b_altlist, cur_list);
			} /* for all data on the indirect block */
		} /* for blocks at that height */
	} /* for height */
	return 0;
fail:
	for (i = 0; i < GFS2_MAX_META_HEIGHT; i++) {
		osi_list_t *list;
		list = &mlp[i];
		while (!osi_list_empty(list)) {
			nbh = osi_list_entry(list->next,
					     struct gfs2_buffer_head, b_altlist);
			osi_list_del(&nbh->b_altlist);
		}
	}
	/* This is an error path, so we need to release the buffer here: */
	brelse(metabh, not_updated);
	return -1;
}

/**
 * check_metatree
 * @ip:
 * @rgd:
 *
 */
int check_metatree(struct gfs2_inode *ip, struct metawalk_fxns *pass)
{
	osi_list_t metalist[GFS2_MAX_META_HEIGHT];
	osi_list_t *list, *tmp;
	struct gfs2_buffer_head *bh;
	uint64_t block, *ptr;
	uint32_t height = ip->i_di.di_height;
	int  i, head_size;
	enum update_flags update = not_updated;
	int error = 0;

	if (!height)
		goto end;

	for (i = 0; i < GFS2_MAX_META_HEIGHT; i++)
		osi_list_init(&metalist[i]);

	/* create metalist for each level */
	if (build_and_check_metalist(ip, &metalist[0], pass)){
		stack;
		return -1;
	}

	/* We don't need to record directory blocks - they will be
	 * recorded later...i think... */
        if (S_ISDIR(ip->i_di.di_mode))
		log_debug( _("Directory with height > 0 at %llu (0x%llx)\n"),
			  (unsigned long long)ip->i_di.di_num.no_addr,
			  (unsigned long long)ip->i_di.di_num.no_addr);

	/* check data blocks */
	list = &metalist[height - 1];

	for (tmp = list->next; tmp != list; tmp = tmp->next) {
		bh = osi_list_entry(tmp, struct gfs2_buffer_head, b_altlist);

		head_size = (height != 1 ? sizeof(struct gfs2_meta_header) :
			     sizeof(struct gfs2_dinode));
		ptr = (uint64_t *)(bh->b_data + head_size);

		for ( ; (char *)ptr < (bh->b_data + ip->i_sbd->bsize); ptr++) {
			if (!*ptr)
				continue;

			block =  be64_to_cpu(*ptr);

			if(pass->check_data &&
			   (pass->check_data(ip, block, pass->private) < 0)) {
				stack;
				return -1;
			}
		}
	}

	/* free metalists */
	for (i = 0; i < GFS2_MAX_META_HEIGHT; i++)
	{
		list = &metalist[i];
		while (!osi_list_empty(list))
		{
			bh = osi_list_entry(list->next,
					    struct gfs2_buffer_head, b_altlist);
			brelse(bh, not_updated);
			osi_list_del(&bh->b_altlist);
		}
	}

end:
        if (S_ISDIR(ip->i_di.di_mode)) {
		/* check validity of leaf blocks and leaf chains */
		if (ip->i_di.di_flags & GFS2_DIF_EXHASH) {
			error = check_leaf(ip, &update, pass);
			if(error < 0)
				return -1;
			if(error > 0)
				return 1;
		}
	}

	return 0;
}

/* Checks stuffed inode directories */
static int check_linear_dir(struct gfs2_inode *ip, struct gfs2_buffer_head *bh,
		     enum update_flags *update, struct metawalk_fxns *pass)
{
	int error = 0;
	uint16_t count = 0;

	error = check_entries(ip, bh, 0, DIR_LINEAR, update, &count, pass);
	if(error < 0) {
		stack;
		return -1;
	}

	return error;
}


int check_dir(struct gfs2_sbd *sbp, uint64_t block, struct metawalk_fxns *pass)
{
	struct gfs2_buffer_head *bh;
	struct gfs2_inode *ip;
	enum update_flags update = not_updated;
	int error = 0;

	bh = bread(&sbp->buf_list, block);
	ip = fsck_inode_get(sbp, bh);

	if(ip->i_di.di_flags & GFS2_DIF_EXHASH) {
		error = check_leaf(ip, &update, pass);
		if(error < 0) {
			stack;
			fsck_inode_put(ip, not_updated); /* does brelse(bh); */
			return -1;
		}
	}
	else {
		error = check_linear_dir(ip, bh, &update, pass);
		if(error < 0) {
			stack;
			fsck_inode_put(ip, not_updated); /* does brelse(bh); */
			return -1;
		}
	}

	fsck_inode_put(ip, opts.no ? not_updated : update); /* does a brelse */
	return error;
}

static int remove_dentry(struct gfs2_inode *ip, struct gfs2_dirent *dent,
			 struct gfs2_dirent *prev_de,
			 struct gfs2_buffer_head *bh,
			 char *filename, enum update_flags *update,
			 uint16_t *count, void *private)
{
	/* the metawalk_fxn's private field must be set to the dentry
	 * block we want to clear */
	uint64_t *dentryblock = (uint64_t *) private;
	struct gfs2_dirent dentry, *de;

	memset(&dentry, 0, sizeof(struct gfs2_dirent));
	gfs2_dirent_in(&dentry, (char *)dent);
	de = &dentry;
	*update = (opts.no ? not_updated : updated);

	if(de->de_inum.no_addr == *dentryblock)
		dirent2_del(ip, bh, prev_de, dent);
	else
		(*count)++;

	return 0;

}

int remove_dentry_from_dir(struct gfs2_sbd *sbp, uint64_t dir,
			   uint64_t dentryblock)
{
	struct metawalk_fxns remove_dentry_fxns = {0};
	struct gfs2_block_query q;
	int error;

	log_debug( _("Removing dentry %" PRIu64 " (0x%" PRIx64 ") from directory %"
			  PRIu64" (0x%" PRIx64 ")\n"), dentryblock, dentryblock, dir, dir);
	if(gfs2_check_range(sbp, dir)) {
		log_err( _("Parent directory out of range\n"));
		return 1;
	}
	remove_dentry_fxns.private = &dentryblock;
	remove_dentry_fxns.check_dentry = remove_dentry;

	if(gfs2_block_check(sbp, bl, dir, &q)) {
		stack;
		return -1;
	}
	if(q.block_type != gfs2_inode_dir) {
		log_info( _("Parent block is not a directory...ignoring\n"));
		return 1;
	}
	/* Need to run check_dir with a private var of dentryblock,
	 * and fxns that remove that dentry if found */
	error = check_dir(sbp, dir, &remove_dentry_fxns);

	return error;
}

/* FIXME: These should be merged with the hash routines in inode_hash.c */
static uint32_t dinode_hash(uint64_t block_no)
{
	unsigned int h;

	h = fsck_hash(&block_no, sizeof (uint64_t));
	h &= FSCK_HASH_MASK;

	return h;
}

int find_di(struct gfs2_sbd *sbp, uint64_t childblock, struct dir_info **dip)
{
	osi_list_t *bucket = &dir_hash[dinode_hash(childblock)];
	osi_list_t *tmp;
	struct dir_info *di = NULL;

	osi_list_foreach(tmp, bucket) {
		di = osi_list_entry(tmp, struct dir_info, list);
		if(di->dinode == childblock) {
			*dip = di;
			return 0;
		}
	}
	*dip = NULL;
	return -1;

}

int dinode_hash_insert(osi_list_t *buckets, uint64_t key, struct dir_info *di)
{
	osi_list_t *tmp;
	osi_list_t *bucket = &buckets[dinode_hash(key)];
	struct dir_info *dtmp = NULL;

	if(osi_list_empty(bucket)) {
		osi_list_add(&di->list, bucket);
		return 0;
	}

	osi_list_foreach(tmp, bucket) {
		dtmp = osi_list_entry(tmp, struct dir_info, list);
		if(dtmp->dinode < key) {
			continue;
		}
		else {
			osi_list_add_prev(&di->list, tmp);
			return 0;
		}
	}
	osi_list_add_prev(&di->list, bucket);
	return 0;
}

int dinode_hash_remove(osi_list_t *buckets, uint64_t key)
{
	osi_list_t *tmp;
	osi_list_t *bucket = &buckets[dinode_hash(key)];
	struct dir_info *dtmp = NULL;

	if(osi_list_empty(bucket)) {
		return -1;
	}
	osi_list_foreach(tmp, bucket) {
		dtmp = osi_list_entry(tmp, struct dir_info, list);
		if(dtmp->dinode == key) {
			osi_list_del(tmp);
			return 0;
		}
	}
	return -1;
}
