#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <string.h>
#include <inttypes.h>
#include <sys/types.h>
#include <linux/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <curses.h>
#include <term.h>
#include <time.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <dirent.h>

#include <linux/gfs2_ondisk.h>
#include "copyright.cf"

#include "hexedit.h"
#include "libgfs2.h"
#include "extended.h"
#include "gfs2hex.h"

static void print_block_details(struct iinfo *ind, int level, int cur_height,
				int pndx, uint64_t file_offset);

/* ------------------------------------------------------------------------ */
/* get_height                                                               */
/* ------------------------------------------------------------------------ */
static int get_height(void)
{
	int cur_height = 0, i;

	if (gfs2_struct_type != GFS2_METATYPE_DI) {
		for (i = 0; i <= blockhist && i < 5; i++) {
			if (blockstack[(blockhist - i) %
				       BLOCK_STACK_SIZE].gfs2_struct_type ==
			    GFS2_METATYPE_DI)
				break;
			cur_height++;
		}
	}
	return cur_height;
}

/* ------------------------------------------------------------------------ */
/* _do_indirect_extended                                                    */
/* ------------------------------------------------------------------------ */
static int _do_indirect_extended(char *diebuf, struct iinfo *iinf, int hgt)
{
	unsigned int x, y;
	uint64_t p;
	int i_blocks;

	i_blocks = 0;
	for (x = 0; x < 512; x++) {
		iinf->ii[x].is_dir = 0;
		iinf->ii[x].height = 0;
		iinf->ii[x].block = 0;
		iinf->ii[x].dirents = 0;
		memset(&iinf->ii[x].dirent, 0, sizeof(struct gfs2_dirents));
	}
	for (x = (gfs1 ? sizeof(struct gfs_indirect):
			  sizeof(struct gfs2_meta_header)), y = 0;
		 x < sbd.bsize;
		 x += sizeof(uint64_t), y++) {
		p = be64_to_cpu(*(uint64_t *)(diebuf + x));
		if (p) {
			iinf->ii[i_blocks].block = p;
			iinf->ii[i_blocks].mp.mp_list[hgt] = i_blocks;
			iinf->ii[i_blocks].is_dir = FALSE;
			i_blocks++;
		}
	}
	return i_blocks;
}

/* ------------------------------------------------------------------------ */
/* do_indirect_extended                                                     */
/* ------------------------------------------------------------------------ */
int do_indirect_extended(char *diebuf, struct iinfo *iinf)
{
	return _do_indirect_extended(diebuf, iinf, get_height());
}

/* ------------------------------------------------------------------------ */
/* dinode_valid - check if we have a dinode in recent history               */
/* ------------------------------------------------------------------------ */
static int dinode_valid(void)
{
	int i;

	if (gfs2_struct_type == GFS2_METATYPE_DI)
		return 1;
	for (i = 0; i <= blockhist && i < 5; i++) {
		if (blockstack[(blockhist - i) %
			       BLOCK_STACK_SIZE].gfs2_struct_type ==
		    GFS2_METATYPE_DI)
			return 1;
	}
	return 0;
}

/* ------------------------------------------------------------------------ */
/* metapath_to_lblock - convert from metapath, height to logical block      */
/* ------------------------------------------------------------------------ */
static uint64_t metapath_to_lblock(struct metapath *mp, int hgt)
{
	int h;
	uint64_t lblock = 0;
	uint64_t factor[GFS2_MAX_META_HEIGHT];

	if (di.di_height < 2)
		return mp->mp_list[0];
	/* figure out multiplication factors for each height */
	memset(&factor, 0, sizeof(factor));
	factor[di.di_height - 1] = 1ull;
	for (h = di.di_height - 2; h >= 0; h--)
		factor[h] = factor[h + 1] * sbd.sd_inptrs;
	for (h = 0; h <= hgt; h++)
		lblock += (mp->mp_list[h] * factor[h]);
	return lblock;
}

