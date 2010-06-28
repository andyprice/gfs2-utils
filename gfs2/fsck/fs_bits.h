#ifndef __FS_BITS_H__
#define __FS_BITS_H__

#include "libgfs2.h"
#include "fsck.h"

#define BFITNOENT (0xFFFFFFFF)

struct fs_bitmap
{
	uint32_t   bi_offset;	/* The offset in the buffer of the first byte */
	uint32_t   bi_start;    /* The position of the first byte in this block */
	uint32_t   bi_len;      /* The number of bytes in this block */
};
typedef struct fs_bitmap fs_bitmap_t;

#endif /* __FS_BITS_H__ */
