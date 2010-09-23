#ifndef _METAWALK_H
#define _METAWALK_H

#define DIR_LINEAR 1
#define DIR_EXHASH 2

struct metawalk_fxns;

extern int check_inode_eattr(struct gfs2_inode *ip,
			     struct metawalk_fxns *pass);
extern int check_metatree(struct gfs2_inode *ip, struct metawalk_fxns *pass);
extern int check_dir(struct gfs2_sbd *sbp, uint64_t block,
		     struct metawalk_fxns *pass);
extern int check_linear_dir(struct gfs2_inode *ip, struct gfs2_buffer_head *bh,
			    struct metawalk_fxns *pass);
extern int remove_dentry_from_dir(struct gfs2_sbd *sbp, uint64_t dir,
						   uint64_t dentryblock);
extern int delete_block(struct gfs2_inode *ip, uint64_t block,
		 struct gfs2_buffer_head **bh, const char *btype,
		 void *private);
extern int delete_metadata(struct gfs2_inode *ip, uint64_t block,
			   struct gfs2_buffer_head **bh, int h, void *private);
extern int delete_leaf(struct gfs2_inode *ip, uint64_t block,
		struct gfs2_buffer_head *bh, void *private);
extern int delete_data(struct gfs2_inode *ip, uint64_t block, void *private);
extern int delete_eattr_indir(struct gfs2_inode *ip, uint64_t block, uint64_t parent,
		       struct gfs2_buffer_head **bh, void *private);
extern int delete_eattr_leaf(struct gfs2_inode *ip, uint64_t block, uint64_t parent,
		      struct gfs2_buffer_head **bh, void *private);
extern int _fsck_blockmap_set(struct gfs2_inode *ip, uint64_t bblock,
		       const char *btype, enum gfs2_mark_block mark,
		       const char *caller, int line);
extern int check_n_fix_bitmap(struct gfs2_sbd *sdp, uint64_t blk,
		       enum gfs2_mark_block new_blockmap_state);
extern void reprocess_inode(struct gfs2_inode *ip, const char *desc);
extern struct duptree *dupfind(uint64_t block);
extern struct gfs2_inode *fsck_system_inode(struct gfs2_sbd *sdp,
					    uint64_t block);

#define is_duplicate(dblock) ((dupfind(dblock)) ? 1 : 0)

#define fsck_blockmap_set(ip, b, bt, m) _fsck_blockmap_set(ip, b, bt, m, \
							   __FILE__, __LINE__)

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
	int (*check_leaf) (struct gfs2_inode *ip, uint64_t block,
			   struct gfs2_buffer_head *bh, void *private);
	int (*check_metalist) (struct gfs2_inode *ip, uint64_t block,
			       struct gfs2_buffer_head **bh, int h,
			       void *private);
	int (*check_data) (struct gfs2_inode *ip, uint64_t block,
			   void *private);
	int (*check_eattr_indir) (struct gfs2_inode *ip, uint64_t block,
				  uint64_t parent,
				  struct gfs2_buffer_head **bh, void *private);
	int (*check_eattr_leaf) (struct gfs2_inode *ip, uint64_t block,
				 uint64_t parent, struct gfs2_buffer_head **bh,
				 void *private);
	int (*check_dentry) (struct gfs2_inode *ip, struct gfs2_dirent *de,
			     struct gfs2_dirent *prev,
			     struct gfs2_buffer_head *bh,
			     char *filename, uint16_t *count, void *private);
	int (*check_eattr_entry) (struct gfs2_inode *ip,
				  struct gfs2_buffer_head *leaf_bh,
				  struct gfs2_ea_header *ea_hdr,
				  struct gfs2_ea_header *ea_hdr_prev,
				  void *private);
	int (*check_eattr_extentry) (struct gfs2_inode *ip,
				     uint64_t *ea_data_ptr,
				     struct gfs2_buffer_head *leaf_bh,
				     struct gfs2_ea_header *ea_hdr,
				     struct gfs2_ea_header *ea_hdr_prev,
				     void *private);
	int (*finish_eattr_indir) (struct gfs2_inode *ip, int leaf_pointers,
				   int leaf_pointer_errors, void *private);
	void (*big_file_msg) (struct gfs2_inode *ip, uint64_t blks_checked);
};

#endif /* _METAWALK_H */
