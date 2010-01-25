#include "clusterautoconfig.h"

#include <inttypes.h>
#include <unistd.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include "libgfs2.h"

static int gfs2_blockmap_create(struct gfs2_bmap *bmap, uint64_t size)
{
	bmap->size = size;

	/* Have to add 1 to BLOCKMAP_SIZE since it's 0-based and mallocs
	 * must be 1-based */
	bmap->mapsize = BLOCKMAP_SIZE4(size);

	if(!(bmap->map = malloc(sizeof(char) * bmap->mapsize)))
		return -ENOMEM;
	if(!memset(bmap->map, 0, sizeof(char) * bmap->mapsize)) {
		free(bmap->map);
		bmap->map = NULL;
		return -ENOMEM;
	}
	return 0;
}

static void gfs2_blockmap_destroy(struct gfs2_bmap *bmap)
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

	if(gfs2_blockmap_create(il, size)) {
		*addl_mem_needed = il->mapsize;
		free(il);
		il = NULL;
	}
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

void gfs2_special_add(struct special_blocks *blocklist, uint64_t block)
{
	struct special_blocks *b;

	b = malloc(sizeof(struct special_blocks));
	if (b) {
		memset(b, 0, sizeof(*b));
		b->block = block;
		osi_list_add(&b->list, &blocklist->list);
	}
}

void gfs2_special_set(struct special_blocks *blocklist, uint64_t block)
{
	if (blockfind(blocklist, block))
		return;
	gfs2_special_add(blocklist, block);
}

void gfs2_special_clear(struct special_blocks *blocklist, uint64_t block)
{
	struct special_blocks *b;

	b = blockfind(blocklist, block);
	if (b) {
		osi_list_del(&b->list);
		free(b);
	}
}

int gfs2_blockmap_set(struct gfs2_bmap *bmap, uint64_t bblock,
		      enum gfs2_mark_block mark)
{
	static unsigned char *byte;
	static uint64_t b;

	if(bblock > bmap->size)
		return -1;

	byte = bmap->map + BLOCKMAP_SIZE4(bblock);
	b = BLOCKMAP_BYTE_OFFSET4(bblock);
	*byte &= ~(BLOCKMAP_MASK4 << b);
	*byte |= (mark & BLOCKMAP_MASK4) << b;
	return 0;
}

void *gfs2_bmap_destroy(struct gfs2_sbd *sdp, struct gfs2_bmap *il)
{
	if(il) {
		gfs2_blockmap_destroy(il);
		free(il);
		il = NULL;
	}
	gfs2_special_free(&sdp->eattr_blocks);
	return il;
}
