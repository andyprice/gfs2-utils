#include "clusterautoconfig.h"

#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <string.h>
#include <inttypes.h>
#include <sys/types.h>
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

#include "copyright.cf"

#include "hexedit.h"
#include "libgfs2.h"
#include "gfs2hex.h"
#include "extended.h"
#include "journal.h"
#include "struct_print.h"

const char *mtypes[] = {"none", "sb", "rg", "rb", "di", "in", "lf", "jd",
			"lh", "ld", "ea", "ed", "lb", "13", "qc"};
const char *allocdesc[2][5] = {
	{"Free ", "Data ", "Unlnk", "Meta ", "Resrv"},
	{"Free ", "Data ", "FreeM", "Meta ", "Resrv"},};

static struct gfs2_buffer_head *bh;
static int pgnum;
static long int gziplevel = 9;
static int termcols;
static struct lgfs2_inum gfs1_quota_di;
static struct lgfs2_inum gfs1_license_di;

int details = 0;
char *device = NULL;

/* ------------------------------------------------------------------------- */
/* erase - clear the screen */
/* ------------------------------------------------------------------------- */
static void Erase(void)
{
	bkgd(A_NORMAL|COLOR_PAIR(COLOR_NORMAL));
	/* clear();*/ /* doesn't set background correctly */
	erase();
	/*bkgd(bg);*/
}

/* ------------------------------------------------------------------------- */
/* display_title_lines */
/* ------------------------------------------------------------------------- */
static void display_title_lines(void)
{
	Erase();
	COLORS_TITLE;
	move(0, 0);
	printw("%-80s",TITLE1);
	move(termlines, 0);
	printw("%-79s",TITLE2);
	COLORS_NORMAL;
}

/* ------------------------------------------------------------------------- */
/* bobgets - get a string                                                    */
/* returns: 1 if user exited by hitting enter                                */
/*          0 if user exited by hitting escape                               */
/* ------------------------------------------------------------------------- */
static int bobgets(char string[],int x,int y,int sz,int *ch)
{
	int done,runningy,rc;

	move(x,y);
	done=FALSE;
	COLORS_INVERSE;
	move(x,y);
	addstr(string);
	move(x,y);
	curs_set(2);
	refresh();
	runningy=y;
	rc=0;
	while (!done) {
		*ch = getch();
		
		if(*ch < 0x0100 && isprint(*ch)) {
			char *p=string+strlen(string); // end of the string

			*(p+1)='\0';
			while (insert && p > &string[runningy-y]) {
				*p=*(p-1);
				p--;
			}
			string[runningy-y]=*ch;
			runningy++;
			move(x,y);
			addstr(string);
			if (runningy-y >= sz) {
				rc=1;
				*ch = KEY_RIGHT;
				done = TRUE;
			}
		}
		else {
			// special character, is it one we recognize?
			switch(*ch)
			{
			case(KEY_ENTER):
			case('\n'):
			case('\r'):
				rc=1;
				done=TRUE;
				string[runningy-y] = '\0';
				break;
			case(KEY_CANCEL):
			case(0x01B):
				rc=0;
				done=TRUE;
				break;
			case(KEY_LEFT):
				if (dmode == HEX_MODE) {
					done = TRUE;
					rc = 1;
				}
				else
					runningy--;
				break;
			case(KEY_RIGHT):
				if (dmode == HEX_MODE) {
					done = TRUE;
					rc = 1;
				}
				else
					runningy++;
				break;
			case(KEY_DC):
			case(0x07F):
				if (runningy>=y) {
					char *p;
					p = &string[runningy - y];
					while (*p) {
						*p = *(p + 1);
						p++;
					}
					*p = '\0';
					runningy--;
					// remove the character from the string 
					move(x,y);
					addstr(string);
					COLORS_NORMAL;
					addstr(" ");
					COLORS_INVERSE;
					runningy++;
				}
				break;
			case(KEY_BACKSPACE):
				if (runningy>y) {
					char *p;

					p = &string[runningy - y - 1];
					while (*p) {
						*p = *(p + 1);
						p++;
					}
					*p='\0';
					runningy--;
					// remove the character from the string 
					move(x,y);
					addstr(string);
					COLORS_NORMAL;
					addstr(" ");
					COLORS_INVERSE;
				}
				break;
			case KEY_DOWN:	// Down
				rc=0x5000U;
				done=TRUE;
				break;
			case KEY_UP:	// Up
				rc=0x4800U;
				done=TRUE;
				break;
			case 0x014b:
				insert=!insert;
				move(0,68);
				if (insert)
					printw("insert ");
				else
					printw("replace");
				break;
			default:
				move(0,70);
				printw("%08x",*ch);
				// ignore all other characters
				break;
			} // end switch on non-printable character
		} // end non-printable character
		move(x,runningy);
		refresh();
	} // while !done
	if (sz>0)
		string[sz]='\0';
	COLORS_NORMAL;
	return rc;
}/* bobgets */

/******************************************************************************
** instr - instructions
******************************************************************************/
static void gfs2instr(const char *s1, const char *s2)
{
	COLORS_HIGHLIGHT;
	move(line,0);
	printw("%s", s1);
	COLORS_NORMAL;
	move(line,17);
	printw("%s", s2);
	line++;
}

/******************************************************************************
*******************************************************************************
**
** void print_usage()
**
** Description:
**   This routine prints out the appropriate commands for this application.
**
*******************************************************************************
******************************************************************************/

static void print_usage(void)
{
	line = 2;
	Erase();
	display_title_lines();
	move(line++,0);
	printw("Supported commands: (roughly conforming to the rules of 'less')");
	line++;
	move(line++,0);
	printw("Navigation:");
	gfs2instr("<pg up>/<down>","Move up or down one screen full");
	gfs2instr("<up>/<down>","Move up or down one line");
	gfs2instr("<left>/<right>","Move left or right one byte");
	gfs2instr("<home>","Return to the superblock.");
	gfs2instr("   f","Forward one 4K block");
	gfs2instr("   b","Backward one 4K block");
	gfs2instr("   g","Goto a given block (number, master, root, rindex, jindex, etc)");
	gfs2instr("   j","Jump to the highlighted 64-bit block number.");
	gfs2instr("    ","(You may also arrow up to the block number and hit enter)");
	gfs2instr("<backspace>","Return to a previous block (a block stack is kept)");
	gfs2instr("<space>","Jump forward to block before backspace (opposite of backspace)");
	line++;
	move(line++, 0);
	printw("Other commands:");
	gfs2instr("   h","This Help display");
	gfs2instr("   c","Toggle the color scheme");
	gfs2instr("   m","Switch display mode: hex -> GFS2 structure -> Extended");
	gfs2instr("   q","Quit (same as hitting <escape> key)");
	gfs2instr("<enter>","Edit a value (enter to save, esc to discard)");
	gfs2instr("       ","(Currently only works on the hex display)");
	gfs2instr("<escape>","Quit the program");
	line++;
	move(line++, 0);
	printw("Notes: Areas shown in red are outside the bounds of the struct/file.");
	move(line++, 0);
	printw("       Areas shown in blue are file contents.");
	move(line++, 0);
	printw("       Characters shown in green are selected for edit on <enter>.");
	move(line++, 0);
	move(line++, 0);
	printw("Press any key to return.");
	refresh();
	getch(); // wait for input
	Erase();
}

const struct lgfs2_metadata *get_block_type(char *buf)
{
	uint32_t t = lgfs2_get_block_type(buf);

	if (t != 0)
		return lgfs2_find_mtype(t, sbd.gfs1 ? LGFS2_MD_GFS1 : LGFS2_MD_GFS2);
	return NULL;
}

/**
 * returns: metatype if block is a GFS2 structure block type
 *          0 if block is not a GFS2 structure
 */
int display_block_type(char *buf, uint64_t addr, int from_restore)
{
	const struct lgfs2_metadata *mtype;
	const struct gfs2_meta_header *mh;
	int ret_type = 0; /* return type */

	/* first, print out the kind of GFS2 block this is */
	if (termlines) {
		line = 1;
		move(line, 0);
	}
	print_gfs2("Block #");
	if (termlines) {
		if (edit_row[dmode] == -1)
			COLORS_HIGHLIGHT;
	}
	if (block == RGLIST_DUMMY_BLOCK)
		print_gfs2("RG List       ");
	else if (block == JOURNALS_DUMMY_BLOCK)
		print_gfs2("Journal Status:      ");
	else
		print_gfs2("%"PRIu64"    (0x%"PRIx64")", addr, addr);
	if (termlines) {
		if (edit_row[dmode] == -1)
			COLORS_NORMAL;
	}
	print_gfs2(" ");
	if (!from_restore)
		print_gfs2("of %"PRIu64" (0x%"PRIx64") ", max_block, max_block);
	if (block == RGLIST_DUMMY_BLOCK) {
		ret_type = GFS2_METATYPE_RG;
		struct_len = sbd.gfs1 ? sizeof(struct gfs_rgrp) :
			sizeof(struct gfs2_rgrp);
	} else if (block == JOURNALS_DUMMY_BLOCK) {
		ret_type = GFS2_METATYPE_DI;
		struct_len = 0;
	} else {
		mtype = get_block_type(buf);
		if (mtype != NULL) {
			print_gfs2("(%s)", mtype->display);
			struct_len = mtype->size;
			ret_type = mtype->mh_type;
		} else {
			struct_len = sbd.sd_bsize;
			ret_type = 0;
		}
	}
	mh = (void *)buf;
	eol(0);
	if (from_restore)
		return ret_type;
	if (termlines && dmode == HEX_MODE) {
		int type;
		struct rgrp_tree *rgd;

		rgd = gfs2_blk2rgrpd(&sbd, block);
		if (rgd) {
			gfs2_rgrp_read(&sbd, rgd);
			if ((be32_to_cpu(mh->mh_type) == GFS2_METATYPE_RG) ||
			    (be32_to_cpu(mh->mh_type) == GFS2_METATYPE_RB))
				type = 4;
			else {
				type = lgfs2_get_bitmap(&sbd, block, rgd);
			}
		} else
			type = 4;
		screen_chunk_size = ((termlines - 4) * 16) >> 8 << 8;
		if (!screen_chunk_size)
			screen_chunk_size = 256;
		pgnum = (offset / screen_chunk_size);
		if (type >= 0) {
			print_gfs2("(p.%d of %d--%s)", pgnum + 1,
				   (sbd.sd_bsize % screen_chunk_size) > 0 ?
				   sbd.sd_bsize / screen_chunk_size + 1 : sbd.sd_bsize /
				   screen_chunk_size, allocdesc[sbd.gfs1][type]);
		}
		/*eol(9);*/
		if ((be32_to_cpu(mh->mh_type) == GFS2_METATYPE_RG)) {
			int ptroffset = edit_row[dmode] * 16 + edit_col[dmode];

			if (rgd && (ptroffset >= struct_len || pgnum)) {
				int blknum, b, btype;

				blknum = pgnum * screen_chunk_size;
				blknum += (ptroffset - struct_len);
				blknum *= 4;
				blknum += rgd->rt_data0;

				print_gfs2(" blk ");
				for (b = blknum; b < blknum + 4; b++) {
					btype = lgfs2_get_bitmap(&sbd, b, rgd);
					if (btype >= 0) {
						print_gfs2("0x%x-%s  ", b,
							   allocdesc[sbd.gfs1][btype]);
					}
				}
			}
		} else if ((be32_to_cpu(mh->mh_type) == GFS2_METATYPE_RB)) {
			int ptroffset = edit_row[dmode] * 16 + edit_col[dmode];

			if (rgd && (ptroffset >= struct_len || pgnum)) {
				int blknum, b, btype, rb_number;

				rb_number = block - rgd->rt_addr;
				blknum = 0;
				/* count the number of bytes representing
				   blocks prior to the displayed screen. */
				for (b = 0; b < rb_number; b++) {
					struct_len = (b ?
					      sizeof(struct gfs2_meta_header) :
					      sizeof(struct gfs2_rgrp));
					blknum += (sbd.sd_bsize - struct_len);
				}
				struct_len = sizeof(struct gfs2_meta_header);
				/* add the number of bytes on this screen */
				blknum += (ptroffset - struct_len);
				/* factor in the page number */
				blknum += pgnum * screen_chunk_size;
				/* convert bytes to blocks */
				blknum *= GFS2_NBBY;
				/* add the starting offset for this rgrp */
				blknum += rgd->rt_data0;
				print_gfs2(" blk ");
				for (b = blknum; b < blknum + 4; b++) {
					btype = lgfs2_get_bitmap(&sbd, b, rgd);
					if (btype >= 0) {
						print_gfs2("0x%x-%s  ", b,
							   allocdesc[sbd.gfs1][btype]);
					}
				}
			}
		}
		if (rgd)
			gfs2_rgrp_relse(&sbd, rgd);
 	}
	if (block == sbd.sd_root_dir.in_addr)
		print_gfs2("--------------- Root directory ------------------");
	else if (!sbd.gfs1 && block == sbd.sd_meta_dir.in_addr)
		print_gfs2("-------------- Master directory -----------------");
	else if (!sbd.gfs1 && block == RGLIST_DUMMY_BLOCK)
		print_gfs2("------------------ RG List ----------------------");
	else if (!sbd.gfs1 && block == JOURNALS_DUMMY_BLOCK)
		print_gfs2("-------------------- Journal List --------------------");
	else {
		if (sbd.gfs1) {
			if (block == sbd.sd_rindex_di.in_addr)
				print_gfs2("---------------- rindex file -------------------");
			else if (block == gfs1_quota_di.in_addr)
				print_gfs2("---------------- Quota file --------------------");
			else if (block == sbd.sd_jindex_di.in_addr)
				print_gfs2("--------------- Journal Index ------------------");
			else if (block == gfs1_license_di.in_addr)
				print_gfs2("--------------- License file -------------------");
		}
		else {
			int d;

			for (d = 2; d < 8; d++) {
				if (block == masterdir.dirent[d].inum.in_addr) {
					if (!strncmp(masterdir.dirent[d].filename, "jindex", 6))
						print_gfs2("--------------- Journal Index ------------------");
					else if (!strncmp(masterdir.dirent[d].filename, "per_node", 8))
						print_gfs2("--------------- Per-node Dir -------------------");
					else if (!strncmp(masterdir.dirent[d].filename, "inum", 4))
						print_gfs2("---------------- Inum file ---------------------");
					else if (!strncmp(masterdir.dirent[d].filename, "statfs", 6))
						print_gfs2("---------------- statfs file -------------------");
					else if (!strncmp(masterdir.dirent[d].filename, "rindex", 6))
						print_gfs2("---------------- rindex file -------------------");
					else if (!strncmp(masterdir.dirent[d].filename, "quota", 5))
						print_gfs2("---------------- Quota file --------------------");
				}
			}
		}
	}
	eol(0);
	return ret_type;
}

