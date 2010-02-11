#ifndef __LIBGFS2_DOT_H__
#define __LIBGFS2_DOT_H__

#include <features.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <linux/types.h>
#include <linux/limits.h>
#include <endian.h>
#include <byteswap.h>

#include <linux/gfs2_ondisk.h>
#include "osi_list.h"

__BEGIN_DECLS

#ifndef TRUE
#define TRUE (1)
#endif

#ifndef FALSE
#define FALSE (0)
#endif

#if __BYTE_ORDER == __BIG_ENDIAN

#define be16_to_cpu(x) (x)
#define be32_to_cpu(x) (x)
#define be64_to_cpu(x) (x)

#define cpu_to_be16(x) (x)
#define cpu_to_be32(x) (x)
#define cpu_to_be64(x) (x)

#define le16_to_cpu(x) (bswap_16((x)))
#define le32_to_cpu(x) (bswap_32((x)))
#define le64_to_cpu(x) (bswap_64((x)))

#define cpu_to_le16(x) (bswap_16((x)))
#define cpu_to_le32(x) (bswap_32((x)))
#define cpu_to_le64(x) (bswap_64((x)))

#endif  /*  __BYTE_ORDER == __BIG_ENDIAN  */


#if __BYTE_ORDER == __LITTLE_ENDIAN

#define be16_to_cpu(x) (bswap_16((x)))
#define be32_to_cpu(x) (bswap_32((x)))
#define be64_to_cpu(x) (bswap_64((x)))

#define cpu_to_be16(x) (bswap_16((x)))
#define cpu_to_be32(x) (bswap_32((x)))
#define cpu_to_be64(x) (bswap_64((x))) 

#define le16_to_cpu(x) (x)
#define le32_to_cpu(x) (x)
#define le64_to_cpu(x) (x)

#define cpu_to_le16(x) (x)
#define cpu_to_le32(x) (x)
#define cpu_to_le64(x) (x)

#endif  /*  __BYTE_ORDER == __LITTLE_ENDIAN  */

#define BLOCKMAP_SIZE4(size) (size >> 1)
#define BLOCKMAP_BYTE_OFFSET4(x) ((x & 0x0000000000000001) << 2)
#define BLOCKMAP_MASK4 (0xf)

static __inline__ __attribute__((noreturn, format (printf, 1, 2)))
void die(const char *fmt, ...)
{
	va_list ap;
	fprintf(stderr, "%s: ", __FILE__);
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	exit(-1);
}

struct device {
	uint64_t start;
	uint64_t length;
	uint32_t rgf_flags;
};

struct gfs2_bitmap
{
	uint32_t   bi_offset;  /* The offset in the buffer of the first byte */
	uint32_t   bi_start;   /* The position of the first byte in this block */
	uint32_t   bi_len;     /* The number of bytes in this block */
};

struct rgrp_list {
	osi_list_t list;
	uint64_t start;	   /* The offset of the beginning of this resource group */
	uint64_t length;	/* The length of this resource group */

	struct gfs2_rindex ri;
	struct gfs2_rgrp rg;
	struct gfs2_bitmap *bits;
	struct gfs2_buffer_head **bh;
};

struct gfs2_buffer_head {
	osi_list_t b_altlist; /* alternate list */
	uint64_t b_blocknr;
	int b_modified;
	char *b_data;
	struct gfs2_sbd *sdp;
};

struct special_blocks {
	osi_list_t list;
	uint64_t block;
};

struct gfs2_sbd;
struct gfs2_inode {
	int bh_owned; /* Is this bh owned, iow, should we release it later? */
	struct gfs2_dinode i_di;
	struct gfs2_buffer_head *i_bh;
	struct gfs2_sbd *i_sbd;
};

#define BUF_HASH_SHIFT       (13)    /* # hash buckets = 8K */
#define BUF_HASH_SIZE        (1 << BUF_HASH_SHIFT)
#define BUF_HASH_MASK        (BUF_HASH_SIZE - 1)

