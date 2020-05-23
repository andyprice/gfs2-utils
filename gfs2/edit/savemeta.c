#include "clusterautoconfig.h"

#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>
#include <sys/types.h>
#include <linux/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <curses.h>
#include <term.h>
#include <sys/ioctl.h>
#include <limits.h>
#include <sys/time.h>
#include <linux/gfs2_ondisk.h>
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
	uint32_t sh_magic;
#define SAVEMETA_FORMAT (1)
	uint32_t sh_format; /* In case we want to change the layout */
	uint64_t sh_time; /* When savemeta was run */
	uint64_t sh_fs_bytes; /* Size of the fs */
	uint8_t __reserved[104];
};

struct saved_metablock {
	uint64_t blk;
	uint16_t siglen; /* significant data length */
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

char *restore_buf;
ssize_t restore_left;
off_t restore_off;
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
extern char *device;

static int block_is_a_journal(uint64_t blk)
{
	int j;

	for (j = 0; j < journals_found; j++)
		if (blk == journal_blocks[j])
			return TRUE;
	return FALSE;
}

struct osi_root per_node_tree;
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

	do_dinode_extended(&per_node_di->i_di, per_node_di->i_bh->b_data);
	inode_put(&per_node_di);

	for (i = 0; i < indirect_blocks; i++) {
		int d;
		for (d = 0; d < indirect->ii[i].dirents; d++) {
			int ret = insert_per_node_lookup(indirect->ii[i].dirent[d].block);
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

static size_t di_save_len(struct gfs2_buffer_head *bh, uint64_t owner)
{
	struct gfs2_inode *inode;
	struct gfs2_dinode *dn;
	size_t len;

	if (sbd.gfs1)
		inode = lgfs2_gfs_inode_get(&sbd, bh->b_data);
	else
		inode = lgfs2_inode_get(&sbd, bh);

	if (inode == NULL) {
		fprintf(stderr, "Error reading inode at %"PRIu64": %s\n",
		        bh->b_blocknr, strerror(errno));
		return 0; /* Skip the block */
	}
	dn = &inode->i_di;
	len = sizeof(struct gfs2_dinode);

	/* Do not save (user) data from the inode block unless they are
	   indirect pointers, dirents, symlinks or fs internal data */
	if (dn->di_height != 0 ||
	    S_ISDIR(dn->di_mode) ||
	    S_ISLNK(dn->di_mode) ||
	    (sbd.gfs1 && dn->__pad1 == GFS_FILE_DIR) ||
	    block_is_systemfile(owner))
		len = sbd.bsize;

	inode_put(&inode);
	return len;
}

/*
 * get_gfs_struct_info - get block type and structure length
 *
 * @lbh - The block buffer to examine
 * @owner - The block address of the parent structure
 * @block_type - pointer to integer to hold the block type
 * @gstruct_len - pointer to integer to hold the structure length
 *
 * returns: 0 if successful
 *          -1 if this isn't gfs metadata.
 */
static int get_gfs_struct_info(struct gfs2_buffer_head *lbh, uint64_t owner,
                               int *block_type, size_t *gstruct_len)
{
	struct gfs2_meta_header mh;

	if (block_type != NULL)
		*block_type = 0;
	*gstruct_len = sbd.bsize;

	gfs2_meta_header_in(&mh, lbh->b_data);
	if (mh.mh_magic != GFS2_MAGIC)
		return -1;

	if (block_type != NULL)
		*block_type = mh.mh_type;

	switch (mh.mh_type) {
	case GFS2_METATYPE_SB:   /* 1 (superblock) */
		*gstruct_len = sizeof(struct gfs_sb);
		break;
	case GFS2_METATYPE_RG:   /* 2 (rsrc grp hdr) */
		*gstruct_len = sbd.bsize; /*sizeof(struct gfs_rgrp);*/
		break;
	case GFS2_METATYPE_RB:   /* 3 (rsrc grp bitblk) */
		*gstruct_len = sbd.bsize;
		break;
	case GFS2_METATYPE_DI:   /* 4 (disk inode) */
		*gstruct_len = di_save_len(lbh, owner);
		break;
	case GFS2_METATYPE_IN:   /* 5 (indir inode blklst) */
		*gstruct_len = sbd.bsize; /*sizeof(struct gfs_indirect);*/
		break;
	case GFS2_METATYPE_LF:   /* 6 (leaf dinode blklst) */
		*gstruct_len = sbd.bsize; /*sizeof(struct gfs_leaf);*/
		break;
	case GFS2_METATYPE_JD:   /* 7 (journal data) */
		*gstruct_len = sbd.bsize;
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
		*gstruct_len = sbd.bsize;
		break;
	case GFS2_METATYPE_EA:   /* 10 (extended attr hdr) */
		*gstruct_len = sbd.bsize;
		break;
	case GFS2_METATYPE_ED:   /* 11 (extended attr data) */
		*gstruct_len = sbd.bsize;
		break;
	default:
		*gstruct_len = sbd.bsize;
		break;
	}
	return 0;
}

/* Put out a warm, fuzzy message every second so the user     */
/* doesn't think we hung.  (This may take a long time).       */
/* We only check whether to report every one percent because  */
/* checking every block kills performance.  We only report    */
/* every second because we don't need 100 extra messages in   */
/* logs made from verbose mode.                               */
static void warm_fuzzy_stuff(uint64_t wfsblock, int force)
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
			percent = (wfsblock * 100) / sbd.fssize;
			printf("%llu blocks processed, %llu saved (%llu%%)",
			       (unsigned long long)wfsblock,
			       (unsigned long long)blks_saved,
			       (unsigned long long)percent);
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

static int save_buf(struct metafd *mfd, char *buf, uint64_t addr, unsigned blklen)
{
	struct saved_metablock *savedata;
	size_t outsz;

	/* No need to save trailing zeroes */
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

static int save_bh(struct metafd *mfd, struct gfs2_buffer_head *savebh, uint64_t owner, int *blktype)
{
	size_t blklen;

	/* If this isn't metadata and isn't a system file, we don't want it.
	   Note that we're checking "owner" here rather than blk.  That's
	   because we want to know if the source inode is a system inode
	   not the block within the inode "blk". They may or may not
	   be the same thing. */
	if (get_gfs_struct_info(savebh, owner, blktype, &blklen) &&
	    !block_is_systemfile(owner) && owner != 0)
		return 0; /* Not metadata, and not system file, so skip it */

	return save_buf(mfd, savebh->b_data, savebh->b_blocknr, blklen);
}

static int save_block(int fd, struct metafd *mfd, uint64_t blk, uint64_t owner, int *blktype)
{
	struct gfs2_buffer_head *savebh;
	int err;

	if (gfs2_check_range(&sbd, blk) && blk != LGFS2_SB_ADDR(&sbd)) {
		fprintf(stderr, "Warning: bad pointer 0x%"PRIx64" ignored in block 0x%"PRIx64"\n",
		        blk, owner);
		return 0;
	}
	savebh = bread(&sbd, blk);
	if (savebh == NULL)
		return 1;
	err = save_bh(mfd, savebh, owner, blktype);
	brelse(savebh);
	return err;
}

/*
 * save_ea_block - save off an extended attribute block
 */
static void save_ea_block(struct metafd *mfd, char *buf, uint64_t owner)
{
	int e;
	struct gfs2_ea_header ea;

	for (e = sizeof(struct gfs2_meta_header); e < sbd.bsize; e += ea.ea_rec_len) {
		uint64_t blk, *b;
		int charoff, i;

		gfs2_ea_header_in(&ea, buf + e);
		for (i = 0; i < ea.ea_num_ptrs; i++) {
			charoff = e + ea.ea_name_len +
				sizeof(struct gfs2_ea_header) +
				sizeof(uint64_t) - 1;
			charoff /= sizeof(uint64_t);
			b = (uint64_t *)buf;
			b += charoff + i;
			blk = be64_to_cpu(*b);
			save_block(sbd.device_fd, mfd, blk, owner, NULL);
		}
		if (!ea.ea_rec_len)
			break;
	}
}

/*
 * save_indirect_blocks - save all indirect blocks for the given buffer
 */
static void save_indirect_blocks(struct metafd *mfd, osi_list_t *cur_list,
           struct gfs2_buffer_head *mybh, uint64_t owner, int height, int hgt)
{
	uint64_t old_block = 0, indir_block;
	uint64_t *ptr;
	int head_size, blktype;
	struct gfs2_buffer_head *nbh;

	head_size = (hgt > 1 ?
		     sizeof(struct gfs2_meta_header) :
		     sizeof(struct gfs2_dinode));

	for (ptr = (uint64_t *)(mybh->b_data + head_size);
	     (char *)ptr < (mybh->b_data + sbd.bsize); ptr++) {
		if (!*ptr)
			continue;
		indir_block = be64_to_cpu(*ptr);
		if (indir_block == old_block)
			continue;
		old_block = indir_block;
		save_block(sbd.device_fd, mfd, indir_block, owner, &blktype);
		if (blktype == GFS2_METATYPE_EA) {
			nbh = bread(&sbd, indir_block);
			save_ea_block(mfd, nbh->b_data, owner);
			brelse(nbh);
		}
		if (height != hgt && /* If not at max height and */
		    (!gfs2_check_range(&sbd, indir_block))) {
			nbh = bread(&sbd, indir_block);
			osi_list_add_prev(&nbh->b_altlist, cur_list);
			/* The buffer_head needs to be queued ahead, so
			   don't release it!
			   brelse(nbh);*/
		}
	} /* for all data on the indirect block */
}

static int save_leaf_chain(struct metafd *mfd, struct gfs2_sbd *sdp, uint64_t blk)
{
	struct gfs2_buffer_head *bh;
	struct gfs2_leaf leaf;

	do {
		if (gfs2_check_range(sdp, blk) != 0)
			return 0;
		bh = bread(sdp, blk);
		if (bh == NULL) {
			perror("Failed to read leaf block");
			return 1;
		}
		warm_fuzzy_stuff(blk, FALSE);
		if (gfs2_check_meta(bh->b_data, GFS2_METATYPE_LF) == 0) {
			int ret = save_bh(mfd, bh, blk, NULL);
			if (ret != 0) {
				brelse(bh);
				return ret;
			}
		}
		gfs2_leaf_in(&leaf, bh->b_data);
		brelse(bh);
		blk = leaf.lf_next;
	} while (leaf.lf_next != 0);

	return 0;
}

/*
 * save_inode_data - save off important data associated with an inode
 *
 * mfd - destination file descriptor
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
static void save_inode_data(struct metafd *mfd, uint64_t iblk)
{
	uint32_t height;
	struct gfs2_inode *inode;
	osi_list_t metalist[GFS2_MAX_META_HEIGHT];
	osi_list_t *prev_list, *cur_list, *tmp;
	struct gfs2_buffer_head *metabh, *mybh;
	int i;

	for (i = 0; i < GFS2_MAX_META_HEIGHT; i++)
		osi_list_init(&metalist[i]);
	metabh = bread(&sbd, iblk);
	if (sbd.gfs1) {
		inode = lgfs2_gfs_inode_get(&sbd, metabh->b_data);
	} else {
		inode = lgfs2_inode_get(&sbd, metabh);
	}
	if (inode == NULL) {
		perror("Failed to read inode");
		exit(-1);
	}
	height = inode->i_di.di_height;
	/* If this is a user inode, we don't follow to the file height.
	   We stop one level less.  That way we save off the indirect
	   pointer blocks but not the actual file contents. The exception
	   is directories, where the height represents the level at which
	   the hash table exists, and we have to save the directory data. */
	if (inode->i_di.di_flags & GFS2_DIF_EXHASH &&
	    (S_ISDIR(inode->i_di.di_mode) ||
	     (sbd.gfs1 && inode->i_di.__pad1 == GFS_FILE_DIR)))
		height++;
	else if (height && !(inode->i_di.di_flags & GFS2_DIF_SYSTEM) &&
		 !block_is_systemfile(iblk) && !S_ISDIR(inode->i_di.di_mode))
		height--;
	osi_list_add(&metabh->b_altlist, &metalist[0]);
        for (i = 1; i <= height; i++){
		prev_list = &metalist[i - 1];
		cur_list = &metalist[i];

		for (tmp = prev_list->next; tmp != prev_list; tmp = tmp->next){
			mybh = osi_list_entry(tmp, struct gfs2_buffer_head,
					      b_altlist);
			warm_fuzzy_stuff(iblk, FALSE);
			save_indirect_blocks(mfd, cur_list, mybh, iblk,
					     height, i);
		} /* for blocks at that height */
	} /* for height */
	/* free metalists */
	for (i = 0; i < GFS2_MAX_META_HEIGHT; i++) {
		cur_list = &metalist[i];
		while (!osi_list_empty(cur_list)) {
			mybh = osi_list_entry(cur_list->next,
					    struct gfs2_buffer_head,
					    b_altlist);
			if (mybh == inode->i_bh)
				osi_list_del(&mybh->b_altlist);
			else
				brelse(mybh);
		}
	}
	/* Process directory exhash inodes */
	if (S_ISDIR(inode->i_di.di_mode) &&
	    inode->i_di.di_flags & GFS2_DIF_EXHASH) {
		uint64_t  leaf_no, old_leaf = -1;
		int li;

		for (li = 0; li < (1 << inode->i_di.di_depth); li++) {
			if (lgfs2_get_leaf_ptr(inode, li, &leaf_no)) {
				fprintf(stderr, "Could not read leaf index %d in dinode %"PRIu64"\n", li,
				        (uint64_t)inode->i_di.di_num.no_addr);
				exit(-1);
			}
			if (leaf_no != old_leaf && save_leaf_chain(mfd, &sbd, leaf_no) != 0)
				exit(-1);
			old_leaf = leaf_no;
		}
	}
	if (inode->i_di.di_eattr) { /* if this inode has extended attributes */
		struct gfs2_buffer_head *lbh;
		int mhtype;

		lbh = bread(&sbd, inode->i_di.di_eattr);
		save_block(sbd.device_fd, mfd, inode->i_di.di_eattr, iblk, &mhtype);
		if (mhtype == GFS2_METATYPE_EA)
			save_ea_block(mfd, lbh->b_data, iblk);
		else if (mhtype == GFS2_METATYPE_IN)
			save_indirect_blocks(mfd, cur_list, lbh, iblk, 2, 2);
		brelse(lbh);
	}
	inode_put(&inode);
	brelse(metabh);
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
			struct gfs_jindex ji;
			char jbuf[sizeof(struct gfs_jindex)];

			j_inode = lgfs2_gfs_inode_read(&sbd,
						 sbd1->sb_jindex_di.no_addr);
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
			gfs_jindex_in(&ji, jbuf);
			jblock = ji.ji_addr;
			gfs1_journal_size = (uint64_t)ji.ji_nsegment * 16;
		} else {
			if (journal + 3 > indirect->ii[0].dirents)
				break;
			jblock = indirect->ii[0].dirent[journal + 2].block;
		}
		journal_blocks[journals_found++] = jblock;
	}
}

static void save_allocated(struct rgrp_tree *rgd, struct metafd *mfd)
{
	int blktype;
	uint64_t blk = 0;
	unsigned i, j, m;
	uint64_t *ibuf = malloc(sbd.bsize * GFS2_NBBY * sizeof(uint64_t));

	for (i = 0; i < rgd->ri.ri_length; i++) {
		m = lgfs2_bm_scan(rgd, i, ibuf, GFS2_BLKST_DINODE);

		for (j = 0; j < m; j++) {
			blk = ibuf[j];
			warm_fuzzy_stuff(blk, FALSE);
			save_block(sbd.device_fd, mfd, blk, blk, &blktype);
			if (blktype == GFS2_METATYPE_DI)
				save_inode_data(mfd, blk);
		}
		if (!sbd.gfs1)
			continue;

		/* For gfs1, Save off the free/unlinked meta blocks too.
		 * If we don't, we may run into metadata allocation issues. */
		m = lgfs2_bm_scan(rgd, i, ibuf, GFS2_BLKST_UNLINKED);
		for (j = 0; j < m; j++) {
			save_block(sbd.device_fd, mfd, blk, blk, NULL);
		}
	}
	free(ibuf);
}

static char *rgrp_read(struct gfs2_sbd *sdp, uint64_t addr, unsigned blocks)
{
	size_t len = blocks * sdp->bsize;
	off_t off = addr * sdp->bsize;
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
	uint64_t addr = rgd->ri.ri_addr;
	char *buf;

	buf = rgrp_read(sdp, rgd->ri.ri_addr, rgd->ri.ri_length);
	if (buf == NULL)
		return;

	if (sdp->gfs1)
		gfs_rgrp_in((struct gfs_rgrp *)&rgd->rg, buf);
	else
		gfs2_rgrp_in(&rgd->rg, buf);

	for (unsigned i = 0; i < rgd->ri.ri_length; i++)
		rgd->bits[i].bi_data = buf + (i * sdp->bsize);

	log_debug("RG at %"PRIu64" is %"PRIu32" long\n", addr, (uint32_t)rgd->ri.ri_length);
	/* Save the rg and bitmaps */
	for (unsigned i = 0; i < rgd->ri.ri_length; i++) {
		warm_fuzzy_stuff(rgd->ri.ri_addr + i, FALSE);
		save_buf(mfd, buf + (i * sdp->bsize), rgd->ri.ri_addr + i, sdp->bsize);
	}
	/* Save the other metadata: inodes, etc. if mode is not 'savergs' */
	if (withcontents)
		save_allocated(rgd, mfd);

	free(buf);
	for (unsigned i = 0; i < rgd->ri.ri_length; i++)
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

static int parse_header(char *buf, struct savemeta_header *smh)
{
	struct savemeta_header *smh_be = (void *)buf;

	if (be32_to_cpu(smh_be->sh_magic) != SAVEMETA_MAGIC) {
		printf("No valid file header found. Falling back to old format...\n");
		return 1;
	}
	if (be32_to_cpu(smh_be->sh_format) > SAVEMETA_FORMAT) {
		printf("This version of gfs2_edit is too old to restore this metadata format.\n");
		return -1;
	}
	smh->sh_magic = be32_to_cpu(smh_be->sh_magic);
	smh->sh_format = be32_to_cpu(smh_be->sh_format);
	smh->sh_time = be64_to_cpu(smh_be->sh_time);
	smh->sh_fs_bytes = be64_to_cpu(smh_be->sh_fs_bytes);
	printf("Metadata saved at %s", ctime((time_t *)&smh->sh_time)); /* ctime() adds \n */
	printf("File system size %.2fGB\n", smh->sh_fs_bytes / ((float)(1 << 30)));
	return 0;
}

void savemeta(char *out_fn, int saveoption, int gziplevel)
{
	uint64_t jindex_block;
	struct gfs2_buffer_head *lbh;
	struct metafd mfd;
	struct osi_node *n;
	int err = 0;

	sbd.md.journals = 1;

	mfd = savemetaopen(out_fn, gziplevel);

	blks_saved = 0;
	if (sbd.gfs1)
		sbd.bsize = sbd.sd_sb.sb_bsize;
	printf("There are %llu blocks of %u bytes in the filesystem.\n",
	                     (unsigned long long)sbd.fssize, sbd.bsize);
	if (sbd.gfs1)
		jindex_block = sbd1->sb_jindex_di.no_addr;
	else
		jindex_block = masterblock("jindex");
	lbh = bread(&sbd, jindex_block);
	gfs2_dinode_in(&di, lbh->b_data);
	if (!sbd.gfs1)
		do_dinode_extended(&di, lbh->b_data);
	brelse(lbh);

	printf("Filesystem size: %.2fGB\n", (sbd.fssize * sbd.bsize) / ((float)(1 << 30)));
	get_journal_inode_blocks();

	err = init_per_node_lookup();
	if (err)
		exit(1);

	/* Write the savemeta file header */
	err = save_header(&mfd, sbd.fssize * sbd.bsize);
	if (err) {
		perror("Failed to write metadata file header");
		exit(1);
	}
	/* Save off the superblock */
	save_block(sbd.device_fd, &mfd, GFS2_SB_ADDR * GFS2_BASIC_BLOCK / sbd.bsize, 0, NULL);
	/* If this is gfs1, save off the rindex because it's not
	   part of the file system as it is in gfs2. */
	if (sbd.gfs1) {
		uint64_t blk;
		int j;

		blk = sbd1->sb_rindex_di.no_addr;
		save_block(sbd.device_fd, &mfd, blk, blk, NULL);
		save_inode_data(&mfd, blk);
		/* In GFS1, journals aren't part of the RG space */
		for (j = 0; j < journals_found; j++) {
			log_debug("Saving journal #%d\n", j + 1);
			for (blk = journal_blocks[j];
			     blk < journal_blocks[j] + gfs1_journal_size;
			     blk++)
				save_block(sbd.device_fd, &mfd, blk, blk, NULL);
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
	warm_fuzzy_stuff(sbd.fssize, TRUE);
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

static int restore_block(struct metafd *mfd, struct saved_metablock *svb, char **buf, uint16_t maxlen)
{
	struct saved_metablock *svb_be;
	const char *errstr;
	uint16_t checklen;

	svb_be = (struct saved_metablock *)(restore_buf_next(mfd, sizeof(*svb)));
	if (svb_be == NULL)
		goto read_err;
	svb->blk = be64_to_cpu(svb_be->blk);
	svb->siglen = be16_to_cpu(svb_be->siglen);

	if (sbd.fssize && svb->blk >= sbd.fssize) {
		fprintf(stderr, "Error: File system is too small to restore this metadata.\n");
		fprintf(stderr, "File system is %llu blocks. Restore block = %llu\n",
		        (unsigned long long)sbd.fssize, (unsigned long long)svb->blk);
		return -1;
	}

	if (maxlen)
		checklen = maxlen;
	else
		checklen = sbd.bsize;

	if (checklen && svb->siglen > checklen) {
		fprintf(stderr, "Bad record length: %u for block %"PRIu64" (0x%"PRIx64").\n",
			svb->siglen, svb->blk, svb->blk);
		return -1;
	}

	if (buf != NULL && maxlen != 0) {
		*buf = restore_buf_next(mfd, svb->siglen);
		if (*buf == NULL)
			goto read_err;
	}
	return 0;

read_err:
	if (mfd->eof)
		return 1;

	errstr = mfd->strerr(mfd);
	fprintf(stderr, "Failed to restore block: %s\n", errstr);
	return -1;
}

static int restore_super(struct metafd *mfd, char *buf, int printonly)
{
	int ret;

	gfs2_sb_in(&sbd.sd_sb, buf);
	sbd1 = (struct gfs_sb *)&sbd.sd_sb;
	ret = check_sb(&sbd.sd_sb);
	if (ret < 0) {
		fprintf(stderr, "Error: Invalid superblock in metadata file.\n");
		return -1;
	}
	if (ret == 1)
		sbd.gfs1 = 1;
	sbd.bsize = sbd.sd_sb.sb_bsize;
	if ((!printonly) && lgfs2_sb_write(&sbd.sd_sb, sbd.device_fd, sbd.bsize)) {
		fprintf(stderr, "Failed to write superblock\n");
		return -1;
	}
	printf("Block size is %uB\n", sbd.bsize);
	return 0;
}

static int restore_data(int fd, struct metafd *mfd, int printonly)
{
	struct saved_metablock savedata = {0};
	uint64_t writes = 0;
	char *buf;
	char *bp;

	buf = calloc(1, sbd.bsize);
	if (buf == NULL) {
		perror("Failed to restore data");
		exit(1);
	}

	blks_saved = 0;
	while (TRUE) {
		int err;

		err = restore_block(mfd, &savedata, &bp, sbd.bsize);
		if (err == 1)
			break;
		if (err != 0) {
			free(buf);
			return -1;
		}

		if (printonly) {
			if (printonly > 1 && printonly == savedata.blk) {
				display_block_type(bp, savedata.blk, TRUE);
				display_gfs2(bp);
				break;
			} else if (printonly == 1) {
				print_gfs2("%"PRId64" (l=0x%x): ", blks_saved, savedata.siglen);
				display_block_type(bp, savedata.blk, TRUE);
			}
		} else {
			warm_fuzzy_stuff(savedata.blk, FALSE);
			memcpy(buf, bp, savedata.siglen);
			memset(buf + savedata.siglen, 0, sbd.bsize - savedata.siglen);
			if (pwrite(fd, buf, sbd.bsize, savedata.blk * sbd.bsize) != sbd.bsize) {
				fprintf(stderr, "write error: %s from %s:%d: block %lld (0x%llx)\n",
					strerror(errno), __FUNCTION__, __LINE__,
					(unsigned long long)savedata.blk,
					(unsigned long long)savedata.blk);
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
		warm_fuzzy_stuff(sbd.fssize, 1);
	free(buf);
	return 0;
}

static void complain(const char *complaint)
{
	fprintf(stderr, "%s\n", complaint);
	die("Format is: \ngfs2_edit restoremeta <file to restore> "
	    "<dest file system>\n");
}

static int restore_init(const char *path, struct metafd *mfd, struct savemeta_header *smh, int printonly)
{
	struct gfs2_meta_header *sbmh;
	uint16_t sb_siglen;
	char *end;
	char *bp;
	int ret;

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
	ret = parse_header(bp, smh);
	if (ret == 0) {
		bp = restore_buf + sizeof(*smh);
		restore_off = sizeof(*smh);
	} else if (ret == -1) {
		return -1;
	}
	/* Scan for the position of the superblock. Required to support old formats(?). */
	end = &restore_buf[256 + sizeof(struct saved_metablock) + sizeof(*sbmh)];
	while (bp <= end) {
		sb_siglen = be16_to_cpu(((struct saved_metablock *)bp)->siglen);
		sbmh = (struct gfs2_meta_header *)(bp + sizeof(struct saved_metablock));
		if (sbmh->mh_magic == cpu_to_be32(GFS2_MAGIC) &&
		    sbmh->mh_type == cpu_to_be32(GFS2_METATYPE_SB))
			break;
		bp++;
	}
	if (bp > end) {
		fprintf(stderr, "No superblock found in metadata file\n");
		return -1;
	}
	ret = restore_super(mfd, bp + sizeof(struct saved_metablock), printonly);
	if (ret != 0)
		return ret;

	bp += sizeof(struct saved_metablock) + sb_siglen;
	restore_off = bp - restore_buf;
	restore_left -= restore_off;
	return 0;
}

void restoremeta(const char *in_fn, const char *out_device, uint64_t printonly)
{
	struct savemeta_header smh = {0};
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

	error = restore_init(in_fn, &mfd, &smh, printonly);
	if (error != 0)
		exit(error);

	if (smh.sh_fs_bytes > 0) {
		sbd.fssize = smh.sh_fs_bytes / sbd.bsize;
		printf("Saved file system size is %"PRIu64" blocks, %.2fGB\n",
		       sbd.fssize, smh.sh_fs_bytes / ((float)(1 << 30)));
	}

	printf("This is gfs%c metadata.\n", sbd.gfs1 ? '1': '2');

	if (!printonly) {
		uint64_t space = lseek(sbd.device_fd, 0, SEEK_END) / sbd.bsize;
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
