#ifndef __UTIL_H__
#define __UTIL_H__

#include <sys/stat.h>

#include "fsck.h"
#include "libgfs2.h"

#define fsck_lseek(fd, off) \
  ((lseek((fd), (off), SEEK_SET) == (off)) ? 0 : -1)

#define INODE_VALID 1
#define INODE_INVALID 0

struct alloc_state {
	uint64_t as_blocks;
	uint64_t as_meta_goal;
};

struct di_info *search_list(osi_list_t *list, uint64_t addr);
void big_file_comfort(struct gfs2_inode *ip, uint64_t blks_checked);
void warm_fuzzy_stuff(uint64_t block);
int add_duplicate_ref(struct gfs2_inode *ip, uint64_t block,
		      enum dup_ref_type reftype, int first, int inode_valid);
extern struct inode_with_dups *find_dup_ref_inode(struct duptree *dt,
						  struct gfs2_inode *ip);
extern void dup_listent_delete(struct duptree *dt, struct inode_with_dups *id);

extern const char *reftypes[ref_types + 1];

#define BLOCKMAP_SIZE4(size) ((size) >> 1)
#define BLOCKMAP_BYTE_OFFSET4(x) (((x) & 0x0000000000000001) << 2)
#define BLOCKMAP_MASK4 (0xf)

static inline void astate_save(struct gfs2_inode *ip, struct alloc_state *as)
{
	as->as_blocks = ip->i_di.di_blocks;
	as->as_meta_goal = ip->i_di.di_goal_meta;
}

static inline int astate_changed(struct gfs2_inode *ip, struct alloc_state *as)
{
	if (as->as_blocks != ip->i_di.di_blocks)
		return 1;
	if (as->as_meta_goal != ip->i_di.di_goal_meta)
		return 1;
	return 0;
}

static inline uint8_t block_type(uint64_t bblock)
{
	static unsigned char *byte;
	static uint64_t b;
	static uint8_t btype;

	byte = bl->map + BLOCKMAP_SIZE4(bblock);
	b = BLOCKMAP_BYTE_OFFSET4(bblock);
	btype = (*byte & (BLOCKMAP_MASK4 << b )) >> b;
	return btype;
}

/* blockmap declarations and functions */
enum gfs2_mark_block {
	gfs2_block_free    = (0x0),
	gfs2_block_used    = (0x1),
	gfs2_indir_blk     = (0x2),
	/* These are inode block types (only): */
	gfs2_inode_dir     = (0x3),
	gfs2_inode_file    = (0x4),

	gfs2_inode_lnk     = (0x5),
	gfs2_inode_device  = (0x6), /* char or block device */
	gfs2_inode_fifo    = (0x7),
	gfs2_inode_sock    = (0x8),
	gfs2_inode_invalid = (0x9),

	/* misc block types: */
	gfs2_jdata         = (0xa), /* gfs journaled data blocks */
	gfs2_meta_inval    = (0xb),
	gfs2_leaf_blk      = (0xc),
	gfs2_freemeta      = (0xd), /* was: gfs2_meta_rgrp */
	gfs2_meta_eattr    = (0xe),

	gfs2_bad_block     = (0xf), /* Contains at least one bad block */
};

static const inline char *block_type_string(uint8_t q)
{
	const char *blktyp[] = {
		"free",
		"data",
		"indirect meta",
		"directory",
		"file",

		"symlink",
		"device",
		"fifo",
		"socket",
		"invalid inode",

		"journaled data",
		"invalid meta",
		"dir leaf",
		"free metadata",
		"eattribute",

		"bad"};
	if (q < 16)
		return (blktyp[q]);
	return blktyp[15];
}

/* Must be kept in sync with gfs2_mark_block enum above. Blocks marked as
   invalid or bad are considered metadata until actually freed. */
