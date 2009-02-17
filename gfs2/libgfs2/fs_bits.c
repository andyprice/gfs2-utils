#include <inttypes.h>
#include <stdlib.h>
#include <unistd.h>

#include "libgfs2.h"

/**
 * gfs2_bitfit - Find a free block in the bitmaps
 * @buffer: the buffer that holds the bitmaps
 * @buflen: the length (in bytes) of the buffer
 * @goal: the block to try to allocate
 * @old_state: the state of the block we're looking for
 *
 * Return: the block number that was allocated
 */

uint32_t gfs2_bitfit(unsigned char *buffer, unsigned int buflen,
		     uint32_t goal, unsigned char old_state)
{
	unsigned char *byte, *end, alloc;
	uint32_t blk = goal;
	unsigned int bit;

	byte = buffer + (goal / GFS2_NBBY);
	bit = (goal % GFS2_NBBY) * GFS2_BIT_SIZE;
	end = buffer + buflen;
	alloc = (old_state & 1) ? 0 : 0x55;

	while (byte < end){
		if ((*byte & 0x55) == alloc){
			blk += (8 - bit) >> 1;
			bit = 0;
			byte++;
			continue;
		}

		if (((*byte >> bit) & GFS2_BIT_MASK) == old_state)
			return blk;

		bit += GFS2_BIT_SIZE;
		if (bit >= 8){
			bit = 0;
			byte++;
		}
		blk++;
	}
	return BFITNOENT;
}

/**
 * fs_bitcount - count the number of bits in a certain state
 * @buffer: the buffer that holds the bitmaps
 * @buflen: the length (in bytes) of the buffer
 * @state: the state of the block we're looking for
 *
 * Returns: The number of bits
 */
uint32_t gfs2_bitcount(unsigned char *buffer, unsigned int buflen,
		       unsigned char state)
{
	unsigned char *byte, *end;
	unsigned int bit;
	uint32_t count = 0;

	byte = buffer;
	bit = 0;
	end = buffer + buflen;

	while (byte < end){
		if (((*byte >> bit) & GFS2_BIT_MASK) == state)
			count++;

		bit += GFS2_BIT_SIZE;
		if (bit >= 8){
			bit = 0;
			byte++;
		}
	}
	return count;
}

/*
 * check_range - check if blkno is within FS limits
 * @sdp: super block
 * @blkno: block number
 *
 * Returns: 0 if ok, -1 if out of bounds
 */
int gfs2_check_range(struct gfs2_sbd *sdp, uint64_t blkno)
{
	if((blkno > sdp->fssize) || (blkno <= sdp->sb_addr))
		return -1;
	return 0;
}

/*
 * fs_set_bitmap
 * @sdp: super block
 * @blkno: block number relative to file system
 * @state: one of three possible states
 *
 * This function sets the value of a bit of the
 * file system bitmap.
 *
 * Returns: 0 on success, -1 on error
 */
int gfs2_set_bitmap(struct gfs2_sbd *sdp, uint64_t blkno, int state)
{
	int           buf;
	uint32_t        rgrp_block;
	struct gfs2_bitmap *bits = NULL;
	struct rgrp_list *rgd;
	unsigned char *byte, cur_state;
	unsigned int bit;

	/* FIXME: should GFS2_BLKST_INVALID be allowed */
	if ((state < GFS2_BLKST_FREE) || (state > GFS2_BLKST_DINODE))
		return -1;

	rgd = gfs2_blk2rgrpd(sdp, blkno);

	if(!rgd)
		return -1;

	if(gfs2_rgrp_read(sdp, rgd))
		return -1;
	rgrp_block = (uint32_t)(blkno - rgd->ri.ri_data0);
	for(buf= 0; buf < rgd->ri.ri_length; buf++){
		bits = &(rgd->bits[buf]);
		if(rgrp_block < ((bits->bi_start + bits->bi_len)*GFS2_NBBY))
			break;
	}

	byte = (unsigned char *)(rgd->bh[buf]->b_data + bits->bi_offset) +
		(rgrp_block/GFS2_NBBY - bits->bi_start);
	bit = (rgrp_block % GFS2_NBBY) * GFS2_BIT_SIZE;

	cur_state = (*byte >> bit) & GFS2_BIT_MASK;
	*byte ^= cur_state << bit;
	*byte |= state << bit;

	gfs2_rgrp_relse(rgd, updated);
	return 0;
}
