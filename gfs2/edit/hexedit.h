#ifndef __HEXVIEW_DOT_H__
#define __HEXVIEW_DOT_H__

#include <sys/types.h>
#include <inttypes.h>
#include <limits.h>
#include <linux/gfs2_ondisk.h>
#include <string.h>

#include "libgfs2.h"
#include "copyright.cf"

#ifndef TRUE
#define TRUE 1
#endif
#ifndef FALSE
#define FALSE 0
#endif

#define DMODES 3
enum dsp_mode { HEX_MODE = 0, GFS2_MODE = 1, EXTENDED_MODE = 2, INIT_MODE = 3 };
#define BLOCK_STACK_SIZE 256

#define GFS_FORMAT_SB           (100)  /* Super-Block */
#define GFS_METATYPE_SB         (1)    /* Super-Block */
#define GFS_FORMAT_FS           (1309) /* Filesystem (all-encompassing) */
#define GFS_FORMAT_MULTI        (1401) /* Multi-Host */
/* GFS1 Dinode types  */
#define GFS_FILE_NON            (0)
#define GFS_FILE_REG            (1)    /* regular file */
#define GFS_FILE_DIR            (2)    /* directory */
#define GFS_FILE_LNK            (5)    /* link */
#define GFS_FILE_BLK            (7)    /* block device node */
#define GFS_FILE_CHR            (8)    /* character device node */
#define GFS_FILE_FIFO           (101)  /* fifo/pipe */
#define GFS_FILE_SOCK           (102)  /* socket */

/* GFS 1 journal block types: */
#define GFS_LOG_DESC_METADATA   (300)    /* metadata */
#define GFS_LOG_DESC_IUL        (400)    /* unlinked inode */
#define GFS_LOG_DESC_IDA        (401)    /* de-allocated inode */
#define GFS_LOG_DESC_Q          (402)    /* quota */
#define GFS_LOG_DESC_LAST       (500)    /* final in a logged transaction */

#define pv(struct, member, fmt, fmt2) do {				\
		print_it("  "#member, fmt, fmt2, struct->member);	\
	} while (FALSE);
#define RGLIST_DUMMY_BLOCK -2

extern struct gfs2_sb sb;
extern uint64_t block;
extern int blockhist;
extern int edit_mode;
extern int line;
extern char edit_fmt[80];
extern char estring[1024]; /* edit string */
extern char efield[64];
extern uint64_t dev_offset;
extern uint64_t max_block;
extern struct gfs2_buffer_head *bh;
extern int termlines;
extern int termcols;
extern int insert;
extern const char *termtype;
extern int line;
extern int struct_len;
extern unsigned int offset;
extern int edit_row[DMODES], edit_col[DMODES], print_entry_ndx;
extern int start_row[DMODES], end_row[DMODES], lines_per_row[DMODES];
extern int edit_size[DMODES], last_entry_onscreen[DMODES];
extern char edit_fmt[80];
extern struct gfs2_sbd sbd;
extern struct gfs_sb *sbd1;
extern struct gfs2_inum gfs1_quota_di;   /* kludge because gfs2 sb too small */
extern struct gfs2_inum gfs1_license_di; /* kludge because gfs2 sb too small */
extern struct gfs2_dinode di;
extern int screen_chunk_size; /* how much of the 4K can fit on screen */
extern int gfs2_struct_type;
extern uint64_t block_in_mem;
extern char device[NAME_MAX];
extern int identify;
extern int color_scheme;
extern WINDOW *wind;
extern int gfs1;
extern int editing;
extern uint64_t temp_blk;
extern uint64_t starting_blk;
extern const char *block_type_str[15];
extern int dsplines;
extern int dsp_lines[DMODES];
extern int combined_display;

struct gfs_jindex {
        uint64_t ji_addr;       /* starting block of the journal */
        uint32_t ji_nsegment;   /* number (quantity) of segments in journal */
        uint32_t ji_pad;

        char ji_reserved[64];
};

struct gfs_log_descriptor {
	struct gfs2_meta_header ld_header;

	uint32_t ld_type;       /* GFS_LOG_DESC_... Type of this log chunk */
	uint32_t ld_length;     /* Number of buffers in this chunk */
	uint32_t ld_data1;      /* descriptor-specific field */
	uint32_t ld_data2;      /* descriptor-specific field */
	char ld_reserved[64];
};

