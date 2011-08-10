#include "clusterautoconfig.h"

#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <linux/types.h>
#include <linux/gfs2_ondisk.h>

#include "osi_list.h"
#include "libgfs2.h"

/* GFS1 compatibility functions - so that programs like gfs2_convert
   and gfs2_edit can examine/manipulate GFS1 file systems. */

static __inline__ int fs_is_jdata(struct gfs2_inode *ip)
{
        return ip->i_di.di_flags & GFS2_DIF_JDATA;
}

static __inline__ uint64_t *
gfs1_metapointer(struct gfs2_buffer_head *bh, unsigned int height,
		 struct metapath *mp)
{
	unsigned int head_size = (height > 0) ?
		sizeof(struct gfs_indirect) : sizeof(struct gfs_dinode);

	return ((uint64_t *)(bh->b_data + head_size)) + mp->mp_list[height];
}

void gfs1_lookup_block(struct gfs2_inode *ip, struct gfs2_buffer_head *bh,
		  unsigned int height, struct metapath *mp,
		  int create, int *new, uint64_t *block)
{
	uint64_t *ptr = gfs1_metapointer(bh, height, mp);

	if (*ptr) {
		*block = be64_to_cpu(*ptr);
		return;
	}

	*block = 0;

	if (!create)
		return;

	if (height == ip->i_di.di_height - 1&&
	    !(S_ISDIR(ip->i_di.di_mode)))
		*block = data_alloc(ip);
	else
		*block = meta_alloc(ip);

	*ptr = cpu_to_be64(*block);
	bmodified(bh);
	ip->i_di.di_blocks++;
	bmodified(ip->i_bh);

	*new = 1;
}

void gfs1_block_map(struct gfs2_inode *ip, uint64_t lblock, int *new,
		    uint64_t *dblock, uint32_t *extlen, int prealloc)
{
	struct gfs2_sbd *sdp = ip->i_sbd;
	struct gfs2_buffer_head *bh;
	struct metapath *mp;
	int create = *new;
	unsigned int bsize;
	unsigned int height;
	unsigned int end_of_metadata;
	unsigned int x;

	*new = 0;
	*dblock = 0;
	if (extlen)
		*extlen = 0;

	if (!ip->i_di.di_height) { /* stuffed */
		if (!lblock) {
			*dblock = ip->i_di.di_num.no_addr;
			if (extlen)
				*extlen = 1;
		}
		return;
	}

	bsize = (fs_is_jdata(ip)) ? sdp->sd_jbsize : sdp->bsize;

	height = calc_tree_height(ip, (lblock + 1) * bsize);
	if (ip->i_di.di_height < height) {
		if (!create)
			return;

		build_height(ip, height);
	}

	mp = find_metapath(ip, lblock);
	end_of_metadata = ip->i_di.di_height - 1;

	bh = ip->i_bh;

	for (x = 0; x < end_of_metadata; x++) {
		gfs1_lookup_block(ip, bh, x, mp, create, new, dblock);
		if (bh != ip->i_bh)
			brelse(bh);
		if (!*dblock)
			goto out;

		if (*new) {
			struct gfs2_meta_header mh;

			bh = bget(sdp, *dblock);
			mh.mh_magic = GFS2_MAGIC;
			mh.mh_type = GFS2_METATYPE_IN;
			mh.mh_format = GFS2_FORMAT_IN;
			gfs2_meta_header_out(&mh, bh);
		} else {
			bh = bread(sdp, *dblock);
		}
	}

	if (!prealloc)
		gfs1_lookup_block(ip, bh, end_of_metadata, mp, create, new,
				  dblock);

	if (extlen && *dblock) {
		*extlen = 1;

		if (!*new) {
			uint64_t tmp_dblock;
			int tmp_new;
			unsigned int nptrs;

			nptrs = (end_of_metadata) ? sdp->sd_inptrs : sdp->sd_diptrs;

			while (++mp->mp_list[end_of_metadata] < nptrs) {
				gfs1_lookup_block(ip, bh, end_of_metadata, mp,
						  FALSE, &tmp_new,
						  &tmp_dblock);

				if (*dblock + *extlen != tmp_dblock)
					break;

				(*extlen)++;
			}
		}
	}

	if (bh != ip->i_bh)
		brelse(bh);

 out:
	free(mp);
}

/**
 * gfs1_rindex_read - read in the rg index file
 *                  Stolen from libgfs2/super.c, but modified to handle gfs1.
 * @sdp: the incore superblock pointer
 * fd: optional file handle for rindex file (if meta_fs file system is mounted)
 *     (if fd is <= zero, it will read from raw device)
 * @count1: return count of the rgs.
 *
 * Returns: 0 on success, -1 on failure
 */
