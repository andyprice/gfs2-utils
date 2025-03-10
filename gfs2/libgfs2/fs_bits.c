#include "clusterautoconfig.h"

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>

#include "libgfs2.h"

#if BITS_PER_LONG == 32
#define LBITMASK   (0x55555555UL)
#define LBITSKIP55 (0x55555555UL)
#define LBITSKIP00 (0x00000000UL)
#else
#define LBITMASK   (0x5555555555555555UL)
#define LBITSKIP55 (0x5555555555555555UL)
#define LBITSKIP00 (0x0000000000000000UL)
#endif

#define ALIGN(x,a) (((x)+(a)-1)&~((a)-1))

const char * const lgfs2_blkst_str(int blkst)
{
	switch (blkst) {
	case GFS2_BLKST_FREE:
		return "FREE";
	case GFS2_BLKST_USED:
		return "USED";
	case GFS2_BLKST_UNLINKED:
		return "UNLINKED";
	case GFS2_BLKST_DINODE:
		return "DINODE";
	default:
		return "UNKNOWN";
	}
}

/**
 * bit_search
 * @ptr: Pointer to bitmap data
 * @mask: Mask to use (normally 0x55555.... but adjusted for search start)
 * @state: The state we are searching for
 *
 * We xor the bitmap data with a patter which is the bitwise opposite
 * of what we are looking for, this gives rise to a pattern of ones
 * wherever there is a match. Since we have two bits per entry, we
 * take this pattern, shift it down by one place and then and it with
 * the original. All the even bit positions (0,2,4, etc) then represent
 * successful matches, so we mask with 0x55555..... to remove the unwanted
 * odd bit positions.
 *
 * This allows searching of a whole u64 at once (32 blocks) with a
 * single test (on 64 bit arches).
 */

static inline uint64_t bit_search(const __le64 *ptr,
				       unsigned long long mask,
				       uint8_t state)
{
	unsigned long long tmp;
	static const unsigned long long search[] = {
		[0] = 0xffffffffffffffffULL,
		[1] = 0xaaaaaaaaaaaaaaaaULL,
		[2] = 0x5555555555555555ULL,
		[3] = 0x0000000000000000ULL,
	};
	tmp = le64_to_cpu(*ptr) ^ search[state];
	tmp &= (tmp >> 1);
	tmp &= mask;
	return tmp;
}

/**
 * lgfs2_bitfit - Find a free block in the bitmaps
 * @buffer: the buffer that holds the bitmaps
 * @buflen: the length (in bytes) of the buffer
 * @goal: the block to try to allocate
 * @old_state: the state of the block we're looking for
 *
 * Return: the block number that was allocated
 */
unsigned long lgfs2_bitfit(const unsigned char *buf, const unsigned int len,
			  unsigned long goal, unsigned char state)
{
	uint32_t spoint = (goal << 1) & ((8 * sizeof(uint64_t)) - 1);
	const __le64 *ptr = ((__le64 *)buf) + (goal >> 5);
	const __le64 *end = (__le64 *) (buf + ALIGN(len, sizeof(uint64_t)));
	uint64_t tmp;
	uint64_t mask = 0x5555555555555555ULL;
	uint32_t bit;

	if (state > 3)
		return 0;

	/* Mask off bits we don't care about at the start of the search */
	mask <<= spoint;
	tmp = bit_search(ptr, mask, state);
	ptr++;
	while(tmp == 0 && ptr < end) {
		tmp = bit_search(ptr, 0x5555555555555555ULL, state);
		ptr++;
	}
	/* Mask off any bits which are more than len bytes from the start */
	if (ptr == end && (len & (sizeof(uint64_t) - 1)))
		tmp &= (((uint64_t)~0) >>
			(64 - 8 * (len & (sizeof(uint64_t) - 1))));
	/* Didn't find anything, so return */
	if (tmp == 0)
		return LGFS2_BFITNOENT;
	ptr--;
	bit = ffsll(tmp);
	bit /= 2;	/* two bits per entry in the bitmap */
	return (((const unsigned char *)ptr - buf) * GFS2_NBBY) + bit;
}

