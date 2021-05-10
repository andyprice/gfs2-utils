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

void gfs2_rindex_in(struct gfs2_rindex *ri, char *buf)
{
	struct gfs2_rindex *str = (struct gfs2_rindex *)buf;

	CPIN_64(ri, str, ri_addr);
	CPIN_32(ri, str, ri_length);
	CPIN_32(ri, str, __pad);
	CPIN_64(ri, str, ri_data0);
	CPIN_32(ri, str, ri_data);
	CPIN_32(ri, str, ri_bitbytes);
	CPIN_08(ri, str, ri_reserved, sizeof(ri->ri_reserved));
}

void gfs2_rindex_out(const struct gfs2_rindex *ri, char *buf)
{
	struct gfs2_rindex *str = (struct gfs2_rindex *)buf;

	CPOUT_64(ri, str, ri_addr);
	CPOUT_32(ri, str, ri_length);
	str->__pad = 0;

	CPOUT_64(ri, str, ri_data0);
	CPOUT_32(ri, str, ri_data);

	CPOUT_32(ri, str, ri_bitbytes);

	CPOUT_08(ri, str, ri_reserved, sizeof(ri->ri_reserved));
}

void gfs2_rindex_print(const struct gfs2_rindex *ri)
{
	pv(ri, ri_addr, "%"PRIu64, "0x%"PRIx64);
	pv(ri, ri_length, "%"PRIu32, "0x%"PRIx32);

	pv(ri, ri_data0, "%"PRIu64, "0x%"PRIx64);
	pv(ri, ri_data, "%"PRIu32, "0x%"PRIx32);

	pv(ri, ri_bitbytes, "%"PRIu32, "0x%"PRIx32);
}

void gfs2_rgrp_in(struct gfs2_rgrp *rg, char *buf)
{
	struct gfs2_rgrp *str = (struct gfs2_rgrp *)buf;

	gfs2_meta_header_in(&rg->rg_header, buf);
	CPIN_32(rg, str, rg_flags);
	CPIN_32(rg, str, rg_free);
	CPIN_32(rg, str, rg_dinodes);
	CPIN_32(rg, str, rg_skip);
	CPIN_64(rg, str, rg_igeneration);
	CPIN_64(rg, str, rg_data0);
	CPIN_32(rg, str, rg_data);
	CPIN_32(rg, str, rg_bitbytes);
	CPIN_32(rg, str, rg_crc);
	CPIN_08(rg, str, rg_reserved, sizeof(rg->rg_reserved));
}

