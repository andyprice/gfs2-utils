#include "clusterautoconfig.h"

#include <errno.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <libintl.h>
#define _(String) gettext(String)

#include <logging.h>
#include "fsck.h"
#include "fs_recovery.h"
#include "libgfs2.h"
#include "metawalk.h"
#include "util.h"

#define JOURNAL_NAME_SIZE 18
#define JOURNAL_SEQ_TOLERANCE 10

static unsigned int sd_found_jblocks = 0;
static unsigned int sd_replayed_jblocks = 0;
static unsigned int sd_found_metablocks = 0;
static unsigned int sd_replayed_metablocks = 0;
static unsigned int sd_found_revokes = 0;
static osi_list_t sd_revoke_list;
static unsigned int sd_replay_tail;

struct revoke_replay {
	osi_list_t rr_list;
	uint64_t rr_blkno;
	unsigned int rr_where;
};

static int revoke_add(struct lgfs2_sbd *sdp, uint64_t blkno, unsigned int where)
{
	osi_list_t *tmp, *head = &sd_revoke_list;
	struct revoke_replay *rr;
	int found = 0;

	osi_list_foreach(tmp, head) {
		rr = osi_list_entry(tmp, struct revoke_replay, rr_list);
		if (rr->rr_blkno == blkno) {
			found = 1;
			break;
		}
	}

	if (found) {
		rr->rr_where = where;
		return 0;
	}

	rr = malloc(sizeof(struct revoke_replay));
	if (!rr)
		return -ENOMEM;

	rr->rr_blkno = blkno;
	rr->rr_where = where;
	osi_list_add(&rr->rr_list, head);
	return 1;
}

static int revoke_check(struct lgfs2_sbd *sdp, uint64_t blkno, unsigned int where)
{
	osi_list_t *tmp;
	struct revoke_replay *rr;
	int wrap, a, b;
	int found = 0;

	osi_list_foreach(tmp, &sd_revoke_list) {
		rr = osi_list_entry(tmp, struct revoke_replay, rr_list);
		if (rr->rr_blkno == blkno) {
			found = 1;
			break;
		}
	}

	if (!found)
		return 0;

	wrap = (rr->rr_where < sd_replay_tail);
	a = (sd_replay_tail < where);
	b = (where < rr->rr_where);
	return (wrap) ? (a || b) : (a && b);
}

static void revoke_clean(struct lgfs2_sbd *sdp)
{
	osi_list_t *head = &sd_revoke_list;
	struct revoke_replay *rr;

	while (!osi_list_empty(head)) {
		rr = osi_list_entry(head->next, struct revoke_replay, rr_list);
		osi_list_del(&rr->rr_list);
		free(rr);
	}
}

static void refresh_rgrp(struct lgfs2_sbd *sdp, struct lgfs2_rgrp_tree *rgd,
			 struct lgfs2_buffer_head *bh, uint64_t blkno)
{
	int i;

	log_debug(_("Block is part of rgrp 0x%"PRIx64"; refreshing the rgrp.\n"),
	          rgd->rt_addr);
	for (i = 0; i < rgd->rt_length; i++) {
		if (rgd->rt_addr + i != blkno)
			continue;

		memcpy(rgd->bits[i].bi_data, bh->b_data, sdp->sd_bsize);
		rgd->bits[i].bi_modified = 1;
		if (i == 0) { /* this is the rgrp itself */
			if (sdp->gfs1)
				lgfs2_gfs_rgrp_in(rgd, rgd->bits[0].bi_data);
			else
				lgfs2_rgrp_in(rgd, rgd->bits[0].bi_data);
		}
		break;
	}
}

