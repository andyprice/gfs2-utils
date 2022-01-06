#include "clusterautoconfig.h"

#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <curses.h>
#include <term.h>
#include <sys/ioctl.h>
#include <limits.h>
#include <sys/time.h>
#include <zlib.h>
#include <bzlib.h>
#include <time.h>

#include <logging.h>
#include "osi_list.h"
#include "gfs2hex.h"
#include "hexedit.h"
#include "libgfs2.h"

#define DFT_SAVE_FILE "/tmp/gfsmeta.XXXXXX"
#define MAX_JOURNALS_SAVED 256

/* Header for the savemeta output file */
struct savemeta_header {
#define SAVEMETA_MAGIC (0x01171970)
	__be32 sh_magic;
#define SAVEMETA_FORMAT (1)
	__be32 sh_format; /* In case we want to change the layout */
	__be64 sh_time; /* When savemeta was run */
	__be64 sh_fs_bytes; /* Size of the fs */
	uint8_t __reserved[104];
};

struct savemeta {
	time_t sm_time;
	unsigned sm_format;
	size_t sm_fs_bytes;
};

struct saved_metablock {
	__be64 blk;
	__be16 siglen; /* significant data length */
/* This needs to be packed because old versions of gfs2_edit read and write the
   individual fields separately, so the hole after siglen must be eradicated
   before the struct reflects what's on disk. */
} __attribute__((__packed__));

struct metafd {
	int fd;
	gzFile gzfd;
	BZFILE *bzfd;
	const char *filename;
	int gziplevel;
	int eof;
	int (*read)(struct metafd *mfd, void *buf, unsigned len);
	void (*close)(struct metafd *mfd);
	const char* (*strerr)(struct metafd *mfd);
};

static char *restore_buf;
static ssize_t restore_left;
static off_t restore_off;
#define RESTORE_BUF_SIZE (2 * 1024 * 1024)

static char *restore_buf_next(struct metafd *mfd, size_t required_len)
{
	if (restore_left < required_len) {
		char *tail = restore_buf + restore_off;
		int ret;

		memmove(restore_buf, tail, restore_left);
		ret = mfd->read(mfd, restore_buf + restore_left, RESTORE_BUF_SIZE - restore_left);
		if (ret < (int)required_len - restore_left)
			return NULL;
		restore_left += ret;
		restore_off = 0;
	}
	restore_left -= required_len;
	restore_off += required_len;
	return &restore_buf[restore_off - required_len];
}

/* gzip compression method */

static const char *gz_strerr(struct metafd *mfd)
{
	int err;
	const char *errstr = gzerror(mfd->gzfd, &err);

	if (err == Z_ERRNO)
		return strerror(errno);
	return errstr;
}

static int gz_read(struct metafd *mfd, void *buf, unsigned len)
{
	int ret = gzread(mfd->gzfd, buf, len);
	if (ret < len && gzeof(mfd->gzfd))
		mfd->eof = 1;
	return ret;
}

static void gz_close(struct metafd *mfd)
{
	gzclose(mfd->gzfd);
}

/* This should be tried last because gzip doesn't distinguish between
   decompressing a gzip file and reading an uncompressed file */
static int restore_try_gzip(struct metafd *mfd)
{
	mfd->read = gz_read;
	mfd->close = gz_close;
	mfd->strerr = gz_strerr;
	lseek(mfd->fd, 0, SEEK_SET);
	mfd->gzfd = gzdopen(mfd->fd, "rb");
	if (!mfd->gzfd)
		return 1;
	gzbuffer(mfd->gzfd, (1<<20)); /* Increase zlib's buffers to 1MB */
	restore_left = mfd->read(mfd, restore_buf, RESTORE_BUF_SIZE);
	if (restore_left < 512)
		return -1;
	return 0;
}

/* bzip2 compression method */

static const char *bz_strerr(struct metafd *mfd)
{
	int err;
	const char *errstr = BZ2_bzerror(mfd->bzfd, &err);

	if (err == BZ_IO_ERROR)
		return strerror(errno);
	return errstr;
}

static int bz_read(struct metafd *mfd, void *buf, unsigned len)
{
	int bzerr = BZ_OK;
	int ret;

	ret = BZ2_bzRead(&bzerr, mfd->bzfd, buf, len);
	if (bzerr == BZ_OK)
		return ret;
	if (bzerr == BZ_STREAM_END) {
		mfd->eof = 1;
		return ret;
	}
	return -1;
}

static void bz_close(struct metafd *mfd)
{
	BZ2_bzclose(mfd->bzfd);
}

static int restore_try_bzip(struct metafd *mfd)
{
	int bzerr;
	FILE *f;

	f = fdopen(mfd->fd, "r");
	if (f == NULL)
		return 1;

	mfd->read = bz_read;
	mfd->close = bz_close;
	mfd->strerr = bz_strerr;
	mfd->bzfd = BZ2_bzReadOpen(&bzerr, f, 0, 0, NULL, 0);
	if (!mfd->bzfd)
		return 1;
	restore_left = mfd->read(mfd, restore_buf, RESTORE_BUF_SIZE);
	if (restore_left < 512)
		return -1;
	return 0;
}

static uint64_t blks_saved;
static uint64_t journal_blocks[MAX_JOURNALS_SAVED];
static uint64_t gfs1_journal_size = 0; /* in blocks */
static int journals_found = 0;
int print_level = MSG_NOTICE;

static int block_is_a_journal(uint64_t blk)
{
	int j;

	for (j = 0; j < journals_found; j++)
		if (blk == journal_blocks[j])
			return TRUE;
	return FALSE;
}

static struct osi_root per_node_tree;
struct per_node_node {
	struct osi_node node;
	uint64_t block;
};

static void destroy_per_node_lookup(void)
{
	struct osi_node *n;
	struct per_node_node *pnp;

	while ((n = osi_first(&per_node_tree))) {
		pnp = (struct per_node_node *)n;
		osi_erase(n, &per_node_tree);
		free(pnp);
	}
}