void gfs2_rgrp_out(const struct gfs2_rgrp *rg, char *buf)
{
	struct gfs2_rgrp *str = (struct gfs2_rgrp *)buf;

	gfs2_meta_header_out(&rg->rg_header, buf);
	CPOUT_32(rg, str, rg_flags);
	CPOUT_32(rg, str, rg_free);
	CPOUT_32(rg, str, rg_dinodes);
	CPOUT_32(rg, str, rg_skip);
	CPOUT_64(rg, str, rg_igeneration);
	CPOUT_64(rg, str, rg_data0);
	CPOUT_32(rg, str, rg_data);
	CPOUT_32(rg, str, rg_bitbytes);
	CPOUT_08(rg, str, rg_reserved, sizeof(rg->rg_reserved));
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

void gfs2_dinode_in(struct gfs2_dinode *di, char *buf)
{
	struct gfs2_dinode *str = (struct gfs2_dinode *)buf;

	gfs2_meta_header_in(&di->di_header, buf);
	gfs2_inum_in(&di->di_num, (char *)&str->di_num);

	CPIN_32(di, str, di_mode);
	CPIN_32(di, str, di_uid);
	CPIN_32(di, str, di_gid);
	CPIN_32(di, str, di_nlink);
	CPIN_64(di, str, di_size);
	CPIN_64(di, str, di_blocks);
	CPIN_64(di, str, di_atime);
	CPIN_64(di, str, di_mtime);
	CPIN_64(di, str, di_ctime);
	CPIN_32(di, str, di_major);
	CPIN_32(di, str, di_minor);

	CPIN_64(di, str, di_goal_meta);
	CPIN_64(di, str, di_goal_data);

	CPIN_32(di, str, di_flags);
	CPIN_32(di, str, di_payload_format);
	CPIN_16(di, str, __pad1);
	CPIN_16(di, str, di_height);

	CPIN_16(di, str, di_depth);
	CPIN_32(di, str, di_entries);

	CPIN_64(di, str, di_eattr);

	CPIN_08(di, str, di_reserved, 32);
}

void gfs2_dinode_out(struct gfs2_dinode *di, char *buf)
{
	struct gfs2_dinode *str = (struct gfs2_dinode *)buf;

	gfs2_meta_header_out(&di->di_header, buf);
	gfs2_inum_out(&di->di_num, (char *)&str->di_num);

	CPOUT_32(di, str, di_mode);
	CPOUT_32(di, str, di_uid);
	CPOUT_32(di, str, di_gid);
	CPOUT_32(di, str, di_nlink);
	CPOUT_64(di, str, di_size);
	CPOUT_64(di, str, di_blocks);
	CPOUT_64(di, str, di_atime);
	CPOUT_64(di, str, di_mtime);
	CPOUT_64(di, str, di_ctime);
	CPOUT_32(di, str, di_major);
	CPOUT_32(di, str, di_minor);

	CPOUT_64(di, str, di_goal_meta);
	CPOUT_64(di, str, di_goal_data);

	CPOUT_32(di, str, di_flags);
	CPOUT_32(di, str, di_payload_format);
	CPOUT_16(di, str, __pad1);
	CPOUT_16(di, str, di_height);

	CPOUT_16(di, str, di_depth);
	CPOUT_32(di, str, di_entries);

	CPOUT_64(di, str, di_eattr);

	CPOUT_08(di, str, di_reserved, 32);
}

void gfs2_dinode_print(const struct gfs2_dinode *di)
{
	gfs2_meta_header_print(&di->di_header);
	gfs2_inum_print(&di->di_num);

	pv(di, di_mode, "0%"PRIo32, NULL);
	pv(di, di_uid, "%"PRIu32, "0x%"PRIx32);
	pv(di, di_gid, "%"PRIu32, "0x%"PRIx32);
	pv(di, di_nlink, "%"PRIu32, "0x%"PRIx32);
	pv(di, di_size, "%"PRIu64, "0x%"PRIx64);
	pv(di, di_blocks, "%"PRIu64, "0x%"PRIx64);
	pv(di, di_atime, "%"PRIu64, "0x%"PRIx64);
	pv(di, di_mtime, "%"PRIu64, "0x%"PRIx64);
	pv(di, di_ctime, "%"PRIu64, "0x%"PRIx64);
	pv(di, di_major, "%"PRIu32, "0x%"PRIx32);
	pv(di, di_minor, "%"PRIu32, "0x%"PRIx32);

	pv(di, di_goal_meta, "%"PRIu64, "0x%"PRIx64);
	pv(di, di_goal_data, "%"PRIu64, "0x%"PRIx64);

	pv(di, di_flags, "0x%.8"PRIX32, NULL);
	pv(di, di_payload_format, "%"PRIu32, "0x%"PRIx32);
	pv(di, di_height, "%"PRIu16, "0x%"PRIx16);

	pv(di, di_depth, "%"PRIu16, "0x%"PRIx16);
	pv(di, di_entries, "%"PRIu32, "0x%"PRIx32);

	pv(di, di_eattr, "%"PRIu64, "0x%"PRIx64);
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

void gfs2_log_header_in(struct gfs2_log_header *lh, char *buf)
{
	struct gfs2_log_header *str = (struct gfs2_log_header *)buf;

	gfs2_meta_header_in(&lh->lh_header, buf);
	CPIN_64(lh, str, lh_sequence);
	CPIN_32(lh, str, lh_flags);
	CPIN_32(lh, str, lh_tail);
	CPIN_32(lh, str, lh_blkno);
	CPIN_32(lh, str, lh_hash);
	CPIN_32(lh, str, lh_crc);
	CPIN_32(lh, str, lh_nsec);
	CPIN_64(lh, str, lh_sec);
	CPIN_64(lh, str, lh_addr);
	CPIN_64(lh, str, lh_jinode);
	CPIN_64(lh, str, lh_statfs_addr);
	CPIN_64(lh, str, lh_quota_addr);
	CPIN_64(lh, str, lh_local_total);
	CPIN_64(lh, str, lh_local_free);
	CPIN_64(lh, str, lh_local_dinodes);
}

void gfs2_log_header_out(struct gfs2_log_header *lh, char *buf)
{
	struct gfs2_log_header *str = (struct gfs2_log_header *)buf;

	gfs2_meta_header_out(&lh->lh_header, buf);
	CPOUT_64(lh, str, lh_sequence);
	CPOUT_32(lh, str, lh_flags);
	CPOUT_32(lh, str, lh_tail);
	CPOUT_32(lh, str, lh_blkno);
	CPOUT_32(lh, str, lh_hash);
	CPOUT_32(lh, str, lh_crc);
	CPOUT_32(lh, str, lh_nsec);
	CPOUT_64(lh, str, lh_sec);
	CPOUT_64(lh, str, lh_addr);
	CPOUT_64(lh, str, lh_jinode);
	CPOUT_64(lh, str, lh_statfs_addr);
	CPOUT_64(lh, str, lh_quota_addr);
	CPOUT_64(lh, str, lh_local_total);
	CPOUT_64(lh, str, lh_local_free);
	CPOUT_64(lh, str, lh_local_dinodes);
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