/* ------------------------------------------------------------------------ */
/* display_indirect                                                         */
/* ------------------------------------------------------------------------ */
static int display_indirect(struct iinfo *ind, int indblocks, int level,
			    uint64_t startoff)
{
	int start_line, total_dirents;
	int cur_height = -1, pndx;

	last_entry_onscreen[dmode] = 0;
	if (!has_indirect_blocks())
		return -1;
	if (!level) {
		if (gfs2_struct_type == GFS2_METATYPE_DI) {
			if (S_ISDIR(di.di_mode))
				print_gfs2("This directory contains %d indirect blocks",
					   indblocks);
			else
				print_gfs2("This inode contains %d indirect blocks",
					   indblocks);
		} else
			print_gfs2("This indirect block contains %d indirect blocks",
				   indblocks);
	}
	total_dirents = 0;
	if (dinode_valid() && !S_ISDIR(di.di_mode)) {
		/* See if we are on an inode or have one in history. */
		if (level)
			cur_height = level;
		else {
			cur_height = get_height();
			print_gfs2("  (at height %d of %d)",
				   cur_height, di.di_height);
		}
	}
	eol(0);
	if (!level && indblocks) {
		print_gfs2("Indirect blocks:");
		eol(0);
	}
	start_line = line;
	for (pndx = start_row[dmode];
		 (!termlines || pndx < termlines - start_line - 1
		  + start_row[dmode]) && pndx < indblocks;
		 pndx++) {
		uint64_t file_offset;

		if (pndx && ind->ii[pndx].block == ind->ii[pndx - 1].block)
			continue;
		print_entry_ndx = pndx;
		if (termlines) {
			if (edit_row[dmode] >= 0 &&
			    line - start_line ==
			    edit_row[dmode] - start_row[dmode])
				COLORS_HIGHLIGHT;
			move(line, 1);
		}
		if (!termlines) {
			int h;

			for (h = 0; h < level; h++)
				print_gfs2("   ");
		}
		print_gfs2("%d => ", pndx);
		if (termlines)
			move(line,9);
		print_gfs2("0x%llx / %lld", ind->ii[pndx].block,
			   ind->ii[pndx].block);
		if (termlines) {
			if (edit_row[dmode] >= 0 &&
			    line - start_line ==
			    edit_row[dmode] - start_row[dmode]) { 
				sprintf(estring, "%"PRIx64,
					ind->ii[print_entry_ndx].block);
				strcpy(edit_fmt, "%"PRIx64);
				edit_size[dmode] = strlen(estring);
				COLORS_NORMAL;
			}
		}
		if (dinode_valid() && !S_ISDIR(di.di_mode)) {
			float human_off;
			char h;

			file_offset = metapath_to_lblock(&ind->ii[pndx].mp,
							 cur_height) *
				sbd.bsize;
			print_gfs2("     ");
			h = 'K';
			human_off = (file_offset / 1024.0);
			if (human_off > 1024.0) { h = 'M'; human_off /= 1024.0; }
			if (human_off > 1024.0) { h = 'G'; human_off /= 1024.0; }
			if (human_off > 1024.0) { h = 'T'; human_off /= 1024.0; }
			if (human_off > 1024.0) { h = 'P'; human_off /= 1024.0; }
			if (human_off > 1024.0) { h = 'E'; human_off /= 1024.0; }
			print_gfs2("(data offset 0x%llx / %lld / %6.2f%c)",
				   file_offset, file_offset, human_off, h);
			print_gfs2("   ");
		}
		else
			file_offset = 0;
		if (dinode_valid() && !termlines &&
		    ((level + 1 < di.di_height) ||
		     (S_ISDIR(di.di_mode) && level <= di.di_height))) {
			print_block_details(ind, level, cur_height, pndx,
					    file_offset);
		}
		print_entry_ndx = pndx; /* restore after recursion */
		eol(0);
	} /* for each display row */
	if (line >= 7) /* 7 because it was bumped at the end */
		last_entry_onscreen[dmode] = line - 7;
	eol(0);
	end_row[dmode] = indblocks;
	if (end_row[dmode] < last_entry_onscreen[dmode])
		end_row[dmode] = last_entry_onscreen[dmode];
	lines_per_row[dmode] = 1;
	return 0;
}

