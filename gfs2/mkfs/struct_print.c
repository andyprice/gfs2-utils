#include <stddef.h>
#include <string.h>
#include <inttypes.h>
#include <uuid.h>
#include <stdarg.h>
#include <libgfs2.h>

#include "struct_print.h"

__attribute__((format(printf,2,3)))
static void print_it(const char *label, const char *fmt, ...)
{
	va_list args;

	va_start(args, fmt);
	printf("%s: ", label);
	vprintf(fmt, args);
	va_end(args);
}

#define printbe16(struct, member) do { \
		print_it(" "#member, "%"PRIu16, be16_to_cpu(struct->member)); \
	} while(0)
#define printbe32(struct, member) do { \
		print_it(" "#member, "%"PRIu32, be32_to_cpu(struct->member)); \
	} while(0)
#define printbe64(struct, member) do { \
		print_it(" "#member, "%"PRIu64, be64_to_cpu(struct->member)); \
	} while(0)

void inum_print(void *nop)
{
	struct gfs2_inum *no = nop;

	printbe64(no, no_formal_ino);
	printbe64(no, no_addr);
}

void meta_header_print(void *mhp)
{
	struct gfs2_meta_header *mh = mhp;

	print_it(" mh_magic", "0x%08"PRIX32, be32_to_cpu(mh->mh_magic));
	printbe32(mh, mh_type);
	printbe32(mh, mh_format);
}

void sb_print(void *sbp)
{
	struct gfs2_sb *sb = sbp;
	char readable_uuid[36+1];

	meta_header_print(&sb->sb_header);
	printbe32(sb, sb_fs_format);
	printbe32(sb, sb_multihost_format);
	printbe32(sb, sb_bsize);
	printbe32(sb, sb_bsize_shift);
	inum_print(&sb->sb_master_dir);
	inum_print(&sb->sb_root_dir);
	print_it(" sb_lockproto", "%.64s", sb->sb_lockproto);
	print_it(" sb_locktable", "%.64s", sb->sb_locktable);
	uuid_unparse(sb->sb_uuid, readable_uuid);
	print_it(" uuid", "%36s", readable_uuid);
}

void rindex_print(void *rip)
{
	struct gfs2_rindex *ri = rip;

	printbe64(ri, ri_addr);
	printbe32(ri, ri_length);
	printbe64(ri, ri_data0);
	printbe32(ri, ri_data);
	printbe32(ri, ri_bitbytes);
}

void rgrp_print(void *rgp)
{
	struct gfs2_rgrp *rg = rgp;

	meta_header_print(&rg->rg_header);
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

void quota_print(void *qp)
{
	struct gfs2_quota *q = qp;

	printbe64(q, qu_limit);
	printbe64(q, qu_warn);
	printbe64(q, qu_value);
}

void dinode_print(void *dip)
{
	struct gfs2_dinode *di = dip;

	meta_header_print(&di->di_header);
	inum_print(&di->di_num);

	print_it(" di_mode", "0%"PRIo32, be32_to_cpu(di->di_mode));
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
	print_it(" di_flags", "0x%.8"PRIX32, be32_to_cpu(di->di_flags));
	printbe32(di, di_payload_format);
	printbe16(di, di_height);
	printbe16(di, di_depth);
	printbe32(di, di_entries);
	printbe64(di, di_eattr);
}

void leaf_print(void *lfp)
{
	struct gfs2_leaf *lf = lfp;

	meta_header_print(&lf->lf_header);
	printbe16(lf, lf_depth);
	printbe16(lf, lf_entries);
	printbe32(lf, lf_dirent_format);
	printbe64(lf, lf_next);
	printbe64(lf, lf_inode);
	printbe32(lf, lf_dist);
	printbe32(lf, lf_nsec);
	printbe64(lf, lf_sec);
}

void log_header_print(void *lhp)
{
	struct gfs2_log_header *lh = lhp;

	meta_header_print(&lh->lh_header);
	printbe64(lh, lh_sequence);
	print_it(" lh_flags", "0x%.8"PRIX32, be32_to_cpu(lh->lh_flags));
	printbe32(lh, lh_tail);
	printbe32(lh, lh_blkno);
	print_it(" lh_hash", "0x%.8"PRIX32, be32_to_cpu(lh->lh_hash));
	print_it(" lh_crc", "0x%.8"PRIX32, be32_to_cpu(lh->lh_crc));
	printbe32(lh, lh_nsec);
	printbe64(lh, lh_sec);
	printbe64(lh, lh_addr);
	printbe64(lh, lh_jinode);
	printbe64(lh, lh_statfs_addr);
	printbe64(lh, lh_quota_addr);
	print_it(" lh_local_total", "%"PRId64, be64_to_cpu(lh->lh_local_total));
	print_it(" lh_local_free", "%"PRId64, be64_to_cpu(lh->lh_local_free));
	print_it(" lh_local_dinodes", "%"PRId64, be64_to_cpu(lh->lh_local_dinodes));
}

void log_descriptor_print(void *ldp)
{
	struct gfs2_log_descriptor *ld = ldp;

	meta_header_print(&ld->ld_header);
	printbe32(ld, ld_type);
	printbe32(ld, ld_length);
	printbe32(ld, ld_data1);
	printbe32(ld, ld_data2);
}

void statfs_change_print(void *scp)
{
	struct gfs2_statfs_change *sc = scp;

	print_it(" sc_total", "%"PRId64, be64_to_cpu(sc->sc_total));
	print_it(" sc_free", "%"PRId64, be64_to_cpu(sc->sc_free));
	print_it(" sc_dinodes", "%"PRId64, be64_to_cpu(sc->sc_dinodes));
}

void quota_change_print(void *qcp)
{
	struct gfs2_quota_change *qc = qcp;

	print_it(" qc_change", "%"PRId64, be64_to_cpu(qc->qc_change));
	print_it(" qc_flags", "0x%.8"PRIX32, be32_to_cpu(qc->qc_flags));
	printbe32(qc, qc_id);
}