static int get_pnum(int ptroffset)
{
	int pnum;

	pnum = pgnum * screen_chunk_size;
	pnum += (ptroffset - struct_len);
	pnum /= sizeof(uint64_t);

	return pnum;
}

/* ------------------------------------------------------------------------ */
/* hexdump - hex dump the filesystem block to the screen                    */
/* ------------------------------------------------------------------------ */
static int hexdump(uint64_t startaddr, uint64_t len, int trunc_zeros,
		   uint64_t flagref, uint64_t ref_blk)
{
	const unsigned char *pointer, *ptr2;
	int i;
	uint64_t l;
	const char *lpBuffer = bh->b_data;
	const char *zeros_strt = lpBuffer + sbd.sd_bsize;
	int print_field, cursor_line;
	const struct lgfs2_metadata *m = get_block_type(bh->b_data);
	__be64 *ref;
	int ptroffset = 0;

	strcpy(edit_fmt,"%02x");
	pointer = (unsigned char *)lpBuffer + offset;
	ptr2 = (unsigned char *)lpBuffer + offset;
	ref = (__be64 *)lpBuffer + offset;
	if (trunc_zeros) {
		while (zeros_strt > lpBuffer && (*(zeros_strt - 1) == 0))
			zeros_strt--;
	}
	l = offset;
	print_entry_ndx = 0;
	while (((termlines && line < termlines &&
		 line <= ((screen_chunk_size / 16) + 2)) ||
		(!termlines && l < len)) && l < sbd.sd_bsize) {
		int ptr_not_null = 0;

		if (termlines) {
			move(line, 0);
			COLORS_OFFSETS; /* cyan for offsets */
		}
		if (startaddr < 0xffffffff)
			print_gfs2("%.8"PRIx64, startaddr + l);
		else
			print_gfs2("%.16"PRIx64, startaddr + l);
		if (termlines) {
			if (l < struct_len)
				COLORS_NORMAL; /* normal part of structure */
			else if (gfs2_struct_type == GFS2_METATYPE_DI &&
					 l < struct_len + be64_to_cpu(di->di_size))
				COLORS_CONTENTS; /* after struct but not eof */
			else
				COLORS_SPECIAL; /* beyond end of the struct */
		}
		print_field = -1;
		cursor_line = 0;
		for (i = 0; i < 16; i++) { /* first print it in hex */
			/* Figure out if we have a null pointer--for colors */
			if (((gfs2_struct_type == GFS2_METATYPE_IN) ||
			     (gfs2_struct_type == GFS2_METATYPE_DI &&
			      l < struct_len + be64_to_cpu(di->di_size) &&
			      (be16_to_cpu(di->di_height) > 0 ||
			      !S_ISREG(be32_to_cpu(di->di_mode))))) &&
			    (i==0 || i==8)) {
				int j;

				ptr_not_null = 0;
				for (j = 0; j < 8; j++) {
					if (*(pointer + j)) {
						ptr_not_null = 1;
						break;
					}
				}
			}
			if (termlines) {
				if (l + i < struct_len)
					COLORS_NORMAL; /* in the structure */
				else if (gfs2_struct_type == GFS2_METATYPE_DI
					 && l + i < struct_len + be64_to_cpu(di->di_size)) {
					if ((!di->di_height &&
					     S_ISREG(be32_to_cpu(di->di_mode))) ||
					    !ptr_not_null)
						COLORS_CONTENTS;/*stuff data */
					else
						COLORS_SPECIAL;/* non-null */
				}
				else if (gfs2_struct_type == GFS2_METATYPE_IN){
					if (ptr_not_null)
						COLORS_SPECIAL;/* non-null */
					else
						COLORS_CONTENTS;/* null */
				} else
					COLORS_SPECIAL; /* past the struct */
			}
			if (i%4 == 0)
				print_gfs2(" ");
			if (termlines && line == edit_row[dmode] + 3 &&
				i == edit_col[dmode]) {
				COLORS_HIGHLIGHT; /* in the structure */
				memset(estring,0,3);
				sprintf(estring,"%02x",*pointer);
				cursor_line = 1;
				print_field = (char *)pointer - bh->b_data;
			}
			print_gfs2("%02x",*pointer);
			if (termlines && line == edit_row[dmode] + 3 &&
				i == edit_col[dmode]) {
				if (l < struct_len + offset)
					COLORS_NORMAL; /* in the structure */
				else
					COLORS_SPECIAL; /* beyond structure */
			}
			pointer++;
		}
		print_gfs2(" [");
		for (i=0; i<16; i++) { /* now print it in character format */
			if ((*ptr2 >=' ') && (*ptr2 <= '~'))
				print_gfs2("%c",*ptr2);
			else
				print_gfs2(".");
			ptr2++;
		}
		print_gfs2("] ");
		if (print_field >= 0) {
			if (m) {
				const struct lgfs2_metafield *f;
				unsigned n;
				for (n = 0; n < m->nfields; n++) {
					f = &m->fields[n];
					if (print_field >= f->offset &&
					    print_field < (f->offset + f->length)) {
						print_gfs2("%s", m->fields[n].name);
						break;
					}
				}
			}

		}
		if (m && cursor_line) {
			const uint32_t block_type = m->mh_type;
			int isdir;

			isdir = ((block_type == GFS2_METATYPE_DI) &&
			         (((struct gfs2_dinode*)bh->b_data)->di_height ||
				  S_ISDIR(be32_to_cpu(di->di_mode))));

			if (block_type == GFS2_METATYPE_IN ||
			    block_type == GFS2_METATYPE_LD || isdir) {

				ptroffset = edit_row[dmode] * 16 +
					edit_col[dmode];

				if (ptroffset >= struct_len || pgnum) {
					int pnum = get_pnum(ptroffset);
					if (block_type == GFS2_METATYPE_LD)
						print_gfs2("*");
					print_gfs2("pointer 0x%x", pnum);
				}
			}
		}
		if (line - 3 > last_entry_onscreen[dmode])
			last_entry_onscreen[dmode] = line - 3;
		if (flagref && be64_to_cpu(*ref) == flagref)
			print_gfs2("<------------------------- ref in 0x%"PRIx64" "
				   "to 0x%"PRIx64, ref_blk, flagref);
		ref++;
		if (flagref && be64_to_cpu(*ref) == flagref)
			print_gfs2("<------------------------- ref in 0x%"PRIx64" "
				   "to 0x%"PRIx64, ref_blk, flagref);
		ref++;
		eol(0);
		l += 16;
		print_entry_ndx++;
		/* This should only happen if trunc_zeros is specified: */
		if ((const char *)pointer >= zeros_strt)
			break;
	} /* while */
	if (m && m->mh_type == GFS2_METATYPE_LD && ptroffset >= struct_len) {
		COLORS_NORMAL;
		eol(0);
		print_gfs2("         * 'j' will jump to the journaled block, "
			   "not the absolute block.");
		eol(0);
	}
	if (sbd.gfs1) {
		COLORS_NORMAL;
		print_gfs2("         *** This seems to be a GFS-1 file system ***");
		eol(0);
	}
	return (offset+len);
}/* hexdump */

/* ------------------------------------------------------------------------ */
/* masterblock - find a file (by name) in the master directory and return   */
/*               its block number.                                          */
/* ------------------------------------------------------------------------ */
uint64_t masterblock(const char *fn)
{
	int d;
	
	for (d = 2; d < 8; d++)
		if (!strncmp(masterdir.dirent[d].filename, fn, strlen(fn)))
			return (masterdir.dirent[d].inum.in_addr);
	return 0;
}

/* ------------------------------------------------------------------------ */
/* rgcount - return how many rgrps there are.                               */
/* ------------------------------------------------------------------------ */
static void rgcount(void)
{
	printf("%"PRId64" RGs in this file system.\n",
	       sbd.md.riinode->i_size / sizeof(struct gfs2_rindex));
	inode_put(&sbd.md.riinode);
	gfs2_rgrp_free(&sbd, &sbd.rgtree);
	exit(EXIT_SUCCESS);
}

/* ------------------------------------------------------------------------ */
/* find_rgrp_block - locate the block for a given rgrp number               */
/* ------------------------------------------------------------------------ */
static uint64_t find_rgrp_block(struct gfs2_inode *dif, int rg)
{
	int amt;
	struct gfs2_rindex ri;
	uint64_t foffset, gfs1_adj = 0;

	foffset = rg * sizeof(struct gfs2_rindex);
	if (sbd.gfs1) {
		uint64_t sd_jbsize =
			(sbd.sd_bsize - sizeof(struct gfs2_meta_header));

		gfs1_adj = (foffset / sd_jbsize) *
			sizeof(struct gfs2_meta_header);
		gfs1_adj += sizeof(struct gfs2_meta_header);
	}
	amt = gfs2_readi(dif, &ri, foffset + gfs1_adj, sizeof(ri));
	if (!amt) /* end of file */
		return 0;
	return be64_to_cpu(ri.ri_addr);
}