static int buf_lo_scan_elements(struct lgfs2_inode *ip, unsigned int start,
				struct gfs2_log_descriptor *ld, __be64 *ptr,
				int pass)
{
	struct lgfs2_sbd *sdp = ip->i_sbd;
	unsigned int blks = be32_to_cpu(ld->ld_data1);
	struct lgfs2_buffer_head *bh_log, *bh_ip;
	uint64_t blkno;
	int error = 0;
	struct lgfs2_rgrp_tree *rgd;

	if (pass != 1 || be32_to_cpu(ld->ld_type) != GFS2_LOG_DESC_METADATA)
		return 0;

	lgfs2_replay_incr_blk(ip, &start);

	for (; blks; lgfs2_replay_incr_blk(ip, &start), blks--) {
		struct gfs2_meta_header *mhp;

		sd_found_metablocks++;

		blkno = be64_to_cpu(*ptr);
		ptr++;
		if (revoke_check(sdp, blkno, start))
			continue;

		error = lgfs2_replay_read_block(ip, start, &bh_log);
		if (error)
			return error;

		log_info(_("Journal replay writing metadata block #%"PRIu64" (0x%"PRIx64") for journal+0x%x\n"),
		         blkno, blkno, start);
		bh_ip = lgfs2_bget(sdp, blkno);
		if (!bh_ip) {
			log_err(_("Out of memory when replaying journals.\n"));
			return FSCK_ERROR;
		}
		memcpy(bh_ip->b_data, bh_log->b_data, sdp->sd_bsize);

		mhp = (struct gfs2_meta_header *)bh_ip->b_data;
		if (be32_to_cpu(mhp->mh_magic) != GFS2_MAGIC) {
			log_err(_("Journal corruption detected at block #%"PRIu64" (0x%"PRIx64") for journal+0x%x.\n"),
			        blkno, blkno, start);
			error = -EIO;
		} else {
			lgfs2_bmodified(bh_ip);
			rgd = lgfs2_blk2rgrpd(sdp, blkno);
			if (rgd && blkno < rgd->rt_data0)
				refresh_rgrp(sdp, rgd, bh_ip, blkno);
		}

		lgfs2_brelse(bh_log);
		lgfs2_brelse(bh_ip);
		if (error)
			break;

		sd_replayed_metablocks++;
	}
	return error;
}

static int revoke_lo_scan_elements(struct lgfs2_inode *ip, unsigned int start,
				   struct gfs2_log_descriptor *ld, __be64 *ptr,
				   int pass)
{
	struct lgfs2_sbd *sdp = ip->i_sbd;
	unsigned int blks = be32_to_cpu(ld->ld_length);
	unsigned int revokes = be32_to_cpu(ld->ld_data1);
	struct lgfs2_buffer_head *bh;
	unsigned int offset;
	uint64_t blkno;
	int first = 1;
	int error;

	if (pass != 0 || be32_to_cpu(ld->ld_type) != GFS2_LOG_DESC_REVOKE)
		return 0;

	offset = sizeof(struct gfs2_log_descriptor);

	for (; blks; lgfs2_replay_incr_blk(ip, &start), blks--) {
		error = lgfs2_replay_read_block(ip, start, &bh);
		if (error)
			return error;

		if (!first) {
			if (lgfs2_check_meta(bh->b_data, GFS2_METATYPE_LB))
				continue;
		}
		while (offset + sizeof(uint64_t) <= sdp->sd_bsize) {
			blkno = be64_to_cpu(*(__be64 *)(bh->b_data + offset));
			log_info(_("Journal replay processing revoke for block #%"PRIu64" (0x%"PRIx64") for journal+0x%x\n"),
			         blkno, blkno, start);
			error = revoke_add(sdp, blkno, start);
			if (error < 0)
				return error;
			else if (error)
				sd_found_revokes++;

			if (!--revokes)
				break;
			offset += sizeof(uint64_t);
		}

		lgfs2_bmodified(bh);
		lgfs2_brelse(bh);
		offset = sizeof(struct gfs2_meta_header);
		first = 0;
	}
	return 0;
}

