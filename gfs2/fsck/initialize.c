#include "clusterautoconfig.h"

#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>
#include <libintl.h>
#include <errno.h>

#define _(String) gettext(String)

#include <logging.h>
#include "libgfs2.h"
#include "fsck.h"
#include "util.h"
#include "fs_recovery.h"
#include "metawalk.h"
#include "inode_hash.h"

#define CLEAR_POINTER(x) \
	if (x) { \
		free(x); \
		x = NULL; \
	}
#define HIGHEST_BLOCK 0xffffffffffffffff

static int was_mounted_ro = 0;
static uint64_t possible_root = HIGHEST_BLOCK;
static struct lgfs2_meta_dir fix_md;
static uint64_t blks_2free = 0;

/**
 * block_mounters
 *
 * Change the lock protocol so nobody can mount the fs
 *
 */
static int block_mounters(struct lgfs2_sbd *sdp, int block_em)
{
	if (block_em) {
		/* verify it starts with lock_ */
		if (!strncmp(sdp->sd_lockproto, "lock_", 5)) {
			/* Change lock_ to fsck_ */
			memcpy(sdp->sd_lockproto, "fsck_", 5);
		}
		/* FIXME: Need to do other verification in the else
		 * case */
	} else {
		/* verify it starts with fsck_ */
		/* verify it starts with lock_ */
		if (!strncmp(sdp->sd_lockproto, "fsck_", 5)) {
			/* Change fsck_ to lock_ */
			memcpy(sdp->sd_lockproto, "lock_", 5);
		}
	}

	if (lgfs2_sb_write(sdp, sdp->device_fd)) {
		stack;
		return -1;
	}
	return 0;
}

static void dup_free(struct fsck_cx *cx)
{
	struct osi_node *n;
	struct duptree *dt;

	while ((n = osi_first(&cx->dup_blocks))) {
		dt = (struct duptree *)n;
		dup_delete(cx, dt);
	}
}

static void dirtree_free(struct fsck_cx *cx)
{
	struct osi_node *n;
	struct dir_info *dt;

	while ((n = osi_first(&cx->dirtree))) {
		dt = (struct dir_info *)n;
		dirtree_delete(cx, dt);
	}
}

static void inodetree_free(struct fsck_cx *cx)
{
	struct osi_node *n;
	struct inode_info *dt;

	while ((n = osi_first(&cx->inodetree))) {
		dt = (struct inode_info *)n;
		inodetree_delete(cx, dt);
	}
}

/*
 * empty_super_block - free all structures in the super block
 * sdp: the in-core super block
 *
 * This function frees all allocated structures within the
 * super block.  It does not free the super block itself.
 *
 * Returns: Nothing
 */
static void empty_super_block(struct fsck_cx *cx)
{
	log_info( _("Freeing buffers.\n"));
	lgfs2_rgrp_free(cx->sdp, &cx->sdp->rgtree);

	inodetree_free(cx);
	dirtree_free(cx);
	dup_free(cx);
}


/**
 * set_block_ranges
 * @sdp: superblock
 *
 * Uses info in rgrps and jindex to determine boundaries of the
 * file system.
 *
 * Returns: 0 on success, -1 on failure
 */
static int set_block_ranges(struct lgfs2_sbd *sdp)
{
	struct osi_node *n, *next = NULL;
	struct lgfs2_rgrp_tree *rgd;
	uint64_t rmax = 0;
	uint64_t rmin = 0;
	ssize_t count;
	char *buf;

	log_info( _("Setting block ranges..."));

	for (n = osi_first(&sdp->rgtree); n; n = next) {
		next = osi_next(n);
		rgd = (struct lgfs2_rgrp_tree *)n;
		if (rgd->rt_data0 + rgd->rt_data &&
		    rgd->rt_data0 + rgd->rt_data - 1 > rmax)
			rmax = rgd->rt_data0 + rgd->rt_data - 1;
		if (!rmin || rgd->rt_data0 < rmin)
			rmin = rgd->rt_data0;
	}

	last_fs_block = rmax;
	if (last_fs_block > 0xffffffff && sizeof(unsigned long) <= 4) {
		log_crit(_("This file system is too big for this computer to handle.\n"));
		log_crit(_("Last fs block = 0x%"PRIx64", but sizeof(unsigned long) is %zu bytes.\n"),
		         last_fs_block, sizeof(unsigned long));
		goto fail;
	}

	last_data_block = rmax;
	first_data_block = rmin;

	buf = calloc(1, sdp->sd_bsize);
	if (buf == NULL) {
		log_crit(_("Failed to determine file system boundaries: %s\n"), strerror(errno));
		return -1;
	}
	count = pread(sdp->device_fd, buf, sdp->sd_bsize, (last_fs_block * sdp->sd_bsize));
	free(buf);
	if (count != sdp->sd_bsize) {
		log_crit(_("Failed to read highest block number (%"PRIx64"): %s\n"),
		         last_fs_block, strerror(errno));
		goto fail;
	}

	log_info(_("0x%"PRIx64" to 0x%"PRIx64"\n"), first_data_block, last_data_block);
	return 0;

 fail:
	log_info( _("Error\n"));
	return -1;
}

/**
 * check_rgrp_integrity - verify a rgrp free block count against the bitmap
 */