/* FIXME not sure that i want to keep a record of the inodes or the
 * contents of them, or both ... if I need to write back to them, it
 * would be easier to hold the inode as well  */
struct per_node
{
	struct gfs2_inode *inum;
	struct gfs2_inum_range inum_range;
	struct gfs2_inode *statfs;
	struct gfs2_statfs_change statfs_change;
	struct gfs2_inode *unlinked;
	struct gfs2_inode *quota;
	struct gfs2_quota_change quota_change;
};

struct master_dir
{
	struct gfs2_inode *inum;
	uint64_t next_inum;
	struct gfs2_inode *statfs;
	struct gfs2_statfs_change statfs_change;

	struct gfs2_rindex rindex;
	struct gfs2_inode *qinode;
	struct gfs2_quota quotas;

	struct gfs2_inode       *jiinode;
	struct gfs2_inode       *riinode;
	struct gfs2_inode       *rooti;
	struct gfs2_inode       *pinode;
	
	struct gfs2_inode **journal;      /* Array of journals */
	uint32_t journals;                /* Journal count */
	struct per_node *pn;              /* Array of per_node entries */
};

struct gfs2_sbd {
	struct gfs2_sb sd_sb;    /* a copy of the ondisk structure */
	char lockproto[GFS2_LOCKNAME_LEN];
	char locktable[GFS2_LOCKNAME_LEN];

	unsigned int bsize;	     /* The block size of the FS (in bytes) */
	unsigned int jsize;	     /* Size of journals (in MB) */
	unsigned int rgsize;     /* Size of resource groups (in MB) */
	unsigned int utsize;     /* Size of unlinked tag files (in MB) */
	unsigned int qcsize;     /* Size of quota change files (in MB) */

	int debug;
	int quiet;
	int expert;
	int override;

	char device_name[PATH_MAX];
	char *path_name;

	/* Constants */

	uint32_t sd_fsb2bb;
	uint32_t sd_fsb2bb_shift;
	uint32_t sd_diptrs;
	uint32_t sd_inptrs;
	uint32_t sd_jbsize;
	uint32_t sd_hash_bsize;
	uint32_t sd_hash_bsize_shift;
	uint32_t sd_hash_ptrs;
	uint32_t sd_max_dirres;
	uint32_t sd_max_height;
	uint64_t sd_heightsize[GFS2_MAX_META_HEIGHT];
	uint32_t sd_max_jheight;
	uint64_t sd_jheightsize[GFS2_MAX_META_HEIGHT];

	/* Not specified on the command line, but... */

	int64_t time;

	struct device device;
	uint64_t device_size;

	int device_fd;
	int path_fd;

	uint64_t sb_addr;

	uint64_t orig_fssize;
	uint64_t fssize;
	uint64_t blks_total;
	uint64_t blks_alloced;
	uint64_t dinodes_alloced;

	uint64_t orig_rgrps;
	uint64_t rgrps;
	uint64_t new_rgrps;
	osi_list_t rglist;

	unsigned int orig_journals;

	struct gfs2_inode *master_dir;
	struct master_dir md;

	unsigned int writes;
	int metafs_fd;
	char metafs_path[PATH_MAX]; /* where metafs is mounted */
	struct special_blocks eattr_blocks;
};

struct metapath {
	unsigned int mp_list[GFS2_MAX_META_HEIGHT];
};


#define GFS2_DEFAULT_BSIZE          (4096)
#define GFS2_DEFAULT_JSIZE          (128)
#define GFS2_DEFAULT_RGSIZE         (256)
#define GFS2_DEFAULT_UTSIZE         (1)
#define GFS2_DEFAULT_QCSIZE         (1)
#define GFS2_DEFAULT_LOCKPROTO      "lock_dlm"
#define GFS2_MIN_GROW_SIZE          (10)
#define GFS2_EXCESSIVE_RGS          (10000)

#define DATA (1)
#define META (2)
#define DINODE (3)

