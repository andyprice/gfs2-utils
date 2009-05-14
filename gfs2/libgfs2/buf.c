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

#include "libgfs2.h"

static __inline__ osi_list_t *
blkno2head(struct buf_list *bl, uint64_t blkno)
{
	return bl->buf_hash +
		(gfs2_disk_hash((char *)&blkno, sizeof(uint64_t)) & BUF_HASH_MASK);
}

static int write_buffer(struct buf_list *bl, struct gfs2_buffer_head *bh)
{
	struct gfs2_sbd *sdp = bl->sbp;

	osi_list_del(&bh->b_list);
	osi_list_del(&bh->b_hash);
	bl->num_bufs--;
	if (bh->b_changed) {
		if (lseek(sdp->device_fd, bh->b_blocknr * sdp->bsize,
			  SEEK_SET) != bh->b_blocknr * sdp->bsize) {
			return -1;
		}
		if (write(sdp->device_fd, bh->b_data, sdp->bsize) !=
		    sdp->bsize) {
			return -1;
		}
		sdp->writes++;
	}
	free(bh);
	return 0;
}

void init_buf_list(struct gfs2_sbd *sdp, struct buf_list *bl, uint32_t limit)
{
	int i;

	bl->num_bufs = 0;
	bl->spills = 0;
	bl->limit = limit;
	bl->sbp = sdp;
	osi_list_init(&bl->list);
	for(i = 0; i < BUF_HASH_SIZE; i++)
		osi_list_init(&bl->buf_hash[i]);
}

static int add_buffer(struct buf_list *bl, struct gfs2_buffer_head *bh)
{
	osi_list_t *head = blkno2head(bl, bh->b_blocknr);

	osi_list_add(&bh->b_list, &bl->list);
	osi_list_add(&bh->b_hash, head);
	bl->num_bufs++;

	if (bl->num_bufs * bl->sbp->bsize > bl->limit) {
		int found = 0;
		osi_list_t *tmp, *x;

		for (tmp = bl->list.prev, x = tmp->prev; tmp != &bl->list;
		     tmp = x, x = x->prev) {
			bh = osi_list_entry(tmp, struct gfs2_buffer_head,
					    b_list);
			if (!bh->b_count) {
				if (write_buffer(bl, bh))
					return -1;
				found++;
				if (found >= 10)
					break;
			}
		}
		bl->spills++;
	}
	return 0;
}

struct gfs2_buffer_head *bfind(struct buf_list *bl, uint64_t num)
{
	osi_list_t *head = blkno2head(bl, num);
	osi_list_t *tmp;
	struct gfs2_buffer_head *bh;

	for (tmp = head->next; tmp != head; tmp = tmp->next) {
		bh = osi_list_entry(tmp, struct gfs2_buffer_head, b_hash);
		if (bh->b_blocknr == num) {
			osi_list_del(&bh->b_list);
			osi_list_add(&bh->b_list, &bl->list);
			osi_list_del(&bh->b_hash);
			osi_list_add(&bh->b_hash, head);
			bh->b_count++;
			return bh;
		}
	}

	return NULL;
}

struct gfs2_buffer_head *__bget_generic(struct buf_list *bl, uint64_t num,
					int find_existing, int read_disk,
					int line, const char *caller)
{
	struct gfs2_buffer_head *bh;
	struct gfs2_sbd *sdp = bl->sbp;

	if (find_existing) {
		bh = bfind(bl, num);
		if (bh)
			return bh;
	}
	bh = calloc(1, sizeof(struct gfs2_buffer_head) + sdp->bsize);
	if (bh == NULL)
		return NULL;

	bh->b_count = 1;
	bh->b_blocknr = num;
	bh->b_data = (char *)bh + sizeof(struct gfs2_buffer_head);
	if (read_disk) {
		if (lseek(sdp->device_fd, num * sdp->bsize, SEEK_SET) !=
		    num * sdp->bsize) {
			fprintf(stderr, "bad seek: %s from %s:%d: block "
				"%llu (0x%llx)\n", strerror(errno),
				caller, line, (unsigned long long)num,
				(unsigned long long)num);
			exit(-1);
		}
		if (read(sdp->device_fd, bh->b_data, sdp->bsize) < 0) {
			fprintf(stderr, "bad read: %s from %s:%d: block "
				"%llu (0x%llx)\n", strerror(errno),
				caller, line, (unsigned long long)num,
				(unsigned long long)num);
			exit(-1);
		}
	}
	if (add_buffer(bl, bh)) {
		fprintf(stderr, "bad write: %s from %s:%d: block "
			"%llu (0x%llx)\n", strerror(errno),
			caller, line, (unsigned long long)num,
			(unsigned long long)num);
		exit(-1);
	}
	bh->b_changed = FALSE;

	return bh;
}

