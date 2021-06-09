#include "clusterautoconfig.h"

#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>
#include <time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

#include "osi_list.h"
#include "libgfs2.h"

/* GFS1 compatibility functions - so that programs like gfs2_convert
   and gfs2_edit can examine/manipulate GFS1 file systems. */

static __inline__ int fs_is_jdata(struct gfs2_inode *ip)
{
        return ip->i_flags & GFS2_DIF_JDATA;
}

static __inline__ uint64_t *
gfs1_metapointer(char *buf, unsigned int height, struct metapath *mp)
{
	unsigned int head_size = (height > 0) ?
		sizeof(struct gfs_indirect) : sizeof(struct gfs_dinode);

	return ((uint64_t *)(buf + head_size)) + mp->mp_list[height];
}

int is_gfs_dir(struct gfs2_inode *ip)
{
	if (ip->i_pad1 == GFS_FILE_DIR)
		return 1;
	return 0;
}

void gfs1_lookup_block(struct gfs2_inode *ip, struct gfs2_buffer_head *bh,
		  unsigned int height, struct metapath *mp,
		  int create, int *new, uint64_t *block)
{
	uint64_t *ptr = gfs1_metapointer(bh->b_data, height, mp);

	if (*ptr) {
		*block = be64_to_cpu(*ptr);
		return;
	}

	*block = 0;

	if (!create)
		return;

	if (lgfs2_meta_alloc(ip, block)) {
		*block = 0;
		return;
	}

	*ptr = cpu_to_be64(*block);
	bmodified(bh);
	ip->i_blocks++;
	bmodified(ip->i_bh);

	*new = 1;
}

void gfs1_block_map(struct gfs2_inode *ip, uint64_t lblock, int *new,
		    uint64_t *dblock, uint32_t *extlen, int prealloc)
{
	struct gfs2_sbd *sdp = ip->i_sbd;
	struct gfs2_buffer_head *bh;
	struct metapath mp;
	int create = *new;
	unsigned int bsize;
	unsigned int height;
	unsigned int end_of_metadata;
	unsigned int x;

	*new = 0;
	*dblock = 0;
	if (extlen)
		*extlen = 0;

	if (!ip->i_height) { /* stuffed */
		if (!lblock) {
			*dblock = ip->i_addr;
			if (extlen)
				*extlen = 1;
		}
		return;
	}

	bsize = (fs_is_jdata(ip)) ? sdp->sd_jbsize : sdp->sd_bsize;

	height = calc_tree_height(ip, (lblock + 1) * bsize);
	if (ip->i_height < height) {
		if (!create)
			return;

		build_height(ip, height);
	}

	find_metapath(ip, lblock, &mp);
	end_of_metadata = ip->i_height - 1;

	bh = ip->i_bh;

	for (x = 0; x < end_of_metadata; x++) {
		gfs1_lookup_block(ip, bh, x, &mp, create, new, dblock);
		if (bh != ip->i_bh)
			brelse(bh);
		if (!*dblock)
			return;

		if (*new) {
			struct gfs2_meta_header mh = {
				.mh_magic = cpu_to_be32(GFS2_MAGIC),
				.mh_type = cpu_to_be32(GFS2_METATYPE_IN),
				.mh_format = cpu_to_be32(GFS2_FORMAT_IN)
			};
			bh = bget(sdp, *dblock);
			memcpy(bh->b_data, &mh, sizeof(mh));
			bmodified(bh);
		} else {
			if (*dblock == ip->i_addr)
				bh = ip->i_bh;
			else
				bh = bread(sdp, *dblock);
		}
	}

	if (!prealloc)
		gfs1_lookup_block(ip, bh, end_of_metadata, &mp, create, new,
				  dblock);

	if (extlen && *dblock) {
		*extlen = 1;

		if (!*new) {
			uint64_t tmp_dblock;
			int tmp_new;
			unsigned int nptrs;

			nptrs = (end_of_metadata) ? sdp->sd_inptrs : sdp->sd_diptrs;

			while (++mp.mp_list[end_of_metadata] < nptrs) {
				gfs1_lookup_block(ip, bh, end_of_metadata, &mp,
						  0, &tmp_new,
						  &tmp_dblock);

				if (*dblock + *extlen != tmp_dblock)
					break;

				(*extlen)++;
			}
		}
	}

	if (bh != ip->i_bh)
		brelse(bh);
}

