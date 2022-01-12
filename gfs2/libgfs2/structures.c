#include "clusterautoconfig.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/time.h>
#include <uuid.h>

#include "libgfs2.h"
#include "config.h"
#include "crc32c.h"

int build_master(struct gfs2_sbd *sdp)
{
	struct lgfs2_inum inum;
	uint64_t bn;
	struct gfs2_buffer_head *bh = NULL;
	int err = lgfs2_dinode_alloc(sdp, 1, &bn);

	if (err != 0)
		return -1;

	inum.in_formal_ino = sdp->md.next_inum++;
	inum.in_addr = bn;

	err = init_dinode(sdp, &bh, &inum, S_IFDIR | 0755, GFS2_DIF_SYSTEM, &inum);
	if (err != 0)
		return -1;

	sdp->master_dir = lgfs2_inode_get(sdp, bh);
	if (sdp->master_dir == NULL)
		return -1;

	sdp->master_dir->bh_owned = 1;
	return 0;
}

int lgfs2_sb_write(const struct gfs2_sbd *sdp, int fd)
{
	int i, err = -1;
	struct iovec *iov;
	const size_t sb_addr = GFS2_SB_ADDR * GFS2_BASIC_BLOCK / sdp->sd_bsize;
	const size_t len = sb_addr + 1;

	/* We only need 2 blocks: one for zeroing and a second for the superblock */
	char *buf = calloc(2, sdp->sd_bsize);
	if (buf == NULL)
		return -1;

	iov = malloc(len * sizeof(*iov));
	if (iov == NULL)
		goto out_buf;

	for (i = 0; i < len; i++) {
		iov[i].iov_base = buf;
		iov[i].iov_len = sdp->sd_bsize;
	}

	lgfs2_sb_out(sdp, buf + sdp->sd_bsize);
	iov[sb_addr].iov_base = buf + sdp->sd_bsize;

	if (pwritev(fd, iov, len, 0) < (len * sdp->sd_bsize))
		goto out_iov;

	err = 0;
out_iov:
	free(iov);
out_buf:
	free(buf);
	return err;
}

uint32_t lgfs2_log_header_hash(char *buf)
{
	/* lh_hash only CRCs the fields in the old lh, which ends where lh_crc is now */
	const off_t v1_end = offsetof(struct gfs2_log_header, lh_hash) + 4;

	return gfs2_disk_hash(buf, v1_end);
}

uint32_t lgfs2_log_header_crc(char *buf, unsigned bsize)
{
	/* lh_crc CRCs the rest of the block starting after lh_crc */
	const off_t v1_end = offsetof(struct gfs2_log_header, lh_hash) + 4;
	const unsigned char *lb = (const unsigned char *)buf;

	return crc32c(~0, lb + v1_end + 4, bsize - v1_end - 4);
}

/**
 * Intialise and write the data blocks for a new journal as a contiguous
 * extent. The indirect blocks pointing to these data blocks should have been
 * written separately using lgfs2_write_filemeta() and the extent should have
 * been allocated using lgfs2_file_alloc().
 * ip: The journal's inode
 * Returns 0 on success or -1 with errno set on error.
 */
int lgfs2_write_journal_data(struct gfs2_inode *ip)
{
	struct gfs2_sbd *sdp = ip->i_sbd;
	unsigned blocks = (ip->i_size + sdp->sd_bsize - 1) / sdp->sd_bsize;
	uint64_t jext0 = ip->i_num.in_addr + ip->i_blocks - blocks;
	uint64_t seq = ((blocks) * (random() / (RAND_MAX + 1.0)));
	struct gfs2_log_header *lh;
	uint64_t jblk = jext0;
	char *buf;

	buf = calloc(1, sdp->sd_bsize);
	if (buf == NULL)
		return -1;

	lh = (void *)buf;
	lh->lh_header.mh_magic = cpu_to_be32(GFS2_MAGIC);
	lh->lh_header.mh_type = cpu_to_be32(GFS2_METATYPE_LH);
	lh->lh_header.mh_format = cpu_to_be32(GFS2_FORMAT_LH);
	lh->lh_flags = cpu_to_be32(GFS2_LOG_HEAD_UNMOUNT | GFS2_LOG_HEAD_USERSPACE);
	lh->lh_jinode = cpu_to_be64(ip->i_num.in_addr);

	crc32c_optimization_init();
	do {
		uint32_t hash;

		lh->lh_sequence = cpu_to_be64(seq);
		lh->lh_blkno = cpu_to_be32(jblk - jext0);
		hash = lgfs2_log_header_hash(buf);
		lh->lh_hash = cpu_to_be32(hash);
		lh->lh_addr = cpu_to_be64(jblk);
		hash = lgfs2_log_header_crc(buf, sdp->sd_bsize);
		lh->lh_crc = cpu_to_be32(hash);

		if (pwrite(sdp->device_fd, buf, sdp->sd_bsize, jblk * sdp->sd_bsize) != sdp->sd_bsize) {
			free(buf);
			return -1;
		}

		lh->lh_crc = 0;
		lh->lh_hash = 0;

		if (++seq == blocks)
			seq = 0;

	} while (++jblk < jext0 + blocks);

	free(buf);
	return 0;
}