static int block_is_in_per_node(uint64_t blk)
{
	struct per_node_node *pnp = (struct per_node_node *)per_node_tree.osi_node;

	while (pnp) {
		if (blk < pnp->block)
			pnp = (struct per_node_node *)pnp->node.osi_left;
		else if (blk > pnp->block)
			pnp = (struct per_node_node *)pnp->node.osi_right;
		else
			return 1;
	}

	return 0;
}

static int insert_per_node_lookup(uint64_t blk)
{
	struct osi_node **newn = &per_node_tree.osi_node, *parent = NULL;
	struct per_node_node *pnp;

	while (*newn) {
		struct per_node_node *cur = (struct per_node_node *)*newn;

		parent = *newn;
		if (blk < cur->block)
			newn = &((*newn)->osi_left);
		else if (blk > cur->block)
			newn = &((*newn)->osi_right);
		else
			return 0;
	}

	pnp = calloc(1, sizeof(struct per_node_node));
	if (pnp == NULL) {
		perror("Failed to insert per_node lookup entry");
		return 1;
	}
	pnp->block = blk;
	osi_link_node(&pnp->node, parent, newn);
	osi_insert_color(&pnp->node, &per_node_tree);
	return 0;
}

static int init_per_node_lookup(void)
{
	int i;
	struct gfs2_inode *per_node_di;

	if (sbd.gfs1)
		return FALSE;

	per_node_di = lgfs2_inode_read(&sbd, masterblock("per_node"));
	if (per_node_di == NULL) {
		fprintf(stderr, "Failed to read per_node: %s\n", strerror(errno));
		return 1;
	}

	do_dinode_extended(per_node_di->i_bh->b_data);
	inode_put(&per_node_di);

	for (i = 0; i < indirect_blocks; i++) {
		int d;
		for (d = 0; d < indirect->ii[i].dirents; d++) {
			int ret = insert_per_node_lookup(indirect->ii[i].dirent[d].inum.in_addr);
			if (ret != 0)
				return ret;
		}
	}
	return 0;
}

static int block_is_systemfile(uint64_t blk)
{
	return block_is_jindex(blk) || block_is_inum_file(blk) ||
		block_is_statfs_file(blk) || block_is_quota_file(blk) ||
		block_is_rindex(blk) || block_is_a_journal(blk) ||
		block_is_per_node(blk) || block_is_in_per_node(blk);
}

static size_t di_save_len(const char *buf, uint64_t owner)
{
	const struct gfs2_dinode *dn;
	const struct gfs_dinode *d1;
	uint16_t di_height;
	uint32_t di_mode;
	int gfs1dir;

	dn = (void *)buf;
	d1 = (void *)buf;
	di_mode = be32_to_cpu(dn->di_mode);
	di_height = be16_to_cpu(dn->di_height);
	/* __pad1 is di_type in gfs1 */
	gfs1dir = sbd.gfs1 && (be16_to_cpu(d1->di_type) == GFS_FILE_DIR);

	/* Do not save (user) data from the inode block unless they are
	   indirect pointers, dirents, symlinks or fs internal data */
	if (di_height > 0 || S_ISDIR(di_mode) || S_ISLNK(di_mode) || gfs1dir
	    || block_is_systemfile(owner))
		return sbd.sd_bsize;
	return sizeof(struct gfs2_dinode);
}

/*
 * get_gfs_struct_info - get block type and structure length
 *
 * @buf - The block buffer to examine
 * @owner - The block address of the parent structure
 * @block_type - pointer to integer to hold the block type
 * @gstruct_len - pointer to integer to hold the structure length
 *
 * returns: 0 if successful
 *          -1 if this isn't gfs metadata.
 */
static int get_gfs_struct_info(const char *buf, uint64_t owner, unsigned *block_type,
                               unsigned *gstruct_len)
{
	struct gfs2_meta_header *mh = (struct gfs2_meta_header *)buf;

	if (block_type != NULL)
		*block_type = 0;

	if (gstruct_len != NULL)
		*gstruct_len = sbd.sd_bsize;

	if (be32_to_cpu(mh->mh_magic) != GFS2_MAGIC)
		return -1;

	if (block_type != NULL)
		*block_type = be32_to_cpu(mh->mh_type);

	if (gstruct_len == NULL)
		return 0;

	switch (be32_to_cpu(mh->mh_type)) {
	case GFS2_METATYPE_SB:   /* 1 (superblock) */
		if (sbd.gfs1)
			*gstruct_len = sizeof(struct gfs_sb);
		else
			*gstruct_len = sizeof(struct gfs2_sb);
		break;
	case GFS2_METATYPE_RG:   /* 2 (rsrc grp hdr) */
		*gstruct_len = sbd.sd_bsize; /*sizeof(struct gfs_rgrp);*/
		break;
	case GFS2_METATYPE_RB:   /* 3 (rsrc grp bitblk) */
		*gstruct_len = sbd.sd_bsize;
		break;
	case GFS2_METATYPE_DI:   /* 4 (disk inode) */
		*gstruct_len = di_save_len(buf, owner);
		break;
	case GFS2_METATYPE_IN:   /* 5 (indir inode blklst) */
		*gstruct_len = sbd.sd_bsize; /*sizeof(struct gfs_indirect);*/
		break;
	case GFS2_METATYPE_LF:   /* 6 (leaf dinode blklst) */
		*gstruct_len = sbd.sd_bsize; /*sizeof(struct gfs_leaf);*/
		break;
	case GFS2_METATYPE_JD:   /* 7 (journal data) */
		*gstruct_len = sbd.sd_bsize;
		break;
	case GFS2_METATYPE_LH:   /* 8 (log header) */
		if (sbd.gfs1)
			*gstruct_len = 512; /* gfs copies the log header
					       twice and compares the copy,
					       so we need to save all 512
					       bytes of it. */
		else
			*gstruct_len = sizeof(struct gfs2_log_header);
		break;
	case GFS2_METATYPE_LD:   /* 9 (log descriptor) */
		*gstruct_len = sbd.sd_bsize;
		break;
	case GFS2_METATYPE_EA:   /* 10 (extended attr hdr) */
		*gstruct_len = sbd.sd_bsize;
		break;
	case GFS2_METATYPE_ED:   /* 11 (extended attr data) */
		*gstruct_len = sbd.sd_bsize;
		break;
	default:
		*gstruct_len = sbd.sd_bsize;
		break;
	}
	return 0;
}

