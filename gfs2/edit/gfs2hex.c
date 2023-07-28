#include "clusterautoconfig.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <curses.h>
#include <uuid.h>
#include <libgfs2.h>

#include "hexedit.h"
#include "extended.h"
#include "gfs2hex.h"
#include "struct_print.h"

struct gfs2_dinode *di;
int line, termlines;
char edit_fmt[80];
char estring[1024];
char efield[64];
int edit_mode = 0;
int edit_row[DMODES], edit_col[DMODES];
int edit_size[DMODES], last_entry_onscreen[DMODES];
enum dsp_mode dmode = HEX_MODE; /* display mode */
uint64_t block = 0;
int blockhist = 0;
struct iinfo *indirect;
int indirect_blocks;
struct lgfs2_sbd sbd;
uint64_t starting_blk;
struct blkstack_info blockstack[BLOCK_STACK_SIZE];
int identify = FALSE;
uint64_t max_block = 0;
int start_row[DMODES], end_row[DMODES], lines_per_row[DMODES];
int gfs2_struct_type;
unsigned int offset;
struct indirect_info masterdir;
int print_entry_ndx;
int screen_chunk_size = 512;
uint64_t temp_blk;
int color_scheme = 0;
int struct_len;
uint64_t dev_offset = 0;
int editing = 0;
int insert = 0;
const char *termtype;
WINDOW *wind;
int dsplines = 0;

static const char *block_type_str[15] = {
	"Clump",
	"Superblock",
	"Resource Group Header",
	"Resource Group Bitmap",
	"Dinode",
	"Indirect Block",
	"Leaf",
	"Journaled Data",
	"Log Header",
	"Log descriptor",
	"Ext. attrib",
	"Eattr Data",
	"Log Buffer",
	"Metatype 13",
	"Quota Change",
};

void eol(int col) /* end of line */
{
	if (termlines) {
		line++;
		move(line, col);
	} else {
		printf("\n");
		for (; col > 0; col--)
			printf(" ");
	}
}

void print_gfs2(const char *fmt, ...)
{
	va_list args;
	char string[PATH_MAX];
	
	memset(string, 0, sizeof(string));
	va_start(args, fmt);
	vsprintf(string, fmt, args);
	if (termlines)
		printw("%s", string);
	else
		printf("%s", string);
	va_end(args);
}

void idirent_in(struct idirent *id, void *dep)
{
	struct gfs2_dirent *de = dep;

	lgfs2_inum_in(&id->inum, &de->de_inum);
	id->hash = be32_to_cpu(de->de_hash);
	id->rec_len = be16_to_cpu(de->de_rec_len);
	id->name_len = be16_to_cpu(de->de_name_len);
	id->type = be16_to_cpu(de->de_type);
	id->rahead = be16_to_cpu(de->de_rahead);
	memcpy(id->filename, (char *)de + sizeof(*de), id->name_len);
	id->filename[id->name_len] = '\0';
}

static int indirect_dirent(struct indirect_info *indir, void *ptr, int d)
{
	struct gfs2_dirent *de = ptr;
	int ret = 0;

	if (be16_to_cpu(de->de_rec_len) < sizeof(struct gfs2_dirent) ||
	    be16_to_cpu(de->de_rec_len) > 4096 - sizeof(struct gfs2_dirent))
		return -1;
	if (de->de_inum.no_addr) {
		idirent_in(&indir->dirent[d], ptr);
		indir->block = be64_to_cpu(de->de_inum.no_addr);
		indir->is_dir = TRUE;
		indir->dirents++;
		ret = indir->dirent[d].rec_len;
	}
	return ret;
}

