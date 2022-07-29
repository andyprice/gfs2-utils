#ifndef _INODE_HASH_H
#define _INODE_HASH_H

#include "fsck.h"

struct inode_info;

extern struct inode_info *inodetree_find(struct fsck_cx *cx, uint64_t block);
extern struct inode_info *inodetree_insert(struct fsck_cx *cx, struct lgfs2_inum no);
extern void inodetree_delete(struct fsck_cx *cx, struct inode_info *b);

#endif /* _INODE_HASH_H */