struct gfs2_buffer_head *__bget(struct buf_list *bl, uint64_t num, int line,
				const char *caller)
{
	return __bget_generic(bl, num, TRUE, FALSE, line, caller);
}

struct gfs2_buffer_head *__bread(struct buf_list *bl, uint64_t num, int line,
				 const char *caller)
{
	return __bget_generic(bl, num, TRUE, TRUE, line, caller);
}

struct gfs2_buffer_head *bhold(struct gfs2_buffer_head *bh)
{
	if (!bh->b_count)
		return NULL;
	bh->b_count++;
	return bh;
}

void brelse(struct gfs2_buffer_head *bh, enum update_flags is_updated)
{
    /* We can't just say b_changed = updated because we don't want to     */
	/* set it FALSE if it's TRUE until we write the changed data to disk. */
	if (updated)
		bh->b_changed = TRUE;
	if (!bh->b_count) {
		fprintf(stderr, "buffer count underflow for block %" PRIu64
			" (0x%" PRIx64")\n", bh->b_blocknr, bh->b_blocknr);
		exit(-1);
	}
	bh->b_count--;
}

void __bsync(struct buf_list *bl, int line, const char *caller)
{
	struct gfs2_buffer_head *bh;

	while (!osi_list_empty(&bl->list)) {
		bh = osi_list_entry(bl->list.prev, struct gfs2_buffer_head,
							b_list);
		if (bh->b_count) {
			fprintf(stderr, "buffer still held for block: %" PRIu64
				" (0x%" PRIx64")\n", bh->b_blocknr, bh->b_blocknr);
			exit(-1);
		}
		if (write_buffer(bl, bh)) {
			fprintf(stderr, "bad write: %s from %s:%d: block "
				"%lld (0x%llx)\n", strerror(errno),
				caller, line,
				(unsigned long long)bh->b_blocknr,
				(unsigned long long)bh->b_blocknr);
			exit(-1);
		}
	}
}

/* commit buffers to disk but do not discard */
void __bcommit(struct buf_list *bl, int line, const char *caller)
{
	osi_list_t *tmp, *x;
	struct gfs2_buffer_head *bh;
	struct gfs2_sbd *sdp = bl->sbp;

	osi_list_foreach_safe(tmp, &bl->list, x) {
		bh = osi_list_entry(tmp, struct gfs2_buffer_head, b_list);
		if (!bh->b_count) {            /* if not reserved for later */
			if (write_buffer(bl, bh)) { /* write & free */
				fprintf(stderr, "bad write: %s from %s:%d: "
					"block %lld (0x%llx)\n",
					strerror(errno), caller, line,
					(unsigned long long)bh->b_blocknr,
					(unsigned long long)bh->b_blocknr);
				exit(-1);
			}
		} else if (bh->b_changed) {     /* if buffer has changed */
			if (lseek(sdp->device_fd,
				  bh->b_blocknr * sdp->bsize, SEEK_SET) !=
			    bh->b_blocknr * sdp->bsize) {
				fprintf(stderr, "bad seek: %s from %s:%d: "
					"block %lld (0x%llx)\n",
					strerror(errno), caller, line,
					(unsigned long long)bh->b_blocknr,
					(unsigned long long)bh->b_blocknr);
				exit(-1);
			}
			if (write(sdp->device_fd, bh->b_data, sdp->bsize) !=
			    sdp->bsize) {
				fprintf(stderr, "bad write: %s from %s:%d: "
					"block %lld (0x%llx)\n",
					strerror(errno), caller, line,
					(unsigned long long)bh->b_blocknr,
					(unsigned long long)bh->b_blocknr);
				exit(-1);
			}
			bh->b_changed = FALSE;    /* no longer changed */
		}
	}
	fsync(sdp->device_fd);
}

