#ifndef _METAWALK_H
#define _METAWALK_H

#define DIR_LINEAR 1
#define DIR_EXHASH 2

#include "util.h"

struct metawalk_fxns;

extern int check_inode_eattr(struct fsck_cx *cx, struct lgfs2_inode *ip,
			     struct metawalk_fxns *pass);
extern int check_metatree(struct fsck_cx *cx, struct lgfs2_inode *ip, struct metawalk_fxns *pass);
extern int check_leaf_blks(struct fsck_cx *cx, struct lgfs2_inode *ip, struct metawalk_fxns *pass);
extern int check_dir(struct fsck_cx *cx, struct lgfs2_inode *ip,
		     struct metawalk_fxns *pass);
extern int check_linear_dir(struct fsck_cx *cx, struct lgfs2_inode *ip, struct lgfs2_buffer_head *bh,
			    struct metawalk_fxns *pass);
extern int check_leaf(struct fsck_cx *cx, struct lgfs2_inode *ip, int lindex,
		      struct metawalk_fxns *pass, uint64_t *leaf_no,
		      struct lgfs2_leaf *leaf, int *ref_count);
extern int _fsck_bitmap_set(struct fsck_cx *cx, struct lgfs2_inode *ip, uint64_t bblock,
			    const char *btype, int mark, int error_on_dinode,
			    const char *caller, int line);
extern int check_n_fix_bitmap(struct fsck_cx *cx, struct lgfs2_rgrp_tree *rgd,
			      uint64_t blk, int error_on_dinode,
			      int new_state);
extern struct duptree *dupfind(struct fsck_cx *cx, uint64_t block);
extern struct lgfs2_inode *fsck_system_inode(struct lgfs2_sbd *sdp,
					    uint64_t block);

#define is_duplicate(dblock) ((dupfind(dblock)) ? 1 : 0)

#define fsck_bitmap_set(cx, ip, b, bt, m) \
	_fsck_bitmap_set(cx, ip, b, bt, m, 0, __FUNCTION__, __LINE__)
#define fsck_bitmap_set_noino(cx, ip, b, bt, m) \
	_fsck_bitmap_set(cx, ip, b, bt, m, 1, __FUNCTION__, __LINE__)
enum meta_check_rc {
	META_ERROR = -1,
	META_IS_GOOD = 0,
	META_SKIP_FURTHER = 1,
	META_SKIP_ONE = 2,
};

struct iptr {
	struct lgfs2_inode *ipt_ip;
	struct lgfs2_buffer_head *ipt_bh;
	unsigned ipt_off;
};

#define iptr_ptr(i) ((__be64 *)(i.ipt_bh->b_data + i.ipt_off))
#define iptr_block(i) be64_to_cpu(*iptr_ptr(i))
#define iptr_endptr(i) ((__be64 *)(iptr.ipt_bh->b_data + i.ipt_ip->i_sbd->sd_bsize))
#define iptr_buf(i) (i.ipt_bh->b_data)

/* metawalk_fxns: function pointers to check various parts of the fs
 *
 * The functions should return -1 on fatal errors, 1 if the block
 * should be skipped, and 0 on success
 *
 * private: Data that should be passed to the fxns
 * check_leaf:
 * check_metalist:
 * check_data:
 * check_eattr_indir:
 * check_eattr_leaf:
 * check_dentry:
 * check_eattr_entry:
 * check_eattr_extentry:
 */
struct metawalk_fxns {
	void *private;
	int invalid_meta_is_fatal;
	int readahead;
	int (*check_leaf_depth) (struct fsck_cx *cx, struct lgfs2_inode *ip, uint64_t leaf_no,
				 int ref_count, struct lgfs2_buffer_head *lbh);
	int (*check_leaf) (struct fsck_cx *cx, struct lgfs2_inode *ip, uint64_t block,
			   void *private);
	/* parameters to the check_metalist sub-functions:
	   iptr: reference to the inode and its indirect pointer that we're analyzing
	   block: block number of the metadata block to be checked
	   bh: buffer_head to be returned
	   h: height
	   is_valid: returned as 1 if the metadata block is valid and should
	             be added to the metadata list for further processing.
	   was_duplicate: returns as 1 if the metadata block was determined
	             to be a duplicate reference, in which case we want to
		     skip adding it to the metadata list.
	   private: Pointer to pass-specific data
	   returns: 0 - everything is good, but there may be duplicates
	            1 - skip further processing
	*/
	int (*check_metalist) (struct fsck_cx *cx, struct iptr iptr,
			       struct lgfs2_buffer_head **bh, int h,
			       int *is_valid, int *was_duplicate,
			       void *private);
	int (*check_data) (struct fsck_cx *cx, struct lgfs2_inode *ip, uint64_t metablock,
			   uint64_t block, void *private,
			   struct lgfs2_buffer_head *bh, __be64 *ptr);
	int (*check_eattr_indir) (struct fsck_cx *, struct lgfs2_inode *ip, uint64_t block,
				  uint64_t parent,
				  struct lgfs2_buffer_head **bh, void *private);
	int (*check_eattr_leaf) (struct fsck_cx *cx, struct lgfs2_inode *ip, uint64_t block,
				 uint64_t parent, struct lgfs2_buffer_head **bh,
				 void *private);
	int (*check_dentry) (struct fsck_cx *cx, struct lgfs2_inode *ip, struct gfs2_dirent *de,
			     struct gfs2_dirent *prev,
			     struct lgfs2_buffer_head *bh,
			     char *filename, uint32_t *count,
			     int *lindex, void *private);
	int (*check_eattr_entry) (struct fsck_cx *cx, struct lgfs2_inode *ip,
				  struct lgfs2_buffer_head *leaf_bh,
				  struct gfs2_ea_header *ea_hdr,
				  struct gfs2_ea_header *ea_hdr_prev,
				  void *private);
	int (*check_eattr_extentry) (struct fsck_cx *cx, struct lgfs2_inode *ip, int i,
				     __be64 *ea_data_ptr,
				     struct lgfs2_buffer_head *leaf_bh,
				     uint32_t tot_ealen,
				     struct gfs2_ea_header *ea_hdr,
				     struct gfs2_ea_header *ea_hdr_prev,
				     void *private);
	int (*finish_eattr_indir) (struct fsck_cx *cx, struct lgfs2_inode *ip, int leaf_pointers,
				   int leaf_pointer_errors, void *private);
	void (*big_file_msg) (struct fsck_cx *cx, struct lgfs2_inode *ip, uint64_t blks_checked);
	int (*check_hash_tbl) (struct fsck_cx *cx, struct lgfs2_inode *ip, __be64 *tbl,
			       unsigned hsize, void *private);
	int (*repair_leaf) (struct fsck_cx *cx, struct lgfs2_inode *ip, uint64_t *leaf_no,
			    int lindex, int ref_count, const char *msg);
	int (*undo_check_meta) (struct fsck_cx *cx, struct lgfs2_inode *ip, uint64_t block,
				int h, void *private);
	int (*undo_check_data) (struct fsck_cx *cx, struct lgfs2_inode *ip, uint64_t block,
				void *private);
	int (*delete_block) (struct fsck_cx *cx, struct lgfs2_inode *ip, uint64_t block,
			     struct lgfs2_buffer_head **bh, const char *btype,
			     void *private);
};

#endif /* _METAWALK_H */
