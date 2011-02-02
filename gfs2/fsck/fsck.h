#ifndef _FSCK_H
#define _FSCK_H

#include "libgfs2.h"
#include "osi_tree.h"

#define FSCK_HASH_SHIFT         (13)
#define FSCK_HASH_SIZE          (1 << FSCK_HASH_SHIFT)
#define FSCK_HASH_MASK          (FSCK_HASH_SIZE - 1)

#define query(fmt, args...) fsck_query(fmt, ##args)

/*
 * Exit codes used by fsck-type programs
 * Copied from e2fsck's e2fsck.h
 */
#define FSCK_OK          0      /* No errors */
#define FSCK_NONDESTRUCT 1      /* File system errors corrected */
#define FSCK_REBOOT      2      /* System should be rebooted */
#define FSCK_UNCORRECTED 4      /* File system errors left uncorrected */
#define FSCK_ERROR       8      /* Operational error */
#define FSCK_USAGE       16     /* Usage or syntax error */
#define FSCK_CANCELED    32     /* Aborted with a signal or ^C */
#define FSCK_LIBRARY     128    /* Shared library error */

#define BAD_POINTER_TOLERANCE 10 /* How many bad pointers is too many? */

struct inode_info
{
        struct osi_node node;
        uint64_t   inode;
        uint16_t   link_count;   /* the number of links the inode
                                  * thinks it has */
        uint16_t   counted_links; /* the number of links we've found */
};

struct dir_info
{
        struct osi_node node;
        uint64_t dinode;
        uint64_t treewalk_parent;
        uint64_t dotdot_parent;
        uint8_t  checked:1;

};

struct dir_status {
	uint8_t dotdir:1;
	uint8_t dotdotdir:1;
	uint8_t q;
	uint32_t entry_count;
};

struct duptree {
	struct osi_node node;
	int first_ref_found; /* Has the original reference been found? */
	int refs;
	uint64_t block;
	osi_list_t ref_inode_list; /* list of inodes referencing a dup block */
	osi_list_t ref_invinode_list; /* list of invalid inodes referencing */
};

enum dup_ref_type {
	ref_as_data = 0,
	ref_as_meta = 1,
	ref_as_ea   = 2,
	ref_types   = 3
};

struct inode_with_dups {
	osi_list_t list;
	uint64_t block_no;
	int dup_count;
	int reftypecount[ref_types];
	uint64_t parent;
	char *name;
};

enum rgindex_trust_level { /* how far can we trust our RG index? */
	blind_faith = 0, /* We'd like to trust the rgindex. We always used to
			    before bz 179069. This should cover most cases. */
	ye_of_little_faith = 1, /* The rindex seems trustworthy but there's
				   rg damage that need to be fixed. */
	open_minded = 2, /* At least 1 RG is corrupt. Try to calculate what it
			    should be, in a perfect world where our RGs are all
			    on even boundaries. Blue sky. Chirping birds. */
	distrust = 3,  /* The world isn't perfect, our RGs are not on nice neat
			  boundaries.  The fs must have been messed with by
			  gfs2_grow or something.  Count the RGs by hand. */
	indignation = 4 /* Not only do we have corruption, but the rgrps
			   aren't on even boundaries, so this file system
			   must have been converted from gfs2_convert. */
};

extern struct gfs2_inode *fsck_load_inode(struct gfs2_sbd *sbp, uint64_t block);
extern struct gfs2_inode *fsck_inode_get(struct gfs2_sbd *sdp,
				  struct gfs2_buffer_head *bh);
extern void fsck_inode_put(struct gfs2_inode **ip);

extern int initialize(struct gfs2_sbd *sbp, int force_check, int preen,
		      int *all_clean);
extern void destroy(struct gfs2_sbd *sbp);
extern int pass1(struct gfs2_sbd *sbp);
extern int pass1b(struct gfs2_sbd *sbp);
extern int pass1c(struct gfs2_sbd *sbp);
extern int pass2(struct gfs2_sbd *sbp);
extern int pass3(struct gfs2_sbd *sbp);
extern int pass4(struct gfs2_sbd *sbp);
extern int pass5(struct gfs2_sbd *sbp);
extern int rg_repair(struct gfs2_sbd *sdp, int trust_lvl, int *rg_count,
		     int *sane);
extern void gfs2_dup_free(void);
extern int fsck_query(const char *format, ...)
	__attribute__((format(printf,1,2)));
extern struct dir_info *dirtree_find(uint64_t block);
extern void dup_delete(struct duptree *b);
extern void dirtree_delete(struct dir_info *b);

/* FIXME: Hack to get this going for pass2 - this should be pulled out
 * of pass1 and put somewhere else... */
struct dir_info *dirtree_insert(uint64_t dblock);

extern struct gfs2_options opts;
extern struct gfs2_inode *lf_dip; /* Lost and found directory inode */
extern struct gfs2_bmap *bl;
extern uint64_t last_fs_block, last_reported_block;
extern int64_t last_reported_fblock;
extern int skip_this_pass, fsck_abort;
extern int errors_found, errors_corrected;
extern uint64_t last_data_block;
extern uint64_t first_data_block;
extern struct osi_root dup_blocks;
extern struct osi_root dirtree;
extern struct osi_root inodetree;
extern int dups_found; /* How many duplicate references have we found? */
extern int dups_found_first; /* How many duplicates have we found the original
				reference for? */
#endif /* _FSCK_H */