static void check_rgrp_integrity(struct fsck_cx *cx, struct lgfs2_rgrp_tree *rgd,
				 int *fixit, int *this_rg_fixed,
				 int *this_rg_bad, int *this_rg_cleaned)
{
	uint32_t rg_free, rg_reclaimed, rg_unlinked, rg_usedmeta, rg_useddi;
	int rgb, x, y, off, bytes_to_check, total_bytes_to_check, asked = 0;
	struct lgfs2_sbd *sdp = cx->sdp;
	unsigned int state;
	uint64_t diblock;
	struct lgfs2_buffer_head *bh;

	rg_free = rg_reclaimed = rg_unlinked = rg_usedmeta = rg_useddi = 0;
	total_bytes_to_check = rgd->rt_bitbytes;

	*this_rg_fixed = *this_rg_bad = *this_rg_cleaned = 0;

	diblock = rgd->rt_data0;
	for (rgb = 0; rgb < rgd->rt_length; rgb++){
		/* Count up the free blocks in the bitmap */
		off = (rgb) ? sizeof(struct gfs2_meta_header) :
			sizeof(struct gfs2_rgrp);
		if (total_bytes_to_check <= sdp->sd_bsize - off)
			bytes_to_check = total_bytes_to_check;
		else
			bytes_to_check = sdp->sd_bsize - off;
		total_bytes_to_check -= bytes_to_check;
		for (x = 0; x < bytes_to_check; x++) {
			unsigned char *byte;

			byte = (unsigned char *)&rgd->rt_bits[rgb].bi_data[off + x];
			if (*byte == 0x55) {
				diblock += GFS2_NBBY;
				continue;
			}
			if (*byte == 0x00) {
				diblock += GFS2_NBBY;
				rg_free += GFS2_NBBY;
				continue;
			}
			for (y = 0; y < GFS2_NBBY; y++) {
				state = (*byte >>
					 (GFS2_BIT_SIZE * y)) & GFS2_BIT_MASK;
				if (state == GFS2_BLKST_USED) {
					diblock++;
					continue;
				}
				if (state == GFS2_BLKST_DINODE) {
					diblock++;
					continue;
				}
				if (state == GFS2_BLKST_FREE) {
					diblock++;
					rg_free++;
					continue;
				}
				/* GFS2_BLKST_UNLINKED */
				log_info(_("Unlinked dinode 0x%"PRIx64" found.\n"), diblock);
				if (!asked) {
					asked = 1;
					if (query(cx, _("Okay to reclaim free "
							"metadata in resource group "
							"%"PRIu64" (0x%"PRIx64")? (y/n)"),
						  rgd->rt_addr, rgd->rt_addr))
						*fixit = 1;
				}
				if (!(*fixit)) {
					rg_unlinked++;
					diblock++;
					continue;
				}
				*byte &= ~(GFS2_BIT_MASK <<
					   (GFS2_BIT_SIZE * y));
				rgd->rt_bits[rgb].bi_modified = 1;
				rg_reclaimed++;
				rg_free++;
				rgd->rt_free++;
				log_info(_("Free metadata block %"PRIu64" (0x%"PRIx64") reclaimed.\n"),
				         diblock, diblock);
				bh = lgfs2_bread(sdp, diblock);
				if (!lgfs2_check_meta(bh->b_data, GFS2_METATYPE_DI)) {
					struct lgfs2_inode *ip =
						fsck_inode_get(sdp, rgd, bh);
					if (ip->i_blocks > 1) {
						blks_2free += ip->i_blocks - 1;
						log_info(_("%"PRIu64" blocks "
							   "(total) may need "
							   "to be freed in "
							   "pass 5.\n"),
							 blks_2free);
					}
					fsck_inode_put(&ip);
				}
				lgfs2_brelse(bh);
				diblock++;
			}
		}
	}
	/* The unlinked blocks we reclaim shouldn't be considered errors,
	   since we're just reclaiming them as a courtesy. If we already
	   got permission to reclaim them, we adjust the rgrp counts
	   accordingly. That way, only "real" rgrp count inconsistencies
	   will be reported. */
	if (rg_reclaimed && *fixit) {
		lgfs2_rgrp_out(rgd, rgd->rt_bits[0].bi_data);
		rgd->rt_bits[0].bi_modified = 1;
		*this_rg_cleaned = 1;
		log_info(_("The rgrp at %"PRIu64" (0x%"PRIx64") was cleaned of %d "
			    "free metadata blocks.\n"),
		         rgd->rt_addr, rgd->rt_addr, rg_reclaimed);
	}
	if (rgd->rt_free != rg_free) {
		*this_rg_bad = 1;
		*this_rg_cleaned = 0;
		log_err( _("Error: resource group %"PRIu64" (0x%"PRIx64"): "
			   "free space (%d) does not match bitmap (%d)\n"),
		        rgd->rt_addr, rgd->rt_addr, rgd->rt_free, rg_free);
		if (query(cx, _("Fix the rgrp free blocks count? (y/n)"))) {
			rgd->rt_free = rg_free;
			lgfs2_rgrp_out(rgd, rgd->rt_bits[0].bi_data);
			rgd->rt_bits[0].bi_modified = 1;
			*this_rg_fixed = 1;
			log_err( _("The rgrp was fixed.\n"));
		} else
			log_err( _("The rgrp was not fixed.\n"));
	}
}

/**
 * check_rgrps_integrity - verify rgrp consistency
 * Note: We consider an rgrp "cleaned" if the unlinked meta blocks are
 *       cleaned, so not quite "bad" and not quite "good" but rewritten anyway.
 *
 * Returns: 0 on success, 1 if errors were detected
 */
static void check_rgrps_integrity(struct fsck_cx *cx)
{
	struct osi_node *n, *next = NULL;
	int rgs_good = 0, rgs_bad = 0, rgs_fixed = 0, rgs_cleaned = 0;
	int was_bad = 0, was_fixed = 0, was_cleaned = 0;
	struct lgfs2_rgrp_tree *rgd;
	int reclaim_unlinked = 0;

	log_info( _("Checking the integrity of all resource groups.\n"));
	for (n = osi_first(&cx->sdp->rgtree); n; n = next) {
		next = osi_next(n);
		rgd = (struct lgfs2_rgrp_tree *)n;
		if (fsck_abort)
			return;
		check_rgrp_integrity(cx, rgd, &reclaim_unlinked,
				     &was_fixed, &was_bad, &was_cleaned);
		if (was_fixed)
			rgs_fixed++;
		if (was_cleaned)
			rgs_cleaned++;
		else if (was_bad)
			rgs_bad++;
		else
			rgs_good++;
	}
	if (rgs_bad || rgs_cleaned) {
		log_err( _("RGs: Consistent: %d   Cleaned: %d   Inconsistent: "
			   "%d   Fixed: %d   Total: %d\n"),
			 rgs_good, rgs_cleaned, rgs_bad, rgs_fixed,
			 rgs_good + rgs_bad + rgs_cleaned);
		if (rgs_cleaned && blks_2free)
			log_err(_("%"PRIu64" blocks may need to be freed in pass 5 "
				  "due to the cleaned resource groups.\n"),
			        blks_2free);
	}
}