/* bitmap.c */
struct gfs2_bmap {
	uint64_t size;
	uint64_t mapsize;
	unsigned char *map;
};

/* block_list.c */

enum gfs2_mark_block {
	gfs2_block_free    = (0x0),
	gfs2_block_used    = (0x1),
	gfs2_indir_blk     = (0x2),
	gfs2_inode_dir     = (0x3),
	gfs2_inode_file    = (0x4),

	gfs2_inode_lnk     = (0x5),
	gfs2_inode_blk     = (0x6),
	gfs2_inode_chr     = (0x7),
	gfs2_inode_fifo    = (0x8),
	gfs2_inode_sock    = (0x9),

	gfs2_inode_invalid = (0xa),
	gfs2_meta_inval    = (0xb),
	gfs2_leaf_blk      = (0xc),
	gfs2_meta_rgrp     = (0xd),
	gfs2_meta_eattr    = (0xe),

	gfs2_bad_block     = (0xf), /* Contains at least one bad block */
};

static const inline char *block_type_string(uint8_t q)
{
	const char *blktyp[] = {
		"free",
		"data",
		"indirect data",
		"directory",
		"file",

		"symlink",
		"block device",
		"char device",
		"fifo",
		"socket",

		"invalid inode",
		"invalid meta",
		"dir leaf",
		"rgrp meta",
		"eattribute",

		"bad"};
	if (q < 16)
		return (blktyp[q]);
	return blktyp[15];
}

/* Must be kept in sync with gfs2_mark_block enum above. Blocks marked as
   invalid or bad are considered metadata until actually freed. */
static inline int blockmap_to_bitmap(enum gfs2_mark_block m)
{
	static int bitmap_states[16] = {
		GFS2_BLKST_FREE,
		GFS2_BLKST_USED,
		GFS2_BLKST_USED,
		GFS2_BLKST_DINODE,
		GFS2_BLKST_DINODE,

		GFS2_BLKST_DINODE,
		GFS2_BLKST_DINODE,
		GFS2_BLKST_DINODE,
		GFS2_BLKST_DINODE,
		GFS2_BLKST_DINODE,

		GFS2_BLKST_FREE,
		GFS2_BLKST_FREE,
		GFS2_BLKST_USED,
		GFS2_BLKST_USED,
		GFS2_BLKST_USED,

		GFS2_BLKST_USED
	};
	return bitmap_states[m];
}

extern struct gfs2_bmap *gfs2_bmap_create(struct gfs2_sbd *sdp, uint64_t size,
					  uint64_t *addl_mem_needed);
extern struct special_blocks *blockfind(struct special_blocks *blist, uint64_t num);
extern void gfs2_special_add(struct special_blocks *blocklist, uint64_t block);
extern void gfs2_special_set(struct special_blocks *blocklist, uint64_t block);
extern void gfs2_special_free(struct special_blocks *blist);
extern int gfs2_blockmap_set(struct gfs2_bmap *il, uint64_t block,
			     enum gfs2_mark_block mark);
extern void gfs2_special_clear(struct special_blocks *blocklist,
			       uint64_t block);
extern void *gfs2_bmap_destroy(struct gfs2_sbd *sdp, struct gfs2_bmap *il);

/* buf.c */
extern struct gfs2_buffer_head *__bget_generic(struct gfs2_sbd *sdp,
					       uint64_t num,
					       int read_disk, int line,
					       const char *caller);
extern struct gfs2_buffer_head *__bget(struct gfs2_sbd *sdp, uint64_t num,
				       int line, const char *caller);
extern struct gfs2_buffer_head *__bread(struct gfs2_sbd *sdp, uint64_t num,
					int line, const char *caller);
extern int bwrite(struct gfs2_buffer_head *bh);
extern int brelse(struct gfs2_buffer_head *bh);

#define bmodified(bh) do { bh->b_modified = 1; } while(0)

#define bget_generic(bl, num, find, read) __bget_generic(bl, num, find, read, \
							 __LINE__, \
							 __FUNCTION__)
