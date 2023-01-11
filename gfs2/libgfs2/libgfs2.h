#ifndef __LIBGFS2_DOT_H__
#define __LIBGFS2_DOT_H__

#include <features.h>
#include <inttypes.h>
#include <stdio.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <endian.h>
#include <byteswap.h>
#include <mntent.h>

#include <linux/gfs2_ondisk.h>
#include "osi_list.h"
#include "osi_tree.h"

#ifdef  __cplusplus
extern "C" {
#endif

#if __BYTE_ORDER == __BIG_ENDIAN

#define be16_to_cpu(x) ((__force uint16_t)(__be16)(x))
#define be32_to_cpu(x) ((__force uint32_t)(__be32)(x))
#define be64_to_cpu(x) ((__force uint64_t)(__be64)(x))

#define cpu_to_be16(x) ((__force __be16)(uint16_t)(x))
#define cpu_to_be32(x) ((__force __be32)(uint32_t)(x))
#define cpu_to_be64(x) ((__force __be64)(uint64_t)(x))

#define le16_to_cpu(x) bswap_16((__force uint16_t)(__le16)(x))
#define le32_to_cpu(x) bswap_32((__force uint32_t)(__le32)(x))
#define le64_to_cpu(x) bswap_64((__force uint64_t)(__le64)(x))

#define cpu_to_le16(x) ((__force __le16)bswap_16((x)))
#define cpu_to_le32(x) ((__force __le32)bswap_32((x)))
#define cpu_to_le64(x) ((__force __le64)bswap_64((x)))

#endif  /*  __BYTE_ORDER == __BIG_ENDIAN  */

#if __BYTE_ORDER == __LITTLE_ENDIAN

#define be16_to_cpu(x) bswap_16((__force uint16_t)(__be16)(x))
#define be32_to_cpu(x) bswap_32((__force uint32_t)(__be32)(x))
#define be64_to_cpu(x) bswap_64((__force uint64_t)(__be64)(x))

#define cpu_to_be16(x) ((__force __be16)bswap_16((x)))
#define cpu_to_be32(x) ((__force __be32)bswap_32((x)))
#define cpu_to_be64(x) ((__force __be64)bswap_64((x)))

#define le16_to_cpu(x) ((__force uint16_t)(__le16)(x))
#define le32_to_cpu(x) ((__force uint32_t)(__le32)(x))
#define le64_to_cpu(x) ((__force uint64_t)(__le64)(x))

#define cpu_to_le16(x) ((__force __le16)(uint16_t)(x))
#define cpu_to_le32(x) ((__force __le32)(uint32_t)(x))
#define cpu_to_le64(x) ((__force __le64)(uint64_t)(x))

#endif  /*  __BYTE_ORDER == __LITTLE_ENDIAN  */

enum lgfs2_meta_type {
	LGFS2_MT_GFS2_SB = 0,
	LGFS2_MT_GFS_SB = 1,
	LGFS2_MT_RINDEX = 2,
	LGFS2_MT_GFS2_RGRP = 3,
	LGFS2_MT_GFS_RGRP = 4,
	LGFS2_MT_RGRP_BITMAP = 5,
	LGFS2_MT_GFS2_DINODE = 6,
	LGFS2_MT_GFS_DINODE = 7,
	LGFS2_MT_GFS2_INDIRECT = 8,
	LGFS2_MT_GFS_INDIRECT = 9,
	LGFS2_MT_DIR_LEAF = 10,
	LGFS2_MT_JRNL_DATA = 11,
	LGFS2_MT_GFS2_LOG_HEADER = 12,
	LGFS2_MT_GFS_LOG_HEADER = 13,
	LGFS2_MT_GFS2_LOG_DESC = 14,
	LGFS2_MT_GFS_LOG_DESC = 15,
	LGFS2_MT_GFS2_LOG_BLOCK = 16,
	LGFS2_MT_EA_ATTR = 17,
	LGFS2_MT_EA_DATA = 18,
	LGFS2_MT_GFS2_QUOTA_CHANGE = 19,
	LGFS2_MT_DIRENT = 20,
	LGFS2_MT_EA_HEADER = 21,
	LGFS2_MT_GFS2_INUM_RANGE = 22,
	LGFS2_MT_STATFS_CHANGE = 23,
	LGFS2_MT_GFS_JINDEX = 24,
	LGFS2_MT_GFS_BLOCK_TAG = 25,
	LGFS2_MT_DATA = 26,
	LGFS2_MT_FREE = 27,

	LGFS2_MT_NR,
};

struct lgfs2_symbolic {
	const uint32_t key;
	const char *value;
	unsigned int prefix;
};

struct lgfs2_metafield {
	const char *name;
	const unsigned offset;
	const unsigned length;
	const unsigned flags;

#define LGFS2_MFF_RESERVED 0x00001	/* Field is reserved */
#define LGFS2_MFF_POINTER  0x00002	/* Field is a pointer to a block */
#define LGFS2_MFF_ENUM     0x00004	/* Field is an enum */
#define LGFS2_MFF_MASK     0x00008	/* Field is a bitmask */
#define LGFS2_MFF_UUID     0x00010	/* Field is a UUID */
#define LGFS2_MFF_STRING   0x00020	/* Field in an ASCII string */
#define LGFS2_MFF_UID      0x00040	/* Field is a UID */
#define LGFS2_MFF_GID      0x00080	/* Field is a GID */
#define LGFS2_MFF_MODE     0x00100	/* Field is a file mode */
#define LGFS2_MFF_FSBLOCKS 0x00200	/* Units are fs blocks */
#define LGFS2_MFF_BYTES    0x00400	/* Units are bytes */
#define LGFS2_MFF_SHIFT    0x00800	/* Log_{2} quantity */
#define LGFS2_MFF_CHECK    0x01000	/* Field is a checksum */
#define LGFS2_MFF_SECS     0x02000	/* Units are seconds */
#define LGFS2_MFF_NSECS    0x04000	/* Units are nsecs */
#define LGFS2_MFF_MAJOR    0x08000	/* Major device number */
#define LGFS2_MFF_MINOR    0x10000	/* Minor device number */

	/* If it is a pointer, then this field must be set */
	const unsigned points_to;
	/* If isenum or ismask are set, these must also be filled in */
	const struct lgfs2_symbolic *symtab;
	const unsigned nsyms;
};

struct lgfs2_metadata {
	const unsigned versions:2;
#define LGFS2_MD_GFS1 0x01
#define LGFS2_MD_GFS2 0x02
	const unsigned header:1;
	const uint32_t mh_type;
	const uint32_t mh_format;
	const char *name; /* Struct name */
	const char *display; /* Short name for non-programmers */
	const struct lgfs2_metafield *fields;
	const unsigned nfields;
	const unsigned size;
};

struct lgfs2_dev_info {
	struct stat stat;
	unsigned readonly:1;
	long ra_pages;
	int soft_block_size;
	int logical_block_size;
	unsigned int physical_block_size;
	unsigned int io_min_size;
	unsigned int io_optimal_size;
	int io_align_offset;
	uint64_t size;
};

struct lgfs2_device {
	uint64_t length;
};

struct lgfs2_bitmap
{
	char *bi_data;
	uint32_t bi_offset;  /* The offset in the buffer of the first byte */
	uint32_t bi_start;   /* The position of the first byte in this block */
	uint32_t bi_len;     /* The number of bytes in this block */
	unsigned bi_modified:1;
};

struct lgfs2_sbd;
struct lgfs2_inode;
typedef struct _lgfs2_rgrps *lgfs2_rgrps_t;

struct lgfs2_rgrp_tree {
	struct osi_node node;
	struct lgfs2_bitmap *bits;
	lgfs2_rgrps_t rgrps;

	/* Native-endian counterparts of the on-disk rindex struct */
	uint64_t rt_addr;
	uint64_t rt_data0;
	uint32_t rt_data;
	uint32_t rt_length;
	uint32_t rt_bitbytes;
	/* These 3 fields are duplicated between the rindex and the rgrp */
	/* For now, duplicate them here too, until users can be reworked */
	uint64_t rt_rg_data0;
	uint32_t rt_rg_data;
	uint32_t rt_rg_bitbytes;
	/* Native-endian counterparts of the on-disk rgrp structs */
	uint32_t rt_flags;
	uint32_t rt_free;
	union {
		struct { /* gfs2 */
			uint64_t rt_igeneration;
			uint32_t rt_dinodes;
			uint32_t rt_skip;
		};
		struct { /* gfs1 */
			uint32_t rt_useddi;
			uint32_t rt_freedi;
			struct {
				uint64_t no_formal_ino;
				uint64_t no_addr;
			} rt_freedi_list;
			uint32_t rt_usedmeta;
			uint32_t rt_freemeta;
		};
	};
};

typedef struct lgfs2_rgrp_tree *lgfs2_rgrp_t;

extern lgfs2_rgrps_t lgfs2_rgrps_init(struct lgfs2_sbd *sdp, uint64_t align, uint64_t offset);
extern void lgfs2_rgrps_free(lgfs2_rgrps_t *rgs);
extern uint64_t lgfs2_rindex_entry_new(lgfs2_rgrps_t rgs, struct gfs2_rindex *entry, uint64_t addr, uint32_t len);
extern unsigned lgfs2_rindex_read_fd(int fd, lgfs2_rgrps_t rgs);
extern lgfs2_rgrp_t lgfs2_rindex_read_one(struct lgfs2_inode *rip, lgfs2_rgrps_t rgs, unsigned i);
extern uint64_t lgfs2_rgrp_align_addr(const lgfs2_rgrps_t rgs, uint64_t addr);
extern uint32_t lgfs2_rgrp_align_len(const lgfs2_rgrps_t rgs, uint32_t len);
extern unsigned lgfs2_rgsize_for_data(uint64_t blksreq, unsigned bsize);
extern uint32_t lgfs2_rgrps_plan(const lgfs2_rgrps_t rgs, uint64_t space, uint32_t tgtsize);
extern lgfs2_rgrp_t lgfs2_rgrps_append(lgfs2_rgrps_t rgs, struct gfs2_rindex *entry, uint32_t rg_skip);
extern int lgfs2_rgrp_bitbuf_alloc(lgfs2_rgrp_t rg);
extern void lgfs2_rgrp_bitbuf_free(lgfs2_rgrp_t rg);
extern int lgfs2_rgrp_write(int fd, lgfs2_rgrp_t rg);
extern int lgfs2_rgrps_write_final(int fd, lgfs2_rgrps_t rgs);
extern lgfs2_rgrp_t lgfs2_rgrp_first(lgfs2_rgrps_t rgs);
extern lgfs2_rgrp_t lgfs2_rgrp_last(lgfs2_rgrps_t rgs);
extern lgfs2_rgrp_t lgfs2_rgrp_next(lgfs2_rgrp_t rg);
extern lgfs2_rgrp_t lgfs2_rgrp_prev(lgfs2_rgrp_t rg);
// Temporary function to aid API migration
extern void lgfs2_attach_rgrps(struct lgfs2_sbd *sdp, lgfs2_rgrps_t rgs);

struct lgfs2_buffer_head {
	osi_list_t b_altlist; /* alternate list */
	uint64_t b_blocknr;
	union {
		char *b_data;
		struct iovec iov;
	};
	struct lgfs2_sbd *sdp;
	int b_modified;
};

struct lgfs2_inum {
	uint64_t in_formal_ino;
	uint64_t in_addr;
};

struct lgfs2_inode {
	struct lgfs2_buffer_head *i_bh;
	struct lgfs2_sbd *i_sbd;
	struct lgfs2_rgrp_tree *i_rgd; /* performance hint */
	int bh_owned; /* Is this bh owned, iow, should we release it later? */

	/* Native-endian versions of the dinode fields */
	uint32_t i_magic;
	uint32_t i_mh_type;
	uint32_t i_format;
	struct lgfs2_inum i_num;
	uint32_t i_mode;
	uint32_t i_uid;
	uint32_t i_gid;
	uint32_t i_nlink;
	uint64_t i_size;
	uint64_t i_blocks;
	uint64_t i_atime;
	uint64_t i_mtime;
	uint64_t i_ctime;
	uint32_t i_major;
	uint32_t i_minor;

	uint32_t i_flags;
	uint32_t i_payload_format;
	uint16_t i_height;
	uint16_t i_depth;
	uint32_t i_entries;
	uint64_t i_eattr;
	union {
		struct { /* gfs2 */
			uint64_t i_goal_meta;
			uint64_t i_goal_data;
			uint64_t i_generation;
			uint32_t i_atime_nsec;
			uint32_t i_mtime_nsec;
			uint32_t i_ctime_nsec;
		};
		struct { /* gfs */
			uint64_t i_rgrp;
			uint32_t i_goal_rgrp;
			uint32_t i_goal_dblk;
			uint32_t i_goal_mblk;
			uint16_t i_di_type;
			uint32_t i_incarn;
			struct lgfs2_inum i_next_unused;
		};
	};
};

struct lgfs2_meta_dir
{
	struct lgfs2_inode *inum;
	uint64_t next_inum;
	struct lgfs2_inode *statfs;
	struct lgfs2_inode *qinode;

	struct lgfs2_inode       *jiinode;
	struct lgfs2_inode       *riinode;
	struct lgfs2_inode       *rooti;
	struct lgfs2_inode       *pinode;

	struct lgfs2_inode **journal;      /* Array of journals */
	uint32_t journals;                /* Journal count */
};

#define LGFS2_SB_ADDR(sdp) (GFS2_SB_ADDR >> (sdp)->sd_fsb2bb_shift)
struct lgfs2_sbd {
	/* CPU-endian counterparts to the on-disk superblock fields */
	uint32_t sd_bsize;
	uint32_t sd_fs_format;
	uint32_t sd_multihost_format;
	uint32_t sd_flags; /* gfs1 */
	/* gfs1's sb_jindex_di is gfs2's sb_master_dir */
	union {
		struct lgfs2_inum sd_meta_dir;
		struct lgfs2_inum sd_jindex_di;  /* gfs1 */
	};
	struct lgfs2_inum sd_root_dir;
	struct lgfs2_inum sd_rindex_di;  /* gfs1 */
	struct lgfs2_inum sd_quota_di;   /* gfs1 */
	struct lgfs2_inum sd_license_di; /* gfs1 */
	uint32_t sd_bsize_shift;
	uint32_t sd_seg_size;
	char sd_lockproto[GFS2_LOCKNAME_LEN];
	char sd_locktable[GFS2_LOCKNAME_LEN];
	uint8_t sd_uuid[16];

	/* Constants */

	uint32_t sd_fsb2bb;
	uint32_t sd_fsb2bb_shift;
	uint32_t sd_diptrs;
	uint32_t sd_inptrs;
	uint32_t sd_jbsize;
	uint32_t sd_hash_bsize;
	uint32_t sd_hash_bsize_shift;
	uint32_t sd_hash_ptrs;
	uint32_t sd_blocks_per_bitmap;
	uint32_t sd_max_height;
	uint32_t sd_max_jheight;
	uint64_t sd_heightsize[GFS2_MAX_META_HEIGHT];
	uint64_t sd_jheightsize[GFS2_MAX_META_HEIGHT];

	unsigned int jsize;   /* Size of journals (in MB) */
	unsigned int rgsize;  /* Size of resource groups (in MB) */
	unsigned int qcsize;  /* Size of quota change files (in MB) */

	int64_t sd_time;

	struct lgfs2_dev_info dinfo;
	struct lgfs2_device device;

	int device_fd;
	int path_fd;

	uint64_t fssize;
	uint64_t blks_total;
	uint64_t blks_alloced;
	uint64_t dinodes_alloced;

	uint64_t rgrps;
	struct osi_root rgtree;

	struct lgfs2_inode *master_dir;
	struct lgfs2_meta_dir md;

	unsigned int gfs1:1;
};

struct lgfs2_log_header {
	uint64_t lh_sequence;
	uint32_t lh_flags;
	uint32_t lh_tail;
	uint32_t lh_blkno;
	uint32_t lh_hash;
	uint32_t lh_crc;
	int64_t lh_local_total;
	int64_t lh_local_free;
	int64_t lh_local_dinodes;
};

struct lgfs2_dirent {
	struct lgfs2_inum dr_inum;
	uint32_t dr_hash;
	uint16_t dr_rec_len;
	uint16_t dr_name_len;
	uint16_t dr_type;
	uint16_t dr_rahead;
};

struct lgfs2_leaf {
	uint16_t lf_depth;
	uint16_t lf_entries;
	uint32_t lf_dirent_format;
	uint64_t lf_next;
	uint64_t lf_inode;
	uint32_t lf_dist;
	uint32_t lf_nsec;
	uint64_t lf_sec;
};

struct lgfs2_metapath {
	unsigned int mp_list[GFS2_MAX_META_HEIGHT];
};


#define LGFS2_DEFAULT_BSIZE          (4096)
#define LGFS2_DEFAULT_JSIZE          (128)
#define LGFS2_MAX_JSIZE              (1024)
#define LGFS2_MIN_JSIZE              (8)
#define LGFS2_DEFAULT_RGSIZE         (256)
#define LGFS2_DEFAULT_QCSIZE         (1)
#define LGFS2_DEFAULT_LOCKPROTO      "lock_dlm"

#define LGFS2_MIN_RGSIZE             (32)
#define LGFS2_MAX_RGSIZE             (2048)

#define LGFS2_FS_FORMAT_MIN (1801)
#define LGFS2_FS_FORMAT_MAX (1802)
#define LGFS2_FS_FORMAT_VALID(n) ((n) >= LGFS2_FS_FORMAT_MIN && (n) <= LGFS2_FS_FORMAT_MAX)

/* meta.c */
extern const struct lgfs2_metadata lgfs2_metadata[];
extern const unsigned lgfs2_metadata_size;
extern const struct lgfs2_symbolic lgfs2_metatypes[];
extern const unsigned lgfs2_metatype_size;
extern const struct lgfs2_symbolic lgfs2_metaformats[];
extern const unsigned lgfs2_metaformat_size;
extern const struct lgfs2_symbolic lgfs2_di_flags[];
extern const unsigned lgfs2_di_flag_size;
extern const struct lgfs2_symbolic lgfs2_lh_flags[];
extern const unsigned lgfs2_lh_flag_size;
extern const struct lgfs2_symbolic lgfs2_ld_types[];
extern const unsigned lgfs2_ld_type_size;
extern const struct lgfs2_symbolic lgfs2_ld1_types[];
extern const unsigned lgfs2_ld1_type_size;
extern int lgfs2_selfcheck(void);
extern const struct lgfs2_metadata *lgfs2_find_mtype(uint32_t mh_type, const unsigned versions);
extern const struct lgfs2_metadata *lgfs2_find_mtype_name(const char *name, const unsigned versions);
extern const struct lgfs2_metafield *lgfs2_find_mfield_name(const char *name, const struct lgfs2_metadata *mtype);
extern int lgfs2_field_str(char *str, const size_t size, const char *blk, const struct lgfs2_metafield *field, int hex);
extern int lgfs2_field_assign(char *blk, const struct lgfs2_metafield *field, const void *val);

/* buf.c */
extern struct lgfs2_buffer_head *lgfs2_bget(struct lgfs2_sbd *sdp, uint64_t num);
extern struct lgfs2_buffer_head *__lgfs2_bread(struct lgfs2_sbd *sdp, uint64_t num,
					int line, const char *caller);
extern int lgfs2_bwrite(struct lgfs2_buffer_head *bh);
extern int lgfs2_brelse(struct lgfs2_buffer_head *bh);
extern uint32_t lgfs2_get_block_type(const char *buf);

#define lgfs2_bmodified(bh) do { bh->b_modified = 1; } while(0)

#define lgfs2_bread(bl, num) __lgfs2_bread(bl, num, __LINE__, __FUNCTION__)

/* device_geometry.c */
extern int lgfs2_get_dev_info(int fd, struct lgfs2_dev_info *i);
extern void lgfs2_fix_device_geometry(struct lgfs2_sbd *sdp);

/* fs_bits.c */
#define LGFS2_BFITNOENT (0xFFFFFFFF)

/* functions with blk #'s that are buffer relative */
extern unsigned long lgfs2_bitfit(const unsigned char *buffer,
				 const unsigned int buflen,
				 unsigned long goal, unsigned char old_state);

/* functions with blk #'s that are rgrp relative */
extern int lgfs2_check_range(struct lgfs2_sbd *sdp, uint64_t blkno);

/* functions with blk #'s that are file system relative */
extern int lgfs2_get_bitmap(struct lgfs2_sbd *sdp, uint64_t blkno, struct lgfs2_rgrp_tree *rgd);
extern int lgfs2_set_bitmap(lgfs2_rgrp_t rg, uint64_t blkno, int state);

/* fs_ops.c */
#define LGFS2_IS_LEAF   (1)
#define LGFS2_IS_DINODE (2)

extern void lgfs2_find_metapath(struct lgfs2_inode *ip, uint64_t block, struct lgfs2_metapath *mp);
extern void lgfs2_lookup_block(struct lgfs2_inode *ip, struct lgfs2_buffer_head *bh,
			 unsigned int height, struct lgfs2_metapath *mp,
			 int create, int *new, uint64_t *block);
extern struct lgfs2_inode *lgfs2_inode_get(struct lgfs2_sbd *sdp,
				    struct lgfs2_buffer_head *bh);
extern struct lgfs2_inode *lgfs2_inode_read(struct lgfs2_sbd *sdp, uint64_t di_addr);
extern struct lgfs2_inode *lgfs2_is_system_inode(struct lgfs2_sbd *sdp,
					  uint64_t block);
extern void lgfs2_inode_put(struct lgfs2_inode **ip);
extern uint64_t lgfs2_data_alloc(struct lgfs2_inode *ip);
extern int lgfs2_meta_alloc(struct lgfs2_inode *ip, uint64_t *blkno);
extern int lgfs2_dinode_alloc(struct lgfs2_sbd *sdp, const uint64_t blksreq, uint64_t *blkno);
extern uint64_t lgfs2_space_for_data(const struct lgfs2_sbd *sdp, unsigned bsize, uint64_t bytes);
extern int lgfs2_file_alloc(lgfs2_rgrp_t rg, uint64_t di_size, struct lgfs2_inode *ip, uint32_t flags, unsigned mode);

extern int lgfs2_readi(struct lgfs2_inode *ip, void *buf, uint64_t offset,
		      unsigned int size);
#define lgfs2_writei(ip, buf, offset, size) \
	__lgfs2_writei(ip, buf, offset, size, 1)
extern int __lgfs2_writei(struct lgfs2_inode *ip, void *buf, uint64_t offset,
			 unsigned int size, int resize);
extern int lgfs2_init_dinode(struct lgfs2_sbd *sdp, struct lgfs2_buffer_head **bhp, struct lgfs2_inum *inum,
                       unsigned int mode, uint32_t flags, struct lgfs2_inum *parent);
extern struct lgfs2_inode *lgfs2_createi(struct lgfs2_inode *dip, const char *filename,
				  unsigned int mode, uint32_t flags);
extern struct lgfs2_inode *lgfs2_gfs_createi(struct lgfs2_inode *dip,
				      const char *filename, unsigned int mode,
				      uint32_t flags);
extern void lgfs2_dirent2_del(struct lgfs2_inode *dip, struct lgfs2_buffer_head *bh,
			struct gfs2_dirent *prev, struct gfs2_dirent *cur);
extern int lgfs2_dir_search(struct lgfs2_inode *dip, const char *filename, int len,
		      unsigned int *type, struct lgfs2_inum *inum);
extern int lgfs2_lookupi(struct lgfs2_inode *dip, const char *filename, int len,
			struct lgfs2_inode **ipp);
extern int lgfs2_dir_add(struct lgfs2_inode *dip, const char *filename, int len,
		    struct lgfs2_inum *inum, unsigned int type);
extern int lgfs2_dirent_del(struct lgfs2_inode *dip, const char *filename, int name_len);
extern int lgfs2_block_map(struct lgfs2_inode *ip, uint64_t lblock, int *new, uint64_t *dblock,
                           uint32_t *extlen, int prealloc) __attribute__((warn_unused_result));
extern int lgfs2_get_leaf_ptr(struct lgfs2_inode *dip, uint32_t index, uint64_t *ptr) __attribute__((warn_unused_result));
extern void lgfs2_dir_split_leaf(struct lgfs2_inode *dip, uint32_t start,
			   uint64_t leaf_no, struct lgfs2_buffer_head *obh);
extern void lgfs2_free_block(struct lgfs2_sbd *sdp, uint64_t block);
extern int lgfs2_freedi(struct lgfs2_sbd *sdp, uint64_t block);
extern int lgfs2_get_leaf(struct lgfs2_inode *dip, uint64_t leaf_no,
			 struct lgfs2_buffer_head **bhp);
extern int lgfs2_dirent_first(struct lgfs2_inode *dip,
			     struct lgfs2_buffer_head *bh,
			     struct gfs2_dirent **dent);
extern int lgfs2_dirent_next(struct lgfs2_inode *dip, struct lgfs2_buffer_head *bh,
			    struct gfs2_dirent **dent);
extern int lgfs2_build_height(struct lgfs2_inode *ip, int height) __attribute__((warn_unused_result));
extern int lgfs2_unstuff_dinode(struct lgfs2_inode *ip) __attribute__((warn_unused_result));
extern unsigned int lgfs2_calc_tree_height(struct lgfs2_inode *ip, uint64_t size);
extern uint32_t lgfs2_log_header_hash(char *buf);
extern uint32_t lgfs2_log_header_crc(char *buf, unsigned bsize);

/* gfs1.c - GFS1 backward compatibility structures and functions */

#define GFS_FORMAT_SB           (100)  /* Super-Block */
#define GFS_METATYPE_SB         (1)    /* Super-Block */
#define GFS_FORMAT_FS           (1309) /* Filesystem (all-encompassing) */
#define GFS_FORMAT_MULTI        (1401) /* Multi-Host */
/* GFS1 Dinode types  */
#define GFS_FILE_NON            (0)
#define GFS_FILE_REG            (1)    /* regular file */
#define GFS_FILE_DIR            (2)    /* directory */
#define GFS_FILE_LNK            (5)    /* link */
#define GFS_FILE_BLK            (7)    /* block device node */
#define GFS_FILE_CHR            (8)    /* character device node */
#define GFS_FILE_FIFO           (101)  /* fifo/pipe */
#define GFS_FILE_SOCK           (102)  /* socket */

/* GFS 1 journal block types: */
#define GFS_LOG_DESC_METADATA   (300)    /* metadata */
#define GFS_LOG_DESC_IUL        (400)    /* unlinked inode */
#define GFS_LOG_DESC_IDA        (401)    /* de-allocated inode */
#define GFS_LOG_DESC_Q          (402)    /* quota */
#define GFS_LOG_DESC_LAST       (500)    /* final in a logged transaction */

struct gfs_indirect {
	struct gfs2_meta_header in_header;

	char in_reserved[64];
};

struct gfs_dinode {
	struct gfs2_meta_header di_header;

	struct gfs2_inum di_num; /* formal inode # and block address */

	__be32 di_mode;	/* mode of file */
	__be32 di_uid;	/* owner's user id */
	__be32 di_gid;	/* owner's group id */
	__be32 di_nlink;	/* number (qty) of links to this file */
	__be64 di_size;	/* number (qty) of bytes in file */
	__be64 di_blocks;	/* number (qty) of blocks in file */
	__be64 di_atime;	/* time last accessed */
	__be64 di_mtime;	/* time last modified */
	__be64 di_ctime;	/* time last changed */

	/*  Non-zero only for character or block device nodes  */
	__be32 di_major;	/* device major number */
	__be32 di_minor;	/* device minor number */

	/*  Block allocation strategy  */
	__be64 di_rgrp;	/* dinode rgrp block number */
	__be64 di_goal_rgrp;	/* rgrp to alloc from next */
	__be32 di_goal_dblk;	/* data block goal */
	__be32 di_goal_mblk;	/* metadata block goal */

	__be32 di_flags;	/* GFS_DIF_... */

	/*  struct gfs_rindex, struct gfs_jindex, or struct gfs_dirent */
	__be32 di_payload_format;  /* GFS_FORMAT_... */
	__be16 di_type;	/* GFS_FILE_... type of file */
	__be16 di_height;	/* height of metadata (0 == stuffed) */
	__be32 di_incarn;	/* incarnation (unused, see gfs_meta_header) */
	__be16 di_pad;

	/*  These only apply to directories  */
	__be16 di_depth;	/* Number of bits in the table */
	__be32 di_entries;	/* The # (qty) of entries in the directory */

	/*  This formed an on-disk chain of unused dinodes  */
	struct gfs2_inum di_next_unused;  /* used in old versions only */

	__be64 di_eattr;	/* extended attribute block number */

	char di_reserved[56];
};

struct gfs_sb {
	/*  Order is important; need to be able to read old superblocks
	    in order to support on-disk version upgrades */
	struct gfs2_meta_header sb_header;

	__be32 sb_fs_format;         /* GFS_FORMAT_FS (on-disk version) */
	__be32 sb_multihost_format;  /* GFS_FORMAT_MULTI */
	__be32 sb_flags;             /* ?? */

	__be32 sb_bsize;             /* fundamental FS block size in bytes */
	__be32 sb_bsize_shift;       /* log2(sb_bsize) */
	__be32 sb_seg_size;          /* Journal segment size in FS blocks */

	/* These special inodes do not appear in any on-disk directory. */
	struct gfs2_inum sb_jindex_di;  /* journal index inode */
	struct gfs2_inum sb_rindex_di;  /* resource group index inode */
	struct gfs2_inum sb_root_di;    /* root directory inode */

	/* Default inter-node locking protocol (lock module) and namespace */
	uint8_t sb_lockproto[GFS2_LOCKNAME_LEN]; /* lock protocol name */
	uint8_t sb_locktable[GFS2_LOCKNAME_LEN]; /* unique name for this FS */

	/* More special inodes */
	struct gfs2_inum sb_quota_di;   /* quota inode */
	struct gfs2_inum sb_license_di; /* license inode */

	char sb_reserved[96];
};

struct gfs_rgrp {
	struct gfs2_meta_header rg_header;

	__be32 rg_flags;
	__be32 rg_free;       /* Number (qty) of free data blocks */

	/* Dinodes are USEDMETA, but are handled separately from other METAs */
	__be32 rg_useddi;     /* Number (qty) of dinodes (used or free) */
	__be32 rg_freedi;     /* Number (qty) of unused (free) dinodes */
	struct gfs2_inum rg_freedi_list; /* 1st block in chain of free dinodes */

	/* These META statistics do not include dinodes (used or free) */
	__be32 rg_usedmeta;   /* Number (qty) of used metadata blocks */
	__be32 rg_freemeta;   /* Number (qty) of unused metadata blocks */

	char rg_reserved[64];
};

struct gfs_log_header {
	struct gfs2_meta_header lh_header;

	__be32 lh_flags;      /* GFS_LOG_HEAD_... */
	__be32 lh_pad;

	__be64 lh_first;     /* Block number of first header in this trans */
	__be64 lh_sequence;   /* Sequence number of this transaction */

	__be64 lh_tail;       /* Block number of log tail */
	__be64 lh_last_dump;  /* Block number of last dump */

	uint8_t lh_reserved[64];
};

struct gfs_jindex {
        __be64 ji_addr;       /* starting block of the journal */
        __be32 ji_nsegment;   /* number (quantity) of segments in journal */
        __be32 ji_pad;

        uint8_t ji_reserved[64];
};

struct gfs_log_descriptor {
	struct gfs2_meta_header ld_header;

	__be32 ld_type;       /* GFS_LOG_DESC_... Type of this log chunk */
	__be32 ld_length;     /* Number of buffers in this chunk */
	__be32 ld_data1;      /* descriptor-specific field */
	__be32 ld_data2;      /* descriptor-specific field */
	uint8_t ld_reserved[64];
};

extern int lgfs2_is_gfs_dir(struct lgfs2_inode *ip);
extern void lgfs2_gfs1_lookup_block(struct lgfs2_inode *ip,
			      struct lgfs2_buffer_head *bh,
			      unsigned int height, struct lgfs2_metapath *mp,
			      int create, int *new, uint64_t *block);
extern int lgfs2_gfs1_block_map(struct lgfs2_inode *ip, uint64_t lblock, int *new, uint64_t *dblock,
                                uint32_t *extlen, int prealloc) __attribute__((warn_unused_result));
extern int lgfs2_gfs1_writei(struct lgfs2_inode *ip, void *buf, uint64_t offset,
		       unsigned int size);
extern struct lgfs2_inode *lgfs2_gfs_inode_get(struct lgfs2_sbd *sdp, char *buf);
extern struct lgfs2_inode *lgfs2_gfs_inode_read(struct lgfs2_sbd *sdp, uint64_t di_addr);
extern void lgfs2_gfs_rgrp_in(const lgfs2_rgrp_t rg, void *buf);
extern void lgfs2_gfs_rgrp_out(const lgfs2_rgrp_t rg, void *buf);

/* misc.c */
extern int lgfs2_compute_heightsize(unsigned bsize, uint64_t *heightsize,
		uint32_t *maxheight, uint32_t bsize1, int diptrs, int inptrs);
extern int lgfs2_compute_constants(struct lgfs2_sbd *sdp);
extern int lgfs2_open_mnt(const char *path, int dirflags, int *dirfd, int devflags, int *devfd, struct mntent **mnt);
extern int lgfs2_open_mnt_dev(const char *path, int flags, struct mntent **mnt);
extern int lgfs2_open_mnt_dir(const char *path, int flags, struct mntent **mnt);

/* recovery.c */
extern void lgfs2_replay_incr_blk(struct lgfs2_inode *ip, unsigned int *blk);
extern int lgfs2_replay_read_block(struct lgfs2_inode *ip, unsigned int blk,
				  struct lgfs2_buffer_head **bh);
extern int lgfs2_get_log_header(struct lgfs2_inode *ip, unsigned int blk,
                                struct lgfs2_log_header *head);
extern int lgfs2_find_jhead(struct lgfs2_inode *ip, struct lgfs2_log_header *head);
extern int lgfs2_clean_journal(struct lgfs2_inode *ip, struct lgfs2_log_header *head);

/* rgrp.c */
extern uint32_t lgfs2_rgblocks2bitblocks(const unsigned int bsize, const uint32_t rgblocks,
                                    uint32_t *ri_data) __attribute__((nonnull(3)));
extern int lgfs2_compute_bitstructs(const uint32_t bsize, struct lgfs2_rgrp_tree *rgd);
extern struct lgfs2_rgrp_tree *lgfs2_blk2rgrpd(struct lgfs2_sbd *sdp, uint64_t blk);
extern int lgfs2_rgrp_crc_check(char *buf);
extern void lgfs2_rgrp_crc_set(char *buf);
extern uint64_t lgfs2_rgrp_read(struct lgfs2_sbd *sdp, struct lgfs2_rgrp_tree *rgd);
extern void lgfs2_rgrp_relse(struct lgfs2_sbd *sdp, struct lgfs2_rgrp_tree *rgd);
extern struct lgfs2_rgrp_tree *lgfs2_rgrp_insert(struct osi_root *rgtree,
				     uint64_t rgblock);
extern void lgfs2_rgrp_free(struct lgfs2_sbd *sdp, struct osi_root *rgrp_tree);

/* structures.c */
extern int lgfs2_build_master(struct lgfs2_sbd *sdp);
extern int lgfs2_sb_write(const struct lgfs2_sbd *sdp, int fd);
extern int lgfs2_build_journal(struct lgfs2_sbd *sdp, int j, struct lgfs2_inode *jindex);
extern int lgfs2_write_journal(struct lgfs2_inode *jnl, unsigned bsize, unsigned blocks);
extern int lgfs2_write_journal_data(struct lgfs2_inode *ip);
extern int lgfs2_write_filemeta(struct lgfs2_inode *ip);
extern struct lgfs2_inode *lgfs2_build_jindex(struct lgfs2_inode *metafs, struct lgfs2_inum *jnls, size_t nmemb);
extern struct lgfs2_inode *lgfs2_build_inum(struct lgfs2_sbd *sdp);
extern struct lgfs2_inode *lgfs2_build_statfs(struct lgfs2_sbd *sdp);
extern struct lgfs2_inode *lgfs2_build_rindex(struct lgfs2_sbd *sdp);
extern struct lgfs2_inode *lgfs2_build_quota(struct lgfs2_sbd *sdp);
extern int lgfs2_build_root(struct lgfs2_sbd *sdp);
extern int lgfs2_init_inum(struct lgfs2_sbd *sdp);
extern int lgfs2_init_statfs(struct lgfs2_sbd *sdp, struct gfs2_statfs_change *res);
extern int lgfs2_check_meta(const char *buf, int type);
extern unsigned lgfs2_bm_scan(struct lgfs2_rgrp_tree *rgd, unsigned idx,
			      uint64_t *buf, uint8_t state);
extern struct lgfs2_inode *lgfs2_build_inum_range(struct lgfs2_inode *per_node, unsigned int n);
extern struct lgfs2_inode *lgfs2_build_statfs_change(struct lgfs2_inode *per_node, unsigned int j);
extern struct lgfs2_inode *lgfs2_build_quota_change(struct lgfs2_inode *per_node, unsigned int j);

/* super.c */
extern int lgfs2_check_sb(void *sbp);
extern int lgfs2_read_sb(struct lgfs2_sbd *sdp);
extern int lgfs2_rindex_read(struct lgfs2_sbd *sdp, uint64_t *rgcount, int *ok);
extern int lgfs2_write_sb(struct lgfs2_sbd *sdp);

/* gfs2_disk_hash.c */
extern uint32_t lgfs2_disk_hash(const char *data, int len);

/* ondisk.c */
extern void lgfs2_inum_in(struct lgfs2_inum *i, void *inp);
extern void lgfs2_inum_out(const struct lgfs2_inum *i, void *inp);
extern void lgfs2_sb_in(struct lgfs2_sbd *sdp, void *buf);
extern void lgfs2_sb_out(const struct lgfs2_sbd *sdp, void *buf);
extern void lgfs2_rindex_in(lgfs2_rgrp_t rg, void *buf);
extern void lgfs2_rindex_out(const lgfs2_rgrp_t rg, void *buf);
extern void lgfs2_rgrp_in(lgfs2_rgrp_t rg, void *buf);
extern void lgfs2_rgrp_out(const lgfs2_rgrp_t rg, void *buf);
extern void lgfs2_dinode_in(struct lgfs2_inode *ip, char *buf);
extern void lgfs2_dinode_out(struct lgfs2_inode *ip, char *buf);
extern void lgfs2_dirent_in(struct lgfs2_dirent *d, void *dep);
extern void lgfs2_dirent_out(struct lgfs2_dirent *d, void *dep);
extern void lgfs2_leaf_in(struct lgfs2_leaf *lf, void *lfp);
extern void lgfs2_leaf_out(struct lgfs2_leaf *lf, void *lfp);

#ifdef  __cplusplus
}
#endif

#endif /* __LIBGFS2_DOT_H__ */