/* ------------------------------------------------------------------------ */
/* get_rg_addr                                                              */
/* ------------------------------------------------------------------------ */
static uint64_t get_rg_addr(int rgnum)
{
	uint64_t rgblk = 0, gblock;
	struct gfs2_inode *riinode;

	if (sbd.gfs1)
		gblock = sbd.sd_rindex_di.in_addr;
	else
		gblock = masterblock("rindex");
	riinode = lgfs2_inode_read(&sbd, gblock);
	if (riinode == NULL)
		return 0;
	if (rgnum < riinode->i_size / sizeof(struct gfs2_rindex))
		rgblk = find_rgrp_block(riinode, rgnum);
	else
		fprintf(stderr, "Error: File system only has %"PRId64" RGs.\n",
			riinode->i_size / sizeof(struct gfs2_rindex));
	inode_put(&riinode);
	return rgblk;
}

/* ------------------------------------------------------------------------ */
/* set_rgrp_flags - Set an rgrp's flags to a given value                    */
/* rgnum: which rg to print or modify flags for (0 - X)                     */
/* new_flags: value to set new rg_flags to (if modify == TRUE)              */
/* modify: TRUE if the value is to be modified, FALSE if it's to be printed */
/* full: TRUE if the full RG should be printed.                             */
/* ------------------------------------------------------------------------ */
static void set_rgrp_flags(int rgnum, uint32_t new_flags, int modify, int full)
{
	struct gfs2_buffer_head *rbh;
	struct gfs2_rgrp *rg;
	uint64_t rgblk;

	rgblk = get_rg_addr(rgnum);
	rbh = bread(&sbd, rgblk);
	rg = (void *)rbh->b_data;

	if (modify) {
		uint32_t flags = be32_to_cpu(rg->rg_flags);

		printf("RG #%d (block %"PRIu64" / 0x%"PRIx64") rg_flags changed from 0x%08x to 0x%08x\n",
		       rgnum, rgblk, rgblk, flags, new_flags);
		rg->rg_flags = cpu_to_be32(new_flags);
		bmodified(rbh);
	} else {
		if (full) {
			print_gfs2("RG #%d", rgnum);
			print_gfs2(" located at: %"PRIu64" (0x%"PRIx64")", rgblk, rgblk);
                        eol(0);
			if (sbd.gfs1)
				gfs_rgrp_print(rg);
			else
				rgrp_print(rg);
		}
		else
			printf("RG #%d (block %"PRIu64" / 0x%"PRIx64") rg_flags = 0x%08x\n",
			       rgnum, rgblk, rgblk, be32_to_cpu(rg->rg_flags));
	}
	brelse(rbh);
	if (modify)
		fsync(sbd.device_fd);
}

/* ------------------------------------------------------------------------ */
/* has_indirect_blocks                                                      */
/* ------------------------------------------------------------------------ */
int has_indirect_blocks(void)
{
	struct gfs_dinode *d1 = (struct gfs_dinode *)di;

	if (indirect_blocks || gfs2_struct_type == GFS2_METATYPE_SB ||
	    gfs2_struct_type == GFS2_METATYPE_LF ||
	    (gfs2_struct_type == GFS2_METATYPE_DI &&
	     (S_ISDIR(be32_to_cpu(di->di_mode)) ||
	      (sbd.gfs1 && be16_to_cpu(d1->di_type) == GFS_FILE_DIR))))
		return TRUE;
	return FALSE;
}

int block_is_rindex(uint64_t blk)
{
	if ((sbd.gfs1 && blk == sbd.sd_rindex_di.in_addr) ||
	    (blk == masterblock("rindex")))
		return TRUE;
	return FALSE;
}

int block_is_jindex(uint64_t blk)
{
	if ((sbd.gfs1 && blk == sbd.sd_jindex_di.in_addr))
		return TRUE;
	return FALSE;
}

int block_is_inum_file(uint64_t blk)
{
	if (!sbd.gfs1 && blk == masterblock("inum"))
		return TRUE;
	return FALSE;
}

int block_is_statfs_file(uint64_t blk)
{
	if (sbd.gfs1 && blk == gfs1_license_di.in_addr)
		return TRUE;
	if (!sbd.gfs1 && blk == masterblock("statfs"))
		return TRUE;
	return FALSE;
}

int block_is_quota_file(uint64_t blk)
{
	if (sbd.gfs1 && blk == gfs1_quota_di.in_addr)
		return TRUE;
	if (!sbd.gfs1 && blk == masterblock("quota"))
		return TRUE;
	return FALSE;
}

int block_is_per_node(uint64_t blk)
{
	if (!sbd.gfs1 && blk == masterblock("per_node"))
		return TRUE;
	return FALSE;
}

/* ------------------------------------------------------------------------ */
/* block_has_extended_info                                                  */
/* ------------------------------------------------------------------------ */
static int block_has_extended_info(void)
{
	if (has_indirect_blocks() ||
	    block_is_rindex(block) ||
	    block_is_rgtree(block) ||
	    block_is_journals(block) ||
	    block_is_jindex(block) ||
	    block_is_inum_file(block) ||
	    block_is_statfs_file(block) ||
	    block_is_quota_file(block))
		return TRUE;
	return FALSE;
}

static void read_superblock(int fd)
{
	struct gfs2_meta_header *mh;

	ioctl(fd, BLKFLSBUF, 0);
	memset(&sbd, 0, sizeof(struct gfs2_sbd));
	sbd.sd_bsize = GFS2_DEFAULT_BSIZE;
	sbd.device_fd = fd;
	bh = bread(&sbd, 0x10);
	sbd.jsize = GFS2_DEFAULT_JSIZE;
	sbd.rgsize = GFS2_DEFAULT_RGSIZE;
	sbd.qcsize = GFS2_DEFAULT_QCSIZE;
	sbd.sd_time = time(NULL);
	sbd.rgtree.osi_node = NULL;
	lgfs2_sb_in(&sbd, bh->b_data);
	/* Check to see if this is really gfs1 */
	mh = (struct gfs2_meta_header *)bh->b_data;
	if (sbd.sd_fs_format == GFS_FORMAT_FS &&
	    be32_to_cpu(mh->mh_type) == GFS_METATYPE_SB &&
	    be32_to_cpu(mh->mh_format) == GFS_FORMAT_SB &&
	    sbd.sd_multihost_format == GFS_FORMAT_MULTI) {
		sbd.gfs1 = TRUE;
	}
	else
		sbd.gfs1 = FALSE;
	if (!sbd.sd_bsize)
		sbd.sd_bsize = GFS2_DEFAULT_BSIZE;
	if (lgfs2_get_dev_info(fd, &sbd.dinfo)) {
		perror(device);
		exit(-1);
	}
	if(compute_constants(&sbd)) {
		fprintf(stderr, "Failed to compute constants.\n");
		exit(-1);
	}
	if (sbd.gfs1 || (be32_to_cpu(mh->mh_magic) == GFS2_MAGIC &&
	                 be32_to_cpu(mh->mh_type) == GFS2_METATYPE_SB))
		block = 0x10 * (GFS2_DEFAULT_BSIZE / sbd.sd_bsize);
	else {
		block = starting_blk = 0;
	}
	fix_device_geometry(&sbd);
	if(sbd.gfs1) {
		sbd.sd_inptrs = (sbd.sd_bsize - sizeof(struct gfs_indirect)) /
			sizeof(uint64_t);
		sbd.sd_diptrs = (sbd.sd_bsize - sizeof(struct gfs_dinode)) /
			sizeof(uint64_t);
		sbd.md.riinode = lgfs2_inode_read(&sbd, sbd.sd_rindex_di.in_addr);
	} else {
		sbd.sd_inptrs = (sbd.sd_bsize - sizeof(struct gfs2_meta_header)) /
			sizeof(uint64_t);
		sbd.sd_diptrs = (sbd.sd_bsize - sizeof(struct gfs2_dinode)) /
			sizeof(uint64_t);
		sbd.master_dir = lgfs2_inode_read(&sbd, sbd.sd_meta_dir.in_addr);
		if (sbd.master_dir == NULL) {
			sbd.md.riinode = NULL;
		} else {
			gfs2_lookupi(sbd.master_dir, "rindex", 6, &sbd.md.riinode);
		}
	}
	brelse(bh);
	bh = NULL;
}

static int read_rindex(void)
{
	uint64_t count;
	int ok;

	sbd.fssize = sbd.device.length;
	if (sbd.md.riinode) /* If we found the rindex */
		rindex_read(&sbd, &count, &ok);

	if (!OSI_EMPTY_ROOT(&sbd.rgtree)) {
		struct rgrp_tree *rg = (struct rgrp_tree *)osi_last(&sbd.rgtree);
		sbd.fssize = rg->rt_data0 + rg->rt_data;
	}
	return 0;
}

static int read_master_dir(void)
{
	ioctl(sbd.device_fd, BLKFLSBUF, 0);

	bh = bread(&sbd, sbd.sd_meta_dir.in_addr);
	if (bh == NULL)
		return 1;
	di = (struct gfs2_dinode *)bh->b_data;
	do_dinode_extended(bh->b_data); /* get extended data, if any */
	memcpy(&masterdir, &indirect[0], sizeof(struct indirect_info));
	return 0;
}

int display(int identify_only, int trunc_zeros, uint64_t flagref,
	    uint64_t ref_blk)
{
	uint64_t blk;

	if (block == RGLIST_DUMMY_BLOCK) {
		if (sbd.gfs1)
			blk = sbd.sd_rindex_di.in_addr;
		else
			blk = masterblock("rindex");
	} else if (block == JOURNALS_DUMMY_BLOCK) {
		if (sbd.gfs1)
			blk = sbd.sd_jindex_di.in_addr;
		else
			blk = masterblock("jindex");
	} else
		blk = block;
	if (termlines) {
		display_title_lines();
		move(2,0);
	}
	if (bh == NULL || bh->b_blocknr != blk) { /* If we changed blocks from the last read */
		if (bh != NULL)
			brelse(bh);
		dev_offset = blk * sbd.sd_bsize;
		ioctl(sbd.device_fd, BLKFLSBUF, 0);
		if (!(bh = bread(&sbd, blk))) {
			fprintf(stderr, "read error: %s from %s:%d: "
				"offset %"PRIu64" (0x%"PRIx64")\n",
				strerror(errno), __FUNCTION__, __LINE__,
				dev_offset, dev_offset);
			exit(-1);
		}
	}
	line = 1;
	gfs2_struct_type = display_block_type(bh->b_data, bh->b_blocknr, FALSE);
	if (identify_only)
		return 0;
	indirect_blocks = 0;
	lines_per_row[dmode] = 1;
	if (gfs2_struct_type == GFS2_METATYPE_SB || blk == 0x10 * (4096 / sbd.sd_bsize)) {
		struct indirect_info *ii = &indirect->ii[0];
		struct idirent *id;

		lgfs2_sb_in(&sbd, bh->b_data);
		memset(indirect, 0, sizeof(struct iinfo));
		ii->block = sbd.sd_meta_dir.in_addr;
		ii->is_dir = TRUE;
		ii->dirents = 2;

		id = &ii->dirent[0];
		memcpy(id->filename, "root", 4);
		id->inum = sbd.sd_root_dir;
		id->type = DT_DIR;

		id = &ii->dirent[1];
		memcpy(id->filename, "master", 7);
		id->inum = sbd.sd_meta_dir;
		id->type = DT_DIR;
	}
	else if (gfs2_struct_type == GFS2_METATYPE_DI) {
		di = (struct gfs2_dinode *)bh->b_data;
		do_dinode_extended(bh->b_data); /* get extended data, if any */
	}
	else if (gfs2_struct_type == GFS2_METATYPE_IN) { /* indirect block list */
		if (blockhist) {
			int i;

			for (i = 0; i < 512; i++)
				memcpy(&indirect->ii[i].mp,
				       &blockstack[blockhist - 1].mp,
				       sizeof(struct metapath));
		}
		indirect_blocks = do_indirect_extended(bh->b_data, indirect);
	}
	else if (gfs2_struct_type == GFS2_METATYPE_LF) { /* directory leaf */
		do_leaf_extended(bh->b_data, indirect);
	}

	last_entry_onscreen[dmode] = 0;
	if (dmode == EXTENDED_MODE && !block_has_extended_info())
		dmode = HEX_MODE;
	if (termlines) {
		move(termlines, 63);
		if (dmode==HEX_MODE)
			printw("Mode: Hex %s", (editing?"edit ":"view "));
		else
			printw("Mode: %s", (dmode==GFS2_MODE?"Structure":
					    "Pointers "));
		move(line, 0);
	}
	if (dmode == HEX_MODE) {         /* if hex display mode           */
		uint64_t len = sbd.sd_bsize;

		if (gfs2_struct_type == GFS2_METATYPE_DI)
			len = struct_len + be64_to_cpu(di->di_size);

		hexdump(dev_offset, len, trunc_zeros, flagref, ref_blk);
	} else if (dmode == GFS2_MODE) { /* if structure display */
		if (block != JOURNALS_DUMMY_BLOCK)
			display_gfs2(bh->b_data);
	} else
		display_extended();        /* display extended blocks       */
	/* No else here because display_extended can switch back to hex mode */
	if (termlines)
		refresh();
	return(0);
}