static inline int blockmap_to_bitmap(enum gfs2_mark_block m, int gfs1)
{
	static int bitmap_states[2][16] = {
		/* ---------------------- gfs2 ------------------------------*/
		{GFS2_BLKST_FREE,  /* free */
		 GFS2_BLKST_USED,  /* data */
		 GFS2_BLKST_USED,  /* indirect data or rgrp meta */
		 GFS2_BLKST_DINODE,  /* directory */
		 GFS2_BLKST_DINODE,  /* file */

		 GFS2_BLKST_DINODE,  /* symlink */
		 GFS2_BLKST_DINODE,  /* block or char device */
		 GFS2_BLKST_DINODE,  /* fifo */
		 GFS2_BLKST_DINODE,  /* socket */
		 GFS2_BLKST_FREE,  /* invalid inode */

		 GFS2_BLKST_USED,    /* journaled data */
		 GFS2_BLKST_FREE,  /* invalid meta */
		 GFS2_BLKST_USED,  /* dir leaf */
		 GFS2_BLKST_UNLINKED,  /* GFS unlinked metadata */
		 GFS2_BLKST_USED,  /* eattribute */

		 GFS2_BLKST_DINODE}, /* bad */
		/* ---------------------- gfs1 ----------------------------- */
		{GFS2_BLKST_FREE,  /* free */
		 GFS2_BLKST_USED,  /* data */
		 GFS2_BLKST_DINODE,  /* indirect data or rgrp meta*/
		 GFS2_BLKST_DINODE,  /* directory */
		 GFS2_BLKST_DINODE,  /* file */

		 GFS2_BLKST_DINODE,  /* symlink */
		 GFS2_BLKST_DINODE,  /* block or char device */
		 GFS2_BLKST_DINODE,  /* fifo */
		 GFS2_BLKST_DINODE,  /* socket */
		 GFS2_BLKST_FREE,  /* invalid inode */

		 GFS2_BLKST_DINODE,  /* journaled data */
		 GFS2_BLKST_FREE,  /* invalid meta */
		 GFS2_BLKST_DINODE,  /* dir leaf */
		 GFS2_BLKST_UNLINKED, /* GFS unlinked metadata */
		 GFS2_BLKST_DINODE,  /* eattribute */

		 GFS2_BLKST_DINODE}}; /* bad */
	return bitmap_states[gfs1][m];
}

static inline int is_dir(struct gfs2_dinode *dinode, int gfs1)
{
	if (gfs1 && is_gfs_dir(dinode))
		return 1;
	if (S_ISDIR(dinode->di_mode))
		return 1;

	return 0;
}

static inline uint32_t gfs_to_gfs2_mode(struct gfs2_inode *ip)
{
	uint16_t gfs1mode = ip->i_di.__pad1;

	switch (gfs1mode) {
	case GFS_FILE_DIR:
		return S_IFDIR;
	case GFS_FILE_REG:
		return S_IFREG;
	case GFS_FILE_LNK:
		return S_IFLNK;
	case GFS_FILE_BLK:
		return S_IFBLK;
	case GFS_FILE_CHR:
		return S_IFCHR;
	case GFS_FILE_FIFO:
		return S_IFIFO;
	case GFS_FILE_SOCK:
		return S_IFSOCK;
	default:
		/* This could be an aborted gfs2_convert so look for both. */
		if (ip->i_di.di_entries ||
		    (ip->i_di.di_mode & S_IFMT) == S_IFDIR)
			return S_IFDIR;
		else
			return S_IFREG;
	}
}

extern enum dup_ref_type get_ref_type(struct inode_with_dups *id);
extern struct gfs2_bmap *gfs2_bmap_create(struct gfs2_sbd *sdp, uint64_t size,
					  uint64_t *addl_mem_needed);
extern void *gfs2_bmap_destroy(struct gfs2_sbd *sdp, struct gfs2_bmap *il);
extern int gfs2_blockmap_set(struct gfs2_bmap *il, uint64_t block,
			     enum gfs2_mark_block mark);
extern int set_ip_blockmap(struct gfs2_inode *ip, int instree);
extern char generic_interrupt(const char *caller, const char *where,
                       const char *progress, const char *question,
                       const char *answers);
extern char gfs2_getch(void);
extern uint64_t find_free_blk(struct gfs2_sbd *sdp);
extern uint64_t *get_dir_hash(struct gfs2_inode *ip);
extern void delete_all_dups(struct gfs2_inode *ip);

#define stack log_debug("<backtrace> - %s()\n", __func__)

#endif /* __UTIL_H__ */