static int databuf_lo_scan_elements(struct lgfs2_inode *ip, unsigned int start,
				    struct gfs2_log_descriptor *ld,
				    __be64 *ptr, int pass)
{
	struct lgfs2_sbd *sdp = ip->i_sbd;
	unsigned int blks = be32_to_cpu(ld->ld_data1);
	struct lgfs2_buffer_head *bh_log, *bh_ip;
	uint64_t blkno;
	uint64_t esc;
	int error = 0;

	if (pass != 1 || be32_to_cpu(ld->ld_type) != GFS2_LOG_DESC_JDATA)
		return 0;

	lgfs2_replay_incr_blk(ip, &start);
	for (; blks; lgfs2_replay_incr_blk(ip, &start), blks--) {
		blkno = be64_to_cpu(*ptr);
		ptr++;
		esc = be64_to_cpu(*ptr);
		ptr++;

		sd_found_jblocks++;

		if (revoke_check(sdp, blkno, start))
			continue;

		error = lgfs2_replay_read_block(ip, start, &bh_log);
		if (error)
			return error;

		log_info(_("Journal replay writing data block #%"PRIu64" (0x%"PRIx64") for journal+0x%x\n"),
		         blkno, blkno, start);
		bh_ip = lgfs2_bget(sdp, blkno);
		if (!bh_ip) {
			log_err(_("Out of memory when replaying journals.\n"));
			return FSCK_ERROR;
		}
		memcpy(bh_ip->b_data, bh_log->b_data, sdp->sd_bsize);

		/* Unescape */
		if (esc) {
			__be32 *eptr = (__be32 *)bh_ip->b_data;
			*eptr = cpu_to_be32(GFS2_MAGIC);
		}

		lgfs2_brelse(bh_log);
		lgfs2_bmodified(bh_ip);
		lgfs2_brelse(bh_ip);

		sd_replayed_jblocks++;
	}
	return error;
}

/**
 * foreach_descriptor - go through the active part of the log
 * @ip: the journal incore inode
 * @start: the first log header in the active region
 * @end: the last log header (don't process the contents of this entry))
 *
 * Call a given function once for every log descriptor in the active
 * portion of the log.
 *
 * Returns: errno
 */

static int foreach_descriptor(struct lgfs2_inode *ip, unsigned int start,
		       unsigned int end, int pass)
{
	struct lgfs2_buffer_head *bh;
	struct gfs2_log_descriptor *ld;
	int error = 0;
	uint32_t length;
	__be64 *ptr;
	unsigned int offset = sizeof(struct gfs2_log_descriptor);
	offset += sizeof(__be64) - 1;
	offset &= ~(sizeof(__be64) - 1);

	while (start != end) {
		struct gfs2_meta_header *mhp;

		error = lgfs2_replay_read_block(ip, start, &bh);
		if (error)
			return error;
		mhp = (struct gfs2_meta_header *)bh->b_data;
		if (be32_to_cpu(mhp->mh_magic) != GFS2_MAGIC) {
			lgfs2_bmodified(bh);
			lgfs2_brelse(bh);
			return -EIO;
		}
		ld = (struct gfs2_log_descriptor *)bh->b_data;
		length = be32_to_cpu(ld->ld_length);

		if (be32_to_cpu(ld->ld_header.mh_type) == GFS2_METATYPE_LH) {
			struct lgfs2_log_header lh;

			error = lgfs2_get_log_header(ip, start, &lh);
			if (!error) {
				lgfs2_replay_incr_blk(ip, &start);
				lgfs2_bmodified(bh);
				lgfs2_brelse(bh);
				continue;
			}
			if (error == 1) {
				log_err(_("Journal corruption detected at "
					  "journal+0x%x.\n"), start);
				error = -EIO;
			}
			lgfs2_bmodified(bh);
			lgfs2_brelse(bh);
			return error;
		} else if (lgfs2_check_meta(bh->b_data, GFS2_METATYPE_LD)) {
			lgfs2_bmodified(bh);
			lgfs2_brelse(bh);
			return -EIO;
		}
		ptr = (__be64 *)(bh->b_data + offset);
		error = databuf_lo_scan_elements(ip, start, ld, ptr, pass);
		if (error) {
			lgfs2_bmodified(bh);
			lgfs2_brelse(bh);
			return error;
		}
		error = buf_lo_scan_elements(ip, start, ld, ptr, pass);
		if (error) {
			lgfs2_bmodified(bh);
			lgfs2_brelse(bh);
			return error;
		}
		error = revoke_lo_scan_elements(ip, start, ld, ptr, pass);
		if (error) {
			lgfs2_bmodified(bh);
			lgfs2_brelse(bh);
			return error;
		}

		while (length--)
			lgfs2_replay_incr_blk(ip, &start);

		lgfs2_bmodified(bh);
		lgfs2_brelse(bh);
	}

	return 0;
}

/**
 * check_journal_seq_no - Check and Fix log header sequencing problems
 * @ip: the journal incore inode
 * @fix: if 1, fix the sequence numbers, otherwise just report the problem
 *
 * Returns: The number of sequencing errors (hopefully none).
 */
