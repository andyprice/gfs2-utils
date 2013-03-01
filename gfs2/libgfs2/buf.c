#include "clusterautoconfig.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <linux/types.h>

#include "libgfs2.h"

struct gfs2_buffer_head *bget(struct gfs2_sbd *sdp, uint64_t num)
{
	struct gfs2_buffer_head *bh;

	bh = calloc(1, sizeof(struct gfs2_buffer_head) + sdp->bsize);
	if (bh == NULL)
		return NULL;

	bh->b_blocknr = num;
	bh->sdp = sdp;
	bh->iov.iov_base = (char *)bh + sizeof(struct gfs2_buffer_head);
	bh->iov.iov_len = sdp->bsize;

	return bh;
}

int __breadm(struct gfs2_sbd *sdp, struct gfs2_buffer_head **bhs, size_t n,
	     uint64_t block, int line, const char *caller)
{
	struct iovec *iov = alloca(n * sizeof(struct iovec));
	struct iovec *iovbase = iov;
	uint64_t b = block;
	size_t size = 0;
	size_t i;
	int ret;

	for (i = 0; i < n; i++) {
		bhs[i] = bget(sdp, b++);
		if (bhs[i] == NULL)
			return -1;
		*iov++ = bhs[i]->iov;
		size += bhs[i]->iov.iov_len;
	}

	ret = preadv(sdp->device_fd, iovbase, n, block * sdp->bsize);

	if (ret != size) {
		fprintf(stderr, "bad read: %s from %s:%d: block "
				"%llu (0x%llx)\n", strerror(errno),
				caller, line, (unsigned long long)block,
				(unsigned long long)block);
		exit(-1);
	}

	return 0;
}

struct gfs2_buffer_head *__bread(struct gfs2_sbd *sdp, uint64_t num, int line,
				 const char *caller)
{
	struct gfs2_buffer_head *bh;
	int ret;

	ret = __breadm(sdp, &bh, 1, num, line, caller);
	if (ret >= 0)
		return bh;
	return NULL;
}

int bwrite(struct gfs2_buffer_head *bh)
{
	struct gfs2_sbd *sdp = bh->sdp;

	if (pwritev(sdp->device_fd, &bh->iov, 1, bh->b_blocknr * sdp->bsize) != bh->iov.iov_len)
		return -1;
	bh->b_modified = 0;
	return 0;
}

int brelse(struct gfs2_buffer_head *bh)
{
	int error = 0;

	if (bh->b_blocknr == -1)
		printf("Double free!\n");
	if (bh->b_modified)
		error = bwrite(bh);
	bh->b_blocknr = -1;
	if (bh->b_altlist.next && !osi_list_empty(&bh->b_altlist))
		osi_list_del(&bh->b_altlist);
	free(bh);
	return error;
}

uint32_t lgfs2_get_block_type(const struct gfs2_buffer_head *lbh)
{
	const struct gfs2_meta_header *mh = lbh->iov.iov_base;

	if (be32_to_cpu(mh->mh_magic) == GFS2_MAGIC)
		return be32_to_cpu(mh->mh_type);

	return 0;
}
