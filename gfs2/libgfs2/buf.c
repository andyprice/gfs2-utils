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

#include "libgfs2.h"

#ifndef IOV_MAX
  #ifdef UIO_MAXIOV
    #define IOV_MAX UIO_MAXIOV
  #else
    #define IOV_MAX (1024)
  #endif
#endif

struct lgfs2_buffer_head *lgfs2_bget(struct lgfs2_sbd *sdp, uint64_t num)
{
	struct lgfs2_buffer_head *bh;

	bh = calloc(1, sizeof(struct lgfs2_buffer_head) + sdp->sd_bsize);
	if (bh == NULL)
		return NULL;

	bh->b_blocknr = num;
	bh->sdp = sdp;
	bh->iov.iov_base = (char *)bh + sizeof(struct lgfs2_buffer_head);
	bh->iov.iov_len = sdp->sd_bsize;

	return bh;
}

struct lgfs2_buffer_head *__lgfs2_bread(struct lgfs2_sbd *sdp, uint64_t num, int line,
				 const char *caller)
{
	struct lgfs2_buffer_head *bh;
	ssize_t ret;

	bh = lgfs2_bget(sdp, num);
	if (bh == NULL)
		return NULL;

	ret = pread(sdp->device_fd, bh->b_data, sdp->sd_bsize, num * sdp->sd_bsize);
	if (ret != sdp->sd_bsize) {
		fprintf(stderr, "%s:%d: Error reading block %"PRIu64": %s\n",
		                caller, line, num, strerror(errno));
		free(bh);
		bh = NULL;
	}
	return bh;
}

int lgfs2_bwrite(struct lgfs2_buffer_head *bh)
{
	struct lgfs2_sbd *sdp = bh->sdp;

	if (pwritev(sdp->device_fd, &bh->iov, 1, bh->b_blocknr * sdp->sd_bsize) != bh->iov.iov_len)
		return -1;
	bh->b_modified = 0;
	return 0;
}

int lgfs2_brelse(struct lgfs2_buffer_head *bh)
{
	int error = 0;

	if (bh->b_blocknr == -1)
		printf("Double free!\n");
	if (bh->b_modified)
		error = lgfs2_bwrite(bh);
	bh->b_blocknr = -1;
	if (bh->b_altlist.next && !osi_list_empty(&bh->b_altlist))
		osi_list_del(&bh->b_altlist);
	free(bh);
	return error;
}

/**
 * Free a buffer head, discarding modifications.
 * @bhp: Pointer to the buffer
 */
void lgfs2_bfree(struct lgfs2_buffer_head **bhp)
{
	free(*bhp);
	*bhp = NULL;
}

uint32_t lgfs2_get_block_type(const char *buf)
{
	const struct gfs2_meta_header *mh = (void *)buf;

	if (be32_to_cpu(mh->mh_magic) == GFS2_MAGIC)
		return be32_to_cpu(mh->mh_type);

	return 0;
}