/* ------------------------------------------------------------------------ */
/* push_block - push a block onto the block stack                           */
/* ------------------------------------------------------------------------ */
static void push_block(uint64_t blk)
{
	int i, bhst;

	bhst = blockhist % BLOCK_STACK_SIZE;
	if (blk) {
		blockstack[bhst].dmode = dmode;
		for (i = 0; i < DMODES; i++) {
			blockstack[bhst].start_row[i] = start_row[i];
			blockstack[bhst].end_row[i] = end_row[i];
			blockstack[bhst].edit_row[i] = edit_row[i];
			blockstack[bhst].edit_col[i] = edit_col[i];
			blockstack[bhst].lines_per_row[i] = lines_per_row[i];
		}
		blockstack[bhst].gfs2_struct_type = gfs2_struct_type;
		if (edit_row[dmode] >= 0 && !block_is_rindex(block))
			memcpy(&blockstack[bhst].mp,
			       &indirect->ii[edit_row[dmode]].mp,
			       sizeof(struct metapath));
		blockhist++;
		blockstack[blockhist % BLOCK_STACK_SIZE].block = blk;
	}
}

/* ------------------------------------------------------------------------ */
/* pop_block - pop a block off the block stack                              */
/* ------------------------------------------------------------------------ */
static uint64_t pop_block(void)
{
	int i, bhst;

	if (!blockhist)
		return block;
	blockhist--;
	bhst = blockhist % BLOCK_STACK_SIZE;
	dmode = blockstack[bhst].dmode;
	for (i = 0; i < DMODES; i++) {
		start_row[i] = blockstack[bhst].start_row[i];
		end_row[i] = blockstack[bhst].end_row[i];
		edit_row[i] = blockstack[bhst].edit_row[i];
		edit_col[i] = blockstack[bhst].edit_col[i];
		lines_per_row[i] = blockstack[bhst].lines_per_row[i];
	}
	gfs2_struct_type = blockstack[bhst].gfs2_struct_type;
	return blockstack[bhst].block;
}

/* ------------------------------------------------------------------------ */
/* Find next metadata block of a given type AFTER a given point in the fs   */
/*                                                                          */
/* This is used to find blocks that aren't represented in the bitmaps, such */
/* as the RGs and bitmaps or the superblock.                                */
/* ------------------------------------------------------------------------ */
static uint64_t find_metablockoftype_slow(uint64_t startblk, int metatype, int print)
{
	uint64_t blk, last_fs_block;
	int found = 0;
	struct gfs2_buffer_head *lbh;

	last_fs_block = lseek(sbd.device_fd, 0, SEEK_END) / sbd.sd_bsize;
	for (blk = startblk + 1; blk < last_fs_block; blk++) {
		lbh = bread(&sbd, blk);
		/* Can't use get_block_type here (returns false "none") */
		if (lbh->b_data[0] == 0x01 && lbh->b_data[1] == 0x16 &&
		    lbh->b_data[2] == 0x19 && lbh->b_data[3] == 0x70 &&
		    lbh->b_data[4] == 0x00 && lbh->b_data[5] == 0x00 &&
		    lbh->b_data[6] == 0x00 && lbh->b_data[7] == metatype) {
			found = 1;
			brelse(lbh);
			break;
		}
		brelse(lbh);
	}
	if (!found)
		blk = 0;
	if (print) {
		if (dmode == HEX_MODE)
			printf("0x%"PRIx64"\n", blk);
		else
			printf("%"PRIu64"\n", blk);
	}
	gfs2_rgrp_free(&sbd, &sbd.rgtree);
	if (print)
		exit(0);
	return blk;
}

static int find_rg_metatype(struct rgrp_tree *rgd, uint64_t *blk, uint64_t startblk, int mtype)
{
	int found;
	unsigned i, j, m;
	struct gfs2_buffer_head *bhp = NULL;
	uint64_t *ibuf = malloc(sbd.sd_bsize * GFS2_NBBY * sizeof(uint64_t));

	for (i = 0; i < rgd->rt_length; i++) {
		m = lgfs2_bm_scan(rgd, i, ibuf, GFS2_BLKST_DINODE);

		for (j = 0; j < m; j++) {
			*blk = ibuf[j];
			bhp = bread(&sbd, *blk);
			found = (*blk > startblk) && !lgfs2_check_meta(bhp->b_data, mtype);
			brelse(bhp);
			if (found) {
				free(ibuf);
				return 0;
			}
		}
	}
	free(ibuf);
	return -1;
}

/* ------------------------------------------------------------------------ */
/* Find next "metadata in use" block AFTER a given point in the fs          */
/*                                                                          */
/* This version does its magic by searching the bitmaps of the RG.  After   */
/* all, if we're searching for a dinode, we want a real allocated inode,    */
/* not just some block that used to be an inode in a previous incarnation.  */
/* ------------------------------------------------------------------------ */
static uint64_t find_metablockoftype_rg(uint64_t startblk, int metatype, int print)
{
	struct osi_node *next = NULL;
	uint64_t blk, errblk;
	int first = 1, found = 0;
	struct rgrp_tree *rgd = NULL;

	blk = 0;
	/* Skip the rgs prior to the block we've been given */
	for (next = osi_first(&sbd.rgtree); next; next = osi_next(next)) {
		rgd = (struct rgrp_tree *)next;
		if (first && startblk <= rgd->rt_data0) {
			startblk = rgd->rt_data0;
			break;
		} else if (rgd->rt_addr <= startblk &&
			 startblk < rgd->rt_data0 + rgd->rt_data)
			break;
		else
			rgd = NULL;
		first = 0;
	}
	if (!rgd) {
		if (print)
			printf("0\n");
		gfs2_rgrp_free(&sbd, &sbd.rgtree);
		if (print)
			exit(-1);
	}
	for (; !found && next; next = osi_next(next)){
		rgd = (struct rgrp_tree *)next;
		errblk = gfs2_rgrp_read(&sbd, rgd);
		if (errblk)
			continue;

		found = !find_rg_metatype(rgd, &blk, startblk, metatype);
		if (found)
			break;

		gfs2_rgrp_relse(&sbd, rgd);
	}

	if (!found)
		blk = 0;
	if (print) {
		if (dmode == HEX_MODE)
			printf("0x%"PRIx64"\n", blk);
		else
			printf("%"PRIu64"\n", blk);
	}
	gfs2_rgrp_free(&sbd, &sbd.rgtree);
	if (print)
		exit(0);
	return blk;
}

/* ------------------------------------------------------------------------ */
/* Find next metadata block AFTER a given point in the fs                   */
/* ------------------------------------------------------------------------ */
static uint64_t find_metablockoftype(const char *strtype, int print)
{
	int mtype = 0;
	uint64_t startblk, blk = 0;

	if (print)
		startblk = blockstack[blockhist % BLOCK_STACK_SIZE].block;
	else
		startblk = block;

	for (mtype = GFS2_METATYPE_NONE;
	     mtype <= GFS2_METATYPE_QC; mtype++)
		if (!strcasecmp(strtype, mtypes[mtype]))
			break;
	if (!strcmp(strtype, "dinode"))
		mtype = GFS2_METATYPE_DI;
	if (mtype >= GFS2_METATYPE_NONE && mtype <= GFS2_METATYPE_RB)
		blk = find_metablockoftype_slow(startblk, mtype, print);
	else if (mtype >= GFS2_METATYPE_DI && mtype <= GFS2_METATYPE_QC)
		blk = find_metablockoftype_rg(startblk, mtype, print);
	else if (print) {
		fprintf(stderr, "Error: metadata type not "
			"specified: must be one of:\n");
		fprintf(stderr, "sb rg rb di in lf jd lh ld"
			" ea ed lb 13 qc\n");
		gfs2_rgrp_free(&sbd, &sbd.rgtree);
		exit(-1);
	}
	return blk;
}

/* ------------------------------------------------------------------------ */
/* Check if the word is a keyword such as "sb" or "rindex"                  */
/* Returns: block number if it is, else 0                                   */
/* ------------------------------------------------------------------------ */
uint64_t check_keywords(const char *kword)
{
	uint64_t blk = 0;

	if (!strcmp(kword, "sb") ||!strcmp(kword, "superblock"))
		blk = 0x10 * (4096 / sbd.sd_bsize); /* superblock */
	else if (!strcmp(kword, "root") || !strcmp(kword, "rootdir"))
		blk = sbd.sd_root_dir.in_addr;
	else if (!strcmp(kword, "master")) {
		if (sbd.gfs1)
			fprintf(stderr, "This is GFS1; there's no master directory.\n");
		else if (!sbd.sd_meta_dir.in_addr) {
			fprintf(stderr, "GFS2 master directory not found on %s\n", device);
			exit(-1);
		} else
			blk = sbd.sd_meta_dir.in_addr;
	}
	else if (!strcmp(kword, "jindex")) {
		if (sbd.gfs1)
			blk = sbd.sd_jindex_di.in_addr;
		else
			blk = masterblock("jindex"); /* journal index */
	}
	else if (!sbd.gfs1 && !strcmp(kword, "per_node"))
		blk = masterblock("per_node");
	else if (!sbd.gfs1 && !strcmp(kword, "inum"))
		blk = masterblock("inum");
	else if (!strcmp(kword, "statfs")) {
		if (sbd.gfs1)
			blk = gfs1_license_di.in_addr;
		else
			blk = masterblock("statfs");
	}
	else if (!strcmp(kword, "rindex") || !strcmp(kword, "rgindex")) {
		if (sbd.gfs1)
			blk = sbd.sd_rindex_di.in_addr;
		else
			blk = masterblock("rindex");
	} else if (!strcmp(kword, "rgs")) {
		blk = RGLIST_DUMMY_BLOCK;
	} else if (!strcmp(kword, "quota")) {
		if (sbd.gfs1)
			blk = gfs1_quota_di.in_addr;
		else
			blk = masterblock("quota");
	} else if (!strncmp(kword, "rg ", 3)) {
		int rgnum = 0;

		rgnum = atoi(kword + 3);
		blk = get_rg_addr(rgnum);
	} else if (!strncmp(kword, "journals", 8)) {
		blk = JOURNALS_DUMMY_BLOCK;
	} else if (strlen(kword) > 7 && !strncmp(kword, "journal", 7) && isdigit(kword[7])) {
		uint64_t j_size;

		blk = find_journal_block(kword, &j_size);
	} else if (kword[0]=='/') /* search */
		blk = find_metablockoftype(&kword[1], 0);
	else if (kword[0]=='0' && kword[1]=='x') /* hex addr */
		sscanf(kword, "%"SCNx64, &blk);/* retrieve in hex */
	else
		sscanf(kword, "%"SCNu64, &blk); /* retrieve decimal */

	return blk;
}