/**
 * Print a progress message if one second has elapsed since the last time.
 * pblock: The latest block number processed
 * force:  If this is non-zero, print immediately and add a newline after the
 *         progress message.
 */
static void report_progress(uint64_t pblock, int force)
{
        static struct timeval tv;
        static uint32_t seconds = 0;

	gettimeofday(&tv, NULL);
	if (!seconds)
		seconds = tv.tv_sec;
	if (force || tv.tv_sec - seconds) {
		static uint64_t percent;

		seconds = tv.tv_sec;
		if (sbd.fssize) {
			printf("\r");
			percent = (pblock * 100) / sbd.fssize;
			printf("%"PRIu64" blocks saved (%"PRIu64"%% complete)",
			       blks_saved, percent);
			if (force)
				printf("\n");
			fflush(stdout);
		}
	}
}

/**
 * Open a file and prepare it for writing by savemeta()
 * out_fn: the path to the file, which will be truncated if it exists
 * gziplevel: 0   - do not compress the file,
 *            1-9 - use gzip compression level 1-9
 * Returns a struct metafd containing the opened file descriptor
 */
static struct metafd savemetaopen(char *out_fn, int gziplevel)
{
	struct metafd mfd = {0};
	char gzmode[3] = "w9";
	char dft_fn[] = DFT_SAVE_FILE;
	mode_t mask = umask(S_IXUSR | S_IRWXG | S_IRWXO);
	struct stat st;

	mfd.gziplevel = gziplevel;

	if (!out_fn) {
		out_fn = dft_fn;
		mfd.fd = mkstemp(out_fn);
	} else {
		mfd.fd = open(out_fn, O_RDWR | O_CREAT, 0644);
	}
	umask(mask);
	mfd.filename = out_fn;

	if (mfd.fd < 0) {
		fprintf(stderr, "Can't open %s: %s\n", out_fn, strerror(errno));
		exit(1);
	}
	if (fstat(mfd.fd, &st) == -1) {
		fprintf(stderr, "Failed to stat %s: %s\n", out_fn, strerror(errno));
		exit(1);
	}
	if (S_ISREG(st.st_mode) && ftruncate(mfd.fd, 0)) {
		fprintf(stderr, "Can't truncate %s: %s\n", out_fn, strerror(errno));
		exit(1);
	}

	if (gziplevel > 0) {
		gzmode[1] = '0' + gziplevel;
		mfd.gzfd = gzdopen(mfd.fd, gzmode);
		if (!mfd.gzfd) {
			fprintf(stderr, "gzdopen error: %s\n", strerror(errno));
			exit(1);
		}
		gzbuffer(mfd.gzfd, (1<<20)); /* Increase zlib's buffers to 1MB */
	}

	return mfd;
}

/**
 * Write nbyte bytes from buf to a file opened with savemetaopen()
 * mfd: the file descriptor opened using savemetaopen()
 * buf: the buffer to write data from
 * nbyte: the number of bytes to write
 * Returns the number of bytes written from buf or -1 on error
 */
static ssize_t savemetawrite(struct metafd *mfd, const void *buf, size_t nbyte)
{
	ssize_t ret;
	int gzerr;
	const char *gzerrmsg;

	if (mfd->gziplevel == 0) {
		return write(mfd->fd, buf, nbyte);
	}

	ret = gzwrite(mfd->gzfd, buf, nbyte);
	if (ret != nbyte) {
		gzerrmsg = gzerror(mfd->gzfd, &gzerr);
		if (gzerr != Z_ERRNO) {
			fprintf(stderr, "Error: zlib: %s\n", gzerrmsg);
		}
	}
	return ret;
}

/**
 * Closes a file descriptor previously opened using savemetaopen()
 * mfd: the file descriptor previously opened using savemetaopen()
 * Returns 0 on success or -1 on error
 */
static int savemetaclose(struct metafd *mfd)
{
	int gzret;
	if (mfd->gziplevel > 0) {
		gzret = gzclose(mfd->gzfd);
		if (gzret == Z_STREAM_ERROR) {
			fprintf(stderr, "gzclose: file is not valid\n");
			return -1;
		} else if (gzret == Z_ERRNO) {
			return -1;
		}
	}
	return close(mfd->fd);
}

static int save_buf(struct metafd *mfd, const char *buf, uint64_t addr, unsigned blklen)
{
	struct saved_metablock *savedata;
	size_t outsz;

	/* No need to save trailing zeroes, but leave that for compression to
	   deal with when enabled as this adds a significant overhead */
	if (mfd->gziplevel == 0)
		for (; blklen > 0 && buf[blklen - 1] == '\0'; blklen--);

	if (blklen == 0) /* No significant data; skip. */
		return 0;

	outsz = sizeof(*savedata) + blklen;
	savedata = calloc(1, outsz);
	if (savedata == NULL) {
		perror("Failed to save block");
		exit(1);
	}
	savedata->blk = cpu_to_be64(addr);
	savedata->siglen = cpu_to_be16(blklen);
	memcpy(savedata + 1, buf, blklen);

	if (savemetawrite(mfd, savedata, outsz) != outsz) {
		fprintf(stderr, "write error: %s from %s:%d: block %"PRIu64"\n",
		        strerror(errno), __FUNCTION__, __LINE__, addr);
		free(savedata);
		exit(-1);
	}
	blks_saved++;
	free(savedata);
	return 0;
}

struct block_range {
	struct block_range *next;
	uint64_t start;
	unsigned len;
	unsigned *blktype;
	unsigned *blklen;
	char *buf;
};

