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

#include <logging.h>
#include "fsck.h"
#include "libgfs2.h"
#include "lost_n_found.h"
#include "link.h"
#include "metawalk.h"
#include "util.h"

static void add_dotdot(struct fsck_cx *cx, struct lgfs2_inode *ip)
{
	struct lgfs2_sbd *sdp = ip->i_sbd;
	struct dir_info *di;
	struct lgfs2_inum no;
	int err;

	log_info(_("Adding .. entry to directory %"PRIu64" (0x%"PRIx64") pointing back to lost+found\n"),
	        ip->i_num.in_addr, ip->i_num.in_addr);

	/* If there's a pre-existing .. directory entry, we have to
	   back out the links. */
	di = dirtree_find(cx, ip->i_num.in_addr);
	if (di && valid_block(sdp, di->dotdot_parent.in_addr)) {
		struct lgfs2_inode *dip;

		log_debug(_("Directory (0x%"PRIx64") already had a '..' link to (0x%"PRIx64").\n"),
		          ip->i_num.in_addr, di->dotdot_parent.in_addr);
		dip = fsck_load_inode(sdp, di->dotdot_parent.in_addr);
		if (dip->i_num.in_formal_ino == di->dotdot_parent.in_formal_ino) {
			decr_link_count(cx, di->dotdot_parent.in_addr, ip->i_num.in_addr,
					_(".. unlinked, moving to lost+found"));
			if (dip->i_nlink > 0) {
			  dip->i_nlink--;
			  set_di_nlink(cx, dip); /* keep inode tree in sync */
			  log_debug(_("Decrementing its links to %d\n"),
				    dip->i_nlink);
			  lgfs2_bmodified(dip->i_bh);
			} else if (!dip->i_nlink) {
			  log_debug(_("Its link count is zero.\n"));
			} else {
			  log_debug(_("Its link count is %d!  Changing it to 0.\n"),
			            dip->i_nlink);
			  dip->i_nlink = 0;
			  set_di_nlink(cx, dip); /* keep inode tree in sync */
			  lgfs2_bmodified(dip->i_bh);
			}
		} else {
			log_debug(_("Directory (0x%"PRIx64")'s link to parent "
				    "(0x%"PRIx64") had a formal inode discrepancy: "
				    "was 0x%"PRIx64", expected 0x%"PRIx64"\n"),
				  ip->i_num.in_addr, di->dotdot_parent.in_addr,
				  di->dotdot_parent.in_formal_ino,
				  dip->i_num.in_formal_ino);
			log_debug(_("The parent directory was not changed.\n"));
		}
		fsck_inode_put(&dip);
		di = NULL;
	} else {
		if (di)
			log_debug(_("Couldn't find a valid '..' entry "
				    "for orphan directory (0x%"PRIx64"): "
				    "'..' = 0x%"PRIx64"\n"),
			          ip->i_num.in_addr, di->dotdot_parent.in_addr);
		else
			log_debug(_("Couldn't find directory (0x%"PRIx64") "
				    "in directory tree.\n"),
			          ip->i_num.in_addr);
	}
	if (lgfs2_dirent_del(ip, "..", 2))
		log_warn( _("add_inode_to_lf:  Unable to remove "
			    "\"..\" directory entry.\n"));

	no = lf_dip->i_num;
	err = lgfs2_dir_add(ip, "..", 2, &no, DT_DIR);
	if (err) {
		log_crit(_("Error adding .. directory: %s\n"),
			 strerror(errno));
		exit(FSCK_ERROR);
	}
}

