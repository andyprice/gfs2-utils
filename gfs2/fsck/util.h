#ifndef __UTIL_H__
#define __UTIL_H__

#include <sys/stat.h>

#include "fsck.h"
#include "libgfs2.h"

#define INODE_VALID 1
#define INODE_INVALID 0

struct di_info *search_list(osi_list_t *list, uint64_t addr);
void big_file_comfort(struct fsck_cx *cx, struct lgfs2_inode *ip, uint64_t blks_checked);
void display_progress(uint64_t block);
int add_duplicate_ref(struct fsck_cx *cx, struct lgfs2_inode *ip, uint64_t block,
		      enum dup_ref_type reftype, int first, int inode_valid);
extern struct inode_with_dups *find_dup_ref_inode(struct duptree *dt,
						  struct lgfs2_inode *ip);
extern void dup_listent_delete(struct duptree *dt, struct inode_with_dups *id);
extern int count_dup_meta_refs(struct duptree *dt);
extern const char *reftypes[REF_TYPES + 1];

#define BLOCKMAP_SIZE1(size) ((size) >> 3)
#define BLOCKMAP_SIZE2(size) ((size) >> 2)
#define BLOCKMAP_BYTE_OFFSET2(x) ((x & 0x0000000000000003) << 1)
#define BLOCKMAP_BYTE_OFFSET1(x) (x & 0x0000000000000007)
#define BLOCKMAP_MASK2 (0x3)
#define BLOCKMAP_MASK1 (1)

struct fsck_pass {
	const char *name;
	int (*f)(struct fsck_cx *cx);
};

static inline int block_type(struct bmap *bl, uint64_t bblock)
{
	static unsigned char *byte;
	static uint64_t b;
	static int btype;

	byte = bl->map + BLOCKMAP_SIZE2(bblock);
	b = BLOCKMAP_BYTE_OFFSET2(bblock);
	btype = (*byte & (BLOCKMAP_MASK2 << b )) >> b;
	return btype;
}

static inline int link1_type(struct bmap *bl, uint64_t bblock)
{
	static unsigned char *byte;
	static uint64_t b;
	static int btype;

	byte = bl->map + BLOCKMAP_SIZE1(bblock);
	b = BLOCKMAP_BYTE_OFFSET1(bblock);
	btype = (*byte & (BLOCKMAP_MASK1 << b )) >> b;
	return btype;
}

static inline void link1_destroy(struct bmap *bmap)
{
	if (bmap->map)
		free(bmap->map);
	bmap->size = 0;
	bmap->mapsize = 0;
}

static inline int bitmap_type(struct lgfs2_sbd *sdp, uint64_t bblock)
{
	struct lgfs2_rgrp_tree *rgd;

	rgd = lgfs2_blk2rgrpd(sdp, bblock);
	return lgfs2_get_bitmap(sdp, bblock, rgd);
}

static const inline char *block_type_string(int q)
{
	const char *blktyp[] = {"free", "data", "other", "inode", "invalid"};
	if (q >= GFS2_BLKST_FREE && q <= GFS2_BLKST_DINODE)
		return (blktyp[q]);
	return blktyp[4];
}

static inline int is_dir(struct lgfs2_inode *ip, int gfs1)
{
	if (gfs1 && lgfs2_is_gfs_dir(ip))
		return 1;
	if (S_ISDIR(ip->i_mode))
		return 1;

	return 0;
}

static inline uint32_t gfs_to_gfs2_mode(struct lgfs2_inode *ip)
{
	uint16_t gfs1mode = ip->i_di_type;

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
		if (ip->i_entries ||
		    (ip->i_mode & S_IFMT) == S_IFDIR)
			return S_IFDIR;
		else
			return S_IFREG;
	}
}

extern enum dup_ref_type get_ref_type(struct inode_with_dups *id);
extern char generic_interrupt(const char *caller, const char *where,
                       const char *progress, const char *question,
                       const char *answers);
extern char fsck_getch(void);
extern uint64_t find_free_blk(struct lgfs2_sbd *sdp);
extern __be64 *get_dir_hash(struct lgfs2_inode *ip);
extern void delete_all_dups(struct fsck_cx *cx, struct lgfs2_inode *ip);
extern void print_pass_duration(const char *name, struct timeval *start);

#define stack log_debug("<backtrace> - %s()\n", __func__)

#endif /* __UTIL_H__ */