static int block_range_prepare(struct block_range *br)
{
	br->buf = calloc(br->len, sbd.sd_bsize + sizeof(*br->blktype) + sizeof(*br->blklen));
	if (br->buf == NULL) {
		perror("Failed to allocate block range buffer");
		return 1;
	}
	br->blktype = (unsigned *)(br->buf + (br->len * sbd.sd_bsize));
	br->blklen = br->blktype + br->len;
	return 0;
}

static int block_range_check(struct block_range *br)
{
	if (br->start >= LGFS2_SB_ADDR(&sbd) && br->start + br->len <= sbd.fssize)
		return 0;

	fprintf(stderr, "Warning: bad range 0x%"PRIx64" (%u blocks) ignored.\n",
		br->start, br->len);
	free(br->buf);
	br->buf = NULL;
	return 1;
}

static void block_range_setinfo(struct block_range *br, uint64_t owner)
{
	for (unsigned i = 0; i < br->len; i++) {
		char *buf = br->buf + (i * sbd.sd_bsize);
		uint64_t addr = br->start + i;
		uint64_t _owner = (owner == 0) ? addr : owner;

		if (get_gfs_struct_info(buf, _owner, br->blktype + i, br->blklen + i) &&
		    !block_is_systemfile(_owner)) {
			br->blklen[i] = 0;
		}
	}
}

static void block_range_free(struct block_range **brp)
{
	free((*brp)->buf);
	free(*brp);
	*brp = NULL;
}

struct block_range_queue {
	struct block_range *tail;
	struct block_range **head;
};

static void block_range_queue_init(struct block_range_queue *q)
{
	q->head = &q->tail;
}

static void block_range_queue_insert(struct block_range_queue *q, struct block_range *br)
{
	*q->head = br;
	q->head = &br->next;
}

static struct block_range *block_range_queue_pop(struct block_range_queue *q)
{
	struct block_range *br = q->tail;

	q->tail = br->next;
	br->next = NULL;
	return br;
}

static int save_range(struct metafd *mfd, struct block_range *br)
{
	for (unsigned i = 0; i < br->len; i++) {
		int err;

		err = save_buf(mfd, br->buf + (i * sbd.sd_bsize), br->start + i, br->blklen[i]);
		if (err != 0)
			return err;
	}
	return 0;
}

static int check_read_range(int fd, struct block_range *br, uint64_t owner)
{
	size_t size;

	if (block_range_prepare(br) != 0)
		return 1;

	if (block_range_check(br) != 0)
		return 1;

	size = br->len * sbd.sd_bsize;
	if (pread(sbd.device_fd, br->buf, size, sbd.sd_bsize * br->start) != size) {
		fprintf(stderr, "Failed to read block range 0x%"PRIx64" (%u blocks): %s\n",
		        br->start, br->len, strerror(errno));
		free(br->buf);
		br->buf = NULL;
		return 1;
	}
	block_range_setinfo(br, owner);
	return 0;
}

static char *check_read_block(int fd, uint64_t blk, uint64_t owner, int *blktype, size_t *blklen)
{
	struct block_range br = {
		.start = blk,
		.len = 1
	};

	if (check_read_range(fd, &br, owner) != 0)
		return NULL;
	if (blklen != NULL)
		*blklen = *br.blklen;
	if (blktype != NULL)
		*blktype = *br.blktype;
	return br.buf;
}

/*
 * save_ea_block - save off an extended attribute block
 */
static void save_ea_block(struct metafd *mfd, char *buf, uint64_t owner)
{
	struct gfs2_ea_header *ea;
	uint32_t rec_len = 0;
	int e;

	for (e = sizeof(struct gfs2_meta_header); e < sbd.sd_bsize; e += rec_len) {
		uint64_t blk;
		int charoff, i;
		__be64 *b;

		ea = (void *)(buf + e);
		/* ea_num_ptrs and ea_name_len are u8 so no endianness worries */
		for (i = 0; i < ea->ea_num_ptrs; i++) {
			char *_buf;

			charoff = e + ea->ea_name_len +
				sizeof(struct gfs2_ea_header) +
				sizeof(uint64_t) - 1;
			charoff /= sizeof(uint64_t);
			b = (__be64 *)buf;
			b += charoff + i;
			blk = be64_to_cpu(*b);
			_buf = check_read_block(sbd.device_fd, blk, owner, NULL, NULL);
			if (_buf != NULL) {
				save_buf(mfd, _buf, blk, sbd.sd_bsize);
				free(_buf);
			}
		}
		rec_len = be32_to_cpu(ea->ea_rec_len);
		if (rec_len == 0)
			break;
	}
}

static void save_indirect_range(struct metafd *mfd, struct block_range **brp, uint64_t owner,
                                struct block_range_queue *q)
{
	struct block_range *br = *brp;

	if (check_read_range(sbd.device_fd, br, owner) != 0)
		return;

	save_range(mfd, br);
	for (unsigned i = 0; i < br->len; i++) {
		if (br->blktype[i] == GFS2_METATYPE_EA)
			save_ea_block(mfd, br->buf + (i * sbd.sd_bsize), owner);
	}
	if (q) {
		block_range_queue_insert(q, br);
		*brp = NULL; /* The list now has ownership of it */
	} else {
		free(br->buf);
		br->buf = NULL;
	}
}

static void save_indirect_blocks(struct metafd *mfd, char *buf, uint64_t owner,
                                 struct block_range_queue *q, unsigned headsize)
{
	uint64_t old_block = 0, indir_block;
	struct block_range *br = NULL;
	__be64 *ptr;

	for (ptr = (__be64 *)(buf + headsize);
	     (char *)ptr < (buf + sbd.sd_bsize); ptr++) {
		if (!*ptr)
			continue;

		indir_block = be64_to_cpu(*ptr);
		if (indir_block == old_block)
			continue;
		old_block = indir_block;

		if (br == NULL) {
new_range:
			br = calloc(1, sizeof(*br));
			if (br == NULL) {
				perror("Failed to save indirect blocks");
				return;
			}
			br->start = indir_block;
			br->len = 1;
		} else if (indir_block == br->start + br->len) {
			br->len++;
		} else {
			save_indirect_range(mfd, &br, owner, q);
			if (br == NULL) /* This one was queued up for later */
				goto new_range;
			br->start = indir_block;
			br->len = 1;
		}
	}
	if (br != NULL && br->start != 0)
		save_indirect_range(mfd, &br, owner, q);
	free(br);
}