struct gfs_log_header {
	struct gfs2_meta_header lh_header;

	uint32_t lh_flags;      /* GFS_LOG_HEAD_... */
	uint32_t lh_pad;

	uint64_t lh_first;     /* Block number of first header in this trans */
	uint64_t lh_sequence;   /* Sequence number of this transaction */

	uint64_t lh_tail;       /* Block number of log tail */
	uint64_t lh_last_dump;  /* Block number of last dump */

	char lh_reserved[64];
};

struct gfs_rindex {
	uint64_t ri_addr;     /* block # of 1st block (header) in rgrp */
	uint32_t ri_length;   /* # fs blocks containing rgrp header & bitmap */
	uint32_t ri_pad;

	uint64_t ri_data1;    /* block # of first data/meta block in rgrp */
	uint32_t ri_data;     /* number (qty) of data/meta blocks in rgrp */

	uint32_t ri_bitbytes; /* total # bytes used by block alloc bitmap */

	char ri_reserved[64];
};

struct gfs_rgrp {
	struct gfs2_meta_header rg_header;

	uint32_t rg_flags;      /* ?? */

	uint32_t rg_free;       /* Number (qty) of free data blocks */

	/* Dinodes are USEDMETA, but are handled separately from other METAs */
	uint32_t rg_useddi;     /* Number (qty) of dinodes (used or free) */
	uint32_t rg_freedi;     /* Number (qty) of unused (free) dinodes */
	struct gfs2_inum rg_freedi_list; /* 1st block in chain of free dinodes */

	/* These META statistics do not include dinodes (used or free) */
	uint32_t rg_usedmeta;   /* Number (qty) of used metadata blocks */
	uint32_t rg_freemeta;   /* Number (qty) of unused metadata blocks */

	char rg_reserved[64];
};

struct gfs2_dirents {
	uint64_t block;
	struct gfs2_dirent dirent;
	char filename[NAME_MAX];
};

struct indirect_info {
	int is_dir;
	int height;
	uint64_t block;
	uint32_t dirents;
	uint16_t lf_depth;
	uint16_t lf_entries;
	uint32_t lf_dirent_format;
	uint64_t lf_next;
	struct metapath mp;
	struct gfs2_dirents dirent[64];
};

struct iinfo {
	struct indirect_info ii[512];
};

struct blkstack_info {
	uint64_t block;
	int start_row[DMODES];
	int end_row[DMODES];
	int lines_per_row[DMODES];
	int edit_row[DMODES];
	int edit_col[DMODES];
	enum dsp_mode dmode;
	int gfs2_struct_type;
	struct metapath mp;
};

struct gfs_sb {
	/*  Order is important; need to be able to read old superblocks
	    in order to support on-disk version upgrades */
	struct gfs2_meta_header sb_header;

	uint32_t sb_fs_format;         /* GFS_FORMAT_FS (on-disk version) */
	uint32_t sb_multihost_format;  /* GFS_FORMAT_MULTI */
	uint32_t sb_flags;             /* ?? */

	uint32_t sb_bsize;             /* fundamental FS block size in bytes */
	uint32_t sb_bsize_shift;       /* log2(sb_bsize) */
	uint32_t sb_seg_size;          /* Journal segment size in FS blocks */

	/* These special inodes do not appear in any on-disk directory. */
	struct gfs2_inum sb_jindex_di;  /* journal index inode */
	struct gfs2_inum sb_rindex_di;  /* resource group index inode */
	struct gfs2_inum sb_root_di;    /* root directory inode */

	/* Default inter-node locking protocol (lock module) and namespace */
	char sb_lockproto[GFS2_LOCKNAME_LEN]; /* lock protocol name */
	char sb_locktable[GFS2_LOCKNAME_LEN]; /* unique name for this FS */

	/* More special inodes */
	struct gfs2_inum sb_quota_di;   /* quota inode */
	struct gfs2_inum sb_license_di; /* license inode */

	char sb_reserved[96];
};

extern struct blkstack_info blockstack[BLOCK_STACK_SIZE];
extern struct iinfo *indirect; /* more than the most indirect
			       pointers possible for any given 4K block */
extern struct indirect_info masterdir; /* Master directory info */
extern int indirect_blocks;  /* count of indirect blocks */
extern enum dsp_mode dmode;