#define bget(bl, num) __bget(bl, num, __LINE__, __FUNCTION__)
#define bread(bl, num) __bread(bl, num, __LINE__, __FUNCTION__)
#define bsync(bl) do { __bsync(bl, __LINE__, __FUNCTION__); } while(0)
#define bcommit(bl) do { __bcommit(bl, __LINE__, __FUNCTION__); } while(0)

/* device_geometry.c */
extern int device_geometry(struct gfs2_sbd *sdp);
extern int fix_device_geometry(struct gfs2_sbd *sdp);

/* fs_bits.c */
#define BFITNOENT (0xFFFFFFFF)

/* functions with blk #'s that are buffer relative */
extern uint32_t gfs2_bitcount(unsigned char *buffer, unsigned int buflen,
			      unsigned char state);
extern uint32_t gfs2_bitfit(unsigned char *buffer, unsigned int buflen,
			    uint32_t goal, unsigned char old_state);

/* functions with blk #'s that are rgrp relative */
extern uint32_t gfs2_blkalloc_internal(struct rgrp_list *rgd, uint32_t goal,
				       unsigned char old_state,
				       unsigned char new_state, int do_it);
extern int gfs2_check_range(struct gfs2_sbd *sdp, uint64_t blkno);

/* functions with blk #'s that are file system relative */
extern int gfs2_get_bitmap(struct gfs2_sbd *sdp, uint64_t blkno,
			   struct rgrp_list *rgd);
extern int gfs2_set_bitmap(struct gfs2_sbd *sdp, uint64_t blkno, int state);

/* fs_geometry.c */
extern void rgblocks2bitblocks(unsigned int bsize, uint32_t *rgblocks,
			       uint32_t *bitblocks);
extern void compute_rgrp_layout(struct gfs2_sbd *sdp, int rgsize_specified);
extern void build_rgrps(struct gfs2_sbd *sdp, int write);

/* fs_ops.c */
#define IS_LEAF     (1)
#define IS_DINODE   (2)

extern struct metapath *find_metapath(struct gfs2_inode *ip, uint64_t block);
extern void lookup_block(struct gfs2_inode *ip, struct gfs2_buffer_head *bh,
			 unsigned int height, struct metapath *mp,
			 int create, int *new, uint64_t *block);
extern struct gfs2_inode *inode_get(struct gfs2_sbd *sdp,
				    struct gfs2_buffer_head *bh);
extern struct gfs2_inode *inode_read(struct gfs2_sbd *sdp, uint64_t di_addr);
extern struct gfs2_inode *is_system_inode(struct gfs2_sbd *sdp,
					  uint64_t block);
extern void inode_put(struct gfs2_inode **ip);
extern uint64_t data_alloc(struct gfs2_inode *ip);
extern uint64_t meta_alloc(struct gfs2_inode *ip);
extern uint64_t dinode_alloc(struct gfs2_sbd *sdp);
extern int gfs2_readi(struct gfs2_inode *ip, void *buf, uint64_t offset,
		      unsigned int size);
extern int gfs2_writei(struct gfs2_inode *ip, void *buf, uint64_t offset,
		       unsigned int size);
extern struct gfs2_buffer_head *get_file_buf(struct gfs2_inode *ip,
					     uint64_t lbn, int prealloc);
extern struct gfs2_buffer_head *init_dinode(struct gfs2_sbd *sdp,
					    struct gfs2_inum *inum,
					    unsigned int mode, uint32_t flags,
					    struct gfs2_inum *parent);
extern struct gfs2_inode *createi(struct gfs2_inode *dip, const char *filename,
				  unsigned int mode, uint32_t flags);
extern void dirent2_del(struct gfs2_inode *dip, struct gfs2_buffer_head *bh,
			struct gfs2_dirent *prev, struct gfs2_dirent *cur);
extern int gfs2_lookupi(struct gfs2_inode *dip, const char *filename, int len,
			struct gfs2_inode **ipp);
