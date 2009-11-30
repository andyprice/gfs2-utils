#include "clusterautoconfig.h"

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/time.h>
#include <stdio.h>
#include <libintl.h>
#include <ctype.h>
#define _(String) gettext(String)

#include "libgfs2.h"
#include "fs_bits.h"
#include "util.h"

void big_file_comfort(struct gfs2_inode *ip, uint64_t blks_checked)
{
	static struct timeval tv;
	static uint32_t seconds = 0;
	static uint64_t percent, fsize, chksize;
	uint64_t one_percent = 0;
	int i, cs;
	const char *human_abbrev = " KMGTPE";

	one_percent = ip->i_di.di_blocks / 100;
	if (blks_checked - last_reported_fblock < one_percent)
		return;

	last_reported_block = blks_checked;
	gettimeofday(&tv, NULL);
	if (!seconds)
		seconds = tv.tv_sec;
	if (tv.tv_sec == seconds)
		return;

	fsize = ip->i_di.di_size;
	for (i = 0; i < 6 && fsize > 1024; i++)
		fsize /= 1024;
	chksize = blks_checked * ip->i_sbd->bsize;
	for (cs = 0; cs < 6 && chksize > 1024; cs++)
		chksize /= 1024;
	seconds = tv.tv_sec;
	percent = (blks_checked * 100) / ip->i_di.di_blocks;
	log_notice( _("\rChecking %lld%c of %lld%c of file at %lld (0x%llx)"
		      "- %llu percent complete.                   \r"),
		    (long long)chksize, human_abbrev[cs],
		    (unsigned long long)fsize, human_abbrev[i],
		    (unsigned long long)ip->i_di.di_num.no_addr,
		    (unsigned long long)ip->i_di.di_num.no_addr,
		    (unsigned long long)percent);
	fflush(stdout);
}

/* Put out a warm, fuzzy message every second so the user     */
/* doesn't think we hung.  (This may take a long time).       */
void warm_fuzzy_stuff(uint64_t block)
{
	static uint64_t one_percent = 0;
	static struct timeval tv;
	static uint32_t seconds = 0;
	
	if (!one_percent)
		one_percent = last_fs_block / 100;
	if (block - last_reported_block >= one_percent) {
		last_reported_block = block;
		gettimeofday(&tv, NULL);
		if (!seconds)
			seconds = tv.tv_sec;
		if (tv.tv_sec - seconds) {
			static uint64_t percent;

			seconds = tv.tv_sec;
			if (last_fs_block) {
				percent = (block * 100) / last_fs_block;
				log_notice( _("\r%" PRIu64 " percent complete.\r"), percent);
			}
		}
	}
}

const char *block_type_string(struct gfs2_block_query *q)
{
	const char *blktyp[] = {"free", "used", "indirect data", "inode",
							"file", "symlink", "block dev", "char dev",
							"fifo", "socket", "dir leaf", "journ data",
							"other meta", "eattribute", "unused",
							"invalid"};
	if (q->block_type < 16)
		return (blktyp[q->block_type]);
	return blktyp[15];
}
