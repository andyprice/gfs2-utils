#include "clusterautoconfig.h"

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
#include "journal.h"

/* ------------------------------------------------------------------------ */
/* find_journal_block - figure out where a journal starts, given the name   */
/* Returns: journal block number, changes j_size to the journal size        */
/* ------------------------------------------------------------------------ */
uint64_t find_journal_block(const char *journal, uint64_t *j_size)
{
	int journal_num;
	uint64_t jindex_block, jblock = 0;
	int amtread;
	struct gfs2_buffer_head *jindex_bh, *j_bh;
	char jbuf[sbd.bsize];

	journal_num = atoi(journal + 7);
	/* Figure out the block of the jindex file */
	if (sbd.gfs1)
		jindex_block = sbd1->sb_jindex_di.no_addr;
	else
		jindex_block = masterblock("jindex");
	/* read in the block */
	jindex_bh = bread(&sbd, jindex_block);
	/* get the dinode data from it. */
	gfs2_dinode_in(&di, jindex_bh); /* parse disk inode to struct*/

	if (!sbd.gfs1)
		do_dinode_extended(&di, jindex_bh); /* parse dir. */

	if (sbd.gfs1) {
		struct gfs2_inode *jiinode;
		struct gfs_jindex ji;

		jiinode = lgfs2_inode_get(&sbd, jindex_bh);
		if (jiinode == NULL)
			return 0;
		amtread = gfs2_readi(jiinode, (void *)&jbuf,
				   journal_num * sizeof(struct gfs_jindex),
				   sizeof(struct gfs_jindex));
		if (amtread) {
			gfs_jindex_in(&ji, jbuf);
			jblock = ji.ji_addr;
			*j_size = ji.ji_nsegment * 0x10;
		}
		inode_put(&jiinode);
	} else {
		struct gfs2_dinode jdi;

		jblock = indirect->ii[0].dirent[journal_num + 2].block;
		j_bh = bread(&sbd, jblock);
		gfs2_dinode_in(&jdi, j_bh);/* parse dinode to struct */
		*j_size = jdi.di_size;
		brelse(j_bh);
	}
	brelse(jindex_bh);
	return jblock;
}

static void check_journal_wrap(uint64_t seq, uint64_t *highest_seq)
{
	if (seq < *highest_seq) {
		print_gfs2("------------------------------------------------"
			   "------------------------------------------------");
		eol(0);
		print_gfs2("Journal wrapped here.");
		eol(0);
		print_gfs2("------------------------------------------------"
			   "------------------------------------------------");
		eol(0);
	}
	*highest_seq = seq;
}

static int is_meta(struct gfs2_buffer_head *lbh)
{
	uint32_t check_magic = ((struct gfs2_meta_header *)(lbh->b_data))->mh_magic;

	check_magic = be32_to_cpu(check_magic);
	if (check_magic == GFS2_MAGIC)
		return 1;
	return 0;
}