static struct gfs2_buffer_head *get_file_buf(struct gfs2_inode *ip, uint64_t lbn, int prealloc)
{
	struct gfs2_sbd *sdp = ip->i_sbd;
	uint64_t dbn;
	int new = 1;

	if (ip->i_height == 0)
		unstuff_dinode(ip);

	block_map(ip, lbn, &new, &dbn, NULL, prealloc);
	if (!dbn)
		return NULL;

	if (!prealloc && new &&
	    ip->i_size < (lbn + 1) << sdp->sd_bsize_shift) {
		bmodified(ip->i_bh);
		ip->i_size = (lbn + 1) << sdp->sd_bsize_shift;
	}
	if (dbn == ip->i_num.in_addr)
		return ip->i_bh;
	else
		return bread(sdp, dbn);
}

int write_journal(struct gfs2_inode *jnl, unsigned bsize, unsigned int blocks)
{
	struct gfs2_log_header *lh;
	uint32_t x;
	uint64_t seq = ((blocks) * (random() / (RAND_MAX + 1.0)));
	uint32_t hash;
	unsigned int height;

	/* Build the height up so our journal blocks will be contiguous and */
	/* not broken up by indirect block pages.                           */
	height = calc_tree_height(jnl, (blocks + 1) * bsize);
	build_height(jnl, height);

	for (x = 0; x < blocks; x++) {
		struct gfs2_buffer_head *bh = get_file_buf(jnl, x, 1);
		if (!bh)
			return -1;
		bmodified(bh);
		brelse(bh);
	}
	crc32c_optimization_init();
	for (x = 0; x < blocks; x++) {
		struct gfs2_buffer_head *bh = get_file_buf(jnl, x, 0);
		if (!bh)
			return -1;

		memset(bh->b_data, 0, bsize);
		lh = (void *)bh->b_data;
		lh->lh_header.mh_magic = cpu_to_be32(GFS2_MAGIC);
		lh->lh_header.mh_type = cpu_to_be32(GFS2_METATYPE_LH);
		lh->lh_header.mh_format = cpu_to_be32(GFS2_FORMAT_LH);
		lh->lh_flags = cpu_to_be32(GFS2_LOG_HEAD_UNMOUNT | GFS2_LOG_HEAD_USERSPACE);
		lh->lh_jinode = cpu_to_be64(jnl->i_num.in_addr);
		lh->lh_sequence = cpu_to_be64(seq);
		lh->lh_blkno = cpu_to_be32(x);

		hash = lgfs2_log_header_hash(bh->b_data);
		lh->lh_hash = cpu_to_be32(hash);
		lh->lh_addr = cpu_to_be64(bh->b_blocknr);

		hash = lgfs2_log_header_crc(bh->b_data, bsize);
		lh->lh_crc = cpu_to_be32(hash);
		bmodified(bh);
		brelse(bh);

		if (++seq == blocks)
			seq = 0;
	}

	return 0;
}

int build_journal(struct gfs2_sbd *sdp, int j, struct gfs2_inode *jindex)
{
	char name[256];
	int ret;

	sprintf(name, "journal%u", j);
	sdp->md.journal[j] = createi(jindex, name, S_IFREG | 0600,
				     GFS2_DIF_SYSTEM);
	if (sdp->md.journal[j] == NULL) {
		return errno;
	}
	ret = write_journal(sdp->md.journal[j], sdp->sd_bsize,
			    sdp->jsize << 20 >> sdp->sd_bsize_shift);
	return ret;
}

/**
 * Write a jindex file given a list of journal inums.
 * master: Inode of the master directory
 * jnls: List of inum structures relating to previously created journals.
 * nmemb: The number of entries in the list (number of journals).
 * Returns 0 on success or non-zero on error with errno set.
 */
struct gfs2_inode *lgfs2_build_jindex(struct gfs2_inode *master, struct lgfs2_inum *jnls, size_t nmemb)
{
	char fname[GFS2_FNAMESIZE + 1];
	struct gfs2_inode *jindex;

	if (nmemb == 0 || jnls == NULL) {
		errno = EINVAL;
		return NULL;
	}
	jindex = createi(master, "jindex", S_IFDIR | 0700, GFS2_DIF_SYSTEM);
	if (jindex == NULL)
		return NULL;