static int rebuild_sysdir(struct fsck_cx *cx)
{
	struct lgfs2_sbd *sdp = cx->sdp;
	struct lgfs2_inum inum;
	struct lgfs2_buffer_head *bh = NULL;
	int err = 0;

	log_err(_("The system directory seems to be destroyed.\n"));
	if (!query(cx, _("Okay to rebuild it? (y/n)"))) {
		log_err(_("System directory not rebuilt; aborting.\n"));
		return -1;
	}
	log_err(_("Trying to rebuild the master directory.\n"));
	inum.in_formal_ino = sdp->md.next_inum++;
	inum.in_addr = sdp->sd_meta_dir.in_addr;
	err = lgfs2_init_dinode(sdp, &bh, &inum, S_IFDIR | 0755, GFS2_DIF_SYSTEM, &inum);
	if (err != 0)
		return -1;
	sdp->master_dir = lgfs2_inode_get(sdp, bh);
	if (sdp->master_dir == NULL) {
		log_crit(_("Error reading master: %s\n"), strerror(errno));
		return -1;
	}
	sdp->master_dir->bh_owned = 1;

	if (fix_md.jiinode) {
		inum.in_formal_ino = sdp->md.next_inum++;
		inum.in_addr = fix_md.jiinode->i_num.in_addr;
		err = lgfs2_dir_add(sdp->master_dir, "jindex", 6, &inum,
		              IF2DT(S_IFDIR | 0700));
		if (err) {
			log_crit(_("Error %d adding jindex directory\n"), errno);
			exit(FSCK_ERROR);
		}
		sdp->master_dir->i_nlink++;
	} else {
		err = build_jindex(cx);
		if (err) {
			log_crit(_("Error %d building jindex\n"), err);
			exit(FSCK_ERROR);
		}
	}

	if (fix_md.pinode) {
		inum.in_formal_ino = sdp->md.next_inum++;
		inum.in_addr = fix_md.pinode->i_num.in_addr;
		/* coverity[deref_arg:SUPPRESS] */
		err = lgfs2_dir_add(sdp->master_dir, "per_node", 8, &inum,
			IF2DT(S_IFDIR | 0700));
		if (err) {
			log_crit(_("Error %d adding per_node directory\n"),
				 errno);
			exit(FSCK_ERROR);
		}
		sdp->master_dir->i_nlink++;
	} else {
		/* coverity[double_free:SUPPRESS] */
		err = build_per_node(cx);
		if (err) {
			log_crit(_("Error %d building per_node directory\n"),
			         err);
			exit(FSCK_ERROR);
		}
	}

	if (fix_md.inum) {
		inum.in_formal_ino = sdp->md.next_inum++;
		inum.in_addr = fix_md.inum->i_num.in_addr;
		/* coverity[deref_arg:SUPPRESS] */
		err = lgfs2_dir_add(sdp->master_dir, "inum", 4, &inum,
			IF2DT(S_IFREG | 0600));
		if (err) {
			log_crit(_("Error %d adding inum inode\n"), errno);
			exit(FSCK_ERROR);
		}
	} else {
		sdp->md.inum = lgfs2_build_inum(sdp);
		if (sdp->md.inum == NULL) {
			log_crit(_("Error building inum inode: %s\n"), strerror(errno));
			exit(FSCK_ERROR);
		}
		/* Write the inode but don't free it, to avoid doing an extra lookup */
		/* coverity[deref_after_free:SUPPRESS] */
		lgfs2_dinode_out(sdp->md.inum, sdp->md.inum->i_bh->b_data);
		lgfs2_bwrite(sdp->md.inum->i_bh);
	}

	if (fix_md.statfs) {
		inum.in_formal_ino = sdp->md.next_inum++;
		inum.in_addr = fix_md.statfs->i_num.in_addr;
		/* coverity[deref_arg:SUPPRESS] */
		err = lgfs2_dir_add(sdp->master_dir, "statfs", 6, &inum,
			      IF2DT(S_IFREG | 0600));
		if (err) {
			log_crit(_("Error %d adding statfs inode\n"), errno);
			exit(FSCK_ERROR);
		}
	} else {
		sdp->md.statfs = lgfs2_build_statfs(sdp);
		if (sdp->md.statfs == NULL) {
			log_crit(_("Error %d building statfs inode\n"), err);
			exit(FSCK_ERROR);
		}
		/* Write the inode but don't free it, to avoid doing an extra lookup */
		/* coverity[deref_after_free:SUPPRESS] */
		lgfs2_dinode_out(sdp->md.statfs, sdp->md.statfs->i_bh->b_data);
		lgfs2_bwrite(sdp->md.statfs->i_bh);
	}

	if (fix_md.riinode) {
		inum.in_formal_ino = sdp->md.next_inum++;
		inum.in_addr = fix_md.riinode->i_num.in_addr;
		/* coverity[deref_arg:SUPPRESS] */
		err = lgfs2_dir_add(sdp->master_dir, "rindex", 6, &inum,
			IF2DT(S_IFREG | 0600));
		if (err) {
			log_crit(_("Error %d adding rindex inode\n"), errno);
			exit(FSCK_ERROR);
		}
	} else {
		/* coverity[double_free:SUPPRESS] */
		struct lgfs2_inode *rip = lgfs2_build_rindex(sdp);
		if (rip == NULL) {
			log_crit(_("Error building rindex inode: %s\n"), strerror(errno));
			exit(FSCK_ERROR);
		}
		lgfs2_inode_put(&rip);
	}

	if (fix_md.qinode) {
		inum.in_formal_ino = sdp->md.next_inum++;
		inum.in_addr = fix_md.qinode->i_num.in_addr;
		err = lgfs2_dir_add(sdp->master_dir, "quota", 5, &inum,
			IF2DT(S_IFREG | 0600));
		if (err) {
			log_crit(_("Error %d adding quota inode\n"), errno);
			exit(FSCK_ERROR);
		}
	} else {
		struct lgfs2_inode *qip = lgfs2_build_quota(sdp);
		if (qip == NULL) {
			log_crit(_("Error building quota inode: %s\n"), strerror(errno));
			exit(FSCK_ERROR);
		}
		lgfs2_inode_put(&qip);
	}

	log_err(_("Master directory rebuilt.\n"));
	lgfs2_inode_put(&sdp->md.inum);
	lgfs2_inode_put(&sdp->md.statfs);
	lgfs2_inode_put(&sdp->master_dir);
	return 0;
}

/**
 * lookup_per_node - Make sure the per_node directory is read in
 *
 * This function is used to read in the per_node directory.  It is called
 * twice.  The first call tries to read in the dinode early on.  That ensures
 * that if any journals are missing, we can figure out the number of journals
 * from per_node.  However, we unfortunately can't rebuild per_node at that
 * point in time because our resource groups aren't read in yet.
 * The second time it's called is much later when we can rebuild it.
 *
 * allow_rebuild: 0 if rebuilds are not allowed
 *                1 if rebuilds are allowed
 */
static void lookup_per_node(struct fsck_cx *cx, int allow_rebuild)
{
	struct lgfs2_sbd *sdp = cx->sdp;

	if (sdp->md.pinode)
		return;

	sdp->md.pinode = lgfs2_lookupi(sdp->master_dir, "per_node", 8);
	if (sdp->md.pinode)
		return;
	if (!allow_rebuild) {
		log_err( _("The gfs2 system per_node directory "
			   "inode is missing, so we might not be \nable to "
			   "rebuild missing journals this run.\n"));
		return;
	}

	if (query(cx, _("The gfs2 system per_node directory "
		     "inode is missing. Okay to rebuild it? (y/n) "))) {
		int err;

		/* coverity[freed_arg:SUPPRESS] False positive */
		err = build_per_node(cx);
		if (err) {
			log_crit(_("Error %d rebuilding per_node directory\n"),
				 err);
			exit(FSCK_ERROR);
		}
	}
	/* coverity[identity_transfer:SUPPRESS] False positive */
	sdp->md.pinode = lgfs2_lookupi(sdp->master_dir, "per_node", 8);
	if (!sdp->md.pinode) {
		log_err( _("Unable to rebuild per_node; aborting.\n"));
		exit(FSCK_ERROR);
	}
}

#define RA_WINDOW 32

