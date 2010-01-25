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

#endif /* __UTIL_H__ */