void make_sure_lf_exists(struct fsck_cx *cx, struct lgfs2_inode *ip)
{
	struct dir_info *di;
	struct lgfs2_sbd *sdp = ip->i_sbd;
	int root_entries;

	if (lf_dip)
		return;

	root_entries = sdp->md.rooti->i_entries;
	log_info( _("Locating/Creating lost+found directory\n"));
	lf_dip = lgfs2_createi(sdp->md.rooti, "lost+found", S_IFDIR|0700, 0);
	if (lf_dip == NULL) {
		log_crit(_("Error creating lost+found: %s\n"),
			 strerror(errno));
		exit(FSCK_ERROR);
	}

	/* lgfs2_createi will have incremented the di_nlink link count for the root
	   directory.  We must set the nlink value in the hash table to keep
	   them in sync so that pass4 can detect and fix any descrepancies. */
	set_di_nlink(cx, sdp->md.rooti);

	if (sdp->md.rooti->i_entries > root_entries) {
		struct lgfs2_inum no = lf_dip->i_num;
		lf_was_created = 1;
		/* This is a new lost+found directory, so set its block type
		   and increment link counts for the directories */
		/* FIXME: i'd feel better about this if fs_mkdir returned
		   whether it created a new directory or just found an old one,
		   and we used that instead of the bitmap_type to run this */
		dirtree_insert(cx, no);
		/* Set the bitmap AFTER the dirtree insert so that function
		   check_n_fix_bitmap will realize it's a dinode and adjust
		   the rgrp counts properly. */
		fsck_bitmap_set(cx, ip, lf_dip->i_num.in_addr, _("lost+found dinode"), GFS2_BLKST_DINODE);
		/* root inode links to lost+found */
		no.in_addr = sdp->md.rooti->i_num.in_addr;
		no.in_formal_ino = sdp->md.rooti->i_num.in_formal_ino;
		incr_link_count(cx, no, lf_dip, _("root"));
		/* lost+found link for '.' from itself */
		no.in_addr = lf_dip->i_num.in_addr;
		no.in_formal_ino = lf_dip->i_num.in_formal_ino;
		incr_link_count(cx, no, lf_dip, "\".\"");
		/* lost+found link for '..' back to root */
		incr_link_count(cx, no, sdp->md.rooti, "\"..\"");
	}
	log_info(_("lost+found directory is dinode %"PRIu64" (0x%"PRIx64")\n"),
	         lf_dip->i_num.in_addr, lf_dip->i_num.in_addr);
	di = dirtree_find(cx, lf_dip->i_num.in_addr);
	if (di) {
		log_info( _("Marking lost+found inode connected\n"));
		di->checked = 1;
		di = NULL;
	}
}

/* add_inode_to_lf - Add dir entry to lost+found for the inode
 * @ip: inode to add to lost + found
 *
 * This function adds an entry into the lost and found dir
 * for the given inode.  The name of the entry will be
 * "lost_<ip->i_num.no_addr>".
 *
 * Returns: 0 on success, -1 on failure.
 */
int add_inode_to_lf(struct fsck_cx *cx, struct lgfs2_inode *ip)
{
	char tmp_name[256];
	unsigned inode_type;
	struct lgfs2_inum no;
	int err = 0;
	uint32_t mode;

	make_sure_lf_exists(cx, ip);
	if (ip->i_num.in_addr == lf_dip->i_num.in_addr) {
		log_err( _("Trying to add lost+found to itself...skipping"));
		return 0;
	}

	mode = ip->i_mode & S_IFMT;

	switch (mode) {
	case S_IFDIR:
		add_dotdot(cx, ip);
		sprintf(tmp_name, "lost_dir_%"PRIu64, ip->i_num.in_addr);
		inode_type = DT_DIR;
		break;
	case S_IFREG:
		sprintf(tmp_name, "lost_file_%"PRIu64, ip->i_num.in_addr);
		inode_type = DT_REG;
		break;
	case S_IFLNK:
		sprintf(tmp_name, "lost_link_%"PRIu64, ip->i_num.in_addr);
		inode_type = DT_LNK;
		break;
	case S_IFBLK:
		sprintf(tmp_name, "lost_blkdev_%"PRIu64, ip->i_num.in_addr);
		inode_type = DT_BLK;
		break;
	case S_IFCHR:
		sprintf(tmp_name, "lost_chrdev_%"PRIu64, ip->i_num.in_addr);
		inode_type = DT_CHR;
		break;
	case S_IFIFO:
		sprintf(tmp_name, "lost_fifo_%"PRIu64, ip->i_num.in_addr);
		inode_type = DT_FIFO;
		break;
	case S_IFSOCK:
		sprintf(tmp_name, "lost_socket_%"PRIu64, ip->i_num.in_addr);
		inode_type = DT_SOCK;
		break;
	default:
		sprintf(tmp_name, "lost_%"PRIu64, ip->i_num.in_addr);
		inode_type = DT_REG;
		break;
	}

	no = ip->i_num;
	err = lgfs2_dir_add(lf_dip, tmp_name, strlen(tmp_name), &no, inode_type);
	if (err) {
		log_crit(_("Error adding directory %s: %s\n"),
			 tmp_name, strerror(errno));
		exit(FSCK_ERROR);
	}

	/* This inode is linked from lost+found */
	incr_link_count(cx, no, lf_dip, _("from lost+found"));
	/* If it's a directory, lost+found is back-linked to it via .. */
	if (mode == S_IFDIR) {
		no = lf_dip->i_num;
		incr_link_count(cx, no, ip, _("to lost+found"));
	}
	log_notice(_("Added inode #%"PRIu64" (0x%"PRIx64") to lost+found\n"),
	           ip->i_num.in_addr, ip->i_num.in_addr);
	lgfs2_dinode_out(lf_dip, lf_dip->i_bh->b_data);
	lgfs2_bwrite(lf_dip->i_bh);
	return 0;
}