/* ------------------------------------------------------------------------ */
/* fsck_readi - same as libgfs2's gfs2_readi, but sets absolute block #     */
/*              of the first bit of data read.                              */
/* ------------------------------------------------------------------------ */
static int fsck_readi(struct gfs2_inode *ip, void *rbuf, uint64_t roffset,
	       unsigned int size, uint64_t *abs_block)
{
	struct gfs2_sbd *sdp = ip->i_sbd;
	struct gfs2_buffer_head *lbh;
	uint64_t lblock, dblock;
	unsigned int o;
	uint32_t extlen = 0;
	unsigned int amount;
	int not_new = 0;
	int isdir = !!(S_ISDIR(ip->i_di.di_mode));
	int copied = 0;

	*abs_block = 0;
	if (roffset >= ip->i_di.di_size)
		return 0;
	if ((roffset + size) > ip->i_di.di_size)
		size = ip->i_di.di_size - roffset;
	if (!size)
		return 0;
	if (isdir) {
		o = roffset % sdp->sd_jbsize;
		lblock = roffset / sdp->sd_jbsize;
	} else {
		lblock = roffset >> sdp->sd_sb.sb_bsize_shift;
		o = roffset & (sdp->bsize - 1);
	}

	if (!ip->i_di.di_height) /* inode_is_stuffed */
		o += sizeof(struct gfs2_dinode);
	else if (isdir)
		o += sizeof(struct gfs2_meta_header);

	while (copied < size) {
		amount = size - copied;
		if (amount > sdp->bsize - o)
			amount = sdp->bsize - o;
		if (!extlen)
			block_map(ip, lblock, &not_new, &dblock, &extlen,
				  FALSE);
		if (dblock) {
			lbh = bread(sdp, dblock);
			if (*abs_block == 0)
				*abs_block = lbh->b_blocknr;
			dblock++;
			extlen--;
		} else
			lbh = NULL;
		if (lbh) {
			memcpy(rbuf, lbh->b_data + o, amount);
			brelse(lbh);
		} else {
			memset(rbuf, 0, amount);
		}
		copied += amount;
		lblock++;
		o = (isdir) ? sizeof(struct gfs2_meta_header) : 0;
	}
	return copied;
}

/* ------------------------------------------------------------------------ */
/* print_ld_blocks - print all blocks given in a log descriptor             */
/* returns: the number of block numbers it printed                          */
/* ------------------------------------------------------------------------ */
static int print_ld_blocks(const uint64_t *b, const char *end, int start_line)
{
	int bcount = 0, i = 0;
	static char str[256];

	while (*b && (char *)b < end) {
		if (!termlines ||
		    (print_entry_ndx >= start_row[dmode] &&
		     ((print_entry_ndx - start_row[dmode])+1) *
		     lines_per_row[dmode] <= termlines - start_line - 2)) {
			if (i && i % 4 == 0) {
				eol(0);
				print_gfs2("                    ");
			}
			i++;
			sprintf(str, "0x%llx",
				(unsigned long long)be64_to_cpu(*b));
			print_gfs2("%-18.18s ", str);
			bcount++;
		}
		b++;
		if (sbd.gfs1)
			b++;
	}
	eol(0);
	return bcount;
}