/* ------------------------------------------------------------------------ */
/* goto_block - go to a desired block entered by the user                   */
/* ------------------------------------------------------------------------ */
static uint64_t goto_block(void)
{
	char string[256];
	int ch, delta;

	memset(string, 0, sizeof(string));
	sprintf(string,"%lld", (long long)block);
	if (bobgets(string, 1, 7, 16, &ch)) {
		if (isalnum(string[0]) || string[0] == '/')
			temp_blk = check_keywords(string);
		else if (string[0] == '+' || string[0] == '-') {
			if (string[1] == '0' && string[2] == 'x')
				sscanf(string, "%x", &delta);
			else
				sscanf(string, "%d", &delta);
			temp_blk = block + delta;
		}

		if (temp_blk == RGLIST_DUMMY_BLOCK ||
		    temp_blk == JOURNALS_DUMMY_BLOCK || temp_blk < max_block) {
			offset = 0;
			block = temp_blk;
			push_block(block);
		}
	}
	return block;
}

/* ------------------------------------------------------------------------ */
/* init_colors                                                              */
/* ------------------------------------------------------------------------ */
static void init_colors(void)
{

	if (color_scheme) {
		init_pair(COLOR_TITLE, COLOR_BLACK,  COLOR_CYAN);
		init_pair(COLOR_NORMAL, COLOR_WHITE,  COLOR_BLACK);
		init_pair(COLOR_INVERSE, COLOR_BLACK,  COLOR_WHITE);
		init_pair(COLOR_SPECIAL, COLOR_RED,    COLOR_BLACK);
		init_pair(COLOR_HIGHLIGHT, COLOR_GREEN, COLOR_BLACK);
		init_pair(COLOR_OFFSETS, COLOR_CYAN,   COLOR_BLACK);
		init_pair(COLOR_CONTENTS, COLOR_YELLOW, COLOR_BLACK);
	}
	else {
		init_pair(COLOR_TITLE, COLOR_BLACK,  COLOR_CYAN);
		init_pair(COLOR_NORMAL, COLOR_BLACK,  COLOR_WHITE);
		init_pair(COLOR_INVERSE, COLOR_WHITE,  COLOR_BLACK);
		init_pair(COLOR_SPECIAL, COLOR_MAGENTA, COLOR_WHITE);
		init_pair(COLOR_HIGHLIGHT, COLOR_RED, COLOR_WHITE); /*cursor*/
		init_pair(COLOR_OFFSETS, COLOR_CYAN,   COLOR_WHITE);
		init_pair(COLOR_CONTENTS, COLOR_BLUE, COLOR_WHITE);
	}
}

/* ------------------------------------------------------------------------ */
/* hex_edit - Allow the user to edit the page by entering hex digits        */
/* ------------------------------------------------------------------------ */
static void hex_edit(int *exitch)
{
	int left_off;
	int ch;

	left_off = ((block * sbd.sd_bsize) < 0xffffffff) ? 9 : 17;
	/* 8 and 16 char addresses on screen */
	
	if (bobgets(estring, edit_row[HEX_MODE] + 3,
		    (edit_col[HEX_MODE] * 2) + (edit_col[HEX_MODE] / 4) +
		    left_off, 2, exitch)) {
		if (strstr(edit_fmt,"X") || strstr(edit_fmt,"x")) {
			int hexoffset;
			int i, sl = strlen(estring);
			
			for (i = 0; i < sl; i+=2) {
				hexoffset = (edit_row[HEX_MODE] * 16) +
					edit_col[HEX_MODE] + (i / 2);
				ch = 0x00;
				if (isdigit(estring[i]))
					ch = (estring[i] - '0') * 0x10;
				else if (estring[i] >= 'a' &&
					 estring[i] <= 'f')
					ch = (estring[i]-'a' + 0x0a)*0x10;
				else if (estring[i] >= 'A' &&
					 estring[i] <= 'F')
					ch = (estring[i] - 'A' + 0x0a) * 0x10;
				if (isdigit(estring[i+1]))
					ch += (estring[i+1] - '0');
				else if (estring[i+1] >= 'a' &&
					 estring[i+1] <= 'f')
					ch += (estring[i+1] - 'a' + 0x0a);
				else if (estring[i+1] >= 'A' &&
					 estring[i+1] <= 'F')
					ch += (estring[i+1] - 'A' + 0x0a);
				bh->b_data[offset + hexoffset] = ch;
			}
			if (pwrite(sbd.device_fd, bh->b_data, sbd.sd_bsize, dev_offset) !=
			    sbd.sd_bsize) {
				fprintf(stderr, "write error: %s from %s:%d: "
					"offset %"PRIu64" (0x%"PRIx64")\n",
					strerror(errno),
					__FUNCTION__, __LINE__,
					dev_offset, dev_offset);
				exit(-1);
			}
			fsync(sbd.device_fd);
		}
	}
}

/* ------------------------------------------------------------------------ */
/* page up                                                                  */
/* ------------------------------------------------------------------------ */
static void pageup(void)
{
	if (dmode == EXTENDED_MODE) {
		if (edit_row[dmode] - (dsplines / lines_per_row[dmode]) > 0)
			edit_row[dmode] -= (dsplines / lines_per_row[dmode]);
		else
			edit_row[dmode] = 0;
		if (start_row[dmode] - (dsplines / lines_per_row[dmode]) > 0)
			start_row[dmode] -= (dsplines / lines_per_row[dmode]);
		else
			start_row[dmode] = 0;
	}
	else {
		start_row[dmode] = edit_row[dmode] = 0;
		if (dmode == GFS2_MODE || offset==0) {
			block--;
			if (dmode == HEX_MODE)
				offset = (sbd.sd_bsize % screen_chunk_size) > 0 ?
					screen_chunk_size *
					(sbd.sd_bsize / screen_chunk_size) :
					sbd.sd_bsize - screen_chunk_size;
			else
				offset = 0;
		} else
			offset -= screen_chunk_size;
	}
}

/* ------------------------------------------------------------------------ */
/* page down                                                                */
/* ------------------------------------------------------------------------ */
static void pagedn(void)
{
	if (dmode == EXTENDED_MODE) {
		if ((edit_row[dmode] + dsplines) / lines_per_row[dmode] + 1 <=
		    end_row[dmode]) {
			start_row[dmode] += dsplines / lines_per_row[dmode];
			edit_row[dmode] += dsplines / lines_per_row[dmode];
		} else {
			edit_row[dmode] = end_row[dmode] - 1;
			while (edit_row[dmode] - start_row[dmode]
			       + 1 > last_entry_onscreen[dmode])
				start_row[dmode]++;
		}
	}
	else {
		start_row[dmode] = edit_row[dmode] = 0;
		if (dmode == GFS2_MODE ||
		    offset + screen_chunk_size >= sbd.sd_bsize) {
			block++;
			offset = 0;
		} else
			offset += screen_chunk_size;
	}
}

/* ------------------------------------------------------------------------ */
/* jump - jump to the address the cursor is on                              */
/*                                                                          */
/* If the cursor is in a log descriptor, jump to the log-descriptor version */
/* of the block instead of the "real" block.                                */
/* ------------------------------------------------------------------------ */
static void jump(void)
{
	if (dmode == HEX_MODE) {
		unsigned int col2;
		__be64 *b;
		const struct lgfs2_metadata *mtype = get_block_type(bh->b_data);
		uint32_t block_type = 0;

		if (mtype != NULL)
			block_type = mtype->mh_type;

		/* special exception for log descriptors: jump the journaled
		   version of the block, not the "real" block */
		if (block_type == GFS2_METATYPE_LD) {
			int ptroffset = edit_row[dmode] * 16 + edit_col[dmode];
			int pnum = get_pnum(ptroffset);
			temp_blk = bh->b_blocknr + pnum + 1;
		} else if (edit_row[dmode] >= 0) {
			col2 = edit_col[dmode] & 0x08;/* thus 0-7->0, 8-15->8 */
			b = (__be64 *)&bh->b_data[edit_row[dmode]*16 +
						    offset + col2];
			temp_blk = be64_to_cpu(*b);
		}
	}
	else
		sscanf(estring, "%"SCNx64, &temp_blk);/* retrieve in hex */
	if (temp_blk < max_block) { /* if the block number is valid */
		int i;
		
		offset = 0;
		push_block(temp_blk);
		block = temp_blk;
		for (i = 0; i < DMODES; i++) {
			start_row[i] = end_row[i] = edit_row[i] = 0;
			edit_col[i] = 0;
		}
	}
}

static void print_block_type(uint64_t tblock, const struct lgfs2_metadata *type)
{
	if (type == NULL)
		return;
	if (type->nfields > 0)
		printf("%d (Block %"PRIu64" is type %d: %s)\n", type->mh_type,
		       tblock, type->mh_type, type->display);
	else
		printf("%d (Block %"PRIu64" is type %d: unknown)\n", type->mh_type,
		       tblock, type->mh_type);
}

static void find_print_block_type(void)
{
	uint64_t tblock;
	struct gfs2_buffer_head *lbh;
	const struct lgfs2_metadata *type;

	tblock = blockstack[blockhist % BLOCK_STACK_SIZE].block;
	lbh = bread(&sbd, tblock);
	type = get_block_type(lbh->b_data);
	print_block_type(tblock, type);
	brelse(lbh);
	gfs2_rgrp_free(&sbd, &sbd.rgtree);
	exit(0);
}

/* ------------------------------------------------------------------------ */
/* Find and print the resource group associated with a given block          */
/* ------------------------------------------------------------------------ */
static void find_print_block_rg(int bitmap)
{
	uint64_t rblock, rgblock;
	int i;
	struct rgrp_tree *rgd;

	rblock = blockstack[blockhist % BLOCK_STACK_SIZE].block;
	if (rblock == LGFS2_SB_ADDR(&sbd))
		printf("0 (the superblock is not in the bitmap)\n");
	else {
		rgd = gfs2_blk2rgrpd(&sbd, rblock);
		if (rgd) {
			rgblock = rgd->rt_addr;
			if (bitmap) {
				struct gfs2_bitmap *bits = NULL;

				for (i = 0; i < rgd->rt_length; i++) {
					bits = &(rgd->bits[i]);
					if (rblock - rgd->rt_data0 <
					    ((bits->bi_start + bits->bi_len) *
					     GFS2_NBBY)) {
						break;
					}
				}
				if (i < rgd->rt_length)
					rgblock += i;

			}
			if (dmode == HEX_MODE)
				printf("0x%"PRIx64"\n", rgblock);
			else
				printf("%"PRIu64"\n", rgblock);
		} else {
			printf("-1 (block invalid or part of an rgrp).\n");
		}
	}
	gfs2_rgrp_free(&sbd, &sbd.rgtree);
	exit(0);
}