static unsigned rgrp_reada(struct lgfs2_sbd *sdp, unsigned cur_window,
				struct osi_node *n)
{
	struct lgfs2_rgrp_tree *rgd;
	unsigned i;
	off_t start, len;

	for (i = 0; i < RA_WINDOW; i++, n = osi_next(n)) {
		if (n == NULL)
			return i;
		if (i < cur_window)
			continue;
		rgd = (struct lgfs2_rgrp_tree *)n;
		start = rgd->rt_addr * sdp->sd_bsize;
		len = rgd->rt_length * sdp->sd_bsize;
		(void)posix_fadvise(sdp->device_fd, start, len, POSIX_FADV_WILLNEED);
	}

	return i;
}

/**
 * read_rgrps - attach rgrps to the super block
 * @sdp: incore superblock data
 * @expected: number of resource groups expected (rindex entries)
 *
 * Given the rgrp index inode, link in all rgrps into the super block
 * and be sure that they can be read.
 *
 * Returns: 0 on success, -1 on failure.
 */
static int read_rgrps(struct lgfs2_sbd *sdp, uint64_t expected)
{
	struct lgfs2_rgrp_tree *rgd;
	uint64_t count = 0;
	uint64_t errblock = 0;
	uint64_t rmax = 0;
	struct osi_node *n, *next = NULL;
	unsigned ra_window = 0;

	/* Turn off generic readhead */
	(void)posix_fadvise(sdp->device_fd, 0, 0, POSIX_FADV_RANDOM);

	for (n = osi_first(&sdp->rgtree); n; n = next) {
		next = osi_next(n);
		rgd = (struct lgfs2_rgrp_tree *)n;
		/* Readahead resource group headers */
		if (ra_window < RA_WINDOW/2)
			ra_window = rgrp_reada(sdp, ra_window, n);
		/* Read resource group header */
		errblock = lgfs2_rgrp_read(sdp, rgd);
		if (errblock)
			return errblock;
		ra_window--;
		count++;
		if (rgd->rt_data0 + rgd->rt_data - 1 > rmax)
			rmax = rgd->rt_data0 + rgd->rt_data - 1;
	}

	sdp->fssize = rmax;
	if (count != expected)
		goto fail;

	(void)posix_fadvise(sdp->device_fd, 0, 0, POSIX_FADV_NORMAL);
	return 0;

 fail:
	(void)posix_fadvise(sdp->device_fd, 0, 0, POSIX_FADV_NORMAL);
	lgfs2_rgrp_free(sdp, &sdp->rgtree);
	return -1;
}

static int fetch_rgrps_level(struct fsck_cx *cx, enum rgindex_trust_level lvl, uint64_t *count, int *ok)
{
	int ret = 1;

	const char *level_desc[] = {
		_("Checking if all rgrp and rindex values are good"),
		_("Checking if rindex values may be easily repaired"),
		_("Calculating where the rgrps should be if evenly spaced"),
		_("Trying to rebuild rindex assuming evenly spaced rgrps"),
		_("Trying to rebuild rindex assuming unevenly spaced rgrps"),
	};
	const char *fail_desc[] = {
		_("Some damage was found; we need to take remedial measures"),
		_("rindex is unevenly spaced: either gfs1-style or corrupt"),
		_("rindex calculations don't match: uneven rgrp boundaries"),
		_("Too many rgrp misses: rgrps must be unevenly spaced"),
		_("Too much damage found: we cannot rebuild this rindex"),
	};

	log_notice(_("Level %d resource group check: %s.\n"), lvl + 1, level_desc[lvl]);

	if (rindex_repair(cx, lvl, ok) != 0)
		goto fail;

	if (lgfs2_rindex_read(cx->sdp, count, ok) != 0 || !*ok)
		goto fail;

	ret = read_rgrps(cx->sdp, *count);
	if (ret != 0)
		goto fail;

	log_notice(_("(level %d passed)\n"), lvl + 1);
	return 0;
fail:
	if (ret == -1)
		log_err(_("(level %d failed: %s)\n"), lvl + 1, fail_desc[lvl]);
	else
		log_err(_("(level %d failed at block %d (0x%x): %s)\n"), lvl + 1,
		        ret, ret, fail_desc[lvl]);
	return ret;
}

/**
 * fetch_rgrps - fetch the resource groups from disk, and check their integrity
 */
static int fetch_rgrps(struct fsck_cx *cx)
{
	enum rgindex_trust_level trust_lvl;
	uint64_t rgcount;
	int ok = 1;

	log_notice(_("Validating resource group index.\n"));
	for (trust_lvl = BLIND_FAITH; trust_lvl <= INDIGNATION; trust_lvl++) {
		int ret = 0;

		ret = fetch_rgrps_level(cx, trust_lvl, &rgcount, &ok);
		if (ret == 0)
			break;
		if (fsck_abort)
			break;
	}
	if (trust_lvl > INDIGNATION) {
		log_err( _("Resource group recovery impossible; I can't fix "
			   "this file system.\n"));
		return -1;
	}
	log_info( _("%"PRIu64" resource groups found.\n"), rgcount);

	check_rgrps_integrity(cx);
	return 0;
}

/**
 * init_system_inodes
 *
 * Returns: 0 on success, -1 on failure
 */