static int check_journal_seq_no(struct lgfs2_inode *ip, int fix)
{
	int error = 0, wrapped = 0;
	uint32_t jd_blocks = ip->i_size / ip->i_sbd->sd_bsize;
	uint32_t blk;
	struct lgfs2_log_header lh;
	uint64_t highest_seq = 0, lowest_seq = 0, prev_seq = 0;
	int new = 0;
	uint64_t dblock;
	struct lgfs2_buffer_head *bh;
	int seq_errors = 0;

	memset(&lh, 0, sizeof(lh));
	for (blk = 0; blk < jd_blocks; blk++) {
		error = lgfs2_get_log_header(ip, blk, &lh);
		if (error == 1) /* if not a log header */
			continue; /* just journal data--ignore it */
		if (!lowest_seq || lh.lh_sequence < lowest_seq)
			lowest_seq = lh.lh_sequence;
		if (!highest_seq || lh.lh_sequence > highest_seq)
			highest_seq = lh.lh_sequence;
		if (lh.lh_sequence > prev_seq) {
			prev_seq = lh.lh_sequence;
			continue;
		}
		/* The sequence number is not higher than the previous one,
		   so it's either wrap-around or a sequencing problem. */
		if (!wrapped && lh.lh_sequence == lowest_seq) {
			wrapped = 1;
			prev_seq = lh.lh_sequence;
			continue;
		}
		log_err(_("Journal block %"PRIu32" (0x%"PRIx32"): sequence no. 0x%"PRIx64" "
			   "out of order.\n"), blk, blk, lh.lh_sequence);
		log_info(_("Low: 0x%"PRIx64", High: 0x%"PRIx64", Prev: 0x%"PRIx64"\n"),
		         lowest_seq, highest_seq, prev_seq);
		seq_errors++;
		if (!fix)
			continue;
		highest_seq++;
		prev_seq = highest_seq;
		log_warn(_("Renumbering it as 0x%"PRIx64"\n"), highest_seq);
		lgfs2_block_map(ip, blk, &new, &dblock, NULL, 0);
		bh = lgfs2_bread(ip->i_sbd, dblock);
		((struct gfs2_log_header *)bh->b_data)->lh_sequence = cpu_to_be64(highest_seq);
		lgfs2_bmodified(bh);
		lgfs2_brelse(bh);
	}
	if (seq_errors && fix) {
		log_err(_("%d sequence errors fixed.\n"), seq_errors);
		seq_errors = 0;
	}
	return seq_errors;
}

/**
 * preen_is_safe - Can we safely preen the file system?
 *
 * If a preen option was specified (-a or -p) we're likely to have been
 * called from rc.sysinit.  We need to determine whether this is shared
 * storage or not.  If it's local storage (locking protocol==lock_nolock)
 * it's safe to preen the file system.  If it's lock_dlm, it's likely
 * mounted by other nodes in the cluster, which is dangerous and therefore,
 * we should warn the user to run fsck.gfs2 manually when it's safe.
 */
int preen_is_safe(struct lgfs2_sbd *sdp, int preen, int force_check)
{
	if (!preen)       /* If preen was not specified */
		return 1; /* not called by rc.sysinit--we're okay to preen */
	if (force_check)  /* If check was forced by the user? */
		return 1; /* user's responsibility--we're okay to preen */
	if (!memcmp(sdp->sd_lockproto + 5, "nolock", 6))
		return 1; /* local file system--preen is okay */
	return 0; /* might be mounted on another node--not guaranteed safe */
}

/**
 * gfs2_recover_journal - recovery a given journal
 * @ip: the journal incore inode
 * j: which journal to check
 * preen: Was preen (-a or -p) specified?
 * force_check: Was -f specified to force the check?
 * @was_clean: if the journal was originally clean, this is set to 1.
 *             if the journal was dirty from the start, this is set to 0.
 *
 * Acquire the journal's lock, check to see if the journal is clean, and
 * do recovery if necessary.
 *
 * Returns: errno
 */

