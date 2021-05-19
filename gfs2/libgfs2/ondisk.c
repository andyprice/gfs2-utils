#include "clusterautoconfig.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>
#include <ctype.h>
#include <uuid.h>
#include "libgfs2.h"

#define pv(struct, member, fmt, fmt2) do {				\
		print_it("  "#member, fmt, fmt2, struct->member);	\
	} while (0);
#define pv2(struct, member, fmt, fmt2) do {				\
		print_it("  ", fmt, fmt2, struct->member);		\
	} while (0);

#define printbe16(struct, member) do { \
		print_it("  "#member, "%"PRIu16, "0x%"PRIx16, be16_to_cpu(struct->member)); \
	} while(0)
#define printbe32(struct, member) do { \
		print_it("  "#member, "%"PRIu32, "0x%"PRIx32, be32_to_cpu(struct->member)); \
	} while(0)
#define printbe64(struct, member) do { \
		print_it("  "#member, "%"PRIu64, "0x%"PRIx64, be64_to_cpu(struct->member)); \
	} while(0)
#define print8(struct, member) do { \
		print_it("  "#member, "%"PRIu8, "0x%"PRIx8, struct->member); \
	} while(0)

#define CPIN_08(s1, s2, member, count) {memcpy((s1->member), (s2->member), (count));}
#define CPOUT_08(s1, s2, member, count) {memcpy((s2->member), (s1->member), (count));}
#define CPIN_16(s1, s2, member) {(s1->member) = be16_to_cpu((s2->member));}
#define CPOUT_16(s1, s2, member) {(s2->member) = cpu_to_be16((s1->member));}
#define CPIN_32(s1, s2, member) {(s1->member) = be32_to_cpu((s2->member));}
#define CPOUT_32(s1, s2, member) {(s2->member) = cpu_to_be32((s1->member));}
#define CPIN_64(s1, s2, member) {(s1->member) = be64_to_cpu((s2->member));}
#define CPOUT_64(s1, s2, member) {(s2->member) = cpu_to_be64((s1->member));}

/*
 * gfs2_xxx_in - read in an xxx struct
 * first arg: the cpu-order structure
 * buf: the disk-order block data
 *
 * gfs2_xxx_out - write out an xxx struct
 * first arg: the cpu-order structure
 * buf: the disk-order block data
 *
 * gfs2_xxx_print - print out an xxx struct
 * first arg: the cpu-order structure
 */

void gfs2_inum_in(struct gfs2_inum *no, char *buf)
{
	struct gfs2_inum *str = (struct gfs2_inum *)buf;

	CPIN_64(no, str, no_formal_ino);
	CPIN_64(no, str, no_addr);
}

void gfs2_inum_out(const struct gfs2_inum *no, char *buf)
{
	struct gfs2_inum *str = (struct gfs2_inum *)buf;

	CPOUT_64(no, str, no_formal_ino);
	CPOUT_64(no, str, no_addr);
}

void gfs2_inum_print(const struct gfs2_inum *no)
{
	pv(no, no_formal_ino, "%"PRIu64, "0x%"PRIx64);
	pv(no, no_addr, "%"PRIu64, "0x%"PRIx64);
}

void lgfs2_inum_print(void *nop)
{
	struct gfs2_inum *no = nop;

	printbe64(no, no_formal_ino);
	printbe64(no, no_addr);
}

void gfs2_meta_header_in(struct gfs2_meta_header *mh, const char *buf)
{
	const struct gfs2_meta_header *str = (struct gfs2_meta_header *)buf;

	CPIN_32(mh, str, mh_magic);
	CPIN_32(mh, str, mh_type);
	CPIN_32(mh, str, mh_format);
}

void gfs2_meta_header_out(const struct gfs2_meta_header *mh, char *buf)
{
	struct gfs2_meta_header *str = (struct gfs2_meta_header *)buf;

	CPOUT_32(mh, str, mh_magic);
	CPOUT_32(mh, str, mh_type);
	CPOUT_32(mh, str, mh_format);
	str->__pad0 = 0;
	str->__pad1 = 0;
}