extern void dir_add(struct gfs2_inode *dip, const char *filename, int len,
		    struct gfs2_inum *inum, unsigned int type);
extern int gfs2_dirent_del(struct gfs2_inode *dip, const char *filename,
			   int filename_len);
extern void block_map(struct gfs2_inode *ip, uint64_t lblock, int *new,
		      uint64_t *dblock, uint32_t *extlen, int prealloc);
extern void gfs2_get_leaf_nr(struct gfs2_inode *dip, uint32_t index,
			     uint64_t *leaf_out);
extern void gfs2_put_leaf_nr(struct gfs2_inode *dip, uint32_t inx, uint64_t leaf_out);
extern void gfs2_free_block(struct gfs2_sbd *sdp, uint64_t block);
extern int gfs2_freedi(struct gfs2_sbd *sdp, uint64_t block);
extern int gfs2_get_leaf(struct gfs2_inode *dip, uint64_t leaf_no,
			 struct gfs2_buffer_head **bhp);
extern int gfs2_dirent_first(struct gfs2_inode *dip,
			     struct gfs2_buffer_head *bh,
			     struct gfs2_dirent **dent);
extern int gfs2_dirent_next(struct gfs2_inode *dip, struct gfs2_buffer_head *bh,
			    struct gfs2_dirent **dent);
extern void build_height(struct gfs2_inode *ip, int height);
extern void unstuff_dinode(struct gfs2_inode *ip);
extern unsigned int calc_tree_height(struct gfs2_inode *ip, uint64_t size);
extern int write_journal(struct gfs2_sbd *sdp, struct gfs2_inode *ip,
			 unsigned int j, unsigned int blocks);

/**
 * device_size - figure out a device's size
 * @fd: the file descriptor of a device
 * @bytes: the number of bytes the device holds
 *
 * Returns: -1 on error (with errno set), 0 on success (with @bytes set)
 */

extern int device_size(int fd, uint64_t *bytes);

/* gfs1.c - GFS1 backward compatibility functions */
struct gfs_indirect {
	struct gfs2_meta_header in_header;

	char in_reserved[64];
};

struct gfs_dinode {
	struct gfs2_meta_header di_header;

	struct gfs2_inum di_num; /* formal inode # and block address */

	uint32_t di_mode;	/* mode of file */
	uint32_t di_uid;	/* owner's user id */
	uint32_t di_gid;	/* owner's group id */
	uint32_t di_nlink;	/* number (qty) of links to this file */
	uint64_t di_size;	/* number (qty) of bytes in file */
	uint64_t di_blocks;	/* number (qty) of blocks in file */
	int64_t di_atime;	/* time last accessed */
	int64_t di_mtime;	/* time last modified */
	int64_t di_ctime;	/* time last changed */

	/*  Non-zero only for character or block device nodes  */
	uint32_t di_major;	/* device major number */
	uint32_t di_minor;	/* device minor number */

	/*  Block allocation strategy  */
	uint64_t di_rgrp;	/* dinode rgrp block number */
	uint64_t di_goal_rgrp;	/* rgrp to alloc from next */
	uint32_t di_goal_dblk;	/* data block goal */
	uint32_t di_goal_mblk;	/* metadata block goal */

	uint32_t di_flags;	/* GFS_DIF_... */

	/*  struct gfs_rindex, struct gfs_jindex, or struct gfs_dirent */
	uint32_t di_payload_format;  /* GFS_FORMAT_... */
	uint16_t di_type;	/* GFS_FILE_... type of file */
	uint16_t di_height;	/* height of metadata (0 == stuffed) */
	uint32_t di_incarn;	/* incarnation (unused, see gfs_meta_header) */
	uint16_t di_pad;

	/*  These only apply to directories  */
	uint16_t di_depth;	/* Number of bits in the table */
	uint32_t di_entries;	/* The # (qty) of entries in the directory */

