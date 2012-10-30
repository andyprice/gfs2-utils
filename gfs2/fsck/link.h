#ifndef _LINK_H
#define _LINK_H

int set_di_nlink(struct gfs2_inode *ip);
int incr_link_count(struct gfs2_inum no, struct gfs2_inode *ip,
		    const char *why);
int decr_link_count(uint64_t inode_no, uint64_t referenced_from,
		    const char *why);

#endif /* _LINK_H */