/* ------------------------------------------------------------------------ */
/* print_inode_type                                                         */
/* ------------------------------------------------------------------------ */
static void print_inode_type(__be16 de_type)
{
	if (gfs1) {
		switch(de_type) {
		case GFS_FILE_NON:
			print_gfs2("Unknown");
			break;
		case GFS_FILE_REG:
			print_gfs2("File   ");
			break;
		case GFS_FILE_DIR:
			print_gfs2("Dir    ");
			break;
		case GFS_FILE_LNK:
			print_gfs2("Symlink");
			break;
		case GFS_FILE_BLK:
			print_gfs2("BlkDev ");
			break;
		case GFS_FILE_CHR:
			print_gfs2("ChrDev ");
			break;
		case GFS_FILE_FIFO:
			print_gfs2("Fifo   ");
			break;
		case GFS_FILE_SOCK:
			print_gfs2("Socket ");
			break;
		default:
			print_gfs2("%04x   ", de_type);
			break;
		}
		return;
	}
	switch(de_type) {
	case DT_UNKNOWN:
		print_gfs2("Unknown");
		break;
	case DT_REG:
		print_gfs2("File   ");
		break;
	case DT_DIR:
		print_gfs2("Dir    ");
		break;
	case DT_LNK:
		print_gfs2("Symlink");
		break;
	case DT_BLK:
		print_gfs2("BlkDev ");
		break;
	case DT_CHR:
		print_gfs2("ChrDev ");
		break;
	case DT_FIFO:
		print_gfs2("Fifo   ");
		break;
	case DT_SOCK:
		print_gfs2("Socket ");
		break;
	default:
		print_gfs2("%04x   ", de_type);
		break;
	}
}

/* ------------------------------------------------------------------------ */
/* display_leaf - display directory leaf                                    */
/* ------------------------------------------------------------------------ */
static int display_leaf(struct iinfo *ind)
{
	int start_line, total_dirents = start_row[dmode];
	int d;

	eol(0);
	if (gfs2_struct_type == GFS2_METATYPE_SB)
		print_gfs2("The superblock has 2 directories");
	else
		print_gfs2("Directory block: lf_depth:%d, lf_entries:%d,"
			   "fmt:%d next=0x%llx (%d dirents).",
			   ind->ii[0].lf_depth, ind->ii[0].lf_entries,
			   ind->ii[0].lf_dirent_format,
			   ind->ii[0].lf_next,
			   ind->ii[0].dirents);

	start_line = line;
	for (d = start_row[dmode]; d < ind->ii[0].dirents; d++) {
		if (termlines && d >= termlines - start_line - 2
		    + start_row[dmode])
			break;
		total_dirents++;
		if (ind->ii[0].dirents >= 1) {
			eol(5);
			if (termlines) {
				if (edit_row[dmode] >=0 &&
				    line - start_line - 1 ==
				    edit_row[dmode] - start_row[dmode]) {
					COLORS_HIGHLIGHT;
					sprintf(estring, "%"PRIx64,
						ind->ii[0].dirent[d].block);
					strcpy(edit_fmt, "%"PRIx64);
				}
			}
			print_gfs2("%d. (%d). %lld (0x%llx): ",
				   total_dirents, d + 1,
				   ind->ii[0].dirent[d].block,
				   ind->ii[0].dirent[d].block);
		}
		print_inode_type(ind->ii[0].dirent[d].dirent.de_type);
		print_gfs2(" %s", ind->ii[0].dirent[d].filename);
		if (termlines) {
			if (edit_row[dmode] >= 0 &&
			    line - start_line - 1 == edit_row[dmode] -
			    start_row[dmode])
				COLORS_NORMAL;
		}
	}
	if (line >= 4)
		last_entry_onscreen[dmode] = line - 4;
	eol(0);
	end_row[dmode] = ind->ii[0].dirents;
	if (end_row[dmode] < last_entry_onscreen[dmode])
		end_row[dmode] = last_entry_onscreen[dmode];
	return 0;
}