void gfs2_meta_header_print(const struct gfs2_meta_header *mh)
{
	pv(mh, mh_magic, "0x%08"PRIX32, NULL);
	pv(mh, mh_type, "%"PRIu32, "0x%"PRIx32);
	pv(mh, mh_format, "%"PRIu32, "0x%"PRIx32);
}

void lgfs2_meta_header_print(void *mhp)
{
	struct gfs2_meta_header *mh = mhp;

	print_it("  mh_magic", "0x%08"PRIX32, NULL, be32_to_cpu(mh->mh_magic));
	printbe32(mh, mh_type);
	printbe32(mh, mh_format);
}

void gfs2_sb_in(struct gfs2_sb *sb, char *buf)
{
	struct gfs2_sb *str = (struct gfs2_sb *)buf;

	gfs2_meta_header_in(&sb->sb_header, buf);

	CPIN_32(sb, str, sb_fs_format);
	CPIN_32(sb, str, sb_multihost_format);
	CPIN_32(sb, str, __pad0);                        /* gfs sb_flags */

	CPIN_32(sb, str, sb_bsize);
	CPIN_32(sb, str, sb_bsize_shift);
	CPIN_32(sb, str, __pad1);                        /* gfs sb_seg_size */

	gfs2_inum_in(&sb->sb_master_dir, (char *)&str->sb_master_dir);
	gfs2_inum_in(&sb->sb_root_dir, (char *)&str->sb_root_dir);

	CPIN_08(sb, str, sb_lockproto, GFS2_LOCKNAME_LEN);
	CPIN_08(sb, str, sb_locktable, GFS2_LOCKNAME_LEN);
	gfs2_inum_in(&sb->__pad2, (char *)&str->__pad2); /* gfs rindex */
	gfs2_inum_in(&sb->__pad3, (char *)&str->__pad3); /* gfs quota */
	gfs2_inum_in(&sb->__pad4, (char *)&str->__pad4); /* gfs license */
	CPIN_08(sb, str, sb_uuid, sizeof(sb->sb_uuid));
}

void gfs2_sb_out(const struct gfs2_sb *sb, char *buf)
{
	struct gfs2_sb *str = (struct gfs2_sb *)buf;

	gfs2_meta_header_out(&sb->sb_header, buf);

	CPOUT_32(sb, str, sb_fs_format);
	CPOUT_32(sb, str, sb_multihost_format);
	CPOUT_32(sb, str, __pad0);                        /* gfs sb_flags */

	CPOUT_32(sb, str, sb_bsize);
	CPOUT_32(sb, str, sb_bsize_shift);
	CPOUT_32(sb, str, __pad1);                        /* gfs sb_seg_size */

	gfs2_inum_out(&sb->sb_master_dir, (char *)&str->sb_master_dir);
	gfs2_inum_out(&sb->sb_root_dir, (char *)&str->sb_root_dir);

	CPOUT_08(sb, str, sb_lockproto, GFS2_LOCKNAME_LEN);
	CPOUT_08(sb, str, sb_locktable, GFS2_LOCKNAME_LEN);
	gfs2_inum_out(&sb->__pad2, (char *)&str->__pad2); /* gfs rindex */
	gfs2_inum_out(&sb->__pad3, (char *)&str->__pad3); /* gfs quota */
	gfs2_inum_out(&sb->__pad4, (char *)&str->__pad4); /* gfs license */
	memcpy(str->sb_uuid, sb->sb_uuid, 16);
}

