#include "clusterautoconfig.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <dirent.h>
#include <libintl.h>
#define _(String) gettext(String)

#include "fsck.h"
#include "libgfs2.h"
#include "lost_n_found.h"
#include "link.h"
#include "metawalk.h"
#include "util.h"

/* add_inode_to_lf - Add dir entry to lost+found for the inode
 * @ip: inode to add to lost + found
 *
 * This function adds an entry into the lost and found dir
 * for the given inode.  The name of the entry will be
 * "lost_<ip->i_num.no_addr>".
 *
 * Returns: 0 on success, -1 on failure.
 */
int add_inode_to_lf(struct gfs2_inode *ip){
	char tmp_name[256];
	__be32 inode_type;
	uint64_t lf_blocks;
	struct gfs2_sbd *sdp = ip->i_sbd;
	struct dir_info *di;

	if(!lf_dip) {
		uint8_t q;

		log_info( _("Locating/Creating lost+found directory\n"));

		lf_dip = createi(sdp->md.rooti, "lost+found",
				 S_IFDIR | 0700, 0);
		/* createi will have incremented the di_nlink link count for
		   the root directory.  We must increment the nlink value
		   in the hash table to keep them in sync so that pass4 can
		   detect and fix any descrepancies. */
		set_link_count(sdp->sd_sb.sb_root_dir.no_addr,
			       sdp->md.rooti->i_di.di_nlink);

		q = block_type(lf_dip->i_di.di_num.no_addr);
		if(q != gfs2_inode_dir) {
			/* This is a new lost+found directory, so set its
			 * block type and increment link counts for
			 * the directories */
			/* FIXME: i'd feel better about this if
			 * fs_mkdir returned whether it created a new
			 * directory or just found an old one, and we
			 * used that instead of the block_type to run
			 * this */
			fsck_blockmap_set(ip, lf_dip->i_di.di_num.no_addr,
					  _("lost+found dinode"),
					  gfs2_inode_dir);
			/* root inode links to lost+found */
			increment_link(sdp->md.rooti->i_di.di_num.no_addr,
				       lf_dip->i_di.di_num.no_addr, _("root"));
			/* lost+found link for '.' from itself */
			increment_link(lf_dip->i_di.di_num.no_addr,
				       lf_dip->i_di.di_num.no_addr, "\".\"");
			/* lost+found link for '..' back to root */
			increment_link(lf_dip->i_di.di_num.no_addr,
				       sdp->md.rooti->i_di.di_num.no_addr,
				       "\"..\"");
		}
		log_info( _("lost+found directory is dinode %lld (0x%llx)\n"),
			  (unsigned long long)lf_dip->i_di.di_num.no_addr,
			  (unsigned long long)lf_dip->i_di.di_num.no_addr);
		di = dirtree_find(lf_dip->i_di.di_num.no_addr);
		if (di) {
			log_info( _("Marking lost+found inode connected\n"));
			di->checked = 1;
			di = NULL;
		}
	}
	if(ip->i_di.di_num.no_addr == lf_dip->i_di.di_num.no_addr) {
		log_err( _("Trying to add lost+found to itself...skipping"));
		return 0;
	}
	lf_blocks = lf_dip->i_di.di_blocks;

	switch(ip->i_di.di_mode & S_IFMT){
	case S_IFDIR:
		log_info( _("Adding .. entry pointing to lost+found for "
			    "directory %llu (0x%llx)\n"),
			  (unsigned long long)ip->i_di.di_num.no_addr,
			  (unsigned long long)ip->i_di.di_num.no_addr);

		/* If there's a pre-existing .. directory entry, we have to
		   back out the links. */
		di = dirtree_find(ip->i_di.di_num.no_addr);
		if (di && gfs2_check_range(sdp, di->dotdot_parent) == 0) {
			struct gfs2_inode *dip;

			log_debug(_("Directory %lld (0x%llx) already had a "
				    "\"..\" link to %lld (0x%llx).\n"),
				  (unsigned long long)ip->i_di.di_num.no_addr,
				  (unsigned long long)ip->i_di.di_num.no_addr,
				  (unsigned long long)di->dotdot_parent,
				  (unsigned long long)di->dotdot_parent);
			decrement_link(di->dotdot_parent,
				       ip->i_di.di_num.no_addr,
				       _(".. unlinked, moving to lost+found"));
			dip = fsck_load_inode(sdp, di->dotdot_parent);
			if (dip->i_di.di_nlink > 0) {
				dip->i_di.di_nlink--;
				log_debug(_("Decrementing its links to %d\n"),
					  dip->i_di.di_nlink);
				bmodified(dip->i_bh);
			} else if (!dip->i_di.di_nlink) {
				log_debug(_("Its link count is zero.\n"));
			} else {
				log_debug(_("Its link count is %d!  "
					    "Changing it to 0.\n"),
					  dip->i_di.di_nlink);
				dip->i_di.di_nlink = 0;
				bmodified(dip->i_bh);
			}
			fsck_inode_put(&dip);
			di = NULL;
		} else
			log_debug(_("Couldn't find a valid \"..\" entry "
				    "for orphan directory %lld (0x%llx)\n"),
				  (unsigned long long)ip->i_di.di_num.no_addr,
				  (unsigned long long)ip->i_di.di_num.no_addr);
		if(gfs2_dirent_del(ip, "..", 2))
			log_warn( _("add_inode_to_lf:  Unable to remove "
				    "\"..\" directory entry.\n"));

		dir_add(ip, "..", 2, &(lf_dip->i_di.di_num), DT_DIR);
		sprintf(tmp_name, "lost_dir_%llu",
			(unsigned long long)ip->i_di.di_num.no_addr);
		inode_type = DT_DIR;
		break;
	case S_IFREG:
		sprintf(tmp_name, "lost_file_%llu",
			(unsigned long long)ip->i_di.di_num.no_addr);
		inode_type = DT_REG;
		break;
	case S_IFLNK:
		sprintf(tmp_name, "lost_link_%llu",
			(unsigned long long)ip->i_di.di_num.no_addr);
		inode_type = DT_LNK;
		break;
	case S_IFBLK:
		sprintf(tmp_name, "lost_blkdev_%llu",
			(unsigned long long)ip->i_di.di_num.no_addr);
		inode_type = DT_BLK;
		break;
	case S_IFCHR:
		sprintf(tmp_name, "lost_chrdev_%llu",
			(unsigned long long)ip->i_di.di_num.no_addr);
		inode_type = DT_CHR;
		break;
	case S_IFIFO:
		sprintf(tmp_name, "lost_fifo_%llu",
			(unsigned long long)ip->i_di.di_num.no_addr);
		inode_type = DT_FIFO;
		break;
	case S_IFSOCK:
		sprintf(tmp_name, "lost_socket_%llu",
			(unsigned long long)ip->i_di.di_num.no_addr);
		inode_type = DT_SOCK;
		break;
	default:
		sprintf(tmp_name, "lost_%llu",
			(unsigned long long)ip->i_di.di_num.no_addr);
		inode_type = DT_REG;
		break;
	}

	dir_add(lf_dip, tmp_name, strlen(tmp_name), &(ip->i_di.di_num),
		inode_type);
	/* If the lf directory had new blocks added we have to mark them
	   properly in the bitmap so they're not freed. */
	if (lf_dip->i_di.di_blocks != lf_blocks)
		reprocess_inode(lf_dip, "lost+found");

	/* This inode is linked from lost+found */
	increment_link(ip->i_di.di_num.no_addr, lf_dip->i_di.di_num.no_addr,
		       _("from lost+found"));
	/* If it's a directory, lost+found is back-linked to it via .. */
	if(S_ISDIR(ip->i_di.di_mode))
		increment_link(lf_dip->i_di.di_num.no_addr,
			       ip->i_di.di_mode, _("to lost+found"));

	log_notice( _("Added inode #%llu (0x%llx) to lost+found dir\n"),
		    (unsigned long long)ip->i_di.di_num.no_addr,
		    (unsigned long long)ip->i_di.di_num.no_addr);
	gfs2_dinode_out(&lf_dip->i_di, lf_dip->i_bh);
	bwrite(lf_dip->i_bh);
	return 0;
}