int gfs1_writei(struct gfs2_inode *ip, char *buf, uint64_t offset,
		unsigned int size)
{
	struct gfs2_sbd *sdp = ip->i_sbd;
	struct gfs2_buffer_head *bh;
	uint64_t lblock, dblock;
	uint32_t extlen = 0;
	unsigned int amount;
	int new;
	int journaled = fs_is_jdata(ip);
	const uint64_t start = offset;
	int copied = 0;

	if (!size)
		return 0;

	if (!ip->i_height && /* stuffed */
	    ((start + size) > (sdp->sd_bsize - sizeof(struct gfs_dinode))))
		unstuff_dinode(ip);

	if (journaled) {
		lblock = offset / sdp->sd_jbsize;
		offset %= sdp->sd_jbsize;
	} else {
		lblock = offset >> sdp->sd_bsize_shift;
		offset &= sdp->sd_bsize - 1;
	}

	if (!ip->i_height) /* stuffed */
		offset += sizeof(struct gfs_dinode);
	else if (journaled)
		offset += sizeof(struct gfs2_meta_header);

	while (copied < size) {
		amount = size - copied;
		if (amount > sdp->sd_bsize - offset)
			amount = sdp->sd_bsize - offset;

		if (!extlen){
			new = 1;
			gfs1_block_map(ip, lblock, &new, &dblock, &extlen, 0);
			if (!dblock)
				return -1;
		}

		if (dblock == ip->i_addr)
			bh = ip->i_bh;
		else
			bh = bread(sdp, dblock);

		if (journaled && dblock != ip->i_addr ) {
			struct gfs2_meta_header mh = {
				.mh_magic = cpu_to_be32(GFS2_MAGIC),
				.mh_type = cpu_to_be32(GFS2_METATYPE_JD),
				.mh_format = cpu_to_be32(GFS2_FORMAT_JD)
			};
			memcpy(bh->b_data, &mh, sizeof(mh));
		}

		memcpy(bh->b_data + offset, buf + copied, amount);
		bmodified(bh);
		if (bh != ip->i_bh)
			brelse(bh);

		copied += amount;
		lblock++;
		dblock++;
		extlen--;

		offset = (journaled) ? sizeof(struct gfs2_meta_header) : 0;
	}

	if (ip->i_size < start + copied) {
		bmodified(ip->i_bh);
		ip->i_size = start + copied;
	}
	ip->i_mtime = ip->i_ctime = time(NULL);
	lgfs2_dinode_out(ip, ip->i_bh->b_data);
	bmodified(ip->i_bh);
	return copied;
}

static struct gfs2_inode *__gfs_inode_get(struct gfs2_sbd *sdp, char *buf)
{
	struct gfs_dinode *di;
	struct gfs2_inode *ip;

	ip = calloc(1, sizeof(struct gfs2_inode));
	if (ip == NULL) {
		return NULL;
	}
	di = (struct gfs_dinode *)buf;
	ip->i_magic = be32_to_cpu(di->di_header.mh_magic);
	ip->i_type = be32_to_cpu(di->di_header.mh_type);
	ip->i_format = be32_to_cpu(di->di_header.mh_format);
	ip->i_formal_ino = be64_to_cpu(di->di_num.no_formal_ino);
	ip->i_addr = be64_to_cpu(di->di_num.no_addr);
	ip->i_mode = be32_to_cpu(di->di_mode);
	ip->i_uid = be32_to_cpu(di->di_uid);
	ip->i_gid = be32_to_cpu(di->di_gid);
	ip->i_nlink = be32_to_cpu(di->di_nlink);
	ip->i_size = be64_to_cpu(di->di_size);
	ip->i_blocks = be64_to_cpu(di->di_blocks);
	ip->i_atime = be64_to_cpu(di->di_atime);
	ip->i_mtime = be64_to_cpu(di->di_mtime);
	ip->i_ctime = be64_to_cpu(di->di_ctime);
	ip->i_major = be32_to_cpu(di->di_major);
	ip->i_minor = be32_to_cpu(di->di_minor);
	ip->i_goal_data = (uint64_t)be32_to_cpu(di->di_goal_dblk);
	ip->i_goal_meta = (uint64_t)be32_to_cpu(di->di_goal_mblk);
	ip->i_flags = be32_to_cpu(di->di_flags);
	ip->i_payload_format = be32_to_cpu(di->di_payload_format);
	ip->i_pad1 = be16_to_cpu(di->di_type);
	ip->i_height = be16_to_cpu(di->di_height);
	ip->i_depth = be16_to_cpu(di->di_depth);
	ip->i_entries = be32_to_cpu(di->di_entries);
	ip->i_eattr = be64_to_cpu(di->di_eattr);
	ip->i_sbd = sdp;
	return ip;
}