int gfs1_rindex_read(struct gfs2_sbd *sdp, int fd, int *count1)
{
	unsigned int rg;
	int error;
	struct gfs2_rindex buf;
	struct rgrp_list *rgd, *prev_rgd;
	uint64_t prev_length = 0;

	*count1 = 0;
	prev_rgd = NULL;
	for (rg = 0; ; rg++) {
		if (fd > 0)
			error = read(fd, &buf, sizeof(struct gfs2_rindex));
		else
			error = gfs2_readi(sdp->md.riinode, (char *)&buf,
					   (rg * sizeof(struct gfs2_rindex)),
					   sizeof(struct gfs2_rindex));
		if (!error)
			break;
		if (error != sizeof(struct gfs2_rindex))
			return -1;

		rgd = (struct rgrp_list *)malloc(sizeof(struct rgrp_list));
		if (!rgd) {
			log_crit("Cannot allocate memory for rindex.\n");
			exit(-1);
		}
		memset(rgd, 0, sizeof(struct rgrp_list));
		osi_list_add_prev(&rgd->list, &sdp->rglist);

		gfs2_rindex_in(&rgd->ri, (char *)&buf);

		rgd->start = rgd->ri.ri_addr;
		if (prev_rgd) {
			prev_length = rgd->start - prev_rgd->start;
			prev_rgd->length = prev_length;
		}

		if(gfs2_compute_bitstructs(sdp, rgd))
			return -1;

		(*count1)++;
		prev_rgd = rgd;
	}
	if (prev_rgd)
		prev_rgd->length = prev_length;
	return 0;
}

/**
 * gfs1_ri_update - attach rgrps to the super block
 *                  Stolen from libgfs2/super.c, but modified to handle gfs1.
 * @sdp:
 *
 * Given the rgrp index inode, link in all rgrps into the super block
 * and be sure that they can be read.
 *
 * Returns: 0 on success, -1 on failure.
 */
int gfs1_ri_update(struct gfs2_sbd *sdp, int fd, int *rgcount, int quiet)
{
	struct rgrp_list *rgd;
	struct gfs2_rindex *ri;
	osi_list_t *tmp;
	int count1 = 0, count2 = 0;
	uint64_t errblock = 0;
	uint64_t rmax = 0;

	if (gfs1_rindex_read(sdp, fd, &count1))
	    goto fail;
	for (tmp = sdp->rglist.next; tmp != &sdp->rglist; tmp = tmp->next) {
		rgd = osi_list_entry(tmp, struct rgrp_list, list);
		errblock = gfs2_rgrp_read(sdp, rgd);
		if (errblock)
			return errblock;
		count2++;
		if (!quiet && count2 % 100 == 0) {
			printf(".");
			fflush(stdout);
		}
		ri = &rgd->ri;
		if (ri->ri_data0 + ri->ri_data - 1 > rmax)
			rmax = ri->ri_data0 + ri->ri_data - 1;
	}

	sdp->fssize = rmax;
	*rgcount = count1;
	if (count1 != count2)
		goto fail;

	return 0;

 fail:
	gfs2_rgrp_free(&sdp->rglist);
	return -1;
}

/* ------------------------------------------------------------------------ */
/* gfs_dinode_in */
/* ------------------------------------------------------------------------ */
static void gfs_dinode_in(struct gfs_dinode *di, struct gfs2_buffer_head *bh)
{
	struct gfs_dinode *str = (struct gfs_dinode *)bh->b_data;

	gfs2_meta_header_in(&di->di_header, bh);
	gfs2_inum_in(&di->di_num, (char *)&str->di_num);

	di->di_mode = be32_to_cpu(str->di_mode);
	di->di_uid = be32_to_cpu(str->di_uid);
	di->di_gid = be32_to_cpu(str->di_gid);
	di->di_nlink = be32_to_cpu(str->di_nlink);
	di->di_size = be64_to_cpu(str->di_size);
	di->di_blocks = be64_to_cpu(str->di_blocks);
	di->di_atime = be64_to_cpu(str->di_atime);
	di->di_mtime = be64_to_cpu(str->di_mtime);
	di->di_ctime = be64_to_cpu(str->di_ctime);
	di->di_major = be32_to_cpu(str->di_major);
	di->di_minor = be32_to_cpu(str->di_minor);
	di->di_goal_dblk = be64_to_cpu(str->di_goal_dblk);
	di->di_goal_mblk = be64_to_cpu(str->di_goal_mblk);
	di->di_flags = be32_to_cpu(str->di_flags);
	di->di_payload_format = be32_to_cpu(str->di_payload_format);
	di->di_type = be16_to_cpu(str->di_type);
	di->di_height = be16_to_cpu(str->di_height);
	di->di_depth = be16_to_cpu(str->di_depth);
	di->di_entries = be32_to_cpu(str->di_entries);
	di->di_eattr = be64_to_cpu(str->di_eattr);
}