static int recover_journal(struct lgfs2_inode *ip, int j, int preen,
				int force_check, int *was_clean)
{
	struct lgfs2_sbd *sdp = ip->i_sbd;
	struct lgfs2_log_header head;
	unsigned int pass;
	int error;

	*was_clean = 0;
	log_info( _("jid=%u: Looking at journal...\n"), j);

	osi_list_init(&sd_revoke_list);
	error = lgfs2_find_jhead(ip, &head);
	if (!error) {
		error = check_journal_seq_no(ip, 0);
		if (error > JOURNAL_SEQ_TOLERANCE) {
			log_err( _("Journal #%d (\"journal%d\") has %d "
				   "sequencing errors; tolerance is %d.\n"),
				 j+1, j, error, JOURNAL_SEQ_TOLERANCE);
			goto out;
		}
	}
	if (error) {
		if (opts.no) {
			log_err( _("Journal #%d (\"journal%d\") is corrupt\n"),j+1, j);
			log_err( _("Not fixing it due to the -n option.\n"));
			goto out;
		}
		if (!preen_is_safe(sdp, preen, force_check)) {
			log_err(_("Journal #%d (\"journal%d\") is corrupt.\n"),
				j+1, j);
			log_err(_("I'm not fixing it because it may be unsafe:\n"
				 "Locking protocol is not lock_nolock and "
				 "the -a or -p option was specified.\n"));
			log_err(_("Please make sure no node has the file system "
				 "mounted then rerun fsck.gfs2 manually "
				 "without -a or -p.\n"));
			goto out;
		}
		if (!query( _("\nJournal #%d (\"journal%d\") is "
			      "corrupt.  Okay to repair it? (y/n)"),
			    j+1, j)) {
			log_err( _("jid=%u: The journal was not repaired.\n"),
				 j);
			goto out;
		}
		log_info( _("jid=%u: Repairing journal...\n"), j);
		error = check_journal_seq_no(ip, 1);
		if (error) {
			log_err( _("jid=%u: Unable to fix the bad journal.\n"), 
				 j);
			goto out;
		}
		error = lgfs2_find_jhead(ip, &head);
		if (error) {
			log_err( _("jid=%u: Unable to fix the bad journal.\n"),
				 j);
			goto out;
		}
		log_err( _("jid=%u: The journal was successfully fixed.\n"),
			 j);
	}
	if (head.lh_flags & GFS2_LOG_HEAD_UNMOUNT) {
		log_info( _("jid=%u: Journal is clean.\n"), j);
		*was_clean = 1;
		return 0;
	}
	if (opts.no) {
		log_err(_("Journal #%d (\"journal%d\") is dirty\n"),j+1, j);
		log_err(_("not replaying due to the -n option.\n"));
		goto out;
	}
	if (!preen_is_safe(sdp, preen, force_check)) {
		log_err( _("Journal #%d (\"journal%d\") is dirty\n"), j+1, j);
		log_err( _("I'm not replaying it because it may be unsafe:\n"
			   "Locking protocol is not lock_nolock and "
			   "the -a or -p option was specified.\n"));
		log_err( _("Please make sure no node has the file system "
			   "mounted then rerun fsck.gfs2 manually "
			   "without -a or -p.\n"));
		error = FSCK_ERROR;
		goto out;
	}
	if (!query( _("\nJournal #%d (\"journal%d\") is dirty.  Okay to "
		      "replay it? (y/n)"), j+1, j))
		goto reinit;

	log_info( _("jid=%u: Replaying journal...\n"), j);

	sd_found_jblocks = sd_replayed_jblocks = 0;
	sd_found_metablocks = sd_replayed_metablocks = 0;
	sd_found_revokes = 0;
	sd_replay_tail = head.lh_tail;
	for (pass = 0; pass < 2; pass++) {
		error = foreach_descriptor(ip, head.lh_tail,
					   head.lh_blkno, pass);
		if (error) {
			log_err(_("Error found during journal replay.\n"));
			goto out;
		}
	}
	log_info( _("jid=%u: Found %u revoke tags\n"), j, sd_found_revokes);
	revoke_clean(sdp);
	error = lgfs2_clean_journal(ip, &head);
	if (error)
		goto out;
	log_err( _("jid=%u: Replayed %u of %u journaled data blocks\n"),
		 j, sd_replayed_jblocks, sd_found_jblocks);
	log_err( _("jid=%u: Replayed %u of %u metadata blocks\n"),
		 j, sd_replayed_metablocks, sd_found_metablocks);

	/* Check for errors and give them the option to reinitialize the
	   journal. */
out:
	if (!error) {
		log_info( _("jid=%u: Done\n"), j);
		return 0;
	}
	log_err( _("jid=%u: Failed\n"), j);
reinit:
	if (query( _("Do you want to clear the journal instead? (y/n)"))) {
		error = lgfs2_write_journal(sdp->md.journal[j], sdp->sd_bsize,
				      sdp->md.journal[j]->i_size /
				      sdp->sd_bsize);
		log_err(_("jid=%u: journal was cleared.\n"), j);
	} else {
		log_err( _("jid=%u: journal not cleared.\n"), j);
	}
	return error;
}

