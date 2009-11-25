#include "clusterautoconfig.h"

#include <inttypes.h>
#include <unistd.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include "libgfs2.h"

/* Must be kept in sync with mark_block enum in libgfs2.h */
static int mark_to_gbmap[16] = {
	FREE, BLOCK_IN_USE, DIR_INDIR_BLK, DIR_INODE, FILE_INODE,
	LNK_INODE, BLK_INODE, CHR_INODE, FIFO_INODE, SOCK_INODE,
	DIR_LEAF_INODE, JOURNAL_BLK, OTHER_META, EATTR_META,
	BAD_BLOCK, INVALID_META
};

#define BITMAP_SIZE4(size) (size >> 1)
#define BITMAP_BYTE_OFFSET4(x) ((x & 0x0000000000000001) << 2)
#define BITMAP_MASK4 (0xf)

static int gfs2_bitmap_create(struct gfs2_bmap *bmap, uint64_t size)
{
	bmap->size = size;

	/* Have to add 1 to BITMAP_SIZE since it's 0-based and mallocs
	 * must be 1-based */
	bmap->mapsize = BITMAP_SIZE4(size);

	if(!(bmap->map = malloc(sizeof(char) * bmap->mapsize)))
		return -ENOMEM;
	if(!memset(bmap->map, 0, sizeof(char) * bmap->mapsize)) {
		free(bmap->map);
		bmap->map = NULL;
		return -ENOMEM;
	}
	return 0;
}

static int gfs2_bitmap_set(struct gfs2_bmap *bmap, uint64_t offset, uint8_t val)
{
	static char *byte;
	static uint64_t b;

	if(offset < bmap->size) {
		byte = bmap->map + BITMAP_SIZE4(offset);
		b = BITMAP_BYTE_OFFSET4(offset);
		*byte |= (val & BITMAP_MASK4) << b;
		return 0;
	}
	return -1;
}

static int gfs2_bitmap_clear(struct gfs2_bmap *bmap, uint64_t offset)
{
	static char *byte;
	static uint64_t b;

	if(offset < bmap->size) {
		byte = bmap->map + BITMAP_SIZE4(offset);
		b = BITMAP_BYTE_OFFSET4(offset);
		*byte &= ~(BITMAP_MASK4 << b);
		return 0;
	}
	return -1;

}

static void gfs2_bitmap_destroy(struct gfs2_bmap *bmap)
{
	if(bmap->map)
		free(bmap->map);
	bmap->size = 0;
	bmap->mapsize = 0;
}

struct gfs2_bmap *gfs2_bmap_create(struct gfs2_sbd *sdp, uint64_t size,
				   uint64_t *addl_mem_needed)
{
	struct gfs2_bmap *il;

	*addl_mem_needed = 0L;
	il = malloc(sizeof(*il));
	if (!il || !memset(il, 0, sizeof(*il)))
		return NULL;

	if(gfs2_bitmap_create(il, size)) {
		*addl_mem_needed = il->mapsize;
		free(il);
		il = NULL;
	}
	osi_list_init(&sdp->dup_blocks.list);
	osi_list_init(&sdp->eattr_blocks.list);
	return il;
}

void gfs2_special_free(struct special_blocks *blist)
{
	struct special_blocks *f;

	while(!osi_list_empty(&blist->list)) {
		f = osi_list_entry(blist->list.next, struct special_blocks,
				   list);
		osi_list_del(&f->list);
		free(f);
	}
}

static void gfs2_dup_free(struct dup_blocks *blist)
{
	struct dup_blocks *f;

	while(!osi_list_empty(&blist->list)) {
		f = osi_list_entry(blist->list.next, struct dup_blocks, list);
		while (!osi_list_empty(&f->ref_inode_list))
			osi_list_del(&f->ref_inode_list);
		osi_list_del(&f->list);
		free(f);
	}
}

struct special_blocks *blockfind(struct special_blocks *blist, uint64_t num)
{
	osi_list_t *head = &blist->list;
	osi_list_t *tmp;
	struct special_blocks *b;

	for (tmp = head->next; tmp != head; tmp = tmp->next) {
		b = osi_list_entry(tmp, struct special_blocks, list);
		if (b->block == num)
			return b;
	}
	return NULL;
}

static struct dup_blocks *dupfind(struct dup_blocks *blist, uint64_t num)
{
	osi_list_t *head = &blist->list;
	osi_list_t *tmp;
	struct dup_blocks *b;