static int save_leaf_chain(struct metafd *mfd, struct gfs2_sbd *sdp, char *buf)
{
	struct gfs2_leaf *leaf = (struct gfs2_leaf *)buf;

	while (leaf->lf_next != 0) {
		uint64_t blk = be64_to_cpu(leaf->lf_next);
		ssize_t r;

		if (gfs2_check_range(sdp, blk) != 0)
			return 0;

		r = pread(sdp->device_fd, buf, sdp->sd_bsize, sdp->sd_bsize * blk);
		if (r != sdp->sd_bsize) {
			fprintf(stderr, "Failed to read leaf block %"PRIx64": %s\n",
			        blk, strerror(errno));
			return 1;
		}
		report_progress(blk, 0);
		if (gfs2_check_meta(buf, GFS2_METATYPE_LF) == 0) {
			int ret = save_buf(mfd, buf, blk, sdp->sd_bsize);
			if (ret != 0)
				return ret;
		}
		leaf = (struct gfs2_leaf *)buf;
	}
	return 0;
}

static void save_leaf_blocks(struct metafd *mfd, struct block_range_queue *q)
{
	while (q->tail != NULL) {
		struct block_range *br = q->tail;

		for (unsigned i = 0; i < br->len; i++) {
			char *buf = br->buf + (i * sbd.sd_bsize);

			save_leaf_chain(mfd, &sbd, buf);
		}
		q->tail = br->next;
		block_range_free(&br);
	}
}

/*
 * save_inode_data - save off important data associated with an inode
 *
 * mfd - destination file descriptor
 * buf - buffer containing the inode block
 * iblk - block number of the inode to save the data for
 *
 * For user files, we don't want anything except all the indirect block
 * pointers that reside on blocks on all but the highest height.
 *
 * For system files like statfs and inum, we want everything because they
 * may contain important clues and no user data.
 *
 * For file system journals, the "data" is a mixture of metadata and
 * journaled data.  We want all the metadata and none of the user data.
 */
static void save_inode_data(struct metafd *mfd, char *ibuf, uint64_t iblk)
{
	struct block_range_queue indq[GFS2_MAX_META_HEIGHT] = {{NULL}};
	struct gfs2_dinode *dip = (struct gfs2_dinode *)ibuf;
	struct gfs_dinode *dip1 = (struct gfs_dinode *)ibuf;
	uint16_t height;
	int is_exhash;

	for (unsigned i = 0; i < GFS2_MAX_META_HEIGHT; i++)
		block_range_queue_init(&indq[i]);

	height = be16_to_cpu(dip->di_height);

	/* If this is a user inode, we don't follow to the file height.
	   We stop one level less.  That way we save off the indirect
	   pointer blocks but not the actual file contents. The exception
	   is directories, where the height represents the level at which
	   the hash table exists, and we have to save the directory data. */

	is_exhash = (S_ISDIR(be32_to_cpu(dip->di_mode)) ||
	             (sbd.gfs1 && be16_to_cpu(dip1->di_type) == GFS_FILE_DIR)) &&
	             be32_to_cpu(dip->di_flags) & GFS2_DIF_EXHASH;
	if (is_exhash)
		height++;
	else if (height > 0 && !(be32_to_cpu(dip->di_flags) & GFS2_DIF_SYSTEM) &&
		 !block_is_systemfile(iblk) && !S_ISDIR(be32_to_cpu(dip->di_mode)))
		height--;

	if (height == 1)
		save_indirect_blocks(mfd, ibuf, iblk, NULL, sizeof(*dip));
	else if (height > 1)
		save_indirect_blocks(mfd, ibuf, iblk, &indq[0], sizeof(*dip));

	for (unsigned i = 1; i < height; i++) {
		struct block_range_queue *nextq = &indq[i];

		if (!is_exhash && i == height - 1)
			nextq = NULL;

		while (indq[i - 1].tail != NULL) {
			struct block_range *q = block_range_queue_pop(&indq[i - 1]);

			for (unsigned j = 0; j < q->len; j++) {
				char *_buf = q->buf + (j * sbd.sd_bsize);

				save_indirect_blocks(mfd, _buf, iblk, nextq, sizeof(dip->di_header));
			}
			report_progress(q->start + q->len, 0);
			block_range_free(&q);
		}
	}
	if (is_exhash)
		save_leaf_blocks(mfd, &indq[height - 1]);
	if (dip->di_eattr) { /* if this inode has extended attributes */
		size_t blklen;
		uint64_t blk;
		int mhtype;
		char *buf;

		blk = be64_to_cpu(dip->di_eattr);
		buf = check_read_block(sbd.device_fd, blk, iblk, &mhtype, &blklen);
		if (buf != NULL) {
			save_buf(mfd, buf, blk, blklen);
			if (mhtype == GFS2_METATYPE_EA)
				save_ea_block(mfd, buf, iblk);
			else if (mhtype == GFS2_METATYPE_IN)
				save_indirect_blocks(mfd, buf, iblk, NULL, sizeof(dip->di_header));
			free(buf);
		}
	}
}