static int init_system_inodes(struct fsck_cx *cx)
{
	struct lgfs2_sbd *sdp = cx->sdp;
	__be64 inumbuf = 0;
	char *buf;
	int err;

	log_info( _("Initializing special inodes...\n"));

	/* Get root dinode */
	sdp->md.rooti = lgfs2_inode_read(sdp, sdp->sd_root_dir.in_addr);
	if (sdp->md.rooti == NULL)
		return -1;

	/* Look for "inum" entry in master dinode */
	sdp->md.inum = lgfs2_lookupi(sdp->master_dir, "inum", 4);
	if (!sdp->md.inum) {
		if (!query(cx, _("The gfs2 system inum inode is missing. "
			      "Okay to rebuild it? (y/n) "))) {
			log_err( _("fsck.gfs2 cannot continue without "
				   "a valid inum file; aborting.\n"));
			goto fail;
		}
		sdp->md.inum = lgfs2_build_inum(sdp);
		if (sdp->md.inum == NULL) {
			log_crit(_("Error rebuilding inum inode: %s\n"), strerror(errno));
			exit(FSCK_ERROR);
		}
		lgfs2_dinode_out(sdp->md.inum, sdp->md.inum->i_bh->b_data);
		if (lgfs2_bwrite(sdp->md.inum->i_bh) != 0) {
			log_crit(_("System inum inode was not rebuilt. Aborting.\n"));
			goto fail;
		}
	}
	/* Read inum entry into buffer */
	err = lgfs2_readi(sdp->md.inum, &inumbuf, 0,
			 sdp->md.inum->i_size);
	if (err != sdp->md.inum->i_size) {
		log_crit(_("Error %d reading system inum inode. "
			   "Aborting.\n"), err);
		goto fail;
	}
	/* call gfs2_inum_range_in() to retrieve range */
	sdp->md.next_inum = be64_to_cpu(inumbuf);

	sdp->md.statfs = lgfs2_lookupi(sdp->master_dir, "statfs", 6);
	if (!sdp->md.statfs) {
		if (!query(cx, _("The gfs2 system statfs inode is missing. "
			      "Okay to rebuild it? (y/n) "))) {
			log_err( _("fsck.gfs2 cannot continue without a valid "
				   "statfs file; aborting.\n"));
			goto fail;
		}
		sdp->md.statfs = lgfs2_build_statfs(sdp);
		if (sdp->md.statfs == NULL) {
			log_crit(_("Error %d rebuilding statfs inode\n"), err);
			exit(FSCK_ERROR);
		}
		lgfs2_dinode_out(sdp->md.statfs, sdp->md.statfs->i_bh->b_data);
		if (lgfs2_bwrite(sdp->md.statfs->i_bh) != 0) {
			log_err( _("Rebuild of statfs system file failed."));
			log_err( _("fsck.gfs2 cannot continue without "
				   "a valid statfs file; aborting.\n"));
			goto fail;
		}
		lgfs2_init_statfs(sdp, NULL);
	}
	if (sdp->md.statfs->i_size) {
		buf = malloc(sdp->md.statfs->i_size);
		if (buf) {
			err = lgfs2_readi(sdp->md.statfs, buf, 0,
					 sdp->md.statfs->i_size);
			if (err != sdp->md.statfs->i_size) {
				log_crit(_("Error %d reading statfs file. "
					   "Aborting.\n"), err);
				free(buf);
				goto fail;
			}
			free(buf);
		}
	}

	sdp->md.qinode = lgfs2_lookupi(sdp->master_dir, "quota", 5);
	if (!sdp->md.qinode) {
		if (!query(cx, _("The gfs2 system quota inode is missing. "
			      "Okay to rebuild it? (y/n) "))) {
			log_crit(_("System quota inode was not "
				   "rebuilt.  Aborting.\n"));
			goto fail;
		}
		sdp->md.qinode = lgfs2_build_quota(sdp);
		if (sdp->md.qinode == NULL) {
			log_crit(_("Error rebuilding quota inode: %s\n"), strerror(errno));
			exit(FSCK_ERROR);
		}
		lgfs2_dinode_out(sdp->md.qinode, sdp->md.qinode->i_bh->b_data);
		if (lgfs2_bwrite(sdp->md.qinode->i_bh) != 0) {
			log_crit(_("Unable to rebuild system quota file "
				   "inode.  Aborting.\n"));
			goto fail;
		}
	}

	/* Try to lookup the per_node inode.  If it was missing, it is now
	   safe to rebuild it. */
	lookup_per_node(cx, 1);

	/*******************************************************************
	 *******  Now, set boundary fields in the super block  *************
	 *******************************************************************/
	if (set_block_ranges(sdp)){
		log_err( _("Unable to determine the boundaries of the"
			" file system.\n"));
		goto fail;
	}

	return 0;
 fail:
	empty_super_block(cx);

	return -1;
}

/**
 * is_journal_copy - Is this a "real" dinode or a copy inside a journal?
 * A real dinode will be located at the block number in its no_addr.
 * A journal-copy will be at a different block (inside the journal).
 */
static int is_journal_copy(struct lgfs2_inode *ip)
{
	if (ip->i_num.in_addr == ip->i_bh->b_blocknr)
		return 0;
	return 1; /* journal copy */
}

/**
 * peruse_system_dinode - process a system dinode
 *
 * This function looks at a system dinode and tries to figure out which
 * dinode it is: statfs, inum, per_node, master, etc.  Some of them we
 * can deduce from the contents.  For example, di_size will be a multiple
 * of 96 for the rindex.  di_size will be 8 for inum, 24 for statfs, etc.
 * the per_node directory will have a ".." entry that will lead us to
 * the master dinode if it's been destroyed.
 */
static void peruse_system_dinode(struct fsck_cx *cx, struct lgfs2_inode *ip)
{
	struct lgfs2_sbd *sdp = cx->sdp;
	struct lgfs2_inode *child_ip;
	struct lgfs2_inum inum;
	int error;

	if (ip->i_num.in_formal_ino == 2) {
		if (sdp->sd_meta_dir.in_addr)
			return;
		log_warn(_("Found system master directory at: 0x%"PRIx64".\n"),
			 ip->i_num.in_addr);
		sdp->sd_meta_dir.in_addr = ip->i_num.in_addr;
		return;
	}
	if (ip->i_num.in_formal_ino == 3) {
		if (fix_md.jiinode || is_journal_copy(ip))
			goto out_discard_ip;
		log_warn(_("Found system jindex file at: 0x%"PRIx64"\n"), ip->i_num.in_addr);
		fix_md.jiinode = ip;
	} else if (is_dir(ip)) {
		/* Check for a jindex dir entry. Only one system dir has a
		   jindex: master */
		/* coverity[identity_transfer:SUPPRESS] */
		child_ip = lgfs2_lookupi(ip, "jindex", 6);
		if (child_ip) {
			if (fix_md.jiinode || is_journal_copy(ip)) {
				lgfs2_inode_put(&child_ip);
				goto out_discard_ip;
			}
			fix_md.jiinode = child_ip;
			sdp->sd_meta_dir.in_addr = ip->i_num.in_addr;
			log_warn(_("Found system master directory at: 0x%"PRIx64"\n"),
			         ip->i_num.in_addr);
			return;
		}

		/* Check for a statfs_change0 dir entry. Only one system dir
		   has a statfs_change: per_node, and its .. will be master. */
		/* coverity[identity_transfer:SUPPRESS] */
		child_ip = lgfs2_lookupi(ip, "statfs_change0", 14);
		if (child_ip) {
			lgfs2_inode_put(&child_ip);
			if (fix_md.pinode || is_journal_copy(ip))
				goto out_discard_ip;
			log_warn(_("Found system per_node directory at: 0x%"PRIx64"\n"),
			         ip->i_num.in_addr);
			fix_md.pinode = ip;
			error = lgfs2_dir_search(ip, "..", 2, NULL, &inum);
			if (!error && inum.in_addr) {
				sdp->sd_meta_dir.in_addr = inum.in_addr;
				log_warn(_("From per_node's '..' master directory backtracked to: "
					   "0x%"PRIx64"\n"), inum.in_addr);
			}
			return;
		}
		log_debug(_("Unknown system directory at block 0x%"PRIx64"\n"), ip->i_num.in_addr);
		goto out_discard_ip;
	} else if (ip->i_size == 8) {
		if (fix_md.inum || is_journal_copy(ip))
			goto out_discard_ip;
		fix_md.inum = ip;
		log_warn(_("Found system inum file at: 0x%"PRIx64"\n"), ip->i_num.in_addr);
	} else if (ip->i_size == 24) {
		if (fix_md.statfs || is_journal_copy(ip))
			goto out_discard_ip;
		fix_md.statfs = ip;
		log_warn(_("Found system statfs file at: 0x%"PRIx64"\n"), ip->i_num.in_addr);
	} else if ((ip->i_size % 96) == 0) {
		if (fix_md.riinode || is_journal_copy(ip))
			goto out_discard_ip;
		fix_md.riinode = ip;
		log_warn(_("Found system rindex file at: 0x%"PRIx64"\n"), ip->i_num.in_addr);
	} else if (!fix_md.qinode && ip->i_size >= 176 &&
	           ip->i_num.in_formal_ino >= 12 &&
	           ip->i_num.in_formal_ino <= 100) {
		if (is_journal_copy(ip))
			goto out_discard_ip;
		fix_md.qinode = ip;
		log_warn(_("Found system quota file at: 0x%"PRIx64"\n"), ip->i_num.in_addr);
	} else {
out_discard_ip:
		lgfs2_inode_put(&ip);
	}
}