/* ------------------------------------------------------------------------ */
/* find/change/print block allocation (what the bitmap says about block)    */
/* ------------------------------------------------------------------------ */
static void find_change_block_alloc(int *newval)
{
	uint64_t ablock;
	int type;
	struct rgrp_tree *rgd;

	if (newval &&
	    (*newval < GFS2_BLKST_FREE || *newval > GFS2_BLKST_DINODE)) {
		int i;

		printf("Error: value %d is not valid.\nValid values are:\n",
		       *newval);
		for (i = GFS2_BLKST_FREE; i <= GFS2_BLKST_DINODE; i++)
			printf("%d - %s\n", i, allocdesc[sbd.gfs1][i]);
		gfs2_rgrp_free(&sbd, &sbd.rgtree);
		exit(-1);
	}
	ablock = blockstack[blockhist % BLOCK_STACK_SIZE].block;
	if (ablock == LGFS2_SB_ADDR(&sbd))
		printf("3 (the superblock is not in the bitmap)\n");
	else {
		rgd = gfs2_blk2rgrpd(&sbd, ablock);
		if (rgd) {
			gfs2_rgrp_read(&sbd, rgd);
			if (newval) {
				if (gfs2_set_bitmap(rgd, ablock, *newval))
					printf("-1 (block invalid or part of an rgrp).\n");
				else
					printf("%d\n", *newval);
			} else {
				type = lgfs2_get_bitmap(&sbd, ablock, rgd);
				if (type < 0) {
					printf("-1 (block invalid or part of "
					       "an rgrp).\n");
					exit(-1);
				}
				printf("%d (%s)\n", type, allocdesc[sbd.gfs1][type]);
			}
			gfs2_rgrp_relse(&sbd, rgd);
		} else {
			gfs2_rgrp_free(&sbd, &sbd.rgtree);
			printf("-1 (block invalid or part of an rgrp).\n");
			exit(-1);
		}
	}
	gfs2_rgrp_free(&sbd, &sbd.rgtree);
	if (newval)
		fsync(sbd.device_fd);
	exit(0);
}

/**
 * process request to print a certain field from a previously pushed block
 */
static void process_field(const char *field, const char *nstr)
{
	uint64_t fblock;
	struct gfs2_buffer_head *rbh;
	const struct lgfs2_metadata *mtype;
	const struct lgfs2_metafield *mfield;

	fblock = blockstack[blockhist % BLOCK_STACK_SIZE].block;
	rbh = bread(&sbd, fblock);
	mtype = get_block_type(rbh->b_data);
	if (mtype == NULL) {
		fprintf(stderr, "Metadata type of block %"PRIx64" not recognised\n",
		        fblock);
		exit(1);
	}

	mfield = lgfs2_find_mfield_name(field, mtype);
	if (mfield == NULL) {
		fprintf(stderr, "No field '%s' in block type '%s'\n", field, mtype->name);
		exit(1);
	}

	if (nstr != device) {
		int err = 0;
		if (mfield->flags & (LGFS2_MFF_UUID|LGFS2_MFF_STRING)) {
			err = lgfs2_field_assign(rbh->b_data, mfield, nstr);
		} else {
			uint64_t val = 0;
			err = sscanf(nstr, "%"SCNi64, &val);
			if (err == 1)
				err = lgfs2_field_assign(rbh->b_data, mfield, &val);
			else
				err = -1;
		}
		if (err != 0) {
			fprintf(stderr, "Could not set '%s' to '%s': %s\n", field, nstr,
			        strerror(errno));
			exit(1);
		}
		bmodified(rbh);
	}

	if (!termlines) {
		char str[GFS2_LOCKNAME_LEN] = "";
		lgfs2_field_str(str, GFS2_LOCKNAME_LEN, rbh->b_data, mfield, (dmode == HEX_MODE));
		printf("%s\n", str);
	}

	brelse(rbh);
	fsync(sbd.device_fd);
	exit(0);
}

/* ------------------------------------------------------------------------ */
/* interactive_mode - accept keystrokes from user and display structures    */
/* ------------------------------------------------------------------------ */
static void interactive_mode(void)
{
	int ch = 0, Quit;

	if ((wind = initscr()) == NULL) {
		fprintf(stderr, "Error: unable to initialize screen.");
		eol(0);
		exit(-1);
	}
	getmaxyx(stdscr, termlines, termcols);
	termlines--;
	/* Do our initial screen stuff: */
	clear(); /* don't use Erase */
	start_color();
	noecho();
	keypad(stdscr, TRUE);
	raw();
	curs_set(0);
	init_colors();
	/* Accept keystrokes and act on them accordingly */
	Quit = FALSE;
	editing = FALSE;
	while (!Quit) {
		display(FALSE, 0, 0, 0);
		if (editing) {
			if (edit_row[dmode] == -1)
				block = goto_block();
			else {
				if (dmode == HEX_MODE)
					hex_edit(&ch);
				else if (dmode == GFS2_MODE) {
					bobgets(estring, edit_row[dmode]+4, 24,
						10, &ch);
					process_field(efield, estring);
				} else
					bobgets(estring, edit_row[dmode]+6, 14,
						edit_size[dmode], &ch);
			}
		}
		else
			while ((ch=getch()) == 0); // wait for input

		switch (ch)
		{
		/* --------------------------------------------------------- */
		/* escape or 'q' */
		/* --------------------------------------------------------- */
		case 0x1b:
		case 0x03:
		case 'q':
			if (editing)
				editing = FALSE;
			else
				Quit=TRUE;
			break;
		/* --------------------------------------------------------- */
		/* home - return to the superblock                           */
		/* --------------------------------------------------------- */
		case KEY_HOME:
			if (dmode == EXTENDED_MODE) {
				start_row[dmode] = end_row[dmode] = 0;
				edit_row[dmode] = 0;
			}
			else {
				block = 0x10 * (4096 / sbd.sd_bsize);
				push_block(block);
				offset = 0;
			}
			break;
		/* --------------------------------------------------------- */
		/* backspace - return to the previous block on the stack     */
		/* --------------------------------------------------------- */
		case KEY_BACKSPACE:
		case 0x7f:
			block = pop_block();
			offset = 0;
			break;
		/* --------------------------------------------------------- */
		/* space - go down the block stack (opposite of backspace)   */
		/* --------------------------------------------------------- */
		case ' ':
			blockhist++;
			block = blockstack[blockhist % BLOCK_STACK_SIZE].block;
			offset = 0;
			break;
		/* --------------------------------------------------------- */
		/* arrow up */
		/* --------------------------------------------------------- */
		case KEY_UP:
		case '-':
			if (dmode == EXTENDED_MODE) {
				if (edit_row[dmode] > 0)
					edit_row[dmode]--;
				if (edit_row[dmode] < start_row[dmode])
					start_row[dmode] = edit_row[dmode];
			}
			else {
				if (edit_row[dmode] >= 0)
					edit_row[dmode]--;
			}
			break;
		/* --------------------------------------------------------- */
		/* arrow down */
		/* --------------------------------------------------------- */
		case KEY_DOWN:
		case '+':
			if (dmode == EXTENDED_MODE) {
				if (edit_row[dmode] + 1 < end_row[dmode]) {
					if (edit_row[dmode] - start_row[dmode]
					    + 1 > last_entry_onscreen[dmode])
						start_row[dmode]++;
					edit_row[dmode]++;
				}
			}
			else {
				if (edit_row[dmode] < last_entry_onscreen[dmode])
					edit_row[dmode]++;
			}
			break;
		/* --------------------------------------------------------- */
		/* arrow left */
		/* --------------------------------------------------------- */
		case KEY_LEFT:
			if (dmode == HEX_MODE) {
				if (edit_col[dmode] > 0)
					edit_col[dmode]--;
				else
					edit_col[dmode] = 15;
			}
			break;
		/* --------------------------------------------------------- */
		/* arrow right */
		/* --------------------------------------------------------- */
		case KEY_RIGHT:
			if (dmode == HEX_MODE) {
				if (edit_col[dmode] < 15)
					edit_col[dmode]++;
				else
					edit_col[dmode] = 0;
			}
			break;
		/* --------------------------------------------------------- */
		/* m - change display mode key */
		/* --------------------------------------------------------- */
		case 'm':
			dmode = ((dmode + 1) % DMODES);
			break;
		/* --------------------------------------------------------- */
		/* J - Jump to highlighted block number */
		/* --------------------------------------------------------- */
		case 'j':
			jump();
			break;
		/* --------------------------------------------------------- */
		/* g - goto block */
		/* --------------------------------------------------------- */
		case 'g':
			block = goto_block();
			break;
		/* --------------------------------------------------------- */
		/* h - help key */
		/* --------------------------------------------------------- */
		case 'h':
			print_usage();
			break;
		/* --------------------------------------------------------- */
		/* e - change to extended mode */
		/* --------------------------------------------------------- */
		case 'e':
			dmode = EXTENDED_MODE;
			break;
		/* --------------------------------------------------------- */
		/* b - Back one 4K block */
		/* --------------------------------------------------------- */
		case 'b':
			start_row[dmode] = end_row[dmode] = edit_row[dmode] = 0;
			if (block > 0)
				block--;
			offset = 0;
			break;
		/* --------------------------------------------------------- */
		/* c - Change color scheme */
		/* --------------------------------------------------------- */
		case 'c':
			color_scheme = !color_scheme;
			init_colors();
			break;
		/* --------------------------------------------------------- */
		/* page up key */
		/* --------------------------------------------------------- */
		case 0x19:                    // ctrl-y for vt100
		case KEY_PPAGE:		      // PgUp
		case 0x15:                    // ctrl-u for vi compat.
		case 0x02:                   // ctrl-b for less compat.
			pageup();
			break;
		/* --------------------------------------------------------- */
		/* end - Jump to the end of the list */
		/* --------------------------------------------------------- */
		case 0x168:
			if (dmode == EXTENDED_MODE) {
				int ents_per_screen = dsplines /
					lines_per_row[dmode];

				edit_row[dmode] = end_row[dmode] - 1;
				if ((edit_row[dmode] - ents_per_screen)+1 > 0)
					start_row[dmode] = edit_row[dmode] - 
						ents_per_screen + 1;
				else
					start_row[dmode] = 0;
			}
			/* TODO: Make end key work for other display modes. */
			break;
		/* --------------------------------------------------------- */
		/* f - Forward one 4K block */
		/* --------------------------------------------------------- */
		case 'f':
			start_row[dmode]=end_row[dmode]=edit_row[dmode] = 0;
			lines_per_row[dmode] = 1;
			block++;
			offset = 0;
			break;
		/* --------------------------------------------------------- */
		/* page down key */
		/* --------------------------------------------------------- */
		case 0x16:                    // ctrl-v for vt100
		case KEY_NPAGE:		      // PgDown
		case 0x04:                    // ctrl-d for vi compat.
			pagedn();
			break;
		/* --------------------------------------------------------- */
		/* enter key - change a value */
		/* --------------------------------------------------------- */
		case KEY_ENTER:
		case('\n'):
		case('\r'):
			editing = !editing;
			break;
		case KEY_RESIZE:
			getmaxyx(stdscr, termlines, termcols);
			termlines--;
			break;
		default:
			move(termlines - 1, 0);
			printw("Keystroke not understood: 0x%03x",ch);
			refresh();
			usleep(50000);
			break;
		} /* switch */
	} /* while !Quit */

    Erase();
    refresh();
    endwin();
}/* interactive_mode */

