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
	char buf[];
/* This needs to be packed because old versions of gfs2_edit read and write the
   individual fields separately, so the hole after siglen must be eradicated
   before the struct reflects what's on disk. */
} __attribute__((__packed__));

struct metafd {
	int fd;
	gzFile gzfd;
	const char *filename;
	int gziplevel;
};

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

static int block_is_in_per_node(void)
{
	struct per_node_node *pnp = (struct per_node_node *)per_node_tree.osi_node;

	while (pnp) {
		if (block < pnp->block)
			pnp = (struct per_node_node *)pnp->node.osi_left;
		else if (block > pnp->block)
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

	do_dinode_extended(&per_node_di->i_di, per_node_di->i_bh);
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

static int block_is_systemfile(void)
{
	return block_is_jindex() || block_is_inum_file() ||
		block_is_statfs_file() || block_is_quota_file() ||
		block_is_rindex() || block_is_a_journal(block) ||
		block_is_per_node() || block_is_in_per_node();
}

/**
 * anthropomorphize - make a uint64_t number more human
 */
static const char *anthropomorphize(unsigned long long inhuman_value)
{
	const char *symbols = " KMGTPE";
	int i;
	unsigned long long val = inhuman_value, remainder = 0;
	static char out_val[32];

	memset(out_val, 0, sizeof(out_val));
	for (i = 0; i < 6 && val > 1024; i++) {
		remainder = val % 1024;
		val /= 1024;
	}
	sprintf(out_val, "%llu.%llu%cB", val, remainder, symbols[i]);
	return out_val;
}

/*
 * get_gfs_struct_info - get block type and structure length
 *
 * @block_type - pointer to integer to hold the block type
 * @struct_length - pointer to integet to hold the structure length
 *
 * returns: 0 if successful
 *          -1 if this isn't gfs metadata.
 */
static int get_gfs_struct_info(struct gfs2_buffer_head *lbh, int *block_type,
			       int *gstruct_len)
{
	struct gfs2_meta_header mh;
	struct gfs2_inode *inode;

	*block_type = 0;
	*gstruct_len = sbd.bsize;

	gfs2_meta_header_in(&mh, lbh);
	if (mh.mh_magic != GFS2_MAGIC)
		return -1;

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
		if (sbd.gfs1) {
			inode = lgfs2_gfs_inode_get(&sbd, lbh);
		} else {
			inode = lgfs2_inode_get(&sbd, lbh);
		}
		if (inode == NULL) {
			perror("Error reading inode");
			exit(-1);
		}
		if (S_ISDIR(inode->i_di.di_mode) ||
		     (sbd.gfs1 && inode->i_di.__pad1 == GFS_FILE_DIR))
			*gstruct_len = sbd.bsize;
		else if (!inode->i_di.di_height && !block_is_systemfile() &&
			 !S_ISDIR(inode->i_di.di_mode))
			*gstruct_len = sizeof(struct gfs2_dinode);
		else
			*gstruct_len = sbd.bsize;
		inode_put(&inode);
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
	struct metafd mfd = {-1, NULL, NULL, gziplevel};
	char gzmode[3] = "w9";
	char dft_fn[] = DFT_SAVE_FILE;
	mode_t mask = umask(S_IXUSR | S_IRWXG | S_IRWXO);

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

	if (ftruncate(mfd.fd, 0)) {
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

static int save_block(int fd, struct metafd *mfd, uint64_t blk)
{
	int blktype, blklen;
	size_t outsz;
	struct gfs2_buffer_head *savebh;
	struct saved_metablock *savedata;

	if (gfs2_check_range(&sbd, blk) && blk != sbd.sb_addr) {
		fprintf(stderr, "\nWarning: bad block pointer '0x%llx' "
			"ignored in block (block %llu (0x%llx))",
			(unsigned long long)blk,
			(unsigned long long)block, (unsigned long long)block);
		return 0;
	}
	savebh = bread(&sbd, blk);

	/* If this isn't metadata and isn't a system file, we don't want it.
	   Note that we're checking "block" here rather than blk.  That's
	   because we want to know if the source inode's "block" is a system
	   inode, not the block within the inode "blk". They may or may not
	   be the same thing. */
	if (get_gfs_struct_info(savebh, &blktype, &blklen) &&
	    !block_is_systemfile()) {
		brelse(savebh);
		return 0; /* Not metadata, and not system file, so skip it */
	}

	/* No need to save trailing zeroes */
	for (; blklen > 0 && savebh->b_data[blklen - 1] == '\0'; blklen--);

	if (blklen == 0) /* No significant data; skip. */
		return 0;

	outsz = sizeof(*savedata) + blklen;
	savedata = calloc(1, outsz);
	if (savedata == NULL) {
		perror("Failed to save block");
		exit(1);
	}
	savedata->blk = cpu_to_be64(blk);
	savedata->siglen = cpu_to_be16(blklen);
	memcpy(savedata->buf, savebh->b_data, blklen);

	if (savemetawrite(mfd, savedata, outsz) != outsz) {
		fprintf(stderr, "write error: %s from %s:%d: block %lld (0x%llx)\n",
		        strerror(errno), __FUNCTION__, __LINE__,
			(unsigned long long)savedata->blk,
			(unsigned long long)savedata->blk);
		free(savedata);
		exit(-1);
	}

	blks_saved++;
	free(savedata);
	brelse(savebh);
	return blktype;
}

/*
 * save_ea_block - save off an extended attribute block
 */
static void save_ea_block(struct metafd *mfd, struct gfs2_buffer_head *metabh)
{
	int e;
	struct gfs2_ea_header ea;

	for (e = sizeof(struct gfs2_meta_header); e < sbd.bsize; e += ea.ea_rec_len) {
		uint64_t blk, *b;
		int charoff, i;

		gfs2_ea_header_in(&ea, metabh->b_data + e);
		for (i = 0; i < ea.ea_num_ptrs; i++) {
			charoff = e + ea.ea_name_len +
				sizeof(struct gfs2_ea_header) +
				sizeof(uint64_t) - 1;
			charoff /= sizeof(uint64_t);
			b = (uint64_t *)(metabh->b_data);
			b += charoff + i;
			blk = be64_to_cpu(*b);
			save_block(sbd.device_fd, mfd, blk);
		}
		if (!ea.ea_rec_len)
			break;
	}
}

/*
 * save_indirect_blocks - save all indirect blocks for the given buffer
 */
static void save_indirect_blocks(struct metafd *mfd, osi_list_t *cur_list,
			  struct gfs2_buffer_head *mybh, int height, int hgt)
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
		blktype = save_block(sbd.device_fd, mfd, indir_block);
		if (blktype == GFS2_METATYPE_EA) {
			nbh = bread(&sbd, indir_block);
			save_ea_block(mfd, nbh);
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

/*
 * save_inode_data - save off important data associated with an inode
 *
 * mfd - destination file descriptor
 * block - block number of the inode to save the data for
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
static void save_inode_data(struct metafd *mfd)
{
	uint32_t height;
	struct gfs2_inode *inode;
	osi_list_t metalist[GFS2_MAX_META_HEIGHT];
	osi_list_t *prev_list, *cur_list, *tmp;
	struct gfs2_buffer_head *metabh, *mybh;
	int i;

	for (i = 0; i < GFS2_MAX_META_HEIGHT; i++)
		osi_list_init(&metalist[i]);
	metabh = bread(&sbd, block);
	if (sbd.gfs1) {
		inode = lgfs2_gfs_inode_get(&sbd, metabh);
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
		 !block_is_systemfile() && !S_ISDIR(inode->i_di.di_mode))
		height--;
	osi_list_add(&metabh->b_altlist, &metalist[0]);
        for (i = 1; i <= height; i++){
		prev_list = &metalist[i - 1];
		cur_list = &metalist[i];

		for (tmp = prev_list->next; tmp != prev_list; tmp = tmp->next){
			mybh = osi_list_entry(tmp, struct gfs2_buffer_head,
					      b_altlist);
			warm_fuzzy_stuff(block, FALSE);
			save_indirect_blocks(mfd, cur_list, mybh,
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
			if (leaf_no == old_leaf ||
			    gfs2_check_range(&sbd, leaf_no) != 0)
				continue;
			old_leaf = leaf_no;
			mybh = bread(&sbd, leaf_no);
			warm_fuzzy_stuff(block, FALSE);
			if (gfs2_check_meta(mybh, GFS2_METATYPE_LF) == 0)
				save_block(sbd.device_fd, mfd, leaf_no);
			brelse(mybh);
		}
	}
	if (inode->i_di.di_eattr) { /* if this inode has extended attributes */
		struct gfs2_meta_header mh;
		struct gfs2_buffer_head *lbh;

		lbh = bread(&sbd, inode->i_di.di_eattr);
		save_block(sbd.device_fd, mfd, inode->i_di.di_eattr);
		gfs2_meta_header_in(&mh, lbh);
		if (mh.mh_magic == GFS2_MAGIC &&
		    mh.mh_type == GFS2_METATYPE_EA)
			save_ea_block(mfd, lbh);
		else if (mh.mh_magic == GFS2_MAGIC &&
			 mh.mh_type == GFS2_METATYPE_IN)
			save_indirect_blocks(mfd, cur_list, lbh, 2, 2);
		else {
			if (mh.mh_magic == GFS2_MAGIC) /* if it's metadata */
				save_block(sbd.device_fd, mfd,
					   inode->i_di.di_eattr);
			fprintf(stderr,
				"\nWarning: corrupt extended "
				"attribute at block %llu (0x%llx) "
				"detected in inode %lld (0x%llx).\n",
				(unsigned long long)inode->i_di.di_eattr,
				(unsigned long long)inode->i_di.di_eattr,
				(unsigned long long)block,
				(unsigned long long)block);
		}
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
			gfs1_journal_size = ji.ji_nsegment * 16;
		} else {
			if (journal > indirect->ii[0].dirents - 3)
				break;
			jblock = indirect->ii[0].dirent[journal + 2].block;
		}
		journal_blocks[journals_found++] = jblock;
	}
}

static void save_allocated(struct rgrp_tree *rgd, struct metafd *mfd)
{
	int blktype;
	unsigned i, j, m;
	uint64_t *ibuf = malloc(sbd.bsize * GFS2_NBBY * sizeof(uint64_t));

	for (i = 0; i < rgd->ri.ri_length; i++) {
		m = lgfs2_bm_scan(rgd, i, ibuf, GFS2_BLKST_DINODE);

		for (j = 0; j < m; j++) {
			block = ibuf[j];
			warm_fuzzy_stuff(block, FALSE);
			blktype = save_block(sbd.device_fd, mfd, block);
			if (blktype == GFS2_METATYPE_DI)
				save_inode_data(mfd);
		}

		if (!sbd.gfs1)
			continue;

		/* For gfs1, Save off the free/unlinked meta blocks too.
		 * If we don't, we may run into metadata allocation issues. */
		m = lgfs2_bm_scan(rgd, i, ibuf, GFS2_BLKST_UNLINKED);
		for (j = 0; j < m; j++) {
			save_block(sbd.device_fd, mfd, block);
		}
	}
	free(ibuf);
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

static int read_header(gzFile gzin_fd, struct savemeta_header *smh)
{
	size_t rs;
	struct savemeta_header smh_be = {0};

	gzseek(gzin_fd, 0, SEEK_SET);
	rs = gzread(gzin_fd, &smh_be, sizeof(smh_be));
	if (rs == -1) {
		perror("Failed to read savemeta file header");
		return -1;
	}
	if (rs != sizeof(smh_be))
		return 1;

	smh->sh_magic = be32_to_cpu(smh_be.sh_magic);
	smh->sh_format = be32_to_cpu(smh_be.sh_format);
	smh->sh_time = be64_to_cpu(smh_be.sh_time);
	smh->sh_fs_bytes = be64_to_cpu(smh_be.sh_fs_bytes);

	return 0;
}

static int check_header(struct savemeta_header *smh)
{
	if (smh->sh_magic != SAVEMETA_MAGIC || smh->sh_format > SAVEMETA_FORMAT)
		return -1;
	printf("Metadata saved at %s", ctime((time_t *)&smh->sh_time)); /* ctime() adds \n */
	printf("File system size %s\n", anthropomorphize(smh->sh_fs_bytes));
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
	gfs2_dinode_in(&di, lbh);
	if (!sbd.gfs1)
		do_dinode_extended(&di, lbh);
	brelse(lbh);

	printf("Filesystem size: %s\n", anthropomorphize(sbd.fssize * sbd.bsize));
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
	save_block(sbd.device_fd, &mfd, GFS2_SB_ADDR * GFS2_BASIC_BLOCK / sbd.bsize);
	/* If this is gfs1, save off the rindex because it's not
	   part of the file system as it is in gfs2. */
	if (sbd.gfs1) {
		int j;

		block = sbd1->sb_rindex_di.no_addr;
		save_block(sbd.device_fd, &mfd, block);
		save_inode_data(&mfd);
		/* In GFS1, journals aren't part of the RG space */
		for (j = 0; j < journals_found; j++) {
			log_debug("Saving journal #%d\n", j + 1);
			for (block = journal_blocks[j];
			     block < journal_blocks[j] + gfs1_journal_size;
			     block++)
				save_block(sbd.device_fd, &mfd, block);
		}
	}
	/* Walk through the resource groups saving everything within */
	for (n = osi_first(&sbd.rgtree); n; n = osi_next(n)) {
		struct rgrp_tree *rgd;

		rgd = (struct rgrp_tree *)n;
		if (gfs2_rgrp_read(&sbd, rgd))
			continue;
		log_debug("RG at %lld (0x%llx) is %u long\n",
			  (unsigned long long)rgd->ri.ri_addr,
			  (unsigned long long)rgd->ri.ri_addr,
			  rgd->ri.ri_length);
		/* Save off the rg and bitmaps */
		for (block = rgd->ri.ri_addr;
		     block < rgd->ri.ri_data0; block++) {
			warm_fuzzy_stuff(block, FALSE);
			save_block(sbd.device_fd, &mfd, block);
		}
		/* Save off the other metadata: inodes, etc. if mode is not 'savergs' */
		if (saveoption != 2) {
			save_allocated(rgd, &mfd);
		}
		gfs2_rgrp_relse(rgd);
	}
	/* Clean up */
	/* There may be a gap between end of file system and end of device */
	/* so we tell the user that we've processed everything. */
	block = sbd.fssize;
	warm_fuzzy_stuff(block, TRUE);
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
	gfs2_rgrp_free(&sbd.rgtree);
	exit(0);
}

static off_t restore_init(gzFile gzfd, struct savemeta_header *smh)
{
	int err;
	unsigned i;
	size_t rs;
	char buf[256];
	off_t startpos = 0;
	struct saved_metablock *svb;
	struct gfs2_meta_header *sbmh;

	err = read_header(gzfd, smh);
	if (err < 0) {
		exit(1);
	} else if (check_header(smh) != 0) {
		printf("No valid file header found. Falling back to old format...\n");
	} else if (err == 0) {
		startpos = sizeof(*smh);
	}

	gzseek(gzfd, startpos, SEEK_SET);
	rs = gzread(gzfd, buf, sizeof(buf));
	if (rs != sizeof(buf)) {
		fprintf(stderr, "Error: File is too small.\n");
		exit(1);
	}
	/* Scan for the beginning of the file body. Required to support old formats(?). */
	for (i = 0; i < (256 - sizeof(*svb) - sizeof(*sbmh)); i++) {
		svb = (struct saved_metablock *)&buf[i];
		sbmh = (struct gfs2_meta_header *)svb->buf;
		if (sbmh->mh_magic == cpu_to_be32(GFS2_MAGIC) &&
		     sbmh->mh_type == cpu_to_be32(GFS2_METATYPE_SB))
			break;
	}
	if (i == (sizeof(buf) - sizeof(*svb) - sizeof(*sbmh)))
		i = 0;
	return startpos + i; /* File offset of saved sb */
}


static int restore_block(gzFile gzfd, struct saved_metablock *svb, uint16_t maxlen)
{
	int gzerr;
	int ret;
	uint16_t checklen;
	const char *errstr;

	ret = gzread(gzfd, svb, sizeof(*svb));
	if (ret < sizeof(*svb)) {
		goto gzread_err;
	}
	svb->blk = be64_to_cpu(svb->blk);
	svb->siglen = be16_to_cpu(svb->siglen);

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

	if (maxlen) {
		ret = gzread(gzfd, svb + 1, svb->siglen);
		if (ret < svb->siglen) {
			goto gzread_err;
		}
	}

	return 0;

gzread_err:
	if (gzeof(gzfd))
		return 1;

	errstr = gzerror(gzfd, &gzerr);
	if (gzerr == Z_ERRNO)
		errstr = strerror(errno);
	fprintf(stderr, "Failed to restore block: %s\n", errstr);
	return -1;
}

static int restore_super(gzFile gzfd, off_t pos)
{
	int ret;
	struct saved_metablock *svb;
	struct gfs2_buffer_head dummy_bh;
	size_t len = sizeof(*svb) + sizeof(struct gfs2_sb);

	svb = calloc(1, len);
	if (svb == NULL) {
		perror("Failed to restore super block");
		exit(1);
	}
	gzseek(gzfd, pos, SEEK_SET);
	ret = restore_block(gzfd, svb, sizeof(struct gfs2_sb));
	if (ret == 1) {
		fprintf(stderr, "Reached end of file while restoring superblock\n");
		goto err;
	} else if (ret != 0) {
		goto err;
	}

	dummy_bh.b_data = (char *)svb->buf;
	gfs2_sb_in(&sbd.sd_sb, &dummy_bh);
	sbd1 = (struct gfs_sb *)&sbd.sd_sb;
	ret = check_sb(&sbd.sd_sb);
	if (ret < 0) {
		fprintf(stderr,"Error: Invalid superblock data.\n");
		goto err;
	}
	if (ret == 1)
		sbd.gfs1 = 1;
	sbd.bsize = sbd.sd_sb.sb_bsize;
	free(svb);
	printf("Block size is %uB\n", sbd.bsize);
	return 0;
err:
	free(svb);
	return -1;
}

static int find_highest_block(gzFile gzfd, off_t pos, uint64_t fssize)
{
	int err = 0;
	uint64_t highest = 0;
	struct saved_metablock svb = {0};

	while (1) {
		gzseek(gzfd, pos, SEEK_SET);
		err = restore_block(gzfd, &svb, 0);
		if (err == 1)
			break;
		if (err != 0)
			return -1;

		if (svb.blk > highest)
			highest = svb.blk;
		pos += sizeof(svb) + svb.siglen;
	}

	if (fssize > 0) {
		printf("Saved file system size is %"PRIu64" (0x%"PRIx64") blocks, %s\n",
		       fssize, fssize, anthropomorphize(fssize * sbd.bsize));
		sbd.fssize = fssize;
	} else {
		sbd.fssize = highest + 1;
	}

	printf("Highest saved block is %"PRIu64" (0x%"PRIx64")\n", highest, highest);
	return 0;
}

static int restore_data(int fd, gzFile gzin_fd, off_t pos, int printonly)
{
	struct saved_metablock *savedata;
	size_t insz = sizeof(*savedata) + sbd.bsize;
	uint64_t writes = 0;

	savedata = calloc(1, insz);
	if (savedata == NULL) {
		perror("Failed to restore data");
		exit(1);
	}

	gzseek(gzin_fd, pos, SEEK_SET);
	blks_saved = 0;
	while (TRUE) {
		int err;
		err = restore_block(gzin_fd, savedata, sbd.bsize);
		if (err == 1)
			break;
		if (err != 0) {
			free(savedata);
			return -1;
		}

		if (printonly) {
			struct gfs2_buffer_head dummy_bh;
			dummy_bh.b_data = savedata->buf;
			bh = &dummy_bh;
			block = savedata->blk;
			if (printonly > 1 && printonly == block) {
				block_in_mem = block;
				display(0, 0, 0, 0);
				bh = NULL;
				break;
			} else if (printonly == 1) {
				print_gfs2("%"PRId64" (l=0x%x): ", blks_saved, savedata->siglen);
				display_block_type(TRUE);
			}
			bh = NULL;
		} else {
			warm_fuzzy_stuff(savedata->blk, FALSE);
			memset(savedata->buf + savedata->siglen, 0, sbd.bsize - savedata->siglen);
			if (pwrite(fd, savedata->buf, sbd.bsize, savedata->blk * sbd.bsize) != sbd.bsize) {
				fprintf(stderr, "write error: %s from %s:%d: block %lld (0x%llx)\n",
					strerror(errno), __FUNCTION__, __LINE__,
					(unsigned long long)savedata->blk,
					(unsigned long long)savedata->blk);
				free(savedata);
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
	free(savedata);
	return 0;
}

static void complain(const char *complaint)
{
	fprintf(stderr, "%s\n", complaint);
	die("Format is: \ngfs2_edit restoremeta <file to restore> "
	    "<dest file system>\n");
}

void restoremeta(const char *in_fn, const char *out_device, uint64_t printonly)
{
	int error;
	gzFile gzfd;
	off_t pos = 0;
	struct savemeta_header smh = {0};

	termlines = 0;
	if (!in_fn)
		complain("No source file specified.");
	if (!printonly && !out_device)
		complain("No destination file system specified.");

	gzfd = gzopen(in_fn, "rb");
	if (!gzfd)
		die("Can't open source file %s: %s\n",
		    in_fn, strerror(errno));

	if (!printonly) {
		sbd.device_fd = open(out_device, O_RDWR);
		if (sbd.device_fd < 0)
			die("Can't open destination file system %s: %s\n",
			    out_device, strerror(errno));
	} else if (out_device) /* for printsavedmeta, the out_device is an
				  optional block no */
		printonly = check_keywords(out_device);

	pos = restore_init(gzfd, &smh);
	error = restore_super(gzfd, pos);
	if (error)
		exit(1);

	printf("This is gfs%c metadata.\n", sbd.gfs1 ? '1': '2');

	if (!printonly) {
		uint64_t space = lseek(sbd.device_fd, 0, SEEK_END) / sbd.bsize;
		printf("There are %"PRIu64" free blocks on the destination device.\n", space);
	}

	error = find_highest_block(gzfd, pos, sbd.fssize);
	if (error)
		exit(1);

	error = restore_data(sbd.device_fd, gzfd, pos, printonly);
	printf("File %s %s %s.\n", in_fn,
	       (printonly ? "print" : "restore"),
	       (error ? "error" : "successful"));

	gzclose(gzfd);
	if (!printonly)
		close(sbd.device_fd);
	free(indirect);
	exit(error);
}
