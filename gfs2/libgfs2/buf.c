#include "clusterautoconfig.h"

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

struct gfs2_buffer_head *bget(struct gfs2_sbd *sdp, uint64_t num)
{
	struct gfs2_buffer_head *bh;

	bh = calloc(1, sizeof(struct gfs2_buffer_head) + sdp->bsize);
	if (bh == NULL)
		return NULL;

	bh->b_blocknr = num;
	bh->sdp = sdp;
	bh->b_data = (char *)bh + sizeof(struct gfs2_buffer_head);
	return bh;
}

struct gfs2_buffer_head *__bread(struct gfs2_sbd *sdp, uint64_t num, int line,
				 const char *caller)
{
	struct gfs2_buffer_head *bh = bget(sdp, num);
	if (bh == NULL)
		return bh;
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
	return bh;
}

int bwrite(struct gfs2_buffer_head *bh)
{
	struct gfs2_sbd *sdp = bh->sdp;

	if (lseek(sdp->device_fd, bh->b_blocknr * sdp->bsize, SEEK_SET) !=
	    bh->b_blocknr * sdp->bsize) {
		return -1;
	}
	if (write(sdp->device_fd, bh->b_data, sdp->bsize) != sdp->bsize)
		return -1;
	sdp->writes++;
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