/* ------------------------------------------------------------------------ */
/* usage - print command line usage                                         */
/* ------------------------------------------------------------------------ */
static void usage(void)
{
	fprintf(stderr,"\nFormat is: gfs2_edit [-c 1] [-V] [-x] [-h] [identify] [-z <0-9>] [-p structures|blocks][blocktype][blockalloc [val]][blockbits][blockrg][rgcount][rgflags][rgbitmaps][find sb|rg|rb|di|in|lf|jd|lh|ld|ea|ed|lb|13|qc][field <f>[val]] /dev/device\n\n");
	fprintf(stderr,"If only the device is specified, it enters into hexedit mode.\n");
	fprintf(stderr,"identify - prints out only the block type, not the details.\n");
	fprintf(stderr,"printsavedmeta - prints out the saved metadata blocks from a savemeta file.\n");
	fprintf(stderr,"savemeta <file_system> <file.gz> - save off your metadata for analysis and debugging.\n");
	fprintf(stderr,"   (The intelligent way: assume bitmap is correct).\n");
	fprintf(stderr,"savemetaslow - save off your metadata for analysis and debugging.  The SLOW way (block by block).\n");
	fprintf(stderr,"savergs - save off only the resource group information (rindex and rgs).\n");
	fprintf(stderr,"restoremeta - restore metadata for debugging (DANGEROUS).\n");
	fprintf(stderr,"rgcount - print how many RGs in the file system.\n");
	fprintf(stderr,"rgflags rgnum [new flags] - print or modify flags for rg #rgnum (0 - X)\n");
	fprintf(stderr,"rgbitmaps <rgnum> - print out the bitmaps for rgrp "
		"rgnum.\n");
	fprintf(stderr,"rgrepair - find and repair damaged rgrp.\n");
	fprintf(stderr,"-V   prints version number.\n");
	fprintf(stderr,"-c 1 selects alternate color scheme 1\n");
	fprintf(stderr,"-d   prints details (for printing journals)\n");
	fprintf(stderr,"-p   prints GFS2 structures or blocks to stdout.\n");
	fprintf(stderr,"     sb - prints the superblock.\n");
	fprintf(stderr,"     size - prints the filesystem size.\n");
	fprintf(stderr,"     master - prints the master directory.\n");
	fprintf(stderr,"     root - prints the root directory.\n");
	fprintf(stderr,"     jindex - prints the journal index directory.\n");
	fprintf(stderr,"     journals - prints the journal status.\n");
	fprintf(stderr,"     per_node - prints the per_node directory.\n");
	fprintf(stderr,"     inum - prints the inum file.\n");
	fprintf(stderr,"     statfs - prints the statfs file.\n");
	fprintf(stderr,"     rindex - prints the rindex file.\n");
	fprintf(stderr,"     rg X - print resource group X.\n");
	fprintf(stderr,"     rgs - prints all the resource groups (rgs).\n");
	fprintf(stderr,"     quota - prints the quota file.\n");
	fprintf(stderr,"     0x1234 - prints the specified block\n");
	fprintf(stderr,"-p   <block> blocktype - prints the type "
		"of the specified block\n");
	fprintf(stderr,"-p   <block> blockrg - prints the resource group "
		"block corresponding to the specified block\n");
	fprintf(stderr,"-p   <block> blockbits - prints the block with "
		"the bitmap corresponding to the specified block\n");
	fprintf(stderr,"-p   <block> blockalloc [0|1|2|3] - print or change "
		"the allocation type of the specified block\n");
	fprintf(stderr,"-p   <block> field [new_value] - prints or change the "
		"structure field\n");
	fprintf(stderr,"-p   <b> find sb|rg|rb|di|in|lf|jd|lh|ld|ea|ed|lb|"
		"13|qc - find block of given type after block <b>\n");
	fprintf(stderr,"     <b> specifies the starting block for search\n");
	fprintf(stderr,"-z 1 use gzip compression level 1 for savemeta (default 9)\n");
	fprintf(stderr,"-z 0 do not use compression\n");
	fprintf(stderr,"-s   specifies a starting block such as root, rindex, quota, inum.\n");
	fprintf(stderr,"-x   print in hexmode.\n");
	fprintf(stderr,"-h   prints this help.\n\n");
	fprintf(stderr,"Examples:\n");
	fprintf(stderr,"   To run in interactive mode:\n");
	fprintf(stderr,"     gfs2_edit /dev/bobs_vg/lvol0\n");
	fprintf(stderr,"   To print out the superblock and master directory:\n");
	fprintf(stderr,"     gfs2_edit -p sb master /dev/bobs_vg/lvol0\n");
	fprintf(stderr,"   To print out the master directory in hex:\n");
	fprintf(stderr,"     gfs2_edit -x -p master /dev/bobs_vg/lvol0\n");
	fprintf(stderr,"   To print out the block-type for block 0x27381:\n");
	fprintf(stderr,"     gfs2_edit identify -p 0x27381 /dev/bobs_vg/lvol0\n");
	fprintf(stderr,"   To print out the fourth Resource Group. (the first R is #0)\n");
	fprintf(stderr,"     gfs2_edit -p rg 3 /dev/sdb1\n");
	fprintf(stderr,"   To print out the metadata type of block 1234\n");
	fprintf(stderr,"     gfs2_edit -p 1234 blocktype /dev/roth_vg/roth_lb\n");
	fprintf(stderr,"   To print out the allocation type of block 2345\n");
	fprintf(stderr,"     gfs2_edit -p 2345 blockalloc /dev/vg/lv\n");
	fprintf(stderr,"   To change the allocation type of block 2345 to a 'free block'\n");
	fprintf(stderr,"     gfs2_edit -p 2345 blockalloc 0 /dev/vg/lv\n");
	fprintf(stderr,"   To print out the file size of the dinode at block 0x118\n");
	fprintf(stderr,"     gfs2_edit -p 0x118 field di_size /dev/roth_vg/roth_lb\n");
	fprintf(stderr,"   To find any dinode higher than the quota file dinode:\n");
	fprintf(stderr,"     gfs2_edit -p quota find di /dev/x/y\n");
	fprintf(stderr,"   To set the Resource Group flags for rg #7 to 3.\n");
	fprintf(stderr,"     gfs2_edit rgflags 7 3 /dev/sdc2\n");
	fprintf(stderr,"   To save off all metadata for /dev/vg/lv:\n");
	fprintf(stderr,"     gfs2_edit savemeta /dev/vg/lv /tmp/metasave.gz\n");
}/* usage */

/**
 * getgziplevel - Process the -z parameter to savemeta operations
 * argv - argv
 * i    - a pointer to the argv index at which to begin processing
 * The index pointed to by i will be incremented past the -z option if found
 */
static void getgziplevel(char *argv[], int *i)
{
	char *opt, *arg;
	char *endptr;

	arg = argv[1 + *i];
	if (strncmp(arg, "-z", 2)) {
		return;
	} else if (arg[2] != '\0') {
		opt = &arg[2];
	} else {
		(*i)++;
		opt = argv[1 + *i];
	}
	errno = 0;
	gziplevel = strtol(opt, &endptr, 10);
	if (errno || endptr == opt || gziplevel < 0 || gziplevel > 9) {
		fprintf(stderr, "Compression level out of range: %s\n", opt);
		exit(-1);
	}
	(*i)++;
}

static int count_dinode_blks(struct rgrp_tree *rgd, int bitmap,
			     struct gfs2_buffer_head *rbh)
{
	struct gfs2_buffer_head *tbh;
	uint64_t b;
	int dinodes = 0;
	char *byte, cur_state, new_state;
	int bit, off;

	if (bitmap)
		off = sizeof(struct gfs2_meta_header);
	else
		off = sizeof(struct gfs2_rgrp);

	for (b = 0; b < rgd->bits[bitmap].bi_len << GFS2_BIT_SIZE; b++) {
		tbh = bread(&sbd, rgd->rt_data0 +
			    rgd->bits[bitmap].bi_start + b);
		byte = rbh->b_data + off + (b / GFS2_NBBY);
		bit = (b % GFS2_NBBY) * GFS2_BIT_SIZE;
		if (lgfs2_check_meta(tbh->b_data, GFS2_METATYPE_DI) == 0) {
			dinodes++;
			new_state = GFS2_BLKST_DINODE;
		} else {
			new_state = GFS2_BLKST_USED;
		}
		cur_state = (*byte >> bit) & GFS2_BIT_MASK;
		*byte ^= cur_state << bit;
		*byte |= new_state << bit;
		brelse(tbh);
	}
	bmodified(rbh);
	return dinodes;
}

static int count_dinode_bits(struct gfs2_buffer_head *rbh)
{
	uint64_t blk;
	struct gfs2_meta_header *mh = (struct gfs2_meta_header *)rbh->b_data;
	char *byte;
	int bit;
	int dinodes = 0;

	if (be32_to_cpu(mh->mh_type) == GFS2_METATYPE_RG)
		blk = sizeof(struct gfs2_rgrp);
	else
		blk = sizeof(struct gfs2_meta_header);

	for (; blk < sbd.sd_bsize; blk++) {
		byte = rbh->b_data + (blk / GFS2_NBBY);
		bit = (blk % GFS2_NBBY) * GFS2_BIT_SIZE;
		if (((*byte >> bit) & GFS2_BIT_MASK) == GFS2_BLKST_DINODE)
			dinodes++;
	}
	return dinodes;
}

static void rg_repair(void)
{
	struct gfs2_buffer_head *rbh;
	struct rgrp_tree *rgd;
	struct osi_node *n;
	int b;
	int rgs_fixed = 0;
	int dinodes_found = 0, dinodes_total = 0;

	/* Walk through the resource groups saving everything within */
	for (n = osi_first(&sbd.rgtree); n; n = osi_next(n)) {
		rgd = (struct rgrp_tree *)n;
		if (gfs2_rgrp_read(&sbd, rgd) == 0) { /* was read in okay */
			gfs2_rgrp_relse(&sbd, rgd);
			continue; /* ignore it */
		}
		/* If we get here, it's because we have an rgrp in the rindex
		   file that can't be read in. So attempt to repair it.
		   If we find a damaged rgrp or bitmap, fix the metadata.
		   Then scan all its blocks: if we find a dinode, set the
		   repaired bitmap to GFS2_BLKST_DINODE. Set all others to
		   GFS2_BLKST_USED so fsck can sort it out. If we set them
		   to FREE, fsck would just nuke it all. */
		printf("Resource group at block %"PRIu64" (0x%"PRIx64") appears to be "
		       "damaged. Attempting to fix it (in reverse order).\n",
		       rgd->rt_addr, rgd->rt_addr);

		for (b = rgd->rt_length - 1; b >= 0; b--) {
			int mtype = (b ? GFS2_METATYPE_RB : GFS2_METATYPE_RG);
			struct gfs2_meta_header *mh;

			printf("Bitmap #%d:", b);
			rbh = bread(&sbd, rgd->rt_addr + b);
			if (lgfs2_check_meta(rbh->b_data, mtype)) { /* wrong type */
				printf("Damaged. Repairing...");
				/* Fix the meta header */
				memset(rbh->b_data, 0, sbd.sd_bsize);
				mh = (struct gfs2_meta_header *)rbh->b_data;
				mh->mh_magic = cpu_to_be32(GFS2_MAGIC);
				mh->mh_type = cpu_to_be32(mtype);
				if (b)
					mh->mh_format =
						cpu_to_be32(GFS2_FORMAT_RB);
				else
					mh->mh_format =
						cpu_to_be32(GFS2_FORMAT_RG);
				bmodified(rbh);
				/* Count the dinode blocks */
				dinodes_found = count_dinode_blks(rgd, b, rbh);
			} else { /* bitmap info is okay: tally it. */
				printf("Undamaged. Analyzing...");
				dinodes_found = count_dinode_bits(rbh);
			}
			printf("Dinodes found: %d\n", dinodes_found);
			dinodes_total += dinodes_found;
			if (b == 0) { /* rgrp itself was damaged */
				rgd->rt_dinodes = dinodes_total;
				rgd->rt_free = 0;
			}
			brelse(rbh);
		}
		rgs_fixed++;
	}
	if (rgs_fixed)
		printf("%d resource groups fixed.\n"
		       "You should run fsck.gfs2 to reconcile the bitmaps.\n",
		       rgs_fixed);
	else
		printf("All resource groups are okay. No repairs needed.\n");
	exit(0);
}