/* ------------------------------------------------------------------------ */
/* print_block_details                                                      */
/* ------------------------------------------------------------------------ */
static void print_block_details(struct iinfo *ind, int level, int cur_height,
				int pndx, uint64_t file_offset)
{
	struct iinfo *more_indir;
	int more_ind;
	char *tmpbuf;
	uint64_t thisblk;

	thisblk = ind->ii[pndx].block;
	more_indir = malloc(sizeof(struct iinfo));
	if (!more_indir) {
		fprintf(stderr, "Out of memory in function "
			"display_indirect\n");
		return;
	}
	tmpbuf = malloc(sbd.bsize);
	if (!tmpbuf) {
		fprintf(stderr, "Out of memory in function "
			"display_indirect\n");
		return;
	}
	while (thisblk) {
		lseek(sbd.device_fd, thisblk * sbd.bsize, SEEK_SET);
		/* read in the desired block */
		if (read(sbd.device_fd, tmpbuf, sbd.bsize) != sbd.bsize) {
			fprintf(stderr, "bad read: %s from %s:%d: block %lld "
				"(0x%llx)\n", strerror(errno), __FUNCTION__,
				__LINE__,
				(unsigned long long)ind->ii[pndx].block,
				(unsigned long long)ind->ii[pndx].block);
			exit(-1);
		}
		thisblk = 0;
		memset(more_indir, 0, sizeof(struct iinfo));
		if (S_ISDIR(di.di_mode) && level == di.di_height) {
			thisblk = do_leaf_extended(tmpbuf, more_indir);
			display_leaf(more_indir);
		} else {
			int x;

			for (x = 0; x < 512; x++) {
				memcpy(&more_indir->ii[x].mp,
				       &ind->ii[pndx].mp,
				       sizeof(struct metapath));
				more_indir->ii[x].mp.mp_list[cur_height+1] = x;
			}
			more_ind = _do_indirect_extended(tmpbuf, more_indir,
							 cur_height + 1);
			display_indirect(more_indir, more_ind, level + 1,
					 file_offset);
		}
		if (thisblk) {
			eol(0);
			if (termlines)
				move(line,9);
			print_gfs2("Continuation block 0x%llx / %lld",
				   thisblk, thisblk);
		}
	}
	free(tmpbuf);
	free(more_indir);
}

/* ------------------------------------------------------------------------ */
/* gfs_jindex_print - print an jindex entry.                                */
/* ------------------------------------------------------------------------ */
static void gfs_jindex_print(struct gfs_jindex *ji)
{
        pv((unsigned long long)ji, ji_addr, "%llu", "0x%llx");
        pv(ji, ji_nsegment, "%u", "0x%x");
        pv(ji, ji_pad, "%u", "0x%x");
}

/* ------------------------------------------------------------------------ */
/* print_jindex - print the jindex file.                                    */
/* ------------------------------------------------------------------------ */
static int print_jindex(struct gfs2_inode *dij)
{
	int error, start_line;
	struct gfs_jindex ji;
	char jbuf[sizeof(struct gfs_jindex)];

	start_line = line;
	error = 0;
	print_gfs2("Journal index entries found: %d.",
		   dij->i_di.di_size / sizeof(struct gfs_jindex));
	eol(0);
	lines_per_row[dmode] = 4;
	for (print_entry_ndx=0; ; print_entry_ndx++) {
		error = gfs2_readi(dij, (void *)&jbuf,
				   print_entry_ndx*sizeof(struct gfs_jindex),
				   sizeof(struct gfs_jindex));
		gfs_jindex_in(&ji, jbuf);
		if (!error) /* end of file */
			break;
		if (!termlines ||
		    (print_entry_ndx >= start_row[dmode] &&
		     ((print_entry_ndx - start_row[dmode])+1) *
		     lines_per_row[dmode] <= termlines - start_line - 2)) {
			if (edit_row[dmode] == print_entry_ndx) {
				COLORS_HIGHLIGHT;
				strcpy(efield, "ji_addr");
				sprintf(estring, "%" PRIx64, ji.ji_addr);
			}
			print_gfs2("Journal #%d", print_entry_ndx);
			eol(0);
			if (edit_row[dmode] == print_entry_ndx)
				COLORS_NORMAL;
			gfs_jindex_print(&ji);
			last_entry_onscreen[dmode] = print_entry_ndx;
		}
	}
	end_row[dmode] = print_entry_ndx;
	return error;
}

