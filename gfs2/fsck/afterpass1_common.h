#ifndef _AFTERPASS1_H
#define _AFTERPASS1_H

#include "util.h"
#include "metawalk.h"

extern int delete_metadata(struct iptr iptr, struct lgfs2_buffer_head **bh, int h, int *is_valid,
			   int *was_duplicate, void *private);
extern int delete_leaf(struct lgfs2_inode *ip, uint64_t block, void *private);
extern int delete_data(struct lgfs2_inode *ip, uint64_t metablock,
		       uint64_t block, void *private,
		       struct lgfs2_buffer_head *bh, __be64 *ptr);
extern int delete_eattr_indir(struct lgfs2_inode *ip, uint64_t block, uint64_t parent,
		       struct lgfs2_buffer_head **bh, void *private);
extern int delete_eattr_leaf(struct lgfs2_inode *ip, uint64_t block, uint64_t parent,
		      struct lgfs2_buffer_head **bh, void *private);
extern int delete_eattr_entry(struct lgfs2_inode *ip,
			      struct lgfs2_buffer_head *leaf_bh,
			      struct gfs2_ea_header *ea_hdr,
			      struct gfs2_ea_header *ea_hdr_prev,
			      void *private);
extern int delete_eattr_extentry(struct lgfs2_inode *ip, int i,
				 __be64 *ea_data_ptr,
				 struct lgfs2_buffer_head *leaf_bh,
				 uint32_t tot_ealen,
				 struct gfs2_ea_header *ea_hdr,
				 struct gfs2_ea_header *ea_hdr_prev,
				 void *private);
extern int remove_dentry_from_dir(struct lgfs2_sbd *sdp, uint64_t dir,
						   uint64_t dentryblock);
#endif