/**
 * peruse_user_dinode - process a user dinode trying to find the root directory
 *
 */
static void peruse_user_dinode(struct fsck_cx *cx, struct lgfs2_inode *ip)
{
	struct lgfs2_sbd *sdp = cx->sdp;
	struct lgfs2_inode *parent_ip;
	struct lgfs2_inum inum;
	int error;

	if (sdp->sd_root_dir.in_addr) /* if we know the root dinode */
		return;             /* we don't need to find the root */
	if (!is_dir(ip))  /* if this isn't a directory */
		return;             /* it can't lead us to the root anyway */

	if (ip->i_num.in_formal_ino == 1) {
		struct lgfs2_buffer_head *root_bh;

		if (ip->i_num.in_addr == ip->i_bh->b_blocknr) {
			log_warn(_("Found the root directory at: 0x%"PRIx64".\n"),
			         ip->i_num.in_addr);
			sdp->sd_root_dir.in_addr = ip->i_num.in_addr;
			return;
		}
		log_warn(_("The root dinode should be at block 0x%"PRIx64" but it "
			   "seems to be destroyed.\n"),
		         ip->i_num.in_addr);
		log_warn(_("Found a copy of the root directory in a journal "
			   "at block: 0x%"PRIx64".\n"),
			 ip->i_bh->b_blocknr);
		if (!query(cx, _("Do you want to replace the root dinode from the copy? (y/n)"))) {
			log_err(_("Damaged root dinode not fixed.\n"));
			return;
		}
		root_bh = lgfs2_bread(sdp, ip->i_num.in_addr);
		memcpy(root_bh->b_data, ip->i_bh->b_data, sdp->sd_bsize);
		lgfs2_bmodified(root_bh);
		lgfs2_brelse(root_bh);
		log_warn(_("Root directory copied from the journal.\n"));
		return;
	}
	/* coverity[check_after_deref:SUPPRESS] */
	while (ip) {
		/* coverity[identity_transfer:SUPPRESS] */
		parent_ip = lgfs2_lookupi(ip, "..", 2);
		if (parent_ip && parent_ip->i_num.in_addr == ip->i_num.in_addr) {
			log_warn(_("Found the root directory at: 0x%"PRIx64"\n"),
				 ip->i_num.in_addr);
			sdp->sd_root_dir.in_addr = ip->i_num.in_addr;
			lgfs2_inode_put(&parent_ip);
			lgfs2_inode_put(&ip);
			return;
		}
		if (!parent_ip)
			break;
		lgfs2_inode_put(&ip);
		ip = parent_ip;
	}
	error = lgfs2_dir_search(ip, "..", 2, NULL, &inum);
	if (!error && inum.in_addr && inum.in_addr < possible_root) {
			possible_root = inum.in_addr;
			log_debug(_("Found a possible root at: 0x%"PRIx64"\n"),
				  possible_root);
	}
	lgfs2_inode_put(&ip);
}

/**
 * find_rgs_for_bsize - check a range of blocks for rgrps to determine bsize.
 * Assumes: device is open.
 */
static int find_rgs_for_bsize(struct lgfs2_sbd *sdp, uint64_t startblock,
			      uint32_t *known_bsize)
{
	uint64_t blk, max_rg_size, rb_addr;
	uint32_t bsize, bsize2;
	int found_rg;

	sdp->sd_bsize = LGFS2_DEFAULT_BSIZE;
	max_rg_size = 524288;
	/* Max RG size is 2GB. Max block size is 4K. 2G / 4K blks = 524288,
	   So this is traversing 2GB in 4K block increments. */
	for (blk = startblock; blk < startblock + max_rg_size; blk++) {
		struct lgfs2_buffer_head *bh = lgfs2_bread(sdp, blk);

		found_rg = 0;
		for (bsize = 0; bsize < LGFS2_DEFAULT_BSIZE; bsize += GFS2_BASIC_BLOCK) {
			struct gfs2_meta_header mhp;

			memcpy(&mhp, bh->b_data + bsize, sizeof(mhp));
			if (be32_to_cpu(mhp.mh_magic) != GFS2_MAGIC)
				continue;
			if (be32_to_cpu(mhp.mh_type) == GFS2_METATYPE_RG) {
				found_rg = 1;
				break;
			}
		}
		lgfs2_bfree(&bh);
		if (!found_rg)
			continue;
		/* Try all the block sizes in 512 byte multiples */
		for (bsize2 = GFS2_BASIC_BLOCK; bsize2 <= LGFS2_DEFAULT_BSIZE;
		     bsize2 += GFS2_BASIC_BLOCK) {
			struct lgfs2_buffer_head *rb_bh;
			struct gfs2_meta_header *mh;
			int is_rb;

			rb_addr = (blk * (LGFS2_DEFAULT_BSIZE / bsize2)) +
				(bsize / bsize2) + 1;
			sdp->sd_bsize = bsize2; /* temporarily */
			rb_bh = lgfs2_bread(sdp, rb_addr);
			mh = (struct gfs2_meta_header *)rb_bh->b_data;
			is_rb = (be32_to_cpu(mh->mh_magic) == GFS2_MAGIC &&
			         be32_to_cpu(mh->mh_type) == GFS2_METATYPE_RB);
			lgfs2_brelse(rb_bh);
			if (is_rb) {
				log_debug(_("boff:%d bsize2:%d rg:0x%"PRIx64", "
					    "rb:0x%"PRIx64"\n"), bsize, bsize2,
				          blk, rb_addr);
				*known_bsize = bsize2;
				break;
			}
		}
		if (!(*known_bsize)) {
			sdp->sd_bsize = LGFS2_DEFAULT_BSIZE;
			continue;
		}

		sdp->sd_bsize = *known_bsize;
		log_warn(_("Block size determined to be: %d\n"), *known_bsize);
		return 0;
	}
	return 0;
}