/* We can't use the rangecheck function from pass1 because we haven't gone
 * through initialization properly yet. */
static int rangecheck_jblock(struct lgfs2_inode *ip, uint64_t block)
{
	if((block > ip->i_sbd->fssize) || (block <= LGFS2_SB_ADDR(ip->i_sbd))) {
		log_info( _("Bad block pointer (out of range) found in "
			    "journal inode %"PRIu64" (0x%"PRIx64").\n"),
			  ip->i_num.in_addr, ip->i_num.in_addr);
		return META_ERROR; /* Exits check_metatree quicker */
	}
	return META_IS_GOOD;
}

static int rangecheck_jmeta(struct fsck_cx *cx, struct iptr iptr, struct lgfs2_buffer_head **bh, int h,
                            int *is_valid, int *was_duplicate, void *private)
{
	struct lgfs2_inode *ip = iptr.ipt_ip;
	uint64_t block = iptr_block(iptr);
	int rc;

	*bh = NULL;
	*was_duplicate = 0;
	*is_valid = 0;
	rc = rangecheck_jblock(ip, block);
	if (rc == META_IS_GOOD) {
		*bh = lgfs2_bread(ip->i_sbd, block);
		*is_valid = (lgfs2_check_meta((*bh)->b_data, GFS2_METATYPE_IN) == 0);
		if (!(*is_valid)) {
			log_err( _("Journal at block %"PRIu64" (0x%"PRIx64") has a bad "
				   "indirect block pointer %"PRIu64" (0x%"PRIx64") "
				   "(points to something that is not an "
				   "indirect block).\n"),
			        ip->i_num.in_addr, ip->i_num.in_addr, block, block);
			lgfs2_brelse(*bh);
			*bh = NULL;
			return META_SKIP_FURTHER;
		}
	}
	return rc;
}

static int rangecheck_jdata(struct fsck_cx *cx, struct lgfs2_inode *ip, uint64_t metablock,
			    uint64_t block, void *private,
			    struct lgfs2_buffer_head *bh, __be64 *ptr)
{
	return rangecheck_jblock(ip, block);
}

static struct metawalk_fxns rangecheck_journal = {
	.private = NULL,
	.invalid_meta_is_fatal = 1,
	.check_metalist = rangecheck_jmeta,
	.check_data = rangecheck_jdata,
};

/*
 * replay_journals - replay the journals
 * sdp: the super block
 * preen: Was preen (-a or -p) specified?
 * force_check: Was -f specified to force the check?
 * @clean_journals - set to the number of clean journals we find
 *
 * There should be a flag to the fsck to enable/disable this
 * feature.  The fsck falls back to clearing the journal if an 
 * inconsistency is found, but only for the bad journal.
 *
 * Returns: 0 on success, -1 on failure
 */
int replay_journals(struct fsck_cx *cx, int preen, int force_check,
		    int *clean_journals)
{
	struct lgfs2_sbd *sdp = cx->sdp;
	int dirty_journals = 0;
	int gave_msg = 0;
	int error = 0;
	int clean = 0;
	int i;

	*clean_journals = 0;

	sdp->jsize = LGFS2_DEFAULT_JSIZE;

	for(i = 0; i < sdp->md.journals; i++) {
		if (sdp->md.journal[i]) {
			error = check_metatree(cx, sdp->md.journal[i],
					       &rangecheck_journal);
			if (error)
				/* Don't use fsck_inode_put here because it's a
				   system file and we need to dismantle it. */
				lgfs2_inode_put(&sdp->md.journal[i]);
			error = 0; /* bad journal is non-fatal */
		}
		if (!sdp->md.journal[i]) {
			log_err(_("File system journal \"journal%d\" is "
				  "missing or corrupt: pass1 will try to "
				  "recreate it.\n"), i);
			continue;
		}
		if (!error) {
			uint64_t jsize = sdp->md.journal[i]->i_size / (1024 * 1024);

			if (sdp->jsize == LGFS2_DEFAULT_JSIZE && jsize &&
			    jsize != sdp->jsize)
				sdp->jsize = jsize;
			error = recover_journal(sdp->md.journal[i], i,
						     preen, force_check,
						     &clean);
			if (!clean)
				dirty_journals++;
			if (!gave_msg && dirty_journals == 1 && !opts.no &&
			    preen_is_safe(sdp, preen, force_check)) {
				gave_msg = 1;
				log_notice( _("Recovering journals (this may "
					      "take a while)\n"));
			}
			*clean_journals += clean;
		}
	}
	/* Sync the buffers to disk so we get a fresh start. */
	fsync(sdp->device_fd);
	return error;
}

