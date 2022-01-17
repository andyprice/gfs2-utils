#include "clusterautoconfig.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>
#include <ctype.h>
#include <uuid.h>
#include "libgfs2.h"

void lgfs2_inum_in(struct lgfs2_inum *i, void *inp)
{
	struct gfs2_inum *in = inp;

	i->in_formal_ino = be64_to_cpu(in->no_formal_ino);
	i->in_addr = be64_to_cpu(in->no_addr);
}

void lgfs2_inum_out(const struct lgfs2_inum *i, void *inp)
{
	struct gfs2_inum *in = inp;

	in->no_formal_ino = cpu_to_be64(i->in_formal_ino);
	in->no_addr = cpu_to_be64(i->in_addr);
}

void lgfs2_sb_in(struct lgfs2_sbd *sdp, void *buf)
{
	struct gfs2_sb *sb = buf;
	struct gfs_sb *sb1 = buf;

	sdp->sd_fs_format = be32_to_cpu(sb->sb_fs_format);
	sdp->sd_multihost_format = be32_to_cpu(sb->sb_multihost_format);
	sdp->sd_flags = be32_to_cpu(sb1->sb_flags);
	sdp->sd_bsize = be32_to_cpu(sb->sb_bsize);
	sdp->sd_bsize_shift = be32_to_cpu(sb->sb_bsize_shift);
	sdp->sd_seg_size = be32_to_cpu(sb1->sb_seg_size);
	lgfs2_inum_in(&sdp->sd_meta_dir, &sb->sb_master_dir);
	lgfs2_inum_in(&sdp->sd_root_dir, &sb->sb_root_dir);
	memcpy(sdp->sd_lockproto, sb->sb_lockproto, GFS2_LOCKNAME_LEN);
	memcpy(sdp->sd_locktable, sb->sb_locktable, GFS2_LOCKNAME_LEN);
	lgfs2_inum_in(&sdp->sd_rindex_di, &sb1->sb_rindex_di);
	lgfs2_inum_in(&sdp->sd_quota_di, &sb1->sb_quota_di);
	lgfs2_inum_in(&sdp->sd_license_di, &sb1->sb_license_di);
	memcpy(sdp->sd_uuid, sb->sb_uuid, sizeof(sdp->sd_uuid));
}

void lgfs2_sb_out(const struct lgfs2_sbd *sdp, void *buf)
{
	struct gfs2_sb *sb = buf;
	struct gfs_sb *sb1 = buf;

	sb->sb_header.mh_magic = cpu_to_be32(GFS2_MAGIC);
	sb->sb_header.mh_type = cpu_to_be32(GFS2_METATYPE_SB);
	sb->sb_header.mh_format = cpu_to_be32(GFS2_FORMAT_SB);
	sb->sb_fs_format = cpu_to_be32(sdp->sd_fs_format);
	sb->sb_multihost_format = cpu_to_be32(sdp->sd_multihost_format);
	sb1->sb_flags = cpu_to_be32(sdp->sd_flags);
	sb->sb_bsize = cpu_to_be32(sdp->sd_bsize);
	sb->sb_bsize_shift = cpu_to_be32(sdp->sd_bsize_shift);
	sb1->sb_seg_size = cpu_to_be32(sdp->sd_seg_size);
	lgfs2_inum_out(&sdp->sd_meta_dir, &sb->sb_master_dir);
	lgfs2_inum_out(&sdp->sd_root_dir, &sb->sb_root_dir);
	memcpy(sb->sb_lockproto, sdp->sd_lockproto, GFS2_LOCKNAME_LEN);
	memcpy(sb->sb_locktable, sdp->sd_locktable, GFS2_LOCKNAME_LEN);
	lgfs2_inum_out(&sdp->sd_rindex_di, &sb1->sb_rindex_di);
	lgfs2_inum_out(&sdp->sd_quota_di, &sb1->sb_quota_di);
	lgfs2_inum_out(&sdp->sd_license_di, &sb1->sb_license_di);
	memcpy(sb->sb_uuid, sdp->sd_uuid, 16);
}

void lgfs2_rindex_in(lgfs2_rgrp_t rg, void *buf)
{
	struct gfs2_rindex *ri = buf;

	rg->rt_addr = be64_to_cpu(ri->ri_addr);
	rg->rt_length = be32_to_cpu(ri->ri_length);
	rg->rt_data0 = be64_to_cpu(ri->ri_data0);
	rg->rt_data = be32_to_cpu(ri->ri_data);
	rg->rt_bitbytes = be32_to_cpu(ri->ri_bitbytes);
}