/**
 * peruse_metadata - check a range of blocks for metadata
 * Assumes: device is open.
 */
static int peruse_metadata(struct fsck_cx *cx, uint64_t startblock)
{
	struct lgfs2_sbd *sdp = cx->sdp;
	uint64_t blk, max_rg_size;
	struct lgfs2_buffer_head *bh;
	struct lgfs2_inode *ip;

	max_rg_size = 2147483648ull / sdp->sd_bsize;
	/* Max RG size is 2GB. 2G / bsize. */
	for (blk = startblock; blk < startblock + max_rg_size; blk++) {
		bh = lgfs2_bread(sdp, blk);
		if (lgfs2_check_meta(bh->b_data, GFS2_METATYPE_DI)) {
			lgfs2_brelse(bh);
			continue;
		}
		ip = lgfs2_inode_get(sdp, bh);
		if (ip == NULL)
			return -1;
		ip->bh_owned = 1; /* lgfs2_inode_put() will free the bh */
		if (ip->i_flags & GFS2_DIF_SYSTEM)
			peruse_system_dinode(cx, ip);
		else
			peruse_user_dinode(cx, ip);
	}
	return 0;
}

/**
 * sb_repair - repair a damaged superblock
 * Assumes: device is open.
 *          The biggest RG size is 2GB
 */
static int sb_repair(struct fsck_cx *cx)
{
	struct lgfs2_sbd *sdp = cx->sdp;
	uint64_t half;
	uint32_t known_bsize = 0;
	int error = 0;

	memset(&fix_md, 0, sizeof(fix_md));
	/* Step 1 - First we need to determine the correct block size. */
	sdp->sd_bsize = LGFS2_DEFAULT_BSIZE;
	log_warn(_("Gathering information to repair the gfs2 superblock.  "
		   "This may take some time.\n"));
	error = find_rgs_for_bsize(sdp, (GFS2_SB_ADDR * GFS2_BASIC_BLOCK) /
				   LGFS2_DEFAULT_BSIZE, &known_bsize);
	if (error)
		return error;
	if (!known_bsize) {
		log_warn(_("Block size not apparent; checking elsewhere.\n"));
		/* First, figure out the device size.  We need that so we can
		   find a suitable start point to determine what's what. */
		half = sdp->dinfo.size / 2; /* in bytes */
		half /= sdp->sd_bsize;
		/* Start looking halfway through the device for gfs2
		   structures.  If there aren't any at all, forget it. */
		error = find_rgs_for_bsize(sdp, half, &known_bsize);
		if (error)
			return error;
	}
	if (!known_bsize) {
		log_err(_("Unable to determine the block size; this "
			  "does not look like a gfs2 file system.\n"));
		return -1;
	}
	/* Step 2 - look for the sytem dinodes */
	error = peruse_metadata(cx, (GFS2_SB_ADDR * GFS2_BASIC_BLOCK) /
				LGFS2_DEFAULT_BSIZE);
	if (error)
		return error;
	if (!sdp->sd_meta_dir.in_addr) {
		log_err(_("Unable to locate the system master  directory.\n"));
		return -1;
	}
	if (!sdp->sd_root_dir.in_addr) {
		log_err(_("Unable to locate the root directory.\n"));
		if (possible_root == HIGHEST_BLOCK) {
			/* Take advantage of the fact that mkfs.gfs2
			   creates master immediately after root. */
			log_err(_("Can't find any dinodes that might "
				  "be the root; using master - 1.\n"));
			possible_root = sdp->sd_meta_dir.in_addr - 1;
		}
		log_err(_("Found a possible root at: 0x%"PRIx64"\n"), possible_root);
		sdp->sd_root_dir.in_addr = possible_root;
		sdp->md.rooti = lgfs2_inode_read(sdp, possible_root);
		if (!sdp->md.rooti || sdp->md.rooti->i_magic != GFS2_MAGIC) {
			struct lgfs2_buffer_head *bh = NULL;
			struct lgfs2_inum inum;

			log_err(_("The root dinode block is destroyed.\n"));
			log_err(_("At this point I recommend "
				  "reinitializing it.\n"
				  "Hopefully everything will later "
				  "be put into lost+found.\n"));
			if (!query(cx, _("Okay to reinitialize the root "
				     "dinode? (y/n)"))) {
				log_err(_("The root dinode was not "
					  "reinitialized; aborting.\n"));
				return -1;
			}
			inum.in_formal_ino = 1;
			inum.in_addr = possible_root;
			error = lgfs2_init_dinode(sdp, &bh, &inum, S_IFDIR | 0755, 0, &inum);
			if (error != 0)
				return -1;
			lgfs2_brelse(bh);
		}
	}
	/* Step 3 - Rebuild the lock protocol and file system table name */
	if (query(cx, _("Okay to fix the GFS2 superblock? (y/n)"))) {
		log_info(_("Found system master directory at: 0x%"PRIx64"\n"),
			 sdp->sd_meta_dir.in_addr);
		sdp->master_dir = lgfs2_inode_read(sdp, sdp->sd_meta_dir.in_addr);
		if (sdp->master_dir == NULL) {
			log_crit(_("Error reading master inode: %s\n"), strerror(errno));
			return -1;
		}
		sdp->master_dir->i_num.in_addr = sdp->sd_meta_dir.in_addr;
		log_info(_("Found the root directory at: 0x%"PRIx64"\n"),
			 sdp->sd_root_dir.in_addr);
		sdp->md.rooti = lgfs2_inode_read(sdp, sdp->sd_root_dir.in_addr);
		if (sdp->md.rooti == NULL) {
			log_crit(_("Error reading root inode: %s\n"), strerror(errno));
			return -1;
		}
		sdp->sd_fs_format = GFS2_FORMAT_FS;
		lgfs2_sb_write(sdp, sdp->device_fd);
		lgfs2_inode_put(&sdp->md.rooti);
		lgfs2_inode_put(&sdp->master_dir);
		sb_fixed = 1;
	} else {
		log_crit(_("GFS2 superblock not fixed; fsck cannot proceed "
			   "without a valid superblock.\n"));
		return -1;
	}
	return 0;
}

/**
 * fill_super_block
 * @sdp:
 *
 * Returns: 0 on success, -1 on failure
 */
static int fill_super_block(struct fsck_cx *cx)
{
	struct lgfs2_sbd *sdp = cx->sdp;
	int ret;

	sync();

	log_info( _("Initializing lists...\n"));
	sdp->rgtree.osi_node = NULL;

	sdp->sd_bsize = LGFS2_DEFAULT_BSIZE;
	if (lgfs2_compute_constants(sdp)) {
		log_crit("%s\n", _("Failed to compute file system constants"));
		return FSCK_ERROR;
	}
	ret = lgfs2_read_sb(sdp);
	if (ret < 0) {
		if (sb_repair(cx) != 0)
			return -1; /* unrepairable, so exit */
		/* Now that we've tried to repair it, re-read it. */
		ret = lgfs2_read_sb(sdp);
		if (ret < 0)
			return FSCK_ERROR;
	}
	if (sdp->sd_fs_format > FSCK_MAX_FORMAT) {
		log_crit(_("Unsupported gfs2 format found: %"PRIu32"\n"), sdp->sd_fs_format);
		log_crit(_("A newer fsck.gfs2 is required to check this file system.\n"));
		return FSCK_USAGE;
	}
	return 0;
}