/*
 * ji_update - fill in journal info
 * sdp: the incore superblock pointer
 *
 * Given the inode for the journal index, read in all
 * the journal inodes.
 *
 * Returns: 0 on success, -1 on failure
 */
int ji_update(struct lgfs2_sbd *sdp)
{
	struct lgfs2_inode *jip, *ip = sdp->md.jiinode;
	char journal_name[JOURNAL_NAME_SIZE];
	int i, error;
	char buf[sizeof(struct gfs_jindex)];

	if (!ip) {
		log_crit(_("Journal index inode not found.\n"));
		return -1;
	}

	/* The per_node directory will have 3 directory entries per node,
	   plus two for "." and "..".  So we subtract the 2 and divide by 3.
	   If per_node is missing or damaged, we have to trust jindex has
	   the correct number of entries. */
	if (sdp->gfs1)
		sdp->md.journals = ip->i_size / sizeof(struct gfs_jindex);
	else if (sdp->md.pinode) /* if per_node was read in properly */
		sdp->md.journals = (sdp->md.pinode->i_entries - 2) / 3;
	else
		sdp->md.journals = ip->i_entries - 2;

	if (!(sdp->md.journal = calloc(sdp->md.journals,
				       sizeof(struct lgfs2_inode *)))) {
		log_err(_("Unable to allocate journal index\n"));
		return -1;
	}
	memset(journal_name, 0, sizeof(*journal_name));
	for (i = 0; i < sdp->md.journals; i++) {
		if (sdp->gfs1) {
			struct gfs_jindex *ji;

			error = lgfs2_readi(ip,
					   buf, i * sizeof(struct gfs_jindex),
					   sizeof(struct gfs_jindex));
			if (!error)
				break;
			if (error != sizeof(struct gfs_jindex)){
				log_err(_("An error occurred while reading the"
					" journal index file.\n"));
				return -1;
			}
			ji = (struct gfs_jindex *)buf;
			sdp->md.journal[i] = lgfs2_inode_read(sdp, be64_to_cpu(ji->ji_addr));
			if (sdp->md.journal[i] == NULL)
				return -1;
		} else {
			/* FIXME check snprintf return code */
			snprintf(journal_name, JOURNAL_NAME_SIZE,
				 "journal%u", i);
			lgfs2_lookupi(sdp->md.jiinode, journal_name,
				     strlen(journal_name), &jip);
			sdp->md.journal[i] = jip;
		}
	}
	return 0;
}

static void bad_journalname(const char *filename, int len)
{
	if (len >= 64)
		len = 63;
	log_debug(_("Journal index entry '%.*s' has an invalid filename.\n"),
	          len, filename);
}

/**
 * check_jindex_dent - check the jindex directory entries
 *
 * This function makes sure the directory entries of the jindex are valid.
 * If they're not '.' or '..' they better have the form journalXXX.
 */
static int check_jindex_dent(struct fsck_cx *cx, struct lgfs2_inode *ip, struct gfs2_dirent *dent,
			     struct gfs2_dirent *prev_de,
			     struct lgfs2_buffer_head *bh, char *filename,
			     uint32_t *count, int *lindex, void *priv)
{
	struct lgfs2_dirent d;
	int i;

	lgfs2_dirent_in(&d, dent);

	if (d.dr_name_len == 1 && filename[0] == '.')
		goto dirent_good;
	if (d.dr_name_len == 2 && filename[0] == '.' && filename[1] == '.')
		goto dirent_good;

	if ((d.dr_name_len >= 11) || /* "journal9999" */
	    (d.dr_name_len <= 7) ||
	    (strncmp(filename, "journal", 7))) {
		bad_journalname(filename, d.dr_name_len);
		return -1;
	}
	for (i = 7; i < d.dr_name_len; i++) {
		if (filename[i] < '0' || filename[i] > '9') {
			bad_journalname(filename, d.dr_name_len);
			return -2;
		}
	}

dirent_good:
	/* Return the number of leaf entries so metawalk doesn't flag this
	   leaf as having none. */
	*count = be16_to_cpu(((struct gfs2_leaf *)bh->b_data)->lf_entries);
	return 0;
}

