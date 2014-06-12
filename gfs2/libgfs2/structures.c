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
#include <linux/types.h>
#include <sys/time.h>

#include "libgfs2.h"
#include "config.h"

int build_master(struct gfs2_sbd *sdp)
{
	struct gfs2_inum inum;
	uint64_t bn;
	struct gfs2_buffer_head *bh;
	int err = lgfs2_dinode_alloc(sdp, 1, &bn);

	if (err != 0)
		return -1;

	inum.no_formal_ino = sdp->md.next_inum++;
	inum.no_addr = bn;

	bh = init_dinode(sdp, &inum, S_IFDIR | 0755, GFS2_DIF_SYSTEM, &inum);

	sdp->master_dir = lgfs2_inode_get(sdp, bh);
	if (sdp->master_dir == NULL)
		return -1;

	if (cfg_debug) {
		printf("\nMaster dir:\n");
		gfs2_dinode_print(&sdp->master_dir->i_di);
	}
	sdp->master_dir->bh_owned = 1;
	return 0;
}

#ifdef GFS2_HAS_UUID
/**
 * Generate a series of random bytes using /dev/urandom.
 * Modified from original code in gen_uuid.c in e2fsprogs/lib
 */
static void get_random_bytes(void *buf, int nbytes)
{
	int i, n = nbytes, fd;
	int lose_counter = 0;
	unsigned char *cp = (unsigned char *) buf;
	struct timeval	tv;

	gettimeofday(&tv, 0);
	fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
	srand((getpid() << 16) ^ getuid() ^ tv.tv_sec ^ tv.tv_usec);
	/* Crank the random number generator a few times */
	gettimeofday(&tv, 0);
	for (i = (tv.tv_sec ^ tv.tv_usec) & 0x1F; i > 0; i--)
		rand();
	if (fd >= 0) {
		while (n > 0) {
			i = read(fd, cp, n);
			if (i <= 0) {
				if (lose_counter++ > 16)
					break;
				continue;
			}
			n -= i;
			cp += i;
			lose_counter = 0;
		}
		close(fd);
	}

	/*
	 * We do this all the time, but this is the only source of
	 * randomness if /dev/random/urandom is out to lunch.
	 */
	for (cp = buf, i = 0; i < nbytes; i++)
		*cp++ ^= (rand() >> 7) & 0xFF;

	return;
}
#endif

/**
 * Initialise a gfs2_sb structure with sensible defaults.
 */
void lgfs2_sb_init(struct gfs2_sb *sb, unsigned bsize)
{
	memset(sb, 0, sizeof(struct gfs2_sb));
	sb->sb_header.mh_magic = GFS2_MAGIC;
	sb->sb_header.mh_type = GFS2_METATYPE_SB;
	sb->sb_header.mh_format = GFS2_FORMAT_SB;
	sb->sb_fs_format = GFS2_FORMAT_FS;
	sb->sb_multihost_format = GFS2_FORMAT_MULTI;
	sb->sb_bsize = bsize;
	sb->sb_bsize_shift = ffs(bsize) - 1;
#ifdef GFS2_HAS_UUID
	get_random_bytes(&sb->sb_uuid, sizeof(sb->sb_uuid));
#endif
}

int lgfs2_sb_write(const struct gfs2_sb *sb, int fd, const unsigned bsize)
{
	int i, err = -1;
	struct iovec *iov;
	const size_t sb_addr = GFS2_SB_ADDR * GFS2_BASIC_BLOCK / bsize;
	const size_t len = sb_addr + 1;

	/* We only need 2 blocks: one for zeroing and a second for the superblock */
	char *buf = calloc(2, bsize);
	if (buf == NULL)
		return -1;

	iov = malloc(len * sizeof(*iov));
	if (iov == NULL)
		goto out_buf;

	for (i = 0; i < len; i++) {
		iov[i].iov_base = buf;
		iov[i].iov_len = bsize;
	}

	gfs2_sb_out(sb, buf + bsize);
	iov[sb_addr].iov_base = buf + bsize;

	if (pwritev(fd, iov, len, 0) < (len * bsize))
		goto out_iov;

	err = 0;
out_iov:
	free(iov);
out_buf:
	free(buf);
	return err;
}

