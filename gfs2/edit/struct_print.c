#include <stddef.h>
#include <stdarg.h>
#include <string.h>
#include <inttypes.h>
#include <uuid.h>
#include <libgfs2.h>

#include "hexedit.h"
#include "gfs2hex.h"
#include "struct_print.h"

static void check_highlight(int highlight)
{
	if (!termlines || line >= termlines) /* If printing or out of bounds */
		return;
	if (dmode == HEX_MODE) {
		if (line == (edit_row[dmode] * lines_per_row[dmode]) + 4) {
			if (highlight) {
				COLORS_HIGHLIGHT;
				last_entry_onscreen[dmode] = print_entry_ndx;
			} else
				COLORS_NORMAL;
		}
	} else {
		if ((line * lines_per_row[dmode]) - 4 ==
			(edit_row[dmode] - start_row[dmode]) * lines_per_row[dmode]) {
			if (highlight) {
				COLORS_HIGHLIGHT;
				last_entry_onscreen[dmode] = print_entry_ndx;
			}
			else
				COLORS_NORMAL;
		}
	}
}

__attribute__((format(printf,2,4)))
static void print_it(const char *label, const char *fmt, const char *fmt2, ...)
{
	va_list args;
	char tmp_string[NAME_MAX];
	const char *fmtstring;
	int decimalsize;

	if (!termlines || line < termlines) {
		va_start(args, fmt2);
		check_highlight(TRUE);
		if (termlines) {
			move(line,0);
			printw("%s", label);
			move(line,24);
		} else {
			if (!strcmp(label, "  "))
				printf("%-11s", label);
			else
				printf("%-24s", label);
		}
		vsprintf(tmp_string, fmt, args);

		if (termlines)
			printw("%s", tmp_string);
		else
			printf("%s", tmp_string);
		check_highlight(FALSE);

		if (fmt2) {
			decimalsize = strlen(tmp_string);
			va_end(args);
			va_start(args, fmt2);
			vsprintf(tmp_string, fmt2, args);
			check_highlight(TRUE);
			if (termlines) {
				move(line, 50);
				printw("%s", tmp_string);
			} else {
				int i;
				for (i=20 - decimalsize; i > 0; i--)
					printf(" ");
				printf("%s", tmp_string);
			}
			check_highlight(FALSE);
		} else {
			if (strstr(fmt,"X") || strstr(fmt,"x"))
				fmtstring="(hex)";
			else if (strstr(fmt,"s"))
				fmtstring="";
			else
				fmtstring="(decimal)";
			if (termlines) {
				move(line, 50);
				printw("%s", fmtstring);
			}
			else
				printf("%s", fmtstring);
		}
		if (termlines) {
			refresh();
			if (line == (edit_row[dmode] * lines_per_row[dmode]) + 4) {
				strncpy(efield, label + 2, 63); /* it's indented */
				efield[63] = '\0';
				strcpy(estring, tmp_string);
				strncpy(edit_fmt, fmt, 79);
				edit_fmt[79] = '\0';
				edit_size[dmode] = strlen(estring);
				COLORS_NORMAL;
			}
			last_entry_onscreen[dmode] = (line / lines_per_row[dmode]) - 4;
		}
		eol(0);
		va_end(args);
	}
}

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

void inum_print(void *nop)
{
	struct gfs2_inum *no = nop;

	printbe64(no, no_formal_ino);
	printbe64(no, no_addr);
}

void meta_header_print(void *mhp)
{
	struct gfs2_meta_header *mh = mhp;

	print_it("  mh_magic", "0x%08"PRIX32, NULL, be32_to_cpu(mh->mh_magic));
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
	print_it("  sb_lockproto", "%.64s", NULL, sb->sb_lockproto);
	print_it("  sb_locktable", "%.64s", NULL, sb->sb_locktable);
	uuid_unparse(sb->sb_uuid, readable_uuid);
	print_it("  uuid", "%36s", NULL, readable_uuid);
}