	for (tmp = head->next; tmp != head; tmp = tmp->next) {
		b = osi_list_entry(tmp, struct dup_blocks, list);
		if (b->block_no == num)
			return b;
	}
	return NULL;
}

void gfs2_special_set(struct special_blocks *blocklist, uint64_t block)
{
	struct special_blocks *b;

	if (blockfind(blocklist, block))
		return;
	b = malloc(sizeof(struct special_blocks));
	if (b) {
		memset(b, 0, sizeof(*b));
		b->block = block;
		osi_list_add(&b->list, &blocklist->list);
	}
	return;
}

static void gfs2_dup_set(struct dup_blocks *blocklist, uint64_t block)
{
	struct dup_blocks *b;

	if (dupfind(blocklist, block))
		return;
	b = malloc(sizeof(struct dup_blocks));
	if (b) {
		memset(b, 0, sizeof(*b));
		b->block_no = block;
		osi_list_init(&b->ref_inode_list);
		osi_list_add(&b->list, &blocklist->list);
	}
	return;
}

static void gfs2_special_clear(struct special_blocks *blocklist, uint64_t block)
{
	struct special_blocks *b;

	b = blockfind(blocklist, block);
	if (b) {
		osi_list_del(&b->list);
		free(b);
	}
}

static void gfs2_dup_clear(struct dup_blocks *blocklist, uint64_t block)
{
	struct dup_blocks *b;

	b = dupfind(blocklist, block);
	if (b) {
		osi_list_del(&b->list);
		free(b);
	}
}

int gfs2_block_mark(struct gfs2_sbd *sdp, struct gfs2_bmap *il,
		    uint64_t block, enum gfs2_mark_block mark)
{
	int err = 0;

	if(mark == gfs2_dup_block)
		gfs2_dup_set(&sdp->dup_blocks, block);
	else if(mark == gfs2_eattr_block)
		gfs2_special_set(&sdp->eattr_blocks, block);
	else
		err = gfs2_bitmap_set(il, block, mark_to_gbmap[mark]);
	return err;
}

/* gfs2_block_unmark clears ONE mark for the given block */
int gfs2_block_unmark(struct gfs2_sbd *sdp, struct gfs2_bmap *il,
		      uint64_t block, enum gfs2_mark_block mark)
{
	int err = 0;

	switch (mark) {
	case gfs2_dup_block:
		gfs2_dup_clear(&sdp->dup_blocks, block);
		break;
	case gfs2_eattr_block:
		gfs2_special_clear(&sdp->eattr_blocks, block);
		break;
	default:
		/* FIXME: check types */
		err = gfs2_bitmap_clear(il, block);
		break;
	}
	return err;
}

/* gfs2_block_clear clears all the marks for the given block */
int gfs2_block_clear(struct gfs2_sbd *sdp, struct gfs2_bmap *il,
		     uint64_t block)
{
	int err;

	gfs2_dup_clear(&sdp->dup_blocks, block);
	gfs2_special_clear(&sdp->eattr_blocks, block);
	err = gfs2_bitmap_clear(il, block);
	return err;
}

int gfs2_block_set(struct gfs2_sbd *sdp, struct gfs2_bmap *il,
		   uint64_t block, enum gfs2_mark_block mark)
{
	int err;

	err = gfs2_block_clear(sdp, il, block); /* clear all block status */
	if(!err)
		err = gfs2_block_mark(sdp, il, block, mark);
	return err;
}

int gfs2_block_check(struct gfs2_sbd *sdp, struct gfs2_bmap *il,
		     uint64_t block, struct gfs2_block_query *val)
{
	static char *byte;
	static uint64_t b;

	if(block >= il->size)
		return -1;

	val->dup_block = (dupfind(&sdp->dup_blocks, block) ? 1 : 0);
	val->eattr_block = (blockfind(&sdp->eattr_blocks, block) ? 1 : 0);
	byte = il->map + BITMAP_SIZE4(block);
	b = BITMAP_BYTE_OFFSET4(block);
	val->block_type = (*byte & (BITMAP_MASK4 << b )) >> b;
	return 0;
}

void *gfs2_bmap_destroy(struct gfs2_sbd *sdp, struct gfs2_bmap *il)
{
	if(il) {
		gfs2_bitmap_destroy(il);
		free(il);
		il = NULL;
	}
	gfs2_dup_free(&sdp->dup_blocks);
	gfs2_special_free(&sdp->eattr_blocks);
	return il;
}