struct gfs2_inode *lgfs2_gfs_inode_get(struct gfs2_sbd *sdp, char *buf)
{
	return __gfs_inode_get(sdp, buf);
}

struct gfs2_inode *lgfs2_gfs_inode_read(struct gfs2_sbd *sdp, uint64_t di_addr)
{
	struct gfs2_buffer_head *bh;
	struct gfs2_inode *ip;

	bh = bget(sdp, di_addr);
	if (bh == NULL)
		return NULL;
	if (pread(sdp->device_fd, bh->b_data, sdp->sd_bsize, di_addr * sdp->sd_bsize) != sdp->sd_bsize) {
		brelse(bh);
		return NULL;
	}
	ip = __gfs_inode_get(sdp, bh->b_data);
	ip->i_bh = bh;
	ip->bh_owned = 1;
	return ip;
}

/* ------------------------------------------------------------------------ */
/* gfs_jindex_in - read in a gfs1 jindex structure.                         */
/* ------------------------------------------------------------------------ */
void gfs_jindex_in(struct gfs_jindex *jindex, char *jbuf)
{
	struct gfs_jindex *str = (struct gfs_jindex *) jbuf;

	jindex->ji_addr = be64_to_cpu(str->ji_addr);
	jindex->ji_nsegment = be32_to_cpu(str->ji_nsegment);
	jindex->ji_pad = be32_to_cpu(str->ji_pad);
	memcpy(jindex->ji_reserved, str->ji_reserved, 64);
}

void lgfs2_gfs_rgrp_in(const lgfs2_rgrp_t rg, void *buf)
{
	struct gfs_rgrp *r = buf;

	rg->rt_flags = be32_to_cpu(r->rg_flags);
	rg->rt_free = be32_to_cpu(r->rg_free);
	rg->rt_useddi = be32_to_cpu(r->rg_useddi);
	rg->rt_freedi = be32_to_cpu(r->rg_freedi);
	rg->rt_freedi_list.no_formal_ino = be64_to_cpu(r->rg_freedi_list.no_formal_ino);
	rg->rt_freedi_list.no_addr = be64_to_cpu(r->rg_freedi_list.no_addr);
	rg->rt_usedmeta = be32_to_cpu(r->rg_usedmeta);
	rg->rt_freemeta = be32_to_cpu(r->rg_freemeta);
}

void lgfs2_gfs_rgrp_out(const lgfs2_rgrp_t rg, void *buf)
{
	struct gfs_rgrp *r = buf;

	r->rg_header.mh_magic = cpu_to_be32(GFS2_MAGIC);
	r->rg_header.mh_type = cpu_to_be32(GFS2_METATYPE_RG);
	r->rg_header.mh_format = cpu_to_be32(GFS2_FORMAT_RG);
	r->rg_flags = cpu_to_be32(rg->rt_flags);
	r->rg_free = cpu_to_be32(rg->rt_free);
	r->rg_useddi = cpu_to_be32(rg->rt_useddi);
	r->rg_freedi = cpu_to_be32(rg->rt_freedi);
	r->rg_freedi_list.no_formal_ino = cpu_to_be64(rg->rt_freedi_list.no_formal_ino);
	r->rg_freedi_list.no_addr = cpu_to_be64(rg->rt_freedi_list.no_addr);
	r->rg_usedmeta = cpu_to_be32(rg->rt_usedmeta);
	r->rg_freemeta = cpu_to_be32(rg->rt_freemeta);

}
