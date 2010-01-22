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

	if(!lf_dip) {
		uint8_t q;

		log_info( _("Locating/Creating lost and found directory\n"));

		lf_dip = createi(ip->i_sbd->md.rooti, "lost+found",
				 S_IFDIR | 0700, 0);
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
			gfs2_blockmap_set(bl, lf_dip->i_di.di_num.no_addr,
					  gfs2_inode_dir);
			/* root inode links to lost+found */
			increment_link(ip->i_sbd->md.rooti->i_di.di_num.no_addr,
				       lf_dip->i_di.di_num.no_addr, _("root"));
			/* lost+found link for '.' from itself */
			increment_link(lf_dip->i_di.di_num.no_addr,
				       lf_dip->i_di.di_num.no_addr, "\".\"");
			/* lost+found link for '..' back to root */
			increment_link(lf_dip->i_di.di_num.no_addr,
				       ip->i_sbd->md.rooti->i_di.di_num.no_addr,
				       "\"..\"");
		}
	}
	if(ip->i_di.di_num.no_addr == lf_dip->i_di.di_num.no_addr) {
		log_err( _("Trying to add lost+found to itself...skipping"));
		return 0;
	}
	switch(ip->i_di.di_mode & S_IFMT){
	case S_IFDIR:
		log_info( _("Adding .. entry pointing to lost+found for "
			    "directory %llu (0x%llx)\n"),
			  (unsigned long long)ip->i_di.di_num.no_addr,
			  (unsigned long long)ip->i_di.di_num.no_addr);

		if(gfs2_dirent_del(ip, "..", 2))
			log_warn( _("add_inode_to_lf: Unable to remove "
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
	/* This inode is linked from lost+found */
  	increment_link(ip->i_di.di_num.no_addr, lf_dip->i_di.di_num.no_addr,
		       _("from lost+found"));
	/* If it's a directory, lost+found is back-linked to it via .. */
	if(S_ISDIR(ip->i_di.di_mode))
		increment_link(lf_dip->i_di.di_num.no_addr,
			       ip->i_di.di_mode, _("to lost+found"));

	log_notice( _("Added inode #%llu to lost+found dir\n"),
		    (unsigned long long)ip->i_di.di_num.no_addr);
	return 0;
}
