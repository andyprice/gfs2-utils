#ifndef __UTIL_H__
#define __UTIL_H__

#include "fsck.h"
#include "libgfs2.h"

#define fsck_lseek(fd, off) \
  ((lseek((fd), (off), SEEK_SET) == (off)) ? 0 : -1)

struct di_info *search_list(osi_list_t *list, uint64_t addr);
void big_file_comfort(struct gfs2_inode *ip, uint64_t blks_checked);
void warm_fuzzy_stuff(uint64_t block);
const char *block_type_string(uint8_t q);
void gfs2_dup_set(uint64_t block);

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