	/*  This formed an on-disk chain of unused dinodes  */
	struct gfs2_inum di_next_unused;  /* used in old versions only */

	uint64_t di_eattr;	/* extended attribute block number */

	char di_reserved[56];
};

extern void gfs1_lookup_block(struct gfs2_inode *ip,
			      struct gfs2_buffer_head *bh,
			      unsigned int height, struct metapath *mp,
			      int create, int *new, uint64_t *block);
extern void gfs1_block_map(struct gfs2_inode *ip, uint64_t lblock, int *new,
			   uint64_t *dblock, uint32_t *extlen, int prealloc);
extern int gfs1_readi(struct gfs2_inode *ip, void *buf, uint64_t offset,
		      unsigned int size);
extern int gfs1_rindex_read(struct gfs2_sbd *sdp, int fd, int *count1);
extern int gfs1_ri_update(struct gfs2_sbd *sdp, int fd, int *rgcount, int quiet);
extern struct gfs2_inode *gfs_inode_get(struct gfs2_sbd *sdp,
					struct gfs2_buffer_head *bh);
extern struct gfs2_inode *gfs_inode_read(struct gfs2_sbd *sdp,
					 uint64_t di_addr);

/* gfs2_log.c */
struct gfs2_options {
	char *device;
	unsigned int yes:1;
	unsigned int no:1;
	unsigned int query:1;
};

extern int print_level;

#define MSG_DEBUG       7
#define MSG_INFO        6
#define MSG_NOTICE      5
#define MSG_WARN        4
#define MSG_ERROR       3
#define MSG_CRITICAL    2
#define MSG_NULL        1