/* ------------------------------------------------------------------------ */
/* parameterpass1 - pre-processing for command-line parameters              */
/* ------------------------------------------------------------------------ */
static void parameterpass1(int argc, char *argv[], int i)
{
	if (!strcasecmp(argv[i], "-V")) {
		printf("%s version %s (built %s %s)\n",
		       argv[0], VERSION, __DATE__, __TIME__);
		printf("%s\n", REDHAT_COPYRIGHT);
		exit(0);
	}
	else if (!strcasecmp(argv[i], "-h") ||
		 !strcasecmp(argv[i], "-help") ||
		 !strcasecmp(argv[i], "-usage")) {
		usage();
		exit(0);
	}
	else if (!strcasecmp(argv[i], "-c")) {
		i++;
		color_scheme = atoi(argv[i]);
	}
	else if (!strcasecmp(argv[i], "-p") ||
		 !strcasecmp(argv[i], "-print")) {
		termlines = 0; /* initial value--we'll figure
				  it out later */
		dmode = GFS2_MODE;
	}
	else if (!strcasecmp(argv[i], "-d") ||
		 !strcasecmp(argv[i], "-details"))
		details = 1;
	else if (!strcasecmp(argv[i], "savemeta"))
		termlines = 0;
	else if (!strcasecmp(argv[i], "savemetaslow"))
		termlines = 0;
	else if (!strcasecmp(argv[i], "savergs"))
		termlines = 0;
	else if (!strcasecmp(argv[i], "printsavedmeta")) {
		if (dmode == INIT_MODE)
			dmode = GFS2_MODE;
		restoremeta(argv[i+1], argv[i+2], TRUE);
	} else if (!strcasecmp(argv[i], "restoremeta")) {
		if (dmode == INIT_MODE)
			dmode = HEX_MODE; /* hopefully not used */
		restoremeta(argv[i+1], argv[i+2], FALSE);
	} else if (!strcmp(argv[i], "rgcount"))
		termlines = 0;
	else if (!strcmp(argv[i], "rgflags"))
		termlines = 0;
	else if (!strcmp(argv[i], "rgrepair"))
		termlines = 0;
	else if (!strcmp(argv[i], "rg"))
		termlines = 0;
	else if (!strcasecmp(argv[i], "-x"))
		dmode = HEX_MODE;
	else if (device == NULL && strchr(argv[i],'/')) {
		device = argv[i];
	}
}

/* ------------------------------------------------------------------------ */
/* process_parameters - process commandline parameters                      */
/* pass - we make two passes through the parameters; the first pass gathers */
/*        normals parameters, device name, etc.  The second pass is for     */
/*        figuring out what structures to print out.                        */
/* ------------------------------------------------------------------------ */
static void process_parameters(int argc, char *argv[], int pass)
{
	int i;
	uint64_t keyword_blk;

	if (argc < 2) {
		fprintf(stderr, "No device specified\n");
		exit(1);
	}
	for (i = 1; i < argc; i++) {
		if (!pass) { /* first pass */
			parameterpass1(argc, argv, i);
			continue;
		}
		/* second pass */
		if (!strcasecmp(argv[i], "-s")) {
			i++;
			if (i >= argc - 1) {
				printf("Error: starting block not specified "
				       "with -s.\n");
				printf("%s -s [starting block | keyword] "
				       "<device>\n", argv[0]);
				printf("For example: %s -s \"rg 3\" "
				       "/dev/exxon_vg/exxon_lv\n", argv[0]);
				exit(EXIT_FAILURE);
			}
			starting_blk = check_keywords(argv[i]);
			continue;
		}
		if (termlines || strchr(argv[i],'/')) /* if print or slash */
			continue;

		if (!strncmp(argv[i], "journal", 7) && isdigit(argv[i][7]) &&
		    strcmp(argv[i+1], "field")) {
			uint64_t blk = 0;

			if (i < argc - 1 && isdigit(argv[i + 1][0])) {
				if (argv[i + 1][0]=='0' && argv[i + 1][1]=='x')
					sscanf(argv[i + 1], "%"SCNx64, &blk);
				else
					sscanf(argv[i + 1], "%"SCNu64, &blk);
			}
			dump_journal(argv[i], blk);
			continue;
		}
		keyword_blk = check_keywords(argv[i]);
		if (keyword_blk)
			push_block(keyword_blk);
		else if (!strcasecmp(argv[i], "-x"))
			dmode = HEX_MODE;
		else if (argv[i][0] == '-') /* if it starts with a dash */
			; /* ignore it--meant for pass == 0 */
		else if (!strcmp(argv[i], "identify"))
			identify = TRUE;
		else if (!strcmp(argv[i], "size")) {
			printf("Device size: %"PRIu64" blocks\n", max_block);
			exit(EXIT_SUCCESS);
		} else if (!strcmp(argv[i], "rgcount"))
			rgcount();
		else if (!strcmp(argv[i], "field")) {
			i++;
			if (i >= argc - 1) {
				printf("Error: field not specified.\n");
				printf("Format is: %s -p <block> field "
				       "<field> [newvalue]\n", argv[0]);
				gfs2_rgrp_free(&sbd, &sbd.rgtree);
				exit(EXIT_FAILURE);
			}
			process_field(argv[i], argv[i + 1]);
		} else if (!strcmp(argv[i], "blocktype")) {
			find_print_block_type();
		} else if (!strcmp(argv[i], "blockrg")) {
			find_print_block_rg(0);
		} else if (!strcmp(argv[i], "blockbits")) {
			find_print_block_rg(1);
		} else if (!strcmp(argv[i], "blockalloc")) {
			if (isdigit(argv[i + 1][0])) {
				int newval;

				if (argv[i + 1][0]=='0' && argv[i + 1][1]=='x')
					sscanf(argv[i + 1], "%x", &newval);
				else
					newval = (uint64_t)atoi(argv[i + 1]);
				find_change_block_alloc(&newval);
			} else {
				find_change_block_alloc(NULL);
			}
		} else if (!strcmp(argv[i], "find")) {
			find_metablockoftype(argv[i + 1], 1);
		} else if (!strcmp(argv[i], "rgflags")) {
			int rg, set = FALSE;
			uint32_t new_flags = 0;

			i++;
			if (i >= argc - 1) {
				printf("Error: rg # not specified.\n");
				printf("Format is: %s rgflags rgnum"
				       "[newvalue]\n", argv[0]);
				gfs2_rgrp_free(&sbd, &sbd.rgtree);
				exit(EXIT_FAILURE);
			}
			if (argv[i][0]=='0' && argv[i][1]=='x')
				sscanf(argv[i], "%"SCNx32, &rg);
			else
				rg = atoi(argv[i]);
			i++;
			if (i < argc - 1 &&
			    isdigit(argv[i][0])) {
				set = TRUE;
				if (argv[i][0]=='0' && argv[i][1]=='x')
					sscanf(argv[i], "%"SCNx32, &new_flags);
				else
					new_flags = atoi(argv[i]);
			}
			set_rgrp_flags(rg, new_flags, set, FALSE);
			gfs2_rgrp_free(&sbd, &sbd.rgtree);
			exit(EXIT_SUCCESS);
		} else if (!strcmp(argv[i], "rg")) {
			int rg;

			i++;
			if (i >= argc - 1) {
				printf("Error: rg # not specified.\n");
				printf("Format is: %s rg rgnum\n", argv[0]);
				gfs2_rgrp_free(&sbd, &sbd.rgtree);
				exit(EXIT_FAILURE);
			}
			rg = atoi(argv[i]);
			if (!strcasecmp(argv[i + 1], "find")) {
				temp_blk = get_rg_addr(rg);
				push_block(temp_blk);
			} else {
				set_rgrp_flags(rg, 0, FALSE, TRUE);
				gfs2_rgrp_free(&sbd, &sbd.rgtree);
				exit(EXIT_SUCCESS);
			}
		} else if (!strcmp(argv[i], "rgbitmaps")) {
			int rg, bmap;
			uint64_t rgblk;
			struct rgrp_tree *rgd;

			i++;
			if (i >= argc - 1) {
				printf("Error: rg # not specified.\n");
				printf("Format is: %s rgbitmaps rgnum\n",
				       argv[0]);
				gfs2_rgrp_free(&sbd, &sbd.rgtree);
				exit(EXIT_FAILURE);
			}
			rg = atoi(argv[i]);
			rgblk = get_rg_addr(rg);
			rgd = gfs2_blk2rgrpd(&sbd, rgblk);
			if (rgd == NULL) {
				printf("Error: rg # is invalid.\n");
				gfs2_rgrp_free(&sbd, &sbd.rgtree);
				exit(EXIT_FAILURE);
			}
			for (bmap = 0; bmap < rgd->rt_length; bmap++)
				push_block(rgblk + bmap);
		}
		else if (!strcmp(argv[i], "rgrepair"))
			rg_repair();
		else if (!strcasecmp(argv[i], "savemeta")) {
			getgziplevel(argv, &i);
			savemeta(argv[i+2], 0, gziplevel);
		} else if (!strcasecmp(argv[i], "savemetaslow")) {
			getgziplevel(argv, &i);
			savemeta(argv[i+2], 1, gziplevel);
		} else if (!strcasecmp(argv[i], "savergs")) {
			getgziplevel(argv, &i);
			savemeta(argv[i+2], 2, gziplevel);
		} else if (isdigit(argv[i][0])) { /* decimal addr */
			sscanf(argv[i], "%"SCNd64, &temp_blk);
			push_block(temp_blk);
		} else {
			fprintf(stderr,"I don't know what '%s' means.\n",
				argv[i]);
			usage();
			exit(EXIT_FAILURE);
		}
	} /* for */
}/* process_parameters */

#ifndef UNITTESTS
int main(int argc, char *argv[])
{
	int i, j, fd;

	indirect = malloc(sizeof(struct iinfo));
	if (indirect == NULL) {
		perror("Failed to allocate indirect info");
		exit(1);
	}
	memset(indirect, 0, sizeof(struct iinfo));
	memset(start_row, 0, sizeof(start_row));
	memset(lines_per_row, 0, sizeof(lines_per_row));
	memset(end_row, 0, sizeof(end_row));
	memset(edit_row, 0, sizeof(edit_row));
	memset(edit_col, 0, sizeof(edit_col));
	memset(edit_size, 0, sizeof(edit_size));
	memset(last_entry_onscreen, 0, sizeof(last_entry_onscreen));
	dmode = INIT_MODE;
	sbd.sd_bsize = 4096;
	block = starting_blk = 0x10;
	for (i = 0; i < BLOCK_STACK_SIZE; i++) {
		blockstack[i].dmode = HEX_MODE;
		blockstack[i].block = block;
		for (j = 0; j < DMODES; j++) {
			blockstack[i].start_row[j] = 0;
			blockstack[i].end_row[j] = 0;
			blockstack[i].edit_row[j] = 0;
			blockstack[i].edit_col[j] = 0;
			blockstack[i].lines_per_row[j] = 0;
		}
	}

	edit_row[GFS2_MODE] = 10; /* Start off at root inode
				     pointer in superblock */
	termlines = 30;  /* assume interactive mode until we find -p */
	process_parameters(argc, argv, 0);
	if (dmode == INIT_MODE)
		dmode = HEX_MODE;

	fd = open(device, O_RDWR);
	if (fd < 0) {
		fprintf(stderr, "Failed to open '%s': %s\n", device, strerror(errno));
		exit(1);
	}
	max_block = lseek(fd, 0, SEEK_END) / sbd.sd_bsize;

	read_superblock(fd);
	if (read_rindex())
		exit(-1);
	max_block = lseek(fd, 0, SEEK_END) / sbd.sd_bsize;
	if (sbd.gfs1)
		edit_row[GFS2_MODE]++;
	else if (read_master_dir() != 0)
		exit(-1);

	process_parameters(argc, argv, 1); /* get what to print from cmdline */

	block = blockstack[0].block = starting_blk * (4096 / sbd.sd_bsize);

	if (termlines)
		interactive_mode();
	else { /* print all the structures requested */
		i = 0;
		while (blockhist > 0) {
			block = blockstack[i + 1].block;
			if (!block)
				break;
			display(identify, 0, 0, 0);
			if (!identify) {
				display_extended();
				printf("-------------------------------------" \
				       "-----------------");
				eol(0);
			}
			block = pop_block();
			i++;
		}
	}
	close(fd);
	if (indirect)
		free(indirect);
	gfs2_rgrp_free(&sbd, &sbd.rgtree);
 	exit(EXIT_SUCCESS);
}
#endif /* UNITTESTS */