int write_journal(struct gfs2_inode *jnl, unsigned bsize, unsigned int blocks)
{
	struct gfs2_log_header lh;
	unsigned int x;
	uint64_t seq = ((blocks) * (random() / (RAND_MAX + 1.0)));
	uint32_t hash;
	unsigned int height;

	/* Build the height up so our journal blocks will be contiguous and */
	/* not broken up by indirect block pages.                           */
	height = calc_tree_height(jnl, (blocks + 1) * bsize);
	build_height(jnl, height);

	memset(&lh, 0, sizeof(struct gfs2_log_header));
	lh.lh_header.mh_magic = GFS2_MAGIC;
	lh.lh_header.mh_type = GFS2_METATYPE_LH;
	lh.lh_header.mh_format = GFS2_FORMAT_LH;
	lh.lh_flags = GFS2_LOG_HEAD_UNMOUNT;

	for (x = 0; x < blocks; x++) {
		struct gfs2_buffer_head *bh = get_file_buf(jnl, x, TRUE);
		if (!bh)
			return -1;
		bmodified(bh);
		brelse(bh);
	}
	for (x = 0; x < blocks; x++) {
		struct gfs2_buffer_head *bh = get_file_buf(jnl, x, FALSE);
		if (!bh)
			return -1;

		memset(bh->b_data, 0, bsize);
		lh.lh_sequence = seq;
		lh.lh_blkno = x;
		gfs2_log_header_out(&lh, bh);
		hash = gfs2_disk_hash(bh->b_data, sizeof(struct gfs2_log_header));
		((struct gfs2_log_header *)bh->b_data)->lh_hash = cpu_to_be32(hash);

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
	ret = write_journal(sdp->md.journal[j], sdp->bsize,
			    sdp->jsize << 20 >> sdp->sd_sb.sb_bsize_shift);
	return ret;
}

int build_jindex(struct gfs2_sbd *sdp)
{
	struct gfs2_inode *jindex;
	unsigned int j;
	int ret;

	jindex = createi(sdp->master_dir, "jindex", S_IFDIR | 0700,
			 GFS2_DIF_SYSTEM);
	if (jindex == NULL) {
		return errno;
	}
	sdp->md.journal = malloc(sdp->md.journals *
				 sizeof(struct gfs2_inode *));
	for (j = 0; j < sdp->md.journals; j++) {
		ret = build_journal(sdp, j, jindex);
		if (ret)
			return ret;
		inode_put(&sdp->md.journal[j]);
	}
	if (cfg_debug) {
		printf("\nJindex:\n");
		gfs2_dinode_print(&jindex->i_di);
	}

	free(sdp->md.journal);
	inode_put(&jindex);
	return 0;
}

int build_inum_range(struct gfs2_inode *per_node, unsigned int j)
{
	char name[256];
	struct gfs2_inode *ip;

	sprintf(name, "inum_range%u", j);
	ip = createi(per_node, name, S_IFREG | 0600,
		     GFS2_DIF_SYSTEM | GFS2_DIF_JDATA);
	if (ip == NULL) {
		return errno;
	}
	ip->i_di.di_size = sizeof(struct gfs2_inum_range);
	gfs2_dinode_out(&ip->i_di, ip->i_bh);

	if (cfg_debug) {
		printf("\nInum Range %u:\n", j);
		gfs2_dinode_print(&ip->i_di);
	}

	inode_put(&ip);
	return 0;
}

int build_statfs_change(struct gfs2_inode *per_node, unsigned int j)
{
	char name[256];
	struct gfs2_inode *ip;

	sprintf(name, "statfs_change%u", j);
	ip = createi(per_node, name, S_IFREG | 0600,
		     GFS2_DIF_SYSTEM | GFS2_DIF_JDATA);
	if (ip == NULL) {
		return errno;
	}
	ip->i_di.di_size = sizeof(struct gfs2_statfs_change);
	gfs2_dinode_out(&ip->i_di, ip->i_bh);

	if (cfg_debug) {
		printf("\nStatFS Change %u:\n", j);
		gfs2_dinode_print(&ip->i_di);
	}

	inode_put(&ip);
	return 0;
}

int build_quota_change(struct gfs2_inode *per_node, unsigned int j)
{
	struct gfs2_sbd *sdp = per_node->i_sbd;
	struct gfs2_meta_header mh;
	char name[256];
	struct gfs2_inode *ip;
	unsigned int blocks = sdp->qcsize << (20 - sdp->sd_sb.sb_bsize_shift);
	unsigned int x;
	unsigned int hgt;
	struct gfs2_buffer_head *bh;

	memset(&mh, 0, sizeof(struct gfs2_meta_header));
	mh.mh_magic = GFS2_MAGIC;
	mh.mh_type = GFS2_METATYPE_QC;
	mh.mh_format = GFS2_FORMAT_QC;

	sprintf(name, "quota_change%u", j);
	ip = createi(per_node, name, S_IFREG | 0600, GFS2_DIF_SYSTEM);
	if (ip == NULL) {
		return errno;
	}

	hgt = calc_tree_height(ip, (blocks + 1) * sdp->bsize);
	build_height(ip, hgt);

	for (x = 0; x < blocks; x++) {
		bh = get_file_buf(ip, x, FALSE);
		if (!bh)
			return -1;

		memset(bh->b_data, 0, sdp->bsize);
		gfs2_meta_header_out_bh(&mh, bh);
		brelse(bh);
	}

	if (cfg_debug) {
		printf("\nQuota Change %u:\n", j);
		gfs2_dinode_print(&ip->i_di);
	}

	inode_put(&ip);
	return 0;
}

int build_per_node(struct gfs2_sbd *sdp)
{
	struct gfs2_inode *per_node;
	unsigned int j;
	int err;

	per_node = createi(sdp->master_dir, "per_node", S_IFDIR | 0700,
			   GFS2_DIF_SYSTEM);
	if (per_node == NULL) {
		return errno;
	}

	for (j = 0; j < sdp->md.journals; j++) {
		err = build_inum_range(per_node, j);
		if (err) {
			return err;
		}
		err = build_statfs_change(per_node, j);
		if (err) {
			return err;
		}
		err = build_quota_change(per_node, j);
		if (err) {
			return err;
		}
	}

	if (cfg_debug) {
		printf("\nper_node:\n");
		gfs2_dinode_print(&per_node->i_di);
	}

	inode_put(&per_node);
	return 0;
}

int build_inum(struct gfs2_sbd *sdp)
{
	struct gfs2_inode *ip;

	ip = createi(sdp->master_dir, "inum", S_IFREG | 0600,
		     GFS2_DIF_SYSTEM | GFS2_DIF_JDATA);
	if (ip == NULL) {
		return errno;
	}

	if (cfg_debug) {
		printf("\nInum Inode:\n");
		gfs2_dinode_print(&ip->i_di);
	}

	inode_put(&ip);
	return 0;
}

int build_statfs(struct gfs2_sbd *sdp)
{
	struct gfs2_inode *ip;

	ip = createi(sdp->master_dir, "statfs", S_IFREG | 0600,
		     GFS2_DIF_SYSTEM | GFS2_DIF_JDATA);
	if (ip == NULL) {
		return errno;
	}

	if (cfg_debug) {
		printf("\nStatFS Inode:\n");
		gfs2_dinode_print(&ip->i_di);
	}

	inode_put(&ip);
	return 0;
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
	ip->i_di.di_payload_format = GFS2_FORMAT_RI;
	bmodified(ip->i_bh);

	for (n = osi_first(&sdp->rgtree); n; n = next) {
		next = osi_next(n);
		rl = (struct rgrp_tree *)n;

		gfs2_rindex_out(&rl->ri, buf);

		count = gfs2_writei(ip, buf, ip->i_di.di_size,
				    sizeof(struct gfs2_rindex));
		if (count != sizeof(struct gfs2_rindex))
			return -1;
	}
	memset(buf, 0, sizeof(struct gfs2_rindex));
	count = __gfs2_writei(ip, buf, ip->i_di.di_size,
			      sizeof(struct gfs2_rindex), 0);
	if (count != sizeof(struct gfs2_rindex))
		return -1;

	if (cfg_debug) {
		printf("\nResource Index:\n");
		gfs2_dinode_print(&ip->i_di);
	}

	inode_put(&ip);
	return 0;
}

int build_quota(struct gfs2_sbd *sdp)
{
	struct gfs2_inode *ip;
	struct gfs2_quota qu;
	char buf[sizeof(struct gfs2_quota)];
	int count;

	ip = createi(sdp->master_dir, "quota", S_IFREG | 0600,
		     GFS2_DIF_SYSTEM | GFS2_DIF_JDATA);
	if (ip == NULL) {
		return errno;
	}
	ip->i_di.di_payload_format = GFS2_FORMAT_QU;
	bmodified(ip->i_bh);

	memset(&qu, 0, sizeof(struct gfs2_quota));
	qu.qu_value = 1;
	gfs2_quota_out(&qu, buf);

	count = gfs2_writei(ip, buf, ip->i_di.di_size, sizeof(struct gfs2_quota));
	if (count != sizeof(struct gfs2_quota))
		return -1;
	count = gfs2_writei(ip, buf, ip->i_di.di_size, sizeof(struct gfs2_quota));
	if (count != sizeof(struct gfs2_quota))
		return -1;

	if (cfg_debug) {
		printf("\nRoot quota:\n");
		gfs2_quota_print(&qu);
	}

	inode_put(&ip);
	return 0;
}

int build_root(struct gfs2_sbd *sdp)
{
	struct gfs2_inum inum;
	uint64_t bn;
	struct gfs2_buffer_head *bh;
	int err = lgfs2_dinode_alloc(sdp, 1, &bn);

	if (err != 0)
		return -1;

	inum.no_formal_ino = sdp->md.next_inum++;
	inum.no_addr = bn;

	bh = init_dinode(sdp, &inum, S_IFDIR | 0755, 0, &inum);
	sdp->md.rooti = lgfs2_inode_get(sdp, bh);
	if (sdp->md.rooti == NULL)
		return -1;

	if (cfg_debug) {
		printf("\nRoot directory:\n");
		gfs2_dinode_print(&sdp->md.rooti->i_di);
	}
	sdp->md.rooti->bh_owned = 1;
	return 0;
}

int do_init_inum(struct gfs2_sbd *sdp)
{
	struct gfs2_inode *ip = sdp->md.inum;
	uint64_t buf;
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
	char buf[sizeof(struct gfs2_statfs_change)];
	int count;

	sc.sc_total = sdp->blks_total;
	sc.sc_free = sdp->blks_total - sdp->blks_alloced;
	sc.sc_dinodes = sdp->dinodes_alloced;

	gfs2_statfs_change_out(&sc, buf);
	count = gfs2_writei(ip, buf, 0, sizeof(struct gfs2_statfs_change));
	if (count != sizeof(struct gfs2_statfs_change))
		return -1;

	if (cfg_debug) {
		printf("\nStatfs:\n");
		gfs2_statfs_change_print(&sc);
	}
	return 0;
}

int gfs2_check_meta(struct gfs2_buffer_head *bh, int type)
{
	uint32_t check_magic = ((struct gfs2_meta_header *)(bh->b_data))->mh_magic;
	uint32_t check_type = ((struct gfs2_meta_header *)(bh->b_data))->mh_type;

	check_magic = be32_to_cpu(check_magic);
	check_type = be32_to_cpu(check_type);
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
		blk = gfs2_bitfit((uint8_t *)bi->bi_bh->b_data + bi->bi_offset,
				  bi->bi_len, blk, state);
		if (blk == BFITNOENT)
			break;
		buf[n++] = blk + (bi->bi_start * GFS2_NBBY) + rgd->ri.ri_data0;
		blk++;
	}

	return n;
}