static struct metawalk_fxns jindex_check_fxns = {
	.private = NULL,
	.check_dentry = check_jindex_dent,
};

int build_jindex(struct lgfs2_sbd *sdp)
{
	struct lgfs2_inode *jindex;

	jindex = lgfs2_createi(sdp->master_dir, "jindex", S_IFDIR | 0700,
			 GFS2_DIF_SYSTEM);
	if (jindex == NULL) {
		return errno;
	}
	sdp->md.journal = malloc(sdp->md.journals * sizeof(struct lgfs2_inode *));
	for (unsigned j = 0; j < sdp->md.journals; j++) {
		int ret = lgfs2_build_journal(sdp, j, jindex);
		if (ret)
			return ret;
		lgfs2_inode_put(&sdp->md.journal[j]);
	}
	free(sdp->md.journal);
	lgfs2_inode_put(&jindex);
	return 0;
}

int init_jindex(struct fsck_cx *cx, int allow_ji_rebuild)
{
	struct lgfs2_sbd *sdp = cx->sdp;

	log_debug(_("Validating the journal index.\n"));
	/* rgrepair requires the journals be read in in order to distinguish
	   "real" rgrps from rgrps that are just copies left in journals. */
	if (sdp->gfs1)
		sdp->md.jiinode = lgfs2_inode_read(sdp, sdp->sd_jindex_di.in_addr);
	else
		lgfs2_lookupi(sdp->master_dir, "jindex", 6, &sdp->md.jiinode);

	if (!sdp->md.jiinode) {
		int err;

		if (!allow_ji_rebuild) {
			log_crit(_("Error: jindex and rindex files are both "
				   "corrupt.\n"));
			return -1;
		}
		if (!query( _("The gfs2 system jindex inode is missing. "
			      "Okay to rebuild it? (y/n) "))) {
			log_crit(_("Error: cannot proceed without a valid "
				   "jindex file.\n"));
			return -1;
		}

		err = build_jindex(sdp);
		if (err) {
			log_crit(_("Error %d rebuilding jindex\n"), err);
			return err;
		}
		lgfs2_lookupi(sdp->master_dir, "jindex", 6, &sdp->md.jiinode);
	}

	/* check for irrelevant entries in jindex. Can't use check_dir because
	   that creates and destroys the inode, which we don't want. */
	if (!sdp->gfs1) {
		int error;

		log_debug(_("Checking the integrity of the journal index.\n"));
		if (sdp->md.jiinode->i_flags & GFS2_DIF_EXHASH)
			error = check_leaf_blks(cx, sdp->md.jiinode,
						&jindex_check_fxns);
		else
			error = check_linear_dir(cx, sdp->md.jiinode,
						 sdp->md.jiinode->i_bh,
						 &jindex_check_fxns);
		if (error) {
			log_err(_("The system journal index is damaged.\n"));
			if (!query( _("Okay to rebuild it? (y/n) "))) {
				log_crit(_("Error: cannot proceed without a "
					   "valid jindex file.\n"));
				return -1;
			}
			lgfs2_inode_put(&sdp->md.jiinode);
			lgfs2_dirent_del(sdp->master_dir, "jindex", 6);
			log_err(_("Corrupt journal index was removed.\n"));
			error = build_jindex(sdp);
			if (error) {
				log_err(_("Error rebuilding journal "
					  "index: Cannot continue.\n"));
				return error;
			}
			lgfs2_lookupi(sdp->master_dir, "jindex", 6,
				     &sdp->md.jiinode);
		}
	}

	/* read in the ji data */
	if (ji_update(sdp)){
		log_err( _("Unable to read jindex inode.\n"));
		return -1;
	}
	return 0;
}