void do_dinode_extended(char *buf)
{
	struct gfs2_dinode *dip = (void *)buf;
	unsigned int x, y, ptroff = 0;
	uint64_t p, last;
	int isdir = S_ISDIR(be32_to_cpu(dip->di_mode));

	indirect_blocks = 0;
	memset(indirect, 0, sizeof(struct iinfo));
	if (be16_to_cpu(dip->di_height) > 0) {
		/* Indirect pointers */
		for (x = sizeof(struct gfs2_dinode); x < sbd.sd_bsize;
			 x += sizeof(uint64_t)) {
			p = be64_to_cpu(*(__be64 *)(buf + x));
			if (p) {
				indirect->ii[indirect_blocks].block = p;
				indirect->ii[indirect_blocks].mp.mp_list[0] =
					ptroff;
				indirect->ii[indirect_blocks].is_dir = FALSE;
				indirect->ii[indirect_blocks].ptroff =
				              (x - sizeof(*dip)) / sizeof(uint64_t);
				indirect_blocks++;
			}
			ptroff++;
		}
	}
	else if (isdir && !(be32_to_cpu(dip->di_flags) & GFS2_DIF_EXHASH)) {
		int skip = 0;

		/* Directory Entries: */
		indirect->ii[0].dirents = 0;
		indirect->ii[0].block = block;
		indirect->ii[0].is_dir = TRUE;
		for (x = sizeof(struct gfs2_dinode); x < sbd.sd_bsize; x += skip) {
			skip = indirect_dirent(indirect->ii, buf + x,
					       indirect->ii[0].dirents);
			if (skip <= 0)
				break;
		}
	}
	else if (isdir && (be32_to_cpu(dip->di_flags) & GFS2_DIF_EXHASH) &&
	         dip->di_height == 0) {
		/* Leaf Pointers: */

		last = be64_to_cpu(*(__be64 *)(buf + sizeof(struct gfs2_dinode)));

		for (x = sizeof(struct gfs2_dinode), y = 0;
			 y < (1 << be16_to_cpu(dip->di_depth));
			 x += sizeof(uint64_t), y++) {
			p = be64_to_cpu(*(__be64 *)(buf + x));

			if (p != last || ((y + 1) * sizeof(uint64_t) == be64_to_cpu(dip->di_size))) {
				struct lgfs2_buffer_head *tmp_bh;
				int skip = 0, direntcount = 0;
				unsigned int bufoffset;

				if (last >= max_block)
					break;
				tmp_bh = lgfs2_bread(&sbd, last);
				indirect->ii[indirect_blocks].dirents = 0;
				for (direntcount = 0, bufoffset = sizeof(struct gfs2_leaf);
					 bufoffset < sbd.sd_bsize;
					 direntcount++, bufoffset += skip) {
					skip = indirect_dirent(&indirect->ii[indirect_blocks],
										   tmp_bh->b_data + bufoffset,
										   direntcount);
					if (skip <= 0)
						break;
				}
				lgfs2_brelse(tmp_bh);
				indirect->ii[indirect_blocks].block = last;
				indirect_blocks++;
				last = p;
			} /* if not duplicate pointer */
		} /* for indirect pointers found */
	} /* if exhash */
}/* do_dinode_extended */

/**
 * Returns: next leaf block, if any, in a chain of leaf blocks
 */
uint64_t do_leaf_extended(char *dlebuf, struct iinfo *indir)
{
	int x, i;
	struct gfs2_dirent *de;

	x = 0;
	memset(indir, 0, sizeof(*indir));
	memcpy(&indir->ii[0].lf, dlebuf, sizeof(struct gfs2_leaf));
	/* Directory Entries: */
	for (i = sizeof(struct gfs2_leaf); i < sbd.sd_bsize; i += be16_to_cpu(de->de_rec_len)) {
		de = (struct gfs2_dirent *)(dlebuf + i);
		if (de->de_inum.no_addr) {
			idirent_in(&indir->ii[0].dirent[x], de);
			indir->ii[0].block = be64_to_cpu(de->de_inum.no_addr);
			indir->ii[0].is_dir = TRUE;
			indir->ii[0].dirents++;
			x++;
		}
		if (be16_to_cpu(de->de_rec_len) <= sizeof(struct gfs2_dirent))
			break;
	}
	return be64_to_cpu(indir->ii[0].lf.lf_next);
}

static void do_eattr_extended(char *buf)
{
	struct gfs2_ea_header *ea;
	uint32_t rec_len = 0;
	unsigned int x;

	eol(0);
	print_gfs2("Eattr Entries:");
	eol(0);

	for (x = sizeof(struct gfs2_meta_header); x < sbd.sd_bsize; x += rec_len)
	{
		eol(0);
		buf += x;
		ea = (struct gfs2_ea_header *)buf;
		ea_header_print(ea);
		rec_len = be32_to_cpu(ea->ea_rec_len);
	}
}

void display_gfs2(void *buf)
{
	struct gfs2_meta_header *mh = buf;
	uint32_t magic;
	uint32_t type;

	magic = be32_to_cpu(mh->mh_magic);
	type = be32_to_cpu(mh->mh_type);

	if (magic != GFS2_MAGIC) {
		print_gfs2("Unknown block type");
		eol(0);
		return;
	}

	if (type <= GFS2_METATYPE_QC)
		print_gfs2("%s:", block_type_str[type]);
	eol(0);

	switch (type)
	{
	case GFS2_METATYPE_SB:
		sb_print(buf);
		break;
	case GFS2_METATYPE_RG:
		rgrp_print(buf);
		break;
	case GFS2_METATYPE_DI:
		dinode_print(di);
		break;
	case GFS2_METATYPE_LF:
		leaf_print(buf);
		break;
	case GFS2_METATYPE_LH:
		log_header_print(buf);
		break;
	case GFS2_METATYPE_LD:
		log_descriptor_print(buf);
		break;
	case GFS2_METATYPE_EA:
		do_eattr_extended(buf);
		break;
	case GFS2_METATYPE_QC:
		quota_change_print(buf);
		break;
	case GFS2_METATYPE_RB:
	case GFS2_METATYPE_IN:
	case GFS2_METATYPE_JD:
	case GFS2_METATYPE_ED:
	case GFS2_METATYPE_LB:
		meta_header_print(mh);
		break;
	default:
		print_gfs2("Unknown block type");
		eol(0);
		break;
	}
}