/*
 * lgfs2_check_range - check if blkno is within FS limits
 * @sdp: super block
 * @blkno: block number
 *
 * Returns: 0 if ok, -1 if out of bounds
 */
int lgfs2_check_range(struct lgfs2_sbd *sdp, uint64_t blkno)
{
	if((blkno > sdp->fssize) || (blkno <= LGFS2_SB_ADDR(sdp)))
		return -1;
	return 0;
}

/*
 * lgfs2_set_bitmap
 * @sdp: super block
 * @blkno: block number relative to file system
 * @state: one of three possible states
 *
 * This function sets the value of a bit of the
 * file system bitmap.
 *
 * Returns: 0 on success, -1 on error
 */
int lgfs2_set_bitmap(lgfs2_rgrp_t rgd, uint64_t blkno, int state)
{
	int           buf;
	uint32_t        rgrp_block;
	struct lgfs2_bitmap *bits = NULL;
	unsigned char *byte, cur_state;
	unsigned int bit;

	/* FIXME: should GFS2_BLKST_INVALID be allowed */
	if ((state < GFS2_BLKST_FREE) || (state > GFS2_BLKST_DINODE))
		return -1;

	if(!rgd || blkno < rgd->rt_data0)
		return -1;

	rgrp_block = (uint32_t)(blkno - rgd->rt_data0);
	for(buf= 0; buf < rgd->rt_length; buf++){
		bits = &(rgd->rt_bits[buf]);
		if(rgrp_block < ((bits->bi_start + bits->bi_len)*GFS2_NBBY))
			break;
	}

	if (bits == NULL)
		return -1;
	byte = (unsigned char *)(bits->bi_data + bits->bi_offset) +
		(rgrp_block/GFS2_NBBY - bits->bi_start);
	bit = (rgrp_block % GFS2_NBBY) * GFS2_BIT_SIZE;

	cur_state = (*byte >> bit) & GFS2_BIT_MASK;
	*byte ^= cur_state << bit;
	*byte |= state << bit;

	bits->bi_modified = 1;
	return 0;
}

/*
 * lgfs2_get_bitmap - get value of FS bitmap
 * @sdp: super block
 * @blkno: block number relative to file system
 *
 * This function gets the value of a bit of the
 * file system bitmap.
 * Possible state values for a block in the bitmap are:
 *  GFS_BLKST_FREE     (0)
 *  GFS_BLKST_USED     (1)
 *  GFS_BLKST_UNLINKED (2)
 *  GFS_BLKST_DINODE   (3)
 *
 * Returns: state on success, -1 on error
 */
int lgfs2_get_bitmap(struct lgfs2_sbd *sdp, uint64_t blkno, struct lgfs2_rgrp_tree *rgd)
{
	uint64_t offset;
	uint32_t i = 0;
	char *byte;
	unsigned int bit;
	struct lgfs2_bitmap *bi;

	if (rgd == NULL) {
		rgd = lgfs2_blk2rgrpd(sdp, blkno);
		if(rgd == NULL)
			return -1;
	}

	offset = blkno - rgd->rt_data0;
	if (offset > UINT_MAX) {
		errno = EINVAL;
		return -1;
	}
	if (offset >= rgd->rt_data0 + rgd->rt_data) {
		errno = E2BIG;
		return -1;
	}

	if (offset >= ((uint64_t)rgd->rt_bits->bi_start + rgd->rt_bits->bi_len) * GFS2_NBBY) {
		offset += (sizeof(struct gfs2_rgrp) - sizeof(struct gfs2_meta_header))
		          * GFS2_NBBY;
		i = offset / sdp->sd_blocks_per_bitmap;
		offset -= (uint64_t)i * sdp->sd_blocks_per_bitmap;
	}

	bi = &rgd->rt_bits[i];
	if (bi->bi_data == NULL)
		return GFS2_BLKST_FREE;

	byte = (bi->bi_data + bi->bi_offset) + (offset/GFS2_NBBY);
	bit = (offset % GFS2_NBBY) * GFS2_BIT_SIZE;

	return (*byte >> bit) & GFS2_BIT_MASK;
}