void gfs_sb_print(void *sbp)
{
	struct gfs_sb *sb = sbp;

	meta_header_print(&sb->sb_header);
	printbe32(sb, sb_fs_format);
	printbe32(sb, sb_multihost_format);
	printbe32(sb, sb_flags);
	printbe32(sb, sb_bsize);
	printbe32(sb, sb_bsize_shift);
	printbe32(sb, sb_seg_size);
	inum_print(&sb->sb_jindex_di);
	inum_print(&sb->sb_rindex_di);
	inum_print(&sb->sb_root_di);
	print_it("  sb_lockproto", "%.64s", NULL, sb->sb_lockproto);
	print_it("  sb_locktable", "%.64s", NULL, sb->sb_locktable);
	inum_print(&sb->sb_quota_di);
	inum_print(&sb->sb_license_di);
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

void gfs_rgrp_print(void *rgp)
{
	struct gfs_rgrp *rg = rgp;

	meta_header_print(&rg->rg_header);
	printbe32(rg, rg_flags);
	printbe32(rg, rg_free);
	printbe32(rg, rg_useddi);
	printbe32(rg, rg_freedi);
	inum_print(&rg->rg_freedi_list);
	printbe32(rg, rg_usedmeta);
	printbe32(rg, rg_freemeta);
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
	struct gfs2_dinode *_di = dip;

	meta_header_print(&_di->di_header);
	inum_print(&_di->di_num);

	print_it("  di_mode", "0%"PRIo32, NULL, be32_to_cpu(di->di_mode));
	printbe32(_di, di_uid);
	printbe32(_di, di_gid);
	printbe32(_di, di_nlink);
	printbe64(_di, di_size);
	printbe64(_di, di_blocks);
	printbe64(_di, di_atime);
	printbe64(_di, di_mtime);
	printbe64(_di, di_ctime);
	printbe32(_di, di_major);
	printbe32(_di, di_minor);
	printbe64(_di, di_goal_meta);
	printbe64(_di, di_goal_data);
	print_it("  di_flags", "0x%.8"PRIX32, NULL, be32_to_cpu(_di->di_flags));
	printbe32(_di, di_payload_format);
	printbe16(_di, di_height);
	printbe16(_di, di_depth);
	printbe32(_di, di_entries);
	printbe64(_di, di_eattr);
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

void ea_header_print(void *eap)
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

void log_header_print(void *lhp)
{
	struct gfs2_log_header *lh = lhp;

	meta_header_print(&lh->lh_header);
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

void gfs_log_header_print(void *lhp)
{
	struct gfs_log_header *lh = lhp;

	meta_header_print(&lh->lh_header);
	print_it("  lh_flags", "%"PRIu32, "0x%.8"PRIx32, be32_to_cpu(lh->lh_flags));
	print_it("  lh_pad", "%"PRIu32, "0x%"PRIx32, be32_to_cpu(lh->lh_pad));
	print_it("  lh_first", "%"PRIu64, "0x%"PRIx64, be64_to_cpu(lh->lh_first));
	print_it("  lh_sequence", "%"PRIu64, "0x%"PRIx64, be64_to_cpu(lh->lh_sequence));
	print_it("  lh_tail", "%"PRIu64, "0x%"PRIx64, be64_to_cpu(lh->lh_tail));
	print_it("  lh_last_dump", "%"PRIu64, "0x%"PRIx64, be64_to_cpu(lh->lh_last_dump));
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

	print_it("  sc_total", "%"PRId64, "0x%"PRIx64, be64_to_cpu(sc->sc_total));
	print_it("  sc_free", "%"PRId64, "0x%"PRIx64, be64_to_cpu(sc->sc_free));
	print_it("  sc_dinodes", "%"PRId64, "0x%"PRIx64, be64_to_cpu(sc->sc_dinodes));
}

void quota_change_print(void *qcp)
{
	struct gfs2_quota_change *qc = qcp;

	print_it("  qc_change", "%"PRId64, "0x%"PRIx64, be64_to_cpu(qc->qc_change));
	print_it("  qc_flags", "0x%.8"PRIX32, NULL, be32_to_cpu(qc->qc_flags));
	printbe32(qc, qc_id);
}

void gfs_jindex_print(struct gfs_jindex *ji)
{
	print_it("  ji_addr", "%"PRIu64, "0x%"PRIx64, be64_to_cpu(ji->ji_addr));
	print_it("  ji_nsegment", "%"PRIu32, "0x%"PRIx32, be32_to_cpu(ji->ji_nsegment));
	print_it("  ji_pad", "%"PRIu32, "0x%"PRIx32, be32_to_cpu(ji->ji_pad));
}