/* ------------------------------------------------------------------------ */
/* parse_rindex - print the rgindex file.                                   */
/* ------------------------------------------------------------------------ */
static int parse_rindex(struct gfs2_inode *dip, int print_rindex)
{
	int error, start_line;
	struct gfs2_rindex ri;
	char rbuf[sizeof(struct gfs_rindex)];
	char highlighted_addr[32];

	start_line = line;
	error = 0;
	print_gfs2("RG index entries found: %d.", dip->i_di.di_size / risize());
	eol(0);
	lines_per_row[dmode] = 6;
	memset(highlighted_addr, 0, sizeof(highlighted_addr));

	for (print_entry_ndx=0; ; print_entry_ndx++) {
		uint64_t roff;

		roff = print_entry_ndx * risize();

		if (gfs1)
			error = gfs1_readi(dip, (void *)&rbuf, roff, risize());
		else
			error = gfs2_readi(dip, (void *)&rbuf, roff, risize());
		if (!error) /* end of file */
			break;
		gfs2_rindex_in(&ri, rbuf);
		if (!termlines ||
			(print_entry_ndx >= start_row[dmode] &&
			 ((print_entry_ndx - start_row[dmode])+1) * lines_per_row[dmode] <=
			 termlines - start_line - 2)) {
			if (edit_row[dmode] == print_entry_ndx) {
				COLORS_HIGHLIGHT;
				sprintf(highlighted_addr, "%llx", (unsigned long long)ri.ri_addr);
			}
			print_gfs2("RG #%d", print_entry_ndx);
			if (!print_rindex)
				print_gfs2(" located at: %llu (0x%llx)",
					   ri.ri_addr, ri.ri_addr);
			eol(0);
			if (edit_row[dmode] == print_entry_ndx)
				COLORS_NORMAL;
			if(print_rindex)
				gfs2_rindex_print(&ri);
			else {
				struct gfs2_buffer_head *tmp_bh;

				tmp_bh = bread(&sbd, ri.ri_addr);
				if (gfs1) {
					struct gfs_rgrp rg1;
					gfs_rgrp_in(&rg1, tmp_bh);
					gfs_rgrp_print(&rg1);
				} else {
					struct gfs2_rgrp rg;
					gfs2_rgrp_in(&rg, tmp_bh);
					gfs2_rgrp_print(&rg);
				}
				brelse(tmp_bh);
			}
			last_entry_onscreen[dmode] = print_entry_ndx;
		}
	}
	strcpy(estring, highlighted_addr);
	end_row[dmode] = print_entry_ndx;
	return error;
}

/* ------------------------------------------------------------------------ */
/* print_inum - print the inum file.                                        */
/* ------------------------------------------------------------------------ */
static int print_inum(struct gfs2_inode *dii)
{
	uint64_t inum, inodenum;
	int rc;
	
	rc = gfs2_readi(dii, (void *)&inum, 0, sizeof(inum));
	if (!rc) {
		print_gfs2("The inum file is empty.");
		eol(0);
		return 0;
	}
	if (rc != sizeof(inum)) {
		print_gfs2("Error reading inum file.");
		eol(0);
		return -1;
	}
	inodenum = be64_to_cpu(inum);
	print_gfs2("Next inode num = %lld (0x%llx)", inodenum, inodenum);
	eol(0);
	return 0;
}