/**
 * init_rindex - read in the rindex file
 */
static int init_rindex(struct fsck_cx *cx)
{
	struct lgfs2_sbd *sdp = cx->sdp;
	struct lgfs2_inode *ip;

	sdp->md.riinode = lgfs2_lookupi(sdp->master_dir, "rindex", 6);
	if (sdp->md.riinode)
		return 0;

	if (!query(cx, _("The gfs2 system rindex inode is missing. "
		      "Okay to rebuild it? (y/n) "))) {
		log_crit(_("Error: Cannot proceed without a valid rindex.\n"));
		return -1;
	}
	ip = lgfs2_build_rindex(sdp);
	if (ip == NULL) {
		log_crit(_("Error rebuilding rindex: %s\n"), strerror(errno));
		return -1;
	}
	lgfs2_inode_put(&ip);
	return 0;
}

/**
 * initialize - initialize superblock pointer
 *
 */
int initialize(struct fsck_cx *cx, int *all_clean)
{
	struct lgfs2_sbd *sdp = cx->sdp;
	int clean_journals = 0, open_flag;
	int err;

	*all_clean = 0;

	if (cx->opts->no)
		open_flag = O_RDONLY;
	else
		open_flag = O_RDWR | O_EXCL;

	sdp->device_fd = open(cx->opts->device, open_flag);
	if (sdp->device_fd < 0) {
		struct mntent *mnt;
		if (open_flag == O_RDONLY || errno != EBUSY) {
			log_crit( _("Unable to open device: %s\n"),
			         cx->opts->device);
			return FSCK_USAGE;
		}
		/* We can't open it EXCL.  It may be already open rw (in which
		   case we want to deny them access) or it may be mounted as
		   the root file system at boot time (in which case we need to
		   allow it.)
		   If the device is busy, but not because it's mounted, fail.
		   This protects against cases where the file system is LVM
		   and perhaps mounted on a different node.
		   Try opening without O_EXCL. */
		sdp->device_fd = lgfs2_open_mnt_dev(cx->opts->device, O_RDWR, &mnt);
		if (sdp->device_fd < 0)
			goto mount_fail;
		/* If the device is mounted, but not mounted RO, fail.  This
		   protects them against cases where the file system is
		   mounted RW, but still allows us to check our own root
		   file system. */
		if (!hasmntopt(mnt, MNTOPT_RO))
			goto close_fail;
		/* The device is mounted RO, so it's likely our own root
		   file system.  We can only do so much to protect the users
		   from themselves. */
		was_mounted_ro = 1;
	}

	if (lgfs2_get_dev_info(sdp->device_fd, &sdp->dinfo)) {
		perror(cx->opts->device);
		return FSCK_ERROR;
	}

	/* read in sb from disk */
	err = fill_super_block(cx);
	if (err != FSCK_OK)
		return err;

	/* Change lock protocol to be fsck_* instead of lock_* */
	if (!cx->opts->no && preen_is_safe(sdp, cx->opts)) {
		if (block_mounters(sdp, 1)) {
			log_err( _("Unable to block other mounters\n"));
			return FSCK_USAGE;
		}
	}

	sdp->master_dir = lgfs2_inode_read(sdp, sdp->sd_meta_dir.in_addr);
	if (sdp->master_dir->i_magic != GFS2_MAGIC ||
	    sdp->master_dir->i_mh_type != GFS2_METATYPE_DI ||
	    !sdp->master_dir->i_size) {
		lgfs2_inode_put(&sdp->master_dir);
		rebuild_sysdir(cx);
		sdp->master_dir = lgfs2_inode_read(sdp, sdp->sd_meta_dir.in_addr);
		if (sdp->master_dir == NULL) {
			log_crit(_("Error reading master directory: %s\n"), strerror(errno));
			return FSCK_ERROR;
		}
	}

	/* Look up the "per_node" inode.  If there are journals missing, we
	   need to figure out what's missing from per_node. And we need all
	   our journals to be there before we can replay them. */
	lookup_per_node(cx, 0);

	/* We need rindex first in case jindex is missing and needs to read
	   in the rgrps before rebuilding it. However, note that if the rindex
	   is damaged, we need the journals to repair it. That's because the
	   journals likely contain rgrps and bitmaps, which we need to ignore
	   when we're trying to find the rgrps. */
	if (init_rindex(cx))
		return FSCK_ERROR;

	if (fetch_rgrps(cx))
		return FSCK_ERROR;

	/* We need to read in jindex in order to replay the journals. If
	   there's an error, we may proceed and let init_system_inodes
	   try to rebuild it. */
	if (init_jindex(cx, 1) == 0) {
		if (replay_journals(cx, &clean_journals)) {
			if (!cx->opts->no && preen_is_safe(sdp, cx->opts))
				block_mounters(sdp, 0);
			stack;
			return FSCK_ERROR;
		}
		if (sdp->md.journals == clean_journals)
			*all_clean = 1;
		else if (cx->opts->force || !cx->opts->preen)
			log_notice( _("\nJournal recovery complete.\n"));

		if (!cx->opts->force && *all_clean && cx->opts->preen)
			return FSCK_OK;
	}

	if (init_system_inodes(cx))
		return FSCK_ERROR;

	return FSCK_OK;

close_fail:
	close(sdp->device_fd);
mount_fail:
	log_crit( _("Device %s is busy.\n"), cx->opts->device);
	return FSCK_USAGE;
}

void destroy(struct fsck_cx *cx)
{
	struct lgfs2_sbd *sdp = cx->sdp;

	if (!cx->opts->no) {
		if (block_mounters(sdp, 0)) {
			log_warn( _("Unable to unblock other mounters - manual intervention required\n"));
			log_warn( _("Use 'gfs2_tool sb <device> proto' to fix\n"));
		}
		log_info( _("Syncing the device.\n"));
		fsync(sdp->device_fd);
	}
	empty_super_block(cx);
	close(sdp->device_fd);
	if (was_mounted_ro && errors_corrected) {
		sdp->device_fd = open("/proc/sys/vm/drop_caches", O_WRONLY);
		if (sdp->device_fd >= 0) {
			if (write(sdp->device_fd, "2", 1) == 2) {
				close(sdp->device_fd);
				return;
			}
			close(sdp->device_fd);
		}
		log_warn(_("fsck.gfs2: Could not flush caches (non-fatal).\n"));
	}
}