	fname[GFS2_FNAMESIZE] = '\0';

	for (unsigned j = 0; j < nmemb; j++) {
		int ret;

		snprintf(fname, GFS2_FNAMESIZE, "journal%u", j);
		ret = dir_add(jindex, fname, strlen(fname), &jnls[j], IF2DT(S_IFREG | 0600));
		if (ret) {
			inode_put(&jindex);
			return NULL;
		}
	}
	return jindex;
}

struct gfs2_inode *build_inum_range(struct gfs2_inode *per_node, unsigned int j)
{
	char name[256];
	struct gfs2_inode *ip;

	sprintf(name, "inum_range%u", j);
	ip = createi(per_node, name, S_IFREG | 0600,
		     GFS2_DIF_SYSTEM | GFS2_DIF_JDATA);
	if (ip == NULL)
		return NULL;

	ip->i_size = sizeof(struct gfs2_inum_range);
	lgfs2_dinode_out(ip, ip->i_bh->b_data);
	bmodified(ip->i_bh);
	return ip;
}

struct gfs2_inode *build_statfs_change(struct gfs2_inode *per_node, unsigned int j)
{
	char name[256];
	struct gfs2_inode *ip;

	sprintf(name, "statfs_change%u", j);
	ip = createi(per_node, name, S_IFREG | 0600,
		     GFS2_DIF_SYSTEM | GFS2_DIF_JDATA);
	if (ip == NULL)
		return NULL;

	ip->i_size = sizeof(struct gfs2_statfs_change);
	lgfs2_dinode_out(ip, ip->i_bh->b_data);
	bmodified(ip->i_bh);
	return ip;
}

struct gfs2_inode *build_quota_change(struct gfs2_inode *per_node, unsigned int j)
{
	struct gfs2_sbd *sdp = per_node->i_sbd;
	struct gfs2_meta_header mh;
	char name[256];
	struct gfs2_inode *ip;
	unsigned int blocks = sdp->qcsize << (20 - sdp->sd_bsize_shift);
	unsigned int x;
	unsigned int hgt;
	struct gfs2_buffer_head *bh;

	memset(&mh, 0, sizeof(struct gfs2_meta_header));
	mh.mh_magic = cpu_to_be32(GFS2_MAGIC);
	mh.mh_type = cpu_to_be32(GFS2_METATYPE_QC);
	mh.mh_format = cpu_to_be32(GFS2_FORMAT_QC);

	sprintf(name, "quota_change%u", j);
	ip = createi(per_node, name, S_IFREG | 0600, GFS2_DIF_SYSTEM);
	if (ip == NULL)
		return NULL;

	hgt = calc_tree_height(ip, (blocks + 1) * sdp->sd_bsize);
	build_height(ip, hgt);

	for (x = 0; x < blocks; x++) {
		bh = get_file_buf(ip, x, 0);
		if (!bh)
			return NULL;

		memset(bh->b_data, 0, sdp->sd_bsize);
		memcpy(bh->b_data, &mh, sizeof(mh));
		bmodified(bh);
		brelse(bh);
	}
	return ip;
}

struct gfs2_inode *build_inum(struct gfs2_sbd *sdp)
{
	struct gfs2_inode *ip;

	ip = createi(sdp->master_dir, "inum", S_IFREG | 0600,
		     GFS2_DIF_SYSTEM | GFS2_DIF_JDATA);
	return ip;
}

struct gfs2_inode *build_statfs(struct gfs2_sbd *sdp)
{
	struct gfs2_inode *ip;

	ip = createi(sdp->master_dir, "statfs", S_IFREG | 0600,
		     GFS2_DIF_SYSTEM | GFS2_DIF_JDATA);
	return ip;
}

int build_rindex(struct gfs2_sbd *sdp)
{
	struct gfs2_inode *ip;
	struct osi_node *n, *next = NULL;
	struct rgrp_tree *rl;
	char buf[sizeof(struct gfs2_rindex)];
	int count;

	ip = createi(sdp->master_dir, "rindex", S_IFREG | 0600,
		     GFS2_DIF_SYSTEM | GFS2_DIF_JDATA);
	if (ip == NULL) {
		return errno;
	}
	ip->i_payload_format = GFS2_FORMAT_RI;
	bmodified(ip->i_bh);

	for (n = osi_first(&sdp->rgtree); n; n = next) {
		next = osi_next(n);
		rl = (struct rgrp_tree *)n;

		lgfs2_rindex_out(rl, buf);

		count = gfs2_writei(ip, buf, ip->i_size, sizeof(struct gfs2_rindex));
		if (count != sizeof(struct gfs2_rindex))
			return -1;
	}
	memset(buf, 0, sizeof(struct gfs2_rindex));
	count = __gfs2_writei(ip, buf, ip->i_size, sizeof(struct gfs2_rindex), 0);
	if (count != sizeof(struct gfs2_rindex))
		return -1;

	if (cfg_debug) {
		printf("\nResource Index:\n");
		lgfs2_dinode_print(ip->i_bh->b_data);
	}

	inode_put(&ip);
	return 0;
}