/* ------------------------------------------------------------------------ */
/* print_statfs - print the statfs file.                                    */
/* ------------------------------------------------------------------------ */
static int print_statfs(struct gfs2_inode *dis)
{
	struct gfs2_statfs_change sfb, sfc;
	int rc;
	
	rc = gfs2_readi(dis, (void *)&sfb, 0, sizeof(sfb));
	if (!rc) {
		print_gfs2("The statfs file is empty.");
		eol(0);
		return 0;
	}
	if (rc != sizeof(sfb)) {
		print_gfs2("Error reading statfs file.");
		eol(0);
		return -1;
	}
	gfs2_statfs_change_in(&sfc, (char *)&sfb);
	print_gfs2("statfs file contents:");
	eol(0);
	gfs2_statfs_change_print(&sfc);
	return 0;
}

/* ------------------------------------------------------------------------ */
/* print_quota - print the quota file.                                      */
/* ------------------------------------------------------------------------ */
static int print_quota(struct gfs2_inode *diq)
{
	struct gfs2_quota qbuf, q;
	int i, error;
	
	print_gfs2("quota file contents:");
	eol(0);
	print_gfs2("quota entries found: %d.", diq->i_di.di_size / sizeof(q));
	eol(0);
	for (i=0; ; i++) {
		error = gfs2_readi(diq, (void *)&qbuf, i * sizeof(q), sizeof(qbuf));
		if (!error)
			break;
		if (error != sizeof(qbuf)) {
			print_gfs2("Error reading quota file.");
			eol(0);
			return -1;
		}
		gfs2_quota_in(&q, (char *)&qbuf);
		print_gfs2("Entry #%d", i + 1);
		eol(0);
		gfs2_quota_print(&q);
	}
	return 0;
}

/* ------------------------------------------------------------------------ */
/* display_extended                                                         */
/* ------------------------------------------------------------------------ */
int display_extended(void)
{
	struct gfs2_inode *tmp_inode;
	struct gfs2_buffer_head *tmp_bh;

	dsplines = termlines - line - 1;
	/* Display any indirect pointers that we have. */
	if (block_is_rindex()) {
		tmp_bh = bread(&sbd, block);
		tmp_inode = inode_get(&sbd, tmp_bh);
		parse_rindex(tmp_inode, TRUE);
		brelse(tmp_bh);
	}
	else if (has_indirect_blocks() && !indirect_blocks &&
		 !display_leaf(indirect))
		return -1;
	else if (display_indirect(indirect, indirect_blocks, 0, 0) == 0)
		return -1;
	else if (block_is_rglist()) {
		if (gfs1)
			tmp_bh = bread(&sbd, sbd1->sb_rindex_di.no_addr);
		else
			tmp_bh = bread(&sbd, masterblock("rindex"));
		tmp_inode = inode_get(&sbd, tmp_bh);
		parse_rindex(tmp_inode, FALSE);
		brelse(tmp_bh);
	}
	else if (block_is_jindex()) {
		tmp_bh = bread(&sbd, block);
		tmp_inode = inode_get(&sbd, tmp_bh);
		print_jindex(tmp_inode);
		brelse(tmp_bh);
	}
	else if (block_is_inum_file()) {
		tmp_bh = bread(&sbd, block);
		tmp_inode = inode_get(&sbd, tmp_bh);
		print_inum(tmp_inode);
		brelse(tmp_bh);
	}
	else if (block_is_statfs_file()) {
		tmp_bh = bread(&sbd, block);
		tmp_inode = inode_get(&sbd, tmp_bh);
		print_statfs(tmp_inode);
		brelse(tmp_bh);
	}
	else if (block_is_quota_file()) {
		tmp_bh = bread(&sbd, block);
		tmp_inode = inode_get(&sbd, tmp_bh);
		print_quota(tmp_inode);
		brelse(tmp_bh);
	}
	return 0;
}