/* ------------------------------------------------------------------------ */
/* risize - size of one rindex entry, whether gfs1 or gfs2                  */
/* ------------------------------------------------------------------------ */
static inline int risize(void)
{
	if (gfs1)
		return sizeof(struct gfs_rindex);
	else
		return sizeof(struct gfs2_rindex);
}

/* ------------------------------------------------------------------------ */
/* block_is_rglist - there's no such block as the rglist.  This is a        */
/*                   special case meant to parse the rindex and follow the  */
/*                   blocks to the real rgs.                                */
/* ------------------------------------------------------------------------ */
static inline int block_is_rglist(void)
{
	if (block == RGLIST_DUMMY_BLOCK)
		return TRUE;
	return FALSE;
}

#define SCREEN_HEIGHT   (16)
#define SCREEN_WIDTH    (16)

/*  Memory macros  */

#define type_alloc(ptr, type, count) \
{ \
  (ptr) = (type *)malloc(sizeof(type) * (count)); \
  if (!(ptr)) \
    die("unable to allocate memory on line %d of file %s\n", \
	__LINE__, __FILE__); \
}

#define printk printw

/*  Divide x by y.  Round up if there is a remainder.  */
#define DIV_RU(x, y) (((x) + (y) - 1) / (y))

#define TITLE1 "gfs2_edit - Global File System Editor (use with extreme caution)"
#define TITLE2 REDHAT_COPYRIGHT " - Press H for help"

#define COLOR_TITLE     1
#define COLOR_NORMAL    2
#define COLOR_INVERSE   3
#define COLOR_SPECIAL   4
#define COLOR_HIGHLIGHT 5
#define COLOR_OFFSETS   6
#define COLOR_CONTENTS  7

#define COLORS_TITLE     \
	do { \
		if (termlines) { \
			attrset(COLOR_PAIR(COLOR_TITLE)); \
			attron(A_BOLD); \
		} \
	} while (0)
#define COLORS_NORMAL    \
	do { \
		if (termlines) { \
			attrset(COLOR_PAIR(COLOR_NORMAL)); \
			attron(A_BOLD); \
		} \
	} while (0)
#define COLORS_INVERSE   \
	do { \
		if (termlines) { \
			attrset(COLOR_PAIR(COLOR_INVERSE)); \
			attron(A_BOLD); \
		} \
	} while (0)
#define COLORS_SPECIAL   \
	do { \
		if (termlines) { \
			attrset(COLOR_PAIR(COLOR_SPECIAL)); \
			attron(A_BOLD); \
		} \
	} while (0)
#define COLORS_HIGHLIGHT \
	do { \
		if (termlines) { \
			attrset(COLOR_PAIR(COLOR_HIGHLIGHT)); \
			attron(A_BOLD); \
		} \
	} while (0)
#define COLORS_OFFSETS   \
	do { \
		if (termlines) { \
			attrset(COLOR_PAIR(COLOR_OFFSETS)); \
			attron(A_BOLD); \
		} \
	} while (0)
#define COLORS_CONTENTS  \
	do { \
		if (termlines) { \
			attrset(COLOR_PAIR(COLOR_CONTENTS)); \
			attron(A_BOLD); \
		} \
	} while (0)

extern int block_is_jindex(void);
extern int block_is_rindex(void);
extern int block_is_inum_file(void);
extern int block_is_statfs_file(void);
extern int block_is_quota_file(void);
extern int block_is_per_node(void);
extern int block_is_in_per_node(void);
extern int display_block_type(int from_restore);
extern void gfs_jindex_in(struct gfs_jindex *jindex, char *buf);
extern void gfs_log_header_in(struct gfs_log_header *head,
			      struct gfs2_buffer_head *bh);
extern void gfs_log_header_print(struct gfs_log_header *lh);
extern void gfs_dinode_in(struct gfs_dinode *di, struct gfs2_buffer_head *bh);
extern void savemeta(char *out_fn, int saveoption);
extern void restoremeta(const char *in_fn, const char *out_device,
			uint64_t printblocksonly);
extern int display(int identify_only);
extern uint64_t check_keywords(const char *kword);
extern uint64_t masterblock(const char *fn);
extern void gfs_rgrp_print(struct gfs_rgrp *rg);
extern void gfs_rgrp_in(struct gfs_rgrp *rgrp, struct gfs2_buffer_head *rbh);
extern int has_indirect_blocks(void);

#endif /* __HEXVIEW_DOT_H__ */
