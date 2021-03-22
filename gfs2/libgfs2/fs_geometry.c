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
#include "config.h"

/**
 * Given a number of blocks in a resource group, return the number of blocks
 * needed for bitmaps. Also calculate the adjusted number of free data blocks
 * in the resource group and store it in *ri_data.
 */
uint32_t rgblocks2bitblocks(const unsigned int bsize, const uint32_t rgblocks, uint32_t *ri_data)
{
	uint32_t mappable = 0;
	uint32_t bitblocks = 0;
	/* Number of blocks mappable by bitmap blocks with these header types */
	const uint32_t blks_rgrp = GFS2_NBBY * (bsize - sizeof(struct gfs2_rgrp));
	const uint32_t blks_meta = GFS2_NBBY * (bsize - sizeof(struct gfs2_meta_header));

	while (blks_rgrp + (blks_meta * bitblocks) < ((rgblocks - bitblocks) & ~(uint32_t)3))
		bitblocks++;

	if (bitblocks > 0)
		mappable = blks_rgrp + (blks_meta * (bitblocks - 1));

	*ri_data = (rgblocks - (bitblocks + 1)) & ~(uint32_t)3;
	if (mappable < *ri_data)
		bitblocks++;

	return bitblocks;
}