struct gfs2_inode *gfs_inode_get(struct gfs2_sbd *sdp,
				 struct gfs2_buffer_head *bh)
{
	struct gfs_dinode gfs1_dinode;
	struct gfs2_inode *ip;

	ip = calloc(1, sizeof(struct gfs2_inode));
	if (ip == NULL) {
		fprintf(stderr, "Out of memory in %s\n", __FUNCTION__);
		exit(-1);
	}

	gfs_dinode_in(&gfs1_dinode, bh);
	memcpy(&ip->i_di.di_header, &gfs1_dinode.di_header,
	       sizeof(struct gfs2_meta_header));
	memcpy(&ip->i_di.di_num, &gfs1_dinode.di_num,
	       sizeof(struct gfs2_inum));
	ip->i_di.di_mode = gfs1_dinode.di_mode;
	ip->i_di.di_uid = gfs1_dinode.di_uid;
	ip->i_di.di_gid = gfs1_dinode.di_gid;
	ip->i_di.di_nlink = gfs1_dinode.di_nlink;
	ip->i_di.di_size = gfs1_dinode.di_size;
	ip->i_di.di_blocks = gfs1_dinode.di_blocks;
	ip->i_di.di_atime = gfs1_dinode.di_atime;
	ip->i_di.di_mtime = gfs1_dinode.di_mtime;
	ip->i_di.di_ctime = gfs1_dinode.di_ctime;
	ip->i_di.di_major = gfs1_dinode.di_major;
	ip->i_di.di_minor = gfs1_dinode.di_minor;
	ip->i_di.di_goal_data = gfs1_dinode.di_goal_dblk;
	ip->i_di.di_goal_meta = gfs1_dinode.di_goal_mblk;
	ip->i_di.di_flags = gfs1_dinode.di_flags;
	ip->i_di.di_payload_format = gfs1_dinode.di_payload_format;
	ip->i_di.__pad1 = gfs1_dinode.di_type;
	ip->i_di.di_height = gfs1_dinode.di_height;
	ip->i_di.di_depth = gfs1_dinode.di_depth;
	ip->i_di.di_entries = gfs1_dinode.di_entries;
	ip->i_di.di_eattr = gfs1_dinode.di_eattr;
	ip->i_bh = bh;
	ip->i_sbd = sdp;
	ip->bh_owned = 0;
	return ip;
}

struct gfs2_inode *gfs_inode_read(struct gfs2_sbd *sdp, uint64_t di_addr)
{
	struct gfs_dinode gfs1_dinode;
	struct gfs2_inode *ip;

	ip = calloc(1, sizeof(struct gfs2_inode));
	if (ip == NULL) {
		fprintf(stderr, "Out of memory in %s\n", __FUNCTION__);
		exit(-1);
	}

	ip->i_bh = bread(sdp, di_addr);
	gfs_dinode_in(&gfs1_dinode, ip->i_bh);
	memcpy(&ip->i_di.di_header, &gfs1_dinode.di_header,
	       sizeof(struct gfs2_meta_header));
	memcpy(&ip->i_di.di_num, &gfs1_dinode.di_num,
	       sizeof(struct gfs2_inum));
	ip->i_di.di_mode = gfs1_dinode.di_mode;
	ip->i_di.di_uid = gfs1_dinode.di_uid;
	ip->i_di.di_gid = gfs1_dinode.di_gid;
	ip->i_di.di_nlink = gfs1_dinode.di_nlink;
	ip->i_di.di_size = gfs1_dinode.di_size;
	ip->i_di.di_blocks = gfs1_dinode.di_blocks;
	ip->i_di.di_atime = gfs1_dinode.di_atime;
	ip->i_di.di_mtime = gfs1_dinode.di_mtime;
	ip->i_di.di_ctime = gfs1_dinode.di_ctime;
	ip->i_di.di_major = gfs1_dinode.di_major;
	ip->i_di.di_minor = gfs1_dinode.di_minor;
	ip->i_di.di_goal_data = gfs1_dinode.di_goal_dblk;
	ip->i_di.di_goal_meta = gfs1_dinode.di_goal_mblk;
	ip->i_di.di_flags = gfs1_dinode.di_flags;
	ip->i_di.di_payload_format = gfs1_dinode.di_payload_format;
	ip->i_di.__pad1 = gfs1_dinode.di_type;
	ip->i_di.di_height = gfs1_dinode.di_height;
	ip->i_di.di_depth = gfs1_dinode.di_depth;
	ip->i_di.di_entries = gfs1_dinode.di_entries;
	ip->i_di.di_eattr = gfs1_dinode.di_eattr;
	ip->i_sbd = sdp;
	ip->bh_owned = 1;
	return ip;
}