static void get_journal_inode_blocks(void)
{
	int journal;

	journals_found = 0;
	memset(journal_blocks, 0, sizeof(journal_blocks));
	/* Save off all the journals--but only the metadata.
	 * This is confusing so I'll explain.  The journals contain important 
	 * metadata.  However, in gfs2 the journals are regular files within
	 * the system directory.  Since they're regular files, the blocks
	 * within the journals are considered data, not metadata.  Therefore,
	 * they won't have been saved by the code above.  We want to dump
	 * these blocks, but we have to be careful.  We only care about the
	 * journal blocks that look like metadata, and we need to not save
	 * journaled user data that may exist there as well. */
	for (journal = 0; ; journal++) { /* while journals exist */
		uint64_t jblock;
		int amt;
		struct gfs2_inode *j_inode = NULL;

		if (sbd.gfs1) {
			struct gfs_jindex *ji;
			char jbuf[sizeof(struct gfs_jindex)];

			j_inode = lgfs2_gfs_inode_read(&sbd, sbd.sd_jindex_di.in_addr);
			if (j_inode == NULL) {
				fprintf(stderr, "Error reading journal inode: %s\n", strerror(errno));
				return;
			}
			amt = gfs2_readi(j_inode, (void *)&jbuf,
					 journal * sizeof(struct gfs_jindex),
					 sizeof(struct gfs_jindex));
			inode_put(&j_inode);
			if (!amt)
				break;
			ji = (struct gfs_jindex *)jbuf;
			jblock = be64_to_cpu(ji->ji_addr);
			gfs1_journal_size = (uint64_t)be32_to_cpu(ji->ji_nsegment) * 16;
		} else {
			if (journal + 3 > indirect->ii[0].dirents)
				break;
			jblock = indirect->ii[0].dirent[journal + 2].inum.in_addr;
		}
		journal_blocks[journals_found++] = jblock;
	}
}

static void save_allocated_range(struct metafd *mfd, struct block_range *br)
{
	if (check_read_range(sbd.device_fd, br, 0) != 0)
		return;

	save_range(mfd, br);
	for (unsigned i = 0; i < br->len; i++) {
		char *buf = br->buf + (i * sbd.sd_bsize);

		if (br->blktype[i] == GFS2_METATYPE_DI)
			save_inode_data(mfd, buf, br->start + i);
	}
	free(br->buf);
}

static void save_allocated(struct rgrp_tree *rgd, struct metafd *mfd)
{
	uint64_t blk = 0;
	unsigned i, j, m;
	uint64_t *ibuf = malloc(sbd.sd_bsize * GFS2_NBBY * sizeof(uint64_t));

	for (i = 0; i < rgd->rt_length; i++) {
		struct block_range br = {0};

		m = lgfs2_bm_scan(rgd, i, ibuf, GFS2_BLKST_DINODE);

		for (j = 0; j < m; j++) {
			blk = ibuf[j];
			if (br.start == 0) {
				br.start = blk;
				br.len = 1;
			} else if (blk == br.start + br.len) {
				br.len++;
			} else {
				save_allocated_range(mfd, &br);
				br.start = blk;
				br.len = 1;
			}
			report_progress(blk, 0);
		}
		if (br.start != 0)
			save_allocated_range(mfd, &br);

		if (!sbd.gfs1)
			continue;

		/* For gfs1, Save off the free/unlinked meta blocks too.
		 * If we don't, we may run into metadata allocation issues. */
		m = lgfs2_bm_scan(rgd, i, ibuf, GFS2_BLKST_UNLINKED);
		for (j = 0; j < m; j++) {
			size_t blen;
			char *buf = check_read_block(sbd.device_fd, blk, blk, NULL, &blen);
			if (buf != NULL) {
				save_buf(mfd, buf, blk, blen);
				free(buf);
			}
		}
	}
	free(ibuf);
}

static char *rgrp_read(struct gfs2_sbd *sdp, uint64_t addr, unsigned blocks)
{
	size_t len = blocks * sdp->sd_bsize;
	off_t off = addr * sdp->sd_bsize;
	char *buf;

	if (blocks == 0 || gfs2_check_range(sdp, addr))
		return NULL;

	buf = calloc(1, len);
	if (buf == NULL)
		return NULL;

	if (pread(sdp->device_fd, buf, len, off) != len) {
		free(buf);
		return NULL;
	}
	return buf;
}

static void save_rgrp(struct gfs2_sbd *sdp, struct metafd *mfd, struct rgrp_tree *rgd, int withcontents)
{
	uint64_t addr = rgd->rt_addr;
	char *buf;

	buf = rgrp_read(sdp, rgd->rt_addr, rgd->rt_length);
	if (buf == NULL)
		return;

	for (unsigned i = 0; i < rgd->rt_length; i++)
		rgd->bits[i].bi_data = buf + (i * sdp->sd_bsize);

	log_debug("RG at %"PRIu64" is %"PRIu32" long\n", addr, rgd->rt_length);
	/* Save the rg and bitmaps */
	for (unsigned i = 0; i < rgd->rt_length; i++) {
		report_progress(rgd->rt_addr + i, 0);
		save_buf(mfd, buf + (i * sdp->sd_bsize), rgd->rt_addr + i, sdp->sd_bsize);
	}
	/* Save the other metadata: inodes, etc. if mode is not 'savergs' */
	if (withcontents)
		save_allocated(rgd, mfd);

	free(buf);
	for (unsigned i = 0; i < rgd->rt_length; i++)
		rgd->bits[i].bi_data = NULL;
}

static int save_header(struct metafd *mfd, uint64_t fsbytes)
{
	struct savemeta_header smh = {
		.sh_magic = cpu_to_be32(SAVEMETA_MAGIC),
		.sh_format = cpu_to_be32(SAVEMETA_FORMAT),
		.sh_time = cpu_to_be64(time(NULL)),
		.sh_fs_bytes = cpu_to_be64(fsbytes)
	};

	if (savemetawrite(mfd, (char *)(&smh), sizeof(smh)) != sizeof(smh))
		return -1;
	return 0;
}

