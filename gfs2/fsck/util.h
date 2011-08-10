#ifndef __UTIL_H__
#define __UTIL_H__

#include "fsck.h"
#include "libgfs2.h"

#define fsck_lseek(fd, off) \
  ((lseek((fd), (off), SEEK_SET) == (off)) ? 0 : -1)

#define INODE_VALID 1
#define INODE_INVALID 0

struct di_info *search_list(osi_list_t *list, uint64_t addr);
void big_file_comfort(struct gfs2_inode *ip, uint64_t blks_checked);
void warm_fuzzy_stuff(uint64_t block);
int add_duplicate_ref(struct gfs2_inode *ip, uint64_t block,
		      enum dup_ref_type reftype, int first, int inode_valid);
extern struct inode_with_dups *find_dup_ref_inode(struct duptree *dt,
						  struct gfs2_inode *ip);
extern void dup_listent_delete(struct inode_with_dups *id);

extern const char *reftypes[3];

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
	gfs2_inode_dir     = (0x3),
	gfs2_inode_file    = (0x4),

	gfs2_inode_lnk     = (0x5),
	gfs2_inode_blk     = (0x6),
	gfs2_inode_chr     = (0x7),
	gfs2_inode_fifo    = (0x8),
	gfs2_inode_sock    = (0x9),

	gfs2_inode_invalid = (0xa),
	gfs2_meta_inval    = (0xb),
	gfs2_leaf_blk      = (0xc),
	gfs2_meta_rgrp     = (0xd),
	gfs2_meta_eattr    = (0xe),

	gfs2_bad_block     = (0xf), /* Contains at least one bad block */
};

static const inline char *block_type_string(uint8_t q)
{
	const char *blktyp[] = {
		"free",
		"data",
		"indirect data",
		"directory",
		"file",

		"symlink",
		"block device",
		"char device",
		"fifo",
		"socket",

		"invalid inode",
		"invalid meta",
		"dir leaf",
		"rgrp meta",
		"eattribute",

		"bad"};
	if (q < 16)
		return (blktyp[q]);
	return blktyp[15];
}

/* Must be kept in sync with gfs2_mark_block enum above. Blocks marked as
   invalid or bad are considered metadata until actually freed. */
static inline int blockmap_to_bitmap(enum gfs2_mark_block m)
{
	static int bitmap_states[16] = {
		GFS2_BLKST_FREE,
		GFS2_BLKST_USED,
		GFS2_BLKST_USED,
		GFS2_BLKST_DINODE,
		GFS2_BLKST_DINODE,

		GFS2_BLKST_DINODE,
		GFS2_BLKST_DINODE,
		GFS2_BLKST_DINODE,
		GFS2_BLKST_DINODE,
		GFS2_BLKST_DINODE,

		GFS2_BLKST_FREE,
		GFS2_BLKST_FREE,
		GFS2_BLKST_USED,
		GFS2_BLKST_USED,
		GFS2_BLKST_USED,

		GFS2_BLKST_USED
	};
	return bitmap_states[m];
}

extern struct gfs2_bmap *gfs2_bmap_create(struct gfs2_sbd *sdp, uint64_t size,
					  uint64_t *addl_mem_needed);
extern void *gfs2_bmap_destroy(struct gfs2_sbd *sdp, struct gfs2_bmap *il);
extern int gfs2_blockmap_set(struct gfs2_bmap *il, uint64_t block,
			     enum gfs2_mark_block mark);

#endif /* __UTIL_H__ */
