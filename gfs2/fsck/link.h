#ifndef _LINK_H
#define _LINK_H

int set_link_count(uint64_t inode_no, uint32_t count);
int increment_link(uint64_t inode_no, uint64_t referenced_from,
		   const char *why);
int decrement_link(uint64_t inode_no, uint64_t referenced_from,
		   const char *why);

#endif /* _LINK_H */