static int parse_header(char *buf, struct savemeta *sm)
{
	struct savemeta_header *smh = (void *)buf;

	if (be32_to_cpu(smh->sh_magic) != SAVEMETA_MAGIC) {
		printf("No valid file header found. Falling back to old format...\n");
		return 1;
	}
	if (be32_to_cpu(smh->sh_format) > SAVEMETA_FORMAT) {
		printf("This version of gfs2_edit is too old to restore this metadata format.\n");
		return -1;
	}
	sm->sm_format = be32_to_cpu(smh->sh_format);
	sm->sm_time = be64_to_cpu(smh->sh_time);
	sm->sm_fs_bytes = be64_to_cpu(smh->sh_fs_bytes);
	printf("Metadata saved at %s", ctime(&sm->sm_time)); /* ctime() adds \n */
	printf("File system size %.2fGB\n", sm->sm_fs_bytes / ((float)(1 << 30)));
	return 0;
}

void savemeta(char *out_fn, int saveoption, int gziplevel)
{
	struct metafd mfd;
	struct osi_node *n;
	uint64_t sb_addr;
	int err = 0;
	char *buf;

	sbd.md.journals = 1;

	mfd = savemetaopen(out_fn, gziplevel);

	blks_saved = 0;
	if (sbd.gfs1)
		sbd.sd_bsize = sbd.sd_bsize;
	printf("There are %"PRIu64" blocks of %u bytes in the filesystem.\n",
	       sbd.fssize, sbd.sd_bsize);

	printf("Filesystem size: %.2fGB\n", (sbd.fssize * sbd.sd_bsize) / ((float)(1 << 30)));
	get_journal_inode_blocks();

	err = init_per_node_lookup();
	if (err)
		exit(1);

	/* Write the savemeta file header */
	err = save_header(&mfd, sbd.fssize * sbd.sd_bsize);
	if (err) {
		perror("Failed to write metadata file header");
		exit(1);
	}
	/* Save off the superblock */
	sb_addr = GFS2_SB_ADDR * GFS2_BASIC_BLOCK / sbd.sd_bsize;
	buf = check_read_block(sbd.device_fd, sb_addr, 0, NULL, NULL);
	if (buf != NULL) {
		if (sbd.gfs1)
			save_buf(&mfd, buf, sb_addr, sizeof(struct gfs_sb));
		else
			save_buf(&mfd, buf, sb_addr, sizeof(struct gfs2_sb));
		free(buf);
	}
	/* If this is gfs1, save off the rindex because it's not
	   part of the file system as it is in gfs2. */
	if (sbd.gfs1) {
		uint64_t blk;
		int j;

		blk = sbd.sd_rindex_di.in_addr;
		buf = check_read_block(sbd.device_fd, blk, blk, NULL, NULL);
		if (buf != NULL) {
			save_buf(&mfd, buf, blk, sbd.sd_bsize);
			save_inode_data(&mfd, buf, blk);
			free(buf);
		}
		/* In GFS1, journals aren't part of the RG space */
		for (j = 0; j < journals_found; j++) {
			uint64_t jb = journal_blocks[j];

			log_debug("Saving journal #%d\n", j + 1);
			for (blk = jb; blk < (jb + gfs1_journal_size); blk++) {
				size_t blen;

				buf = check_read_block(sbd.device_fd, blk, blk, NULL, &blen);
				if (buf != NULL) {
					save_buf(&mfd, buf, blk, blen);
					free(buf);
				}
			}
		}
	}
	/* Walk through the resource groups saving everything within */
	for (n = osi_first(&sbd.rgtree); n; n = osi_next(n)) {
		struct rgrp_tree *rgd;

		rgd = (struct rgrp_tree *)n;
		save_rgrp(&sbd, &mfd, rgd, (saveoption != 2));
	}
	/* Clean up */
	/* There may be a gap between end of file system and end of device */
	/* so we tell the user that we've processed everything. */
	report_progress(sbd.fssize, 1);
	printf("\nMetadata saved to file %s ", mfd.filename);
	if (mfd.gziplevel) {
		printf("(gzipped, level %d).\n", mfd.gziplevel);
	} else {
		printf("(uncompressed).\n");
	}
	savemetaclose(&mfd);
	close(sbd.device_fd);
	destroy_per_node_lookup();
	free(indirect);
	gfs2_rgrp_free(&sbd, &sbd.rgtree);
	exit(0);
}

static char *restore_block(struct metafd *mfd, uint64_t *blk, uint16_t *siglen)
{
	struct saved_metablock *svb;
	const char *errstr;
	char *buf = NULL;

	svb = (struct saved_metablock *)(restore_buf_next(mfd, sizeof(*svb)));
	if (svb == NULL)
		goto nobuffer;
	*blk = be64_to_cpu(svb->blk);
	*siglen = be16_to_cpu(svb->siglen);

	if (sbd.fssize && *blk >= sbd.fssize) {
		fprintf(stderr, "Error: File system is too small to restore this metadata.\n");
		fprintf(stderr, "File system is %"PRIu64" blocks. Restore block = %"PRIu64"\n",
		        sbd.fssize, *blk);
		return NULL;
	}

	if (*siglen > sbd.sd_bsize) {
		fprintf(stderr, "Bad record length: %u for block %"PRIu64" (0x%"PRIx64").\n",
			*siglen, *blk, *blk);
		return NULL;
	}

	buf = restore_buf_next(mfd, *siglen);
	if (buf != NULL) {
		return buf;
	}
nobuffer:
	if (mfd->eof)
		return NULL;

	errstr = mfd->strerr(mfd);
	fprintf(stderr, "Failed to restore block: %s\n", errstr);
	return NULL;
}

static int restore_super(struct metafd *mfd, void *buf, int printonly)
{
	int ret;

	lgfs2_sb_in(&sbd, buf);
	ret = check_sb(buf);
	if (ret < 0) {
		fprintf(stderr, "Error: Invalid superblock in metadata file.\n");
		return -1;
	}
	if (ret == 1)
		sbd.gfs1 = 1;
	sbd.sd_bsize = sbd.sd_bsize;
	if ((!printonly) && lgfs2_sb_write(&sbd, sbd.device_fd)) {
		fprintf(stderr, "Failed to write superblock\n");
		return -1;
	}
	blks_saved++;
	return 0;
}