void lgfs2_rindex_out(const lgfs2_rgrp_t rg, void *buf)
{
	struct gfs2_rindex *ri = buf;

	ri->ri_addr = cpu_to_be64(rg->rt_addr);
	ri->ri_length = cpu_to_be32(rg->rt_length);
	ri->ri_data0 = cpu_to_be64(rg->rt_data0);
	ri->ri_data = cpu_to_be32(rg->rt_data);
	ri->ri_bitbytes = cpu_to_be32(rg->rt_bitbytes);
}

void lgfs2_rgrp_in(lgfs2_rgrp_t rg, void *buf)
{
	struct gfs2_rgrp *r = buf;

	rg->rt_flags = be32_to_cpu(r->rg_flags);
	rg->rt_free = be32_to_cpu(r->rg_free);
	rg->rt_dinodes = be32_to_cpu(r->rg_dinodes);
	rg->rt_skip = be32_to_cpu(r->rg_skip);
	rg->rt_igeneration = be64_to_cpu(r->rg_igeneration);
	rg->rt_rg_data0 = be64_to_cpu(r->rg_data0);
	rg->rt_rg_data = be32_to_cpu(r->rg_data);
	rg->rt_rg_bitbytes = be32_to_cpu(r->rg_bitbytes);
}

void lgfs2_rgrp_out(const lgfs2_rgrp_t rg, void *buf)
{
	struct gfs2_rgrp *r = buf;

	r->rg_header.mh_magic = cpu_to_be32(GFS2_MAGIC);
	r->rg_header.mh_type = cpu_to_be32(GFS2_METATYPE_RG);
	r->rg_header.mh_format = cpu_to_be32(GFS2_FORMAT_RG);
	r->rg_flags = cpu_to_be32(rg->rt_flags);
	r->rg_free = cpu_to_be32(rg->rt_free);
	r->rg_dinodes = cpu_to_be32(rg->rt_dinodes);
	r->rg_skip = cpu_to_be32(rg->rt_skip);
	r->rg_igeneration = cpu_to_be64(rg->rt_igeneration);
	r->rg_data0 = cpu_to_be64(rg->rt_rg_data0);
	r->rg_data = cpu_to_be32(rg->rt_rg_data);
	r->rg_bitbytes = cpu_to_be32(rg->rt_rg_bitbytes);
	lgfs2_rgrp_crc_set(buf);
}

void lgfs2_dinode_in(struct lgfs2_inode *ip, char *buf)
{
	struct gfs2_dinode *di = (struct gfs2_dinode *)buf;

	ip->i_magic = be32_to_cpu(di->di_header.mh_magic);
	ip->i_mh_type = be32_to_cpu(di->di_header.mh_type);
	ip->i_format = be32_to_cpu(di->di_header.mh_format);
	lgfs2_inum_in(&ip->i_num, &di->di_num);
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
	ip->i_goal_meta = be64_to_cpu(di->di_goal_meta);
	ip->i_goal_data = be64_to_cpu(di->di_goal_data);
	ip->i_generation = be64_to_cpu(di->di_generation);
	ip->i_flags = be32_to_cpu(di->di_flags);
	ip->i_payload_format = be32_to_cpu(di->di_payload_format);
	ip->i_height = be16_to_cpu(di->di_height);
	ip->i_depth = be16_to_cpu(di->di_depth);
	ip->i_entries = be32_to_cpu(di->di_entries);
	ip->i_eattr = be64_to_cpu(di->di_eattr);
	ip->i_atime_nsec = be32_to_cpu(di->di_atime_nsec);
	ip->i_mtime_nsec = be32_to_cpu(di->di_mtime_nsec);
	ip->i_ctime_nsec = be32_to_cpu(di->di_ctime_nsec);
}