/* ------------------------------------------------------------------------ */
/* dump_journal - dump a journal file's contents.                           */
/* ------------------------------------------------------------------------ */
void dump_journal(const char *journal)
{
	struct gfs2_buffer_head *j_bh = NULL, dummy_bh;
	uint64_t jblock, j_size, jb, abs_block, saveblk;
	int error, start_line, journal_num;
	struct gfs2_inode *j_inode = NULL;
	int ld_blocks = 0;
	uint64_t highest_seq = 0;
	char *jbuf = NULL;

	start_line = line;
	lines_per_row[dmode] = 1;
	error = 0;
	journal_num = atoi(journal + 7);
	print_gfs2("Dumping journal #%d.", journal_num);
	eol(0);
	jblock = find_journal_block(journal, &j_size);
	if (!jblock)
		return;
	if (!sbd.gfs1) {
		j_bh = bread(&sbd, jblock);
		j_inode = lgfs2_inode_get(&sbd, j_bh);
		if (j_inode == NULL) {
			fprintf(stderr, "Out of memory\n");
			exit(-1);
		}
		jbuf = malloc(sbd.bsize);
		if (jbuf == NULL) {
			fprintf(stderr, "Out of memory\n");
			exit(-1);
		}
	}

	for (jb = 0; jb < j_size; jb += (sbd.gfs1 ? 1:sbd.bsize)) {
		if (sbd.gfs1) {
			if (j_bh)
				brelse(j_bh);
			j_bh = bread(&sbd, jblock + jb);
			abs_block = jblock + jb;
			dummy_bh.b_data = j_bh->b_data;
		} else {
			error = fsck_readi(j_inode, (void *)jbuf, jb,
					   sbd.bsize, &abs_block);
			if (!error) /* end of file */
				break;
			dummy_bh.b_data = jbuf;
		}
		if (get_block_type(&dummy_bh) == GFS2_METATYPE_LD) {
			uint64_t *b;
			struct gfs2_log_descriptor ld;
			int ltndx;
			uint32_t logtypes[2][6] = {
				{GFS2_LOG_DESC_METADATA,
				 GFS2_LOG_DESC_REVOKE,
				 GFS2_LOG_DESC_JDATA,
				 0, 0, 0},
				{GFS_LOG_DESC_METADATA,
				 GFS_LOG_DESC_IUL,
				 GFS_LOG_DESC_IDA,
				 GFS_LOG_DESC_Q,
				 GFS_LOG_DESC_LAST,
				 0}};
			const char *logtypestr[2][6] = {
				{"Metadata", "Revoke", "Jdata",
				 "Unknown", "Unknown", "Unknown"},
				{"Metadata", "Unlinked inode", "Dealloc inode",
				 "Quota", "Final Entry", "Unknown"}};

			print_gfs2("0x%llx (j+%4llx): Log descriptor, ",
				   abs_block, jb / (sbd.gfs1 ? 1 : sbd.bsize));
			gfs2_log_descriptor_in(&ld, &dummy_bh);
			print_gfs2("type %d ", ld.ld_type);

			for (ltndx = 0;; ltndx++) {
				if (ld.ld_type == logtypes[sbd.gfs1][ltndx] ||
				    logtypes[sbd.gfs1][ltndx] == 0)
					break;
			}
			print_gfs2("(%s) ", logtypestr[sbd.gfs1][ltndx]);
			print_gfs2("len:%u, data1: %u",
				   ld.ld_length, ld.ld_data1);
			eol(0);
			print_gfs2("                    ");
			if (sbd.gfs1)
				b = (uint64_t *)(dummy_bh.b_data +
					sizeof(struct gfs_log_descriptor));
			else
				b = (uint64_t *)(dummy_bh.b_data +
					sizeof(struct gfs2_log_descriptor));
			ld_blocks = ld.ld_data1;
			ld_blocks -= print_ld_blocks(b, (dummy_bh.b_data +
							 sbd.bsize),
						     start_line);
		} else if (get_block_type(&dummy_bh) == GFS2_METATYPE_LH) {
			struct gfs2_log_header lh;
			struct gfs_log_header lh1;

			if (sbd.gfs1) {
				gfs_log_header_in(&lh1, &dummy_bh);
				check_journal_wrap(lh1.lh_sequence,
						   &highest_seq);
				print_gfs2("0x%llx (j+%4llx): Log header: "
					   "Flags:%x, Seq: 0x%x, "
					   "1st: 0x%x, tail: 0x%x, "
					   "last: 0x%x", abs_block,
					   jb, lh1.lh_flags, lh1.lh_sequence,
					   lh1.lh_first, lh1.lh_tail,
					   lh1.lh_last_dump);
			} else {
				gfs2_log_header_in(&lh, &dummy_bh);
				check_journal_wrap(lh.lh_sequence,
						   &highest_seq);
				print_gfs2("0x%llx (j+%4llx): Log header: Seq"
					   ": 0x%x, tail: 0x%x, blk: 0x%x",
					   abs_block,
					   jb / sbd.bsize, lh.lh_sequence,
					   lh.lh_tail, lh.lh_blkno);
			}
			eol(0);
		} else if (sbd.gfs1 && ld_blocks > 0) {
			print_gfs2("0x%llx (j+%4llx): GFS log descriptor"
				   " continuation block", abs_block, jb);
			eol(0);
			print_gfs2("                    ");
			ld_blocks -= print_ld_blocks((uint64_t *)dummy_bh.b_data,
						     (dummy_bh.b_data +
						      sbd.bsize), start_line);
		} else if (details && is_meta(&dummy_bh)) {
			saveblk = block;
			block = abs_block;
			display(0);
			block = saveblk;
		}
	}
	inode_put(&j_inode);
	brelse(j_bh);
	blockhist = -1; /* So we don't print anything else */
	free(jbuf);
}
