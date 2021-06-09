#ifndef _LINK_H
#define _LINK_H

extern struct gfs2_bmap nlink1map; /* map of dinodes with nlink == 1 */
extern struct gfs2_bmap clink1map; /* map of dinodes w/counted links == 1 */

enum {
	INCR_LINK_BAD = -1,
	INCR_LINK_GOOD = 0,
	INCR_LINK_INO_MISMATCH = 1,
	INCR_LINK_CHECK_ORIG = 2,
};

int link1_set(struct gfs2_bmap *bmap, uint64_t bblock, int mark);
int set_di_nlink(struct gfs2_inode *ip);
int incr_link_count(struct lgfs2_inum no, struct gfs2_inode *ip, const char *why);
int decr_link_count(uint64_t inode_no, uint64_t referenced_from, int gfs1,
		    const char *why);

#endif /* _LINK_H */