#define print_log(priority, format...) \
	do { print_fsck_log(priority, __FILE__, __LINE__, ## format); } while(0)

#define log_debug(format...) \
	do { if(print_level >= MSG_DEBUG) print_log(MSG_DEBUG, format); } while(0)
#define log_info(format...) \
	do { if(print_level >= MSG_INFO) print_log(MSG_INFO, format); } while(0)

#define log_notice(format...) \
	do { if(print_level >= MSG_NOTICE) print_log(MSG_NOTICE, format); } while(0)

#define log_warn(format...) \
	do { if(print_level >= MSG_WARN) print_log(MSG_WARN, format); } while(0)

#define log_err(format...) \
	do { if(print_level >= MSG_ERROR) print_log(MSG_ERROR, format); } while(0)

#define log_crit(format...) \
	do { if(print_level >= MSG_CRITICAL) print_log(MSG_CRITICAL, format); } while(0)

#define stack log_debug("<backtrace> - %s()\n", __func__)

extern char gfs2_getch(void);
extern void increase_verbosity(void);
extern void decrease_verbosity(void);
extern void print_fsck_log(int priority, const char *file, int line,
			   const char *format, ...)
	__attribute__((format(printf,4,5)));
extern char generic_interrupt(const char *caller, const char *where,
			      const char *progress, const char *question,
			      const char *answers);
extern int gfs2_query(int *setonabort, struct gfs2_options *opts,
		      const char *format, ...)
	__attribute__((format(printf,3,4)));

/* misc.c */

extern int compute_heightsize(struct gfs2_sbd *sdp, uint64_t *heightsize,
		uint32_t *maxheight, uint32_t bsize1, int diptrs, int inptrs);
extern int compute_constants(struct gfs2_sbd *sdp);
extern int is_pathname_mounted(struct gfs2_sbd *sdp, int *ro_mount);
extern int is_gfs2(struct gfs2_sbd *sdp);
extern int find_gfs2_meta(struct gfs2_sbd *sdp);
extern int dir_exists(const char *dir);
extern int check_for_gfs2(struct gfs2_sbd *sdp);
extern int mount_gfs2_meta(struct gfs2_sbd *sdp);
extern void cleanup_metafs(struct gfs2_sbd *sdp);
extern char *find_debugfs_mount(void);
extern char *mp2fsname(char *mp);
extern char *get_sysfs(const char *fsname, const char *filename);
extern int get_sysfs_uint(const char *fsname, const char *filename, unsigned int *val);
extern int set_sysfs(const char *fsname, const char *filename, const char *val);
extern int is_fsname(char *name);

/* recovery.c */
extern void gfs2_replay_incr_blk(struct gfs2_inode *ip, unsigned int *blk);
extern int gfs2_replay_read_block(struct gfs2_inode *ip, unsigned int blk,
				  struct gfs2_buffer_head **bh);
extern int gfs2_revoke_add(struct gfs2_sbd *sdp, uint64_t blkno, unsigned int where);
extern int gfs2_revoke_check(struct gfs2_sbd *sdp, uint64_t blkno,
			     unsigned int where);
extern void gfs2_revoke_clean(struct gfs2_sbd *sdp);
extern int get_log_header(struct gfs2_inode *ip, unsigned int blk,
			  struct gfs2_log_header *head);
extern int find_good_lh(struct gfs2_inode *ip, unsigned int *blk,
			struct gfs2_log_header *head);
extern int jhead_scan(struct gfs2_inode *ip, struct gfs2_log_header *head);
extern int gfs2_find_jhead(struct gfs2_inode *ip, struct gfs2_log_header *head);
extern int clean_journal(struct gfs2_inode *ip, struct gfs2_log_header *head);

/* rgrp.c */
extern int gfs2_compute_bitstructs(struct gfs2_sbd *sdp, struct rgrp_list *rgd);
extern struct rgrp_list *gfs2_blk2rgrpd(struct gfs2_sbd *sdp, uint64_t blk);
extern uint64_t gfs2_rgrp_read(struct gfs2_sbd *sdp, struct rgrp_list *rgd);
extern void gfs2_rgrp_relse(struct rgrp_list *rgd);
extern void gfs2_rgrp_free(osi_list_t *rglist);

/* structures.c */
extern int build_master(struct gfs2_sbd *sdp);
extern void build_sb(struct gfs2_sbd *sdp, const unsigned char *uuid);
extern int build_jindex(struct gfs2_sbd *sdp);
extern int build_per_node(struct gfs2_sbd *sdp);
extern int build_inum(struct gfs2_sbd *sdp);
extern int build_statfs(struct gfs2_sbd *sdp);
extern int build_rindex(struct gfs2_sbd *sdp);
extern int build_quota(struct gfs2_sbd *sdp);
extern int build_root(struct gfs2_sbd *sdp);
extern int do_init_inum(struct gfs2_sbd *sdp);
extern int do_init_statfs(struct gfs2_sbd *sdp);
extern int gfs2_check_meta(struct gfs2_buffer_head *bh, int type);
extern int gfs2_next_rg_meta(struct rgrp_list *rgd, uint64_t *block,
			     int first);
extern int gfs2_next_rg_metatype(struct gfs2_sbd *sdp, struct rgrp_list *rgd,
				 uint64_t *block, uint32_t type, int first);
/* super.c */
extern int check_sb(struct gfs2_sb *sb);
extern int read_sb(struct gfs2_sbd *sdp);
extern int ji_update(struct gfs2_sbd *sdp);
extern int rindex_read(struct gfs2_sbd *sdp, int fd, int *count1);
extern int ri_update(struct gfs2_sbd *sdp, int fd, int *rgcount);
extern int write_sb(struct gfs2_sbd *sdp);

/* ondisk.c */
extern uint32_t gfs2_disk_hash(const char *data, int len);
extern const char *str_uuid(const unsigned char *uuid);
extern void gfs2_print_uuid(const unsigned char *uuid);
extern void print_it(const char *label, const char *fmt, const char *fmt2, ...)
	__attribute__((format(printf,2,4)));

/* Translation functions */

extern void gfs2_inum_in(struct gfs2_inum *no, char *buf);
extern void gfs2_inum_out(struct gfs2_inum *no, char *buf);
extern void gfs2_meta_header_in(struct gfs2_meta_header *mh,
				struct gfs2_buffer_head *bh);
extern void gfs2_meta_header_out(struct gfs2_meta_header *mh,
				 struct gfs2_buffer_head *bh);
extern void gfs2_sb_in(struct gfs2_sb *sb, struct gfs2_buffer_head *bh);
extern void gfs2_sb_out(struct gfs2_sb *sb, struct gfs2_buffer_head *bh);
extern void gfs2_rindex_in(struct gfs2_rindex *ri, char *buf);
extern void gfs2_rindex_out(struct gfs2_rindex *ri, char *buf);
extern void gfs2_rgrp_in(struct gfs2_rgrp *rg, struct gfs2_buffer_head *bh);
extern void gfs2_rgrp_out(struct gfs2_rgrp *rg, struct gfs2_buffer_head *bh);
extern void gfs2_quota_in(struct gfs2_quota *qu, char *buf);
extern void gfs2_quota_out(struct gfs2_quota *qu, char *buf);
extern void gfs2_dinode_in(struct gfs2_dinode *di,
			   struct gfs2_buffer_head *bh);
extern void gfs2_dinode_out(struct gfs2_dinode *di,
			    struct gfs2_buffer_head *bh);
extern void gfs2_dirent_in(struct gfs2_dirent *de, char *buf);
extern void gfs2_dirent_out(struct gfs2_dirent *de, char *buf);
extern void gfs2_leaf_in(struct gfs2_leaf *lf, struct gfs2_buffer_head *bh);
extern void gfs2_leaf_out(struct gfs2_leaf *lf, struct gfs2_buffer_head *bh);
extern void gfs2_ea_header_in(struct gfs2_ea_header *ea, char *buf);
extern void gfs2_ea_header_out(struct gfs2_ea_header *ea, char *buf);
extern void gfs2_log_header_in(struct gfs2_log_header *lh,
			       struct gfs2_buffer_head *bh);
extern void gfs2_log_header_out(struct gfs2_log_header *lh,
				struct gfs2_buffer_head *bh);
extern void gfs2_log_descriptor_in(struct gfs2_log_descriptor *ld,
				   struct gfs2_buffer_head *bh);
extern void gfs2_log_descriptor_out(struct gfs2_log_descriptor *ld,
				    struct gfs2_buffer_head *bh);
extern void gfs2_statfs_change_in(struct gfs2_statfs_change *sc, char *buf);
extern void gfs2_statfs_change_out(struct gfs2_statfs_change *sc, char *buf);
extern void gfs2_quota_change_in(struct gfs2_quota_change *qc,
				 struct gfs2_buffer_head *bh);
extern void gfs2_quota_change_out(struct gfs2_quota_change *qc,
				  struct gfs2_buffer_head *bh);

/* Printing functions */

extern void gfs2_inum_print(struct gfs2_inum *no);
extern void gfs2_meta_header_print(struct gfs2_meta_header *mh);
extern void gfs2_sb_print(struct gfs2_sb *sb);
extern void gfs2_rindex_print(struct gfs2_rindex *ri);
extern void gfs2_rgrp_print(struct gfs2_rgrp *rg);
extern void gfs2_quota_print(struct gfs2_quota *qu);
extern void gfs2_dinode_print(struct gfs2_dinode *di);
extern void gfs2_dirent_print(struct gfs2_dirent *de, char *name);
extern void gfs2_leaf_print(struct gfs2_leaf *lf);
extern void gfs2_ea_header_print(struct gfs2_ea_header *ea, char *name);
extern void gfs2_log_header_print(struct gfs2_log_header *lh);
extern void gfs2_log_descriptor_print(struct gfs2_log_descriptor *ld);
extern void gfs2_statfs_change_print(struct gfs2_statfs_change *sc);
extern void gfs2_quota_change_print(struct gfs2_quota_change *qc);

__END_DECLS

#endif /* __LIBGFS2_DOT_H__ */
