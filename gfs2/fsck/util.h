#ifndef __UTIL_H__
#define __UTIL_H__

#include "libgfs2.h"

#define fsck_lseek(fd, off) \
  ((lseek((fd), (off), SEEK_SET) == (off)) ? 0 : -1)

struct di_info *search_list(osi_list_t *list, uint64_t addr);
void big_file_comfort(struct gfs2_inode *ip, uint64_t blks_checked);
void warm_fuzzy_stuff(uint64_t block);
const char *block_type_string(struct gfs2_block_query *q);
void gfs2_dup_set(uint64_t block);

#endif /* __UTIL_H__ */
