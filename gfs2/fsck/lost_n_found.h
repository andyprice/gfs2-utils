#ifndef __LOST_N_FOUND_H__
#define __LOST_N_FOUND_H__

#include "libgfs2.h"

int add_inode_to_lf(struct fsck_cx *cx, struct lgfs2_inode *ip);
void make_sure_lf_exists(struct fsck_cx *cx, struct lgfs2_inode *ip);

#endif /* __LOST_N_FOUND_H__ */