void lgfs2_sb_print(void *sbp)
{
	struct gfs2_sb *sb = sbp;
	char readable_uuid[36+1];

	lgfs2_meta_header_print(&sb->sb_header);
	printbe32(sb, sb_fs_format);
	printbe32(sb, sb_multihost_format);
	printbe32(sb, sb_bsize);
	printbe32(sb, sb_bsize_shift);
	lgfs2_inum_print(&sb->sb_master_dir);
	lgfs2_inum_print(&sb->sb_root_dir);
	print_it("  sb_lockproto", "%.64s", NULL, sb->sb_lockproto);
	print_it("  sb_locktable", "%.64s", NULL, sb->sb_locktable);
	uuid_unparse(sb->sb_uuid, readable_uuid);
	print_it("  uuid", "%36s", NULL, readable_uuid);
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

void lgfs2_rindex_print(void *rip)
{
	struct gfs2_rindex *ri = rip;

	printbe64(ri, ri_addr);
	printbe32(ri, ri_length);
	printbe64(ri, ri_data0);
	printbe32(ri, ri_data);
	printbe32(ri, ri_bitbytes);
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

void lgfs2_rgrp_print(void *rgp)
{
	struct gfs2_rgrp *rg = rgp;

	lgfs2_meta_header_print(&rg->rg_header);
	printbe32(rg, rg_flags);
	printbe32(rg, rg_free);
	printbe32(rg, rg_dinodes);
	printbe32(rg, rg_skip);
	printbe64(rg, rg_igeneration);
	printbe64(rg, rg_data0);
	printbe32(rg, rg_data);
	printbe32(rg, rg_bitbytes);
	printbe32(rg, rg_crc);
}

void lgfs2_quota_print(void *qp)
{
	struct gfs2_quota *q = qp;

	printbe64(q, qu_limit);
	printbe64(q, qu_warn);
	printbe64(q, qu_value);
}

void lgfs2_dinode_in(struct gfs2_inode *ip, char *buf)
{
	struct gfs2_dinode *di = (struct gfs2_dinode *)buf;

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
	ip->i_goal_meta = be64_to_cpu(di->di_goal_meta);
	ip->i_goal_data = be64_to_cpu(di->di_goal_data);
	ip->i_generation = be64_to_cpu(di->di_generation);
	ip->i_flags = be32_to_cpu(di->di_flags);
	ip->i_payload_format = be32_to_cpu(di->di_payload_format);
	ip->i_pad1 = be16_to_cpu(di->__pad1);
	ip->i_height = be16_to_cpu(di->di_height);
	ip->i_pad2 = be32_to_cpu(di->__pad2);
	ip->i_pad3 = be16_to_cpu(di->__pad3);
	ip->i_depth = be16_to_cpu(di->di_depth);
	ip->i_entries = be32_to_cpu(di->di_entries);
	ip->i_pad4_addr = be64_to_cpu(di->__pad4.no_addr);
	ip->i_pad4_formal_ino = be64_to_cpu(di->__pad4.no_formal_ino);
	ip->i_eattr = be64_to_cpu(di->di_eattr);
	ip->i_atime_nsec = be32_to_cpu(di->di_atime_nsec);
	ip->i_mtime_nsec = be32_to_cpu(di->di_mtime_nsec);
	ip->i_ctime_nsec = be32_to_cpu(di->di_ctime_nsec);
}

void lgfs2_dinode_out(struct gfs2_inode *ip, char *buf)
{
	struct gfs2_dinode *di = (struct gfs2_dinode *)buf;

	di->di_header.mh_magic = cpu_to_be32(ip->i_magic);
	di->di_header.mh_type = cpu_to_be32(ip->i_type);
	di->di_header.mh_format = cpu_to_be32(ip->i_format);
	di->di_num.no_formal_ino = cpu_to_be64(ip->i_formal_ino);
	di->di_num.no_addr = cpu_to_be64(ip->i_addr);
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
	di->__pad1 = cpu_to_be16(ip->i_pad1);
	di->di_height = cpu_to_be16(ip->i_height);
	di->__pad2 = cpu_to_be32(ip->i_pad2);
	di->__pad3 = cpu_to_be16(ip->i_pad3);
	di->di_depth = cpu_to_be16(ip->i_depth);
	di->di_entries = cpu_to_be32(ip->i_entries);

	di->__pad4.no_addr = cpu_to_be64(ip->i_pad4_addr);
	di->__pad4.no_formal_ino = cpu_to_be64(ip->i_pad4_formal_ino);

	di->di_eattr = cpu_to_be64(ip->i_eattr);
	di->di_atime_nsec = cpu_to_be32(ip->i_atime_nsec);
	di->di_mtime_nsec = cpu_to_be32(ip->i_mtime_nsec);
	di->di_ctime_nsec = cpu_to_be32(ip->i_ctime_nsec);
}

void lgfs2_dinode_print(void *dip)
{
	struct gfs2_dinode *di = dip;

	lgfs2_meta_header_print(&di->di_header);
	lgfs2_inum_print(&di->di_num);

	print_it("  di_mode", "0%"PRIo32, NULL, be32_to_cpu(di->di_mode));
	printbe32(di, di_uid);
	printbe32(di, di_gid);
	printbe32(di, di_nlink);
	printbe64(di, di_size);
	printbe64(di, di_blocks);
	printbe64(di, di_atime);
	printbe64(di, di_mtime);
	printbe64(di, di_ctime);
	printbe32(di, di_major);
	printbe32(di, di_minor);
	printbe64(di, di_goal_meta);
	printbe64(di, di_goal_data);
	print_it("  di_flags", "0x%.8"PRIX32, NULL, be32_to_cpu(di->di_flags));
	printbe32(di, di_payload_format);
	printbe16(di, di_height);
	printbe16(di, di_depth);
	printbe32(di, di_entries);
	printbe64(di, di_eattr);
}

void gfs2_dirent_in(struct gfs2_dirent *de, char *buf)
{
	struct gfs2_dirent *str = (struct gfs2_dirent *)buf;

	gfs2_inum_in(&de->de_inum, buf);
	CPIN_32(de, str, de_hash);
	CPIN_16(de, str, de_rec_len);
	CPIN_16(de, str, de_name_len);
	CPIN_16(de, str, de_type);
	CPIN_16(de, str, de_rahead);
	CPIN_32(de, str, de_cookie);
	CPIN_08(de, str, pad3, 8);
}

void gfs2_dirent_out(struct gfs2_dirent *de, char *buf)
{
	struct gfs2_dirent *str = (struct gfs2_dirent *)buf;

	gfs2_inum_out(&de->de_inum, buf);
	CPOUT_32(de, str, de_hash);
	CPOUT_16(de, str, de_rec_len);
	CPOUT_16(de, str, de_name_len);
	CPOUT_16(de, str, de_type);
	CPOUT_16(de, str, de_rahead);
	CPOUT_32(de, str, de_cookie);
	CPOUT_08(de, str, pad3, 8);
}

void gfs2_leaf_in(struct gfs2_leaf *lf, char *buf)
{
	struct gfs2_leaf *str = (struct gfs2_leaf *)buf;

	gfs2_meta_header_in(&lf->lf_header, buf);
	CPIN_16(lf, str, lf_depth);
	CPIN_16(lf, str, lf_entries);
	CPIN_32(lf, str, lf_dirent_format);
	CPIN_64(lf, str, lf_next);
	CPIN_64(lf, str, lf_inode);
	CPIN_32(lf, str, lf_dist);
	CPIN_32(lf, str, lf_nsec);
	CPIN_64(lf, str, lf_sec);
	CPIN_08(lf, str, lf_reserved2, 40);
}

void gfs2_leaf_out(struct gfs2_leaf *lf, char *buf)
{
	struct gfs2_leaf *str = (struct gfs2_leaf *)buf;

	gfs2_meta_header_out(&lf->lf_header, buf);
	CPOUT_16(lf, str, lf_depth);
	CPOUT_16(lf, str, lf_entries);
	CPOUT_32(lf, str, lf_dirent_format);
	CPOUT_64(lf, str, lf_next);
	CPOUT_64(lf, str, lf_inode);
	CPOUT_32(lf, str, lf_dist);
	CPOUT_32(lf, str, lf_nsec);
	CPOUT_64(lf, str, lf_sec);
	CPOUT_08(lf, str, lf_reserved2, 40);
}

void lgfs2_leaf_print(void *lfp)
{
	struct gfs2_leaf *lf = lfp;

	lgfs2_meta_header_print(&lf->lf_header);
	printbe16(lf, lf_depth);
	printbe16(lf, lf_entries);
	printbe32(lf, lf_dirent_format);
	printbe64(lf, lf_next);
	printbe64(lf, lf_inode);
	printbe32(lf, lf_dist);
	printbe32(lf, lf_nsec);
	printbe64(lf, lf_sec);
}

void lgfs2_ea_header_print(void *eap)
{
	char buf[GFS2_EA_MAX_NAME_LEN + 1];
	struct gfs2_ea_header *ea = eap;
	unsigned len = ea->ea_name_len;

	printbe32(ea, ea_rec_len);
	printbe32(ea, ea_data_len);
	print8(ea, ea_name_len);
	print8(ea, ea_type);
	print8(ea, ea_flags);
	print8(ea, ea_num_ptrs);

	if (len > GFS2_EA_MAX_NAME_LEN)
		len = GFS2_EA_MAX_NAME_LEN;
	memcpy(buf, ea + 1, len);
	buf[len] = '\0';
	print_it("  name", "%s", NULL, buf);
}

void lgfs2_log_header_print(void *lhp)
{
	struct gfs2_log_header *lh = lhp;

	lgfs2_meta_header_print(&lh->lh_header);
	printbe64(lh, lh_sequence);
	print_it("  lh_flags", "0x%.8"PRIX32, NULL, be32_to_cpu(lh->lh_flags));
	printbe32(lh, lh_tail);
	printbe32(lh, lh_blkno);
	print_it("  lh_hash", "0x%.8"PRIX32, NULL, be32_to_cpu(lh->lh_hash));
	print_it("  lh_crc", "0x%.8"PRIX32, NULL, be32_to_cpu(lh->lh_crc));
	printbe32(lh, lh_nsec);
	printbe64(lh, lh_sec);
	printbe64(lh, lh_addr);
	printbe64(lh, lh_jinode);
	printbe64(lh, lh_statfs_addr);
	printbe64(lh, lh_quota_addr);
	print_it("  lh_local_total", "%"PRId64, "0x%"PRIx64, be64_to_cpu(lh->lh_local_total));
	print_it("  lh_local_free", "%"PRId64, "0x%"PRIx64, be64_to_cpu(lh->lh_local_free));
	print_it("  lh_local_dinodes", "%"PRId64, "0x%"PRIx64, be64_to_cpu(lh->lh_local_dinodes));
}

void lgfs2_log_descriptor_print(void *ldp)
{
	struct gfs2_log_descriptor *ld = ldp;

	lgfs2_meta_header_print(&ld->ld_header);
	printbe32(ld, ld_type);
	printbe32(ld, ld_length);
	printbe32(ld, ld_data1);
	printbe32(ld, ld_data2);
}

void lgfs2_statfs_change_print(void *scp)
{
	struct gfs2_statfs_change *sc = scp;

	print_it("  sc_total", "%"PRId64, "0x%"PRIx64, be64_to_cpu(sc->sc_total));
	print_it("  sc_free", "%"PRId64, "0x%"PRIx64, be64_to_cpu(sc->sc_free));
	print_it("  sc_dinodes", "%"PRId64, "0x%"PRIx64, be64_to_cpu(sc->sc_dinodes));
}

void lgfs2_quota_change_print(void *qcp)
{
	struct gfs2_quota_change *qc = qcp;

	print_it("  qc_change", "%"PRId64, "0x%"PRIx64, be64_to_cpu(qc->qc_change));
	print_it("  qc_flags", "0x%.8"PRIX32, NULL, be32_to_cpu(qc->qc_flags));
	printbe32(qc, qc_id);
}