static int restore_data(int fd, struct metafd *mfd, int printonly)
{
	uint64_t writes = 0;
	char *buf;

	buf = calloc(1, sbd.sd_bsize);
	if (buf == NULL) {
		perror("Failed to restore data");
		exit(1);
	}

	while (TRUE) {
		uint16_t siglen = 0;
		uint64_t blk = 0;
		char *bp;

		bp = restore_block(mfd, &blk, &siglen);
		if (bp == NULL && mfd->eof)
			break;
		if (bp == NULL) {
			free(buf);
			return -1;
		}
		if (printonly) {
			if (printonly > 1 && printonly == blk) {
				display_block_type(bp, blk, TRUE);
				display_gfs2(bp);
				break;
			} else if (printonly == 1) {
				print_gfs2("%"PRId64" (l=0x%x): ", blks_saved, siglen);
				display_block_type(bp, blk, TRUE);
			}
		} else {
			report_progress(blk, 0);
			memcpy(buf, bp, siglen);
			memset(buf + siglen, 0, sbd.sd_bsize - siglen);
			if (pwrite(fd, buf, sbd.sd_bsize, blk * sbd.sd_bsize) != sbd.sd_bsize) {
				fprintf(stderr, "write error: %s from %s:%d: block %"PRIu64" (0x%"PRIx64")\n",
					strerror(errno), __FUNCTION__, __LINE__, blk, blk);
				free(buf);
				return -1;
			}
			writes++;
			if (writes % 1000 == 0)
				fsync(fd);
		}
		blks_saved++;
	}
	if (!printonly)
		report_progress(sbd.fssize, 1);
	free(buf);
	return 0;
}

static void complain(const char *complaint)
{
	fprintf(stderr, "%s\n", complaint);
	die("Format is: \ngfs2_edit restoremeta <file to restore> "
	    "<dest file system>\n");
}

static int restore_init(const char *path, struct metafd *mfd, int printonly)
{
	struct savemeta sm = {0};
	struct gfs2_sb rsb;
	uint16_t sb_siglen;
	char *end;
	char *bp;
	int ret;

	blks_saved = 0;
	restore_buf = malloc(RESTORE_BUF_SIZE);
	if (restore_buf == NULL) {
		perror("Restore failed");
		return -1;
	}
	restore_off = 0;
	restore_left = 0;

	mfd->filename = path;
	mfd->fd = open(path, O_RDONLY|O_CLOEXEC);
	if (mfd->fd < 0) {
		perror("Could not open metadata file");
		return 1;
	}
	if (restore_try_bzip(mfd) != 0 &&
	    restore_try_gzip(mfd) != 0) {
		fprintf(stderr, "Failed to read metadata file header and superblock\n");
		return -1;
	}
	bp = restore_buf;
	ret = parse_header(bp, &sm);
	if (ret == 0) {
		bp = restore_buf + sizeof(struct savemeta_header);
		restore_off = sizeof(struct savemeta_header);
	} else if (ret == -1) {
		return -1;
	}
	/* Scan for the position of the superblock. Required to support old formats(?). */
	end = &restore_buf[256 + sizeof(struct saved_metablock) + sizeof(struct gfs2_meta_header)];
	while (bp <= end) {
		memcpy(&rsb, bp + sizeof(struct saved_metablock), sizeof(rsb));
		sb_siglen = be16_to_cpu(((struct saved_metablock *)bp)->siglen);
		if (be32_to_cpu(rsb.sb_header.mh_magic) == GFS2_MAGIC &&
		    be32_to_cpu(rsb.sb_header.mh_type) == GFS2_METATYPE_SB)
			break;
		bp++;
	}
	if (bp > end) {
		fprintf(stderr, "No superblock found in metadata file\n");
		return -1;
	}
	bp += sizeof(struct saved_metablock);
	ret = restore_super(mfd, &rsb, printonly);
	if (ret != 0)
		return ret;

	if (sm.sm_fs_bytes > 0) {
		sbd.fssize = sm.sm_fs_bytes / sbd.sd_bsize;
		printf("Saved file system size is %"PRIu64" blocks, %.2fGB\n",
		       sbd.fssize, sm.sm_fs_bytes / ((float)(1 << 30)));
	}
	printf("Block size is %uB\n", sbd.sd_bsize);
	printf("This is gfs%c metadata.\n", sbd.gfs1 ? '1': '2');
	if (printonly > 1 && printonly == LGFS2_SB_ADDR(&sbd)) {
		display_block_type(bp, LGFS2_SB_ADDR(&sbd), TRUE);
		display_gfs2(bp);
	} else if (printonly == 1) {
		print_gfs2("0 (l=0x%x): ", sb_siglen);
		display_block_type(bp, LGFS2_SB_ADDR(&sbd), TRUE);
	}
	bp += sb_siglen;
	restore_off = bp - restore_buf;
	restore_left -= restore_off;
	return 0;
}

void restoremeta(const char *in_fn, const char *out_device, uint64_t printonly)
{
	struct metafd mfd = {0};
	int error;

	termlines = 0;
	if (!in_fn)
		complain("No source file specified.");
	if (!printonly && !out_device)
		complain("No destination file system specified.");

	if (!printonly) {
		sbd.device_fd = open(out_device, O_RDWR);
		if (sbd.device_fd < 0)
			die("Can't open destination file system %s: %s\n",
			    out_device, strerror(errno));
	} else if (out_device) /* for printsavedmeta, the out_device is an
				  optional block no */
		printonly = check_keywords(out_device);

	error = restore_init(in_fn, &mfd, printonly);
	if (error != 0)
		exit(error);

	if (!printonly) {
		uint64_t space = lseek(sbd.device_fd, 0, SEEK_END) / sbd.sd_bsize;
		printf("There are %"PRIu64" free blocks on the destination device.\n", space);
	}

	error = restore_data(sbd.device_fd, &mfd, printonly);
	printf("File %s %s %s.\n", in_fn,
	       (printonly ? "print" : "restore"),
	       (error ? "error" : "successful"));

	mfd.close(&mfd);
	if (!printonly)
		close(sbd.device_fd);
	free(indirect);
	exit(error);
}