int build_quota(struct gfs2_sbd *sdp)
{
	struct gfs2_inode *ip;
	struct gfs2_quota qu;
	int count;

	ip = createi(sdp->master_dir, "quota", S_IFREG | 0600,
		     GFS2_DIF_SYSTEM | GFS2_DIF_JDATA);
	if (ip == NULL) {
		return errno;
	}
	ip->i_payload_format = GFS2_FORMAT_QU;
	bmodified(ip->i_bh);

	memset(&qu, 0, sizeof(struct gfs2_quota));
	qu.qu_value = cpu_to_be64(1);

	count = gfs2_writei(ip, &qu, ip->i_size, sizeof(struct gfs2_quota));
	if (count != sizeof(struct gfs2_quota))
		return -1;
	count = gfs2_writei(ip, &qu, ip->i_size, sizeof(struct gfs2_quota));
	if (count != sizeof(struct gfs2_quota))
		return -1;

	if (cfg_debug) {
		printf("\nRoot quota:\n");
		lgfs2_quota_print(&qu);
	}

	inode_put(&ip);
	return 0;
}

int build_root(struct gfs2_sbd *sdp)
{
	struct lgfs2_inum inum;
	uint64_t bn;
	struct gfs2_buffer_head *bh = NULL;
	int err = lgfs2_dinode_alloc(sdp, 1, &bn);

	if (err != 0)
		return -1;

	inum.in_formal_ino = sdp->md.next_inum++;
	inum.in_addr = bn;

	err = init_dinode(sdp, &bh, &inum, S_IFDIR | 0755, 0, &inum);
	if (err != 0)
		return -1;

	sdp->md.rooti = lgfs2_inode_get(sdp, bh);
	if (sdp->md.rooti == NULL)
		return -1;

	if (cfg_debug) {
		printf("\nRoot directory:\n");
		lgfs2_dinode_print(bh->b_data);
	}
	sdp->md.rooti->bh_owned = 1;
	return 0;
}

int do_init_inum(struct gfs2_sbd *sdp)
{
	struct gfs2_inode *ip = sdp->md.inum;
	__be64 buf;
	int count;

	buf = cpu_to_be64(sdp->md.next_inum);
	count = gfs2_writei(ip, &buf, 0, sizeof(uint64_t));
	if (count != sizeof(uint64_t))
		return -1;

	if (cfg_debug)
		printf("\nNext Inum: %"PRIu64"\n",
		       sdp->md.next_inum);
	return 0;
}

int do_init_statfs(struct gfs2_sbd *sdp)
{
	struct gfs2_inode *ip = sdp->md.statfs;
	struct gfs2_statfs_change sc;
	int count;

	sc.sc_total = cpu_to_be64(sdp->blks_total);
	sc.sc_free = cpu_to_be64(sdp->blks_total - sdp->blks_alloced);
	sc.sc_dinodes = cpu_to_be64(sdp->dinodes_alloced);

	count = gfs2_writei(ip, &sc, 0, sizeof(sc));
	if (count != sizeof(sc))
		return -1;

	if (cfg_debug) {
		printf("\nStatfs:\n");
		lgfs2_statfs_change_print(&sc);
	}
	return 0;
}

int gfs2_check_meta(const char *buf, int type)
{
	struct gfs2_meta_header *mh = (struct gfs2_meta_header *)buf;
	uint32_t check_magic = be32_to_cpu(mh->mh_magic);
	uint32_t check_type = be32_to_cpu(mh->mh_type);

	if((check_magic != GFS2_MAGIC) || (type && (check_type != type)))
		return -1;
	return 0;
}

unsigned lgfs2_bm_scan(struct rgrp_tree *rgd, unsigned idx, uint64_t *buf, uint8_t state)
{
	struct gfs2_bitmap *bi = &rgd->bits[idx];
	unsigned n = 0;
	uint32_t blk = 0;

	while(blk < (bi->bi_len * GFS2_NBBY)) {
		blk = gfs2_bitfit((uint8_t *)bi->bi_data + bi->bi_offset,
				  bi->bi_len, blk, state);
		if (blk == BFITNOENT)
			break;
		buf[n++] = blk + (bi->bi_start * GFS2_NBBY) + rgd->rt_data0;
		blk++;
	}
	return n;
}