void lgfs2_dinode_out(struct lgfs2_inode *ip, char *buf)
{
	struct gfs2_dinode *di = (struct gfs2_dinode *)buf;

	di->di_header.mh_magic = cpu_to_be32(ip->i_magic);
	di->di_header.mh_type = cpu_to_be32(ip->i_mh_type);
	di->di_header.mh_format = cpu_to_be32(ip->i_format);
	lgfs2_inum_out(&ip->i_num, &di->di_num);
	di->di_mode = cpu_to_be32(ip->i_mode);
	di->di_uid = cpu_to_be32(ip->i_uid);
	di->di_gid = cpu_to_be32(ip->i_gid);
	di->di_nlink = cpu_to_be32(ip->i_nlink);
	di->di_size = cpu_to_be64(ip->i_size);
	di->di_blocks = cpu_to_be64(ip->i_blocks);
	di->di_atime = cpu_to_be64(ip->i_atime);
	di->di_mtime = cpu_to_be64(ip->i_mtime);
	di->di_ctime = cpu_to_be64(ip->i_ctime);
	di->di_major = cpu_to_be32(ip->i_major);
	di->di_minor = cpu_to_be32(ip->i_minor);
	di->di_goal_meta = cpu_to_be64(ip->i_goal_meta);
	di->di_goal_data = cpu_to_be64(ip->i_goal_data);
	di->di_generation = cpu_to_be64(ip->i_generation);
	di->di_flags = cpu_to_be32(ip->i_flags);
	di->di_payload_format = cpu_to_be32(ip->i_payload_format);
	di->di_height = cpu_to_be16(ip->i_height);
	di->di_depth = cpu_to_be16(ip->i_depth);
	di->di_entries = cpu_to_be32(ip->i_entries);
	di->di_eattr = cpu_to_be64(ip->i_eattr);
	di->di_atime_nsec = cpu_to_be32(ip->i_atime_nsec);
	di->di_mtime_nsec = cpu_to_be32(ip->i_mtime_nsec);
	di->di_ctime_nsec = cpu_to_be32(ip->i_ctime_nsec);
}

void lgfs2_dirent_in(struct lgfs2_dirent *d, void *dep)
{
	struct gfs2_dirent *de = dep;

	lgfs2_inum_in(&d->dr_inum, &de->de_inum);
	d->dr_hash = be32_to_cpu(de->de_hash);
	d->dr_rec_len = be16_to_cpu(de->de_rec_len);
	d->dr_name_len = be16_to_cpu(de->de_name_len);
	d->dr_type = be16_to_cpu(de->de_type);
	d->dr_rahead = be16_to_cpu(de->de_rahead);
}

void lgfs2_dirent_out(struct lgfs2_dirent *d, void *dep)
{
	struct gfs2_dirent *de = dep;

	lgfs2_inum_out(&d->dr_inum, &de->de_inum);
	de->de_hash = cpu_to_be32(d->dr_hash);
	de->de_rec_len = cpu_to_be16(d->dr_rec_len);
	de->de_name_len = cpu_to_be16(d->dr_name_len);
	de->de_type = cpu_to_be16(d->dr_type);
	de->de_rahead = cpu_to_be16(d->dr_rahead);
}

void lgfs2_leaf_in(struct lgfs2_leaf *lf, void *lfp)
{
	struct gfs2_leaf *l = lfp;

	lf->lf_depth = be16_to_cpu(l->lf_depth);
	lf->lf_entries = be16_to_cpu(l->lf_entries);
	lf->lf_dirent_format = be32_to_cpu(l->lf_dirent_format);
	lf->lf_next = be64_to_cpu(l->lf_next);
	lf->lf_inode = be64_to_cpu(l->lf_inode);
	lf->lf_dist = be32_to_cpu(l->lf_dist);
	lf->lf_nsec = be32_to_cpu(l->lf_nsec);
	lf->lf_sec = be64_to_cpu(l->lf_sec);
}

void lgfs2_leaf_out(struct lgfs2_leaf *lf, void *lfp)
{
	struct gfs2_leaf *l = lfp;

	l->lf_depth = cpu_to_be16(lf->lf_depth);
	l->lf_entries = cpu_to_be16(lf->lf_entries);
	l->lf_dirent_format = cpu_to_be32(lf->lf_dirent_format);
	l->lf_next = cpu_to_be64(lf->lf_next);
	l->lf_inode = cpu_to_be64(lf->lf_inode);
	l->lf_dist = cpu_to_be32(lf->lf_dist);
	l->lf_nsec = cpu_to_be32(lf->lf_nsec);
	l->lf_sec = cpu_to_be64(lf->lf_sec);
}
