#include "clusterautoconfig.h"

#define _GNU_SOURCE
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <fcntl.h>
#include <signal.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <time.h>
#include <math.h>
#include <assert.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <libaio.h>

#include <libgfs2.h>

#define NR_EVENTS 128	/* io_in_flight must be less than this */
#define NR_VEC 16

#define NR_BUCKETS 100
static unsigned long io_buckets[NR_BUCKETS]; /* For I/O latency */
static unsigned long pr_buckets[NR_BUCKETS]; /* For processing time */

struct io_info {
	struct iocb cb; /* Must remain first element of io_info */
	struct iovec iov[NR_VEC];
	struct timespec ts;
	void *buf;
	size_t bsize;
};

struct rgrp {
	u_int64_t start;
	unsigned rb_count;
	u_int32_t estminlen;
};

static struct rgrp **rgrps;
static unsigned nr_rgrps = 0;
static unsigned rg_bsize = 0;
static unsigned fsbsize = 0;
static unsigned long bytes_read = 0;
static int found_rg = 0;
static unsigned long linear_offset = 0; /* Next offset to read (in bytes) */
static unsigned long target_in_flight = 8; /* Number of I/Os we try to keep in flight */
static unsigned long io_in_flight = 0; /* Number of I/Os actually in flight */
static unsigned long io_size = 8 * 1024 * 1024; /* I/O size & alignment */
static struct lgfs2_dev_info dev_info;
static u_int64_t master_inode = 0;
static u_int64_t root_inode = 0;

static void zero_stats(unsigned long buckets[])
{
	unsigned i;

	for (i = 0; i < NR_BUCKETS; i++)
		buckets[i] = 0;
}

static void record_stats(unsigned long buckets[], double t)
{
	double dbucket = floor(10*log10(t*1e9));
	unsigned bucket = (unsigned)dbucket;

	if (bucket > NR_BUCKETS) {
		printf("erk!");
		exit(-1);
	}

	buckets[bucket]++;
}

static void print_stats(unsigned long buckets[], const char *fname)
{
	unsigned i;
	FILE *fp;

	fp = popen("/usr/bin/gnuplot", "w");
	if (fp == NULL) {
		perror("gnuplot");
		return;
	}

	fprintf(fp, "set terminal png size 800,600 linewidth 2 font \"Helvitica,16\"\n"
	            "set encoding utf8\n"
		    "set output '%s'\n"
	            "set xrange [1e-9:1e3]\n"
		    "set format x \"%%.1e\"\n"
		    "set xtics out nomirror 1e-9,1000,1e3\n"
		    "set xlabel \"Read Latency (Seconds)\"\n"
		    "set yrange [.1:10000]\n"
		    "set ylabel \"Time (Seconds)\"\n"
		    "set logscale\n"
		    "plot \"-\" using 1:2 notitle with boxes fill solid border -1\n", fname);

	for (i = 0; i < NR_BUCKETS; i++) {
		double top, bottom, centre;
		top = bottom = (double)i;
		top++;
		top /= 10;
		bottom /= 10;
		top = exp10(top);
		bottom = exp10(bottom);
		centre = ((top - bottom)/2 + bottom) * 1e-9;
		fprintf(fp, "%le %lf\n", centre, centre * (double)buckets[i]);
	}

	fclose(fp);

}

static struct rgrp *new_rgrp(u_int64_t where)
{
	struct rgrp *r = calloc(1, sizeof(struct rgrp));
	size_t nsize;

	r->start = where;

	if (nr_rgrps >= rg_bsize) {
		nsize = rg_bsize ? rg_bsize*2 : getpagesize();
		rgrps = realloc(rgrps, nsize*sizeof(void*));
		if (rgrps == NULL) {
			exit(-1);
		}
		rg_bsize = nsize;
	}

	rgrps[nr_rgrps++] = r;
	found_rg = 1;
	return r;
}

static void new_rb(u_int64_t where)
{
	if (nr_rgrps == 0)
		return;
	rgrps[nr_rgrps-1]->rb_count++;
}

static void est_rg_size(void)
{
	struct rgrp *r = rgrps[nr_rgrps-1];
	if (r->rb_count)
		r->estminlen = (r->rb_count - 1) * ((fsbsize - sizeof(struct gfs2_meta_header))/GFS2_NBBY) * fsbsize;
	found_rg = 0;
}

static struct timespec timespec_diff(struct timespec start, struct timespec end)
{
	struct timespec tmp;

	if ((end.tv_nsec - start.tv_nsec) < 0) {
		tmp.tv_sec = end.tv_sec - start.tv_sec - 1;
		tmp.tv_nsec = 1000000000 + end.tv_nsec - start.tv_nsec;
	} else {
		tmp.tv_sec = end.tv_sec - start.tv_sec;
		tmp.tv_nsec = end.tv_nsec - start.tv_nsec;
	}

	return tmp;
}

static struct iocb *new_cb(int fd, loff_t offset)
{
	struct io_info *info = calloc(1, sizeof(struct io_info));
	struct iocb *cb = &info->cb;
	size_t pgsize = getpagesize();
	size_t bsize = io_size;
	void *buf = NULL;
	struct iovec *iov;

	if (info == NULL)
		return NULL;

	if (posix_memalign(&buf, pgsize, bsize) != 0) {
		perror("posix_memalign");
		free(info);
		return NULL;
	}
	memset(buf, 0, bsize);

	iov = info->iov;
	iov->iov_base = buf;
	iov->iov_len = bsize;

	io_prep_preadv(cb, fd, iov, 1, offset);

	info->buf = buf;
	info->bsize = bsize;

	clock_gettime(CLOCK_MONOTONIC, &info->ts);
	return cb;
}

static inline int gfs2_dirent_sentinel(const struct gfs2_dirent *dent)
{
	return dent->de_inum.no_addr == 0 || dent->de_inum.no_formal_ino == 0;
}

static void do_dentries(const void *bufp, unsigned size)
{
	const struct gfs2_dirent *de;
	const char *buf = bufp;
	unsigned pos = 0;
	u_int16_t esize = 0;
	char name[256];
	u_int16_t nlen;

	while (size) {
		de = (struct gfs2_dirent *)(buf + pos);
		size -= esize;
		esize = be16_to_cpu(de->de_rec_len);
		if (esize < sizeof(struct gfs2_dirent))
			break;
		pos += esize;
		if (esize > size) {
			/* Error in dirent */
			break;
		}
		if (gfs2_dirent_sentinel(de))
			continue;
		nlen = be16_to_cpu(de->de_name_len);
		if (nlen > 255)
			nlen = 255;
		memcpy(name, (de+1), nlen);
		name[nlen] = 0;
		printf("de: %s %lu\n", name, be64_to_cpu(de->de_inum.no_addr));
	}

}

static void new_leaf(const struct gfs2_leaf *lf)
{
	do_dentries((lf+1), fsbsize - sizeof(struct gfs2_leaf));
}

static void new_extent(u_int64_t where, u_int64_t start, unsigned len)
{
//	printf("ext: in block %lu starting at %lu for %u blocks\n",
//			where/fsbsize, start, len);
}

static void new_indir(const void *buf, u_int64_t where, unsigned len)
{
	const u_int64_t *ptr = buf;
	unsigned i;
	u_int64_t estart = 0;
	unsigned elen = 0;

	len /= sizeof(u_int64_t);

//	printf("in: %lu\n", where);

	for (i = 0; i < len; i++) {
		if ((estart + elen) == be64_to_cpu(*ptr)) {
			ptr++;
			elen++;
			continue;
		}
		if (elen) {
			new_extent(where, estart, elen);
			elen = 0;
		}
		if (*ptr) {
			estart = be64_to_cpu(*ptr);
			elen = 1;
		}
		ptr++;
	}

	if (elen)
		new_extent(where, estart, elen);
}

static int is_zero32(const void *ptr)
{
	const u_int64_t *p = ptr;

	if (p[0] | p[1] | p[2] | p[3])
		return 0;
	return 1;
}

static void check_rindex(const void *bufp, u_int64_t where, unsigned len)
{
	unsigned i;
	unsigned zeros = 0;
	unsigned ricount = 0;
	const char *buf = bufp;

	len -= 31;

	for (i = 0; i < len; i += 32) {
		const struct gfs2_rindex *ri = (struct gfs2_rindex *)(buf+i);
		if (is_zero32(buf+i)) {
			zeros++;
			continue;
		}
		if (i && ((zeros == 0) || (zeros > 2)))
			return;
		zeros = 0;
		if (ri->ri_addr == 0 ||
		    ri->ri_length == 0 ||
		    ri->ri_data0 == 0 ||
		    ri->ri_data == 0 ||
		    ri->ri_bitbytes == 0)
			return;
		if (ri->__pad != 0)
			return;
		ricount++;
	}

	if (ricount == 0)
		return;

//	printf("rindex: %lu\n", where/fsbsize);
}

static void new_inode(const struct gfs2_dinode *di, u_int64_t where)
{
	u_int32_t mode = be32_to_cpu(di->di_mode);
	u_int32_t flags = be32_to_cpu(di->di_flags);
	u_int16_t depth;
	u_int16_t height = be16_to_cpu(di->di_height);

//	printf("di: %lu mode: %0x flags: %0x nlink:%u height %hu\n", where/fsbsize, mode, flags, be32_to_cpu(di->di_nlink), height);

	switch(mode & S_IFMT) {
	case S_IFREG:
//		printf("reg: size %lu\n", be64_to_cpu(di->di_size));
		if ((flags & GFS2_DIF_SYSTEM) && (di->di_height == 0)) {
			check_rindex((di+1), where, fsbsize - sizeof(struct gfs2_dinode));
		}
		break;
	case S_IFDIR:
		depth = be16_to_cpu(di->di_depth);
//		printf("dir: depth %hu entries %u\n", depth, be32_to_cpu(di->di_entries));
		if (depth == 0)
			do_dentries((di+1), fsbsize - sizeof(struct gfs2_dinode));
		break;
	default:
		return;
	}

	if (height)
		new_indir((di+1), where, fsbsize - sizeof(struct gfs2_dinode));
}

static void process_block(const void *buf, loff_t where)
{
	const struct gfs2_meta_header *mh = buf;
	const struct gfs2_sb *sb = buf;
	const struct gfs2_dinode *di = buf;
	const struct gfs2_leaf *lf = buf;
	u_int32_t mh_type = be32_to_cpu(mh->mh_type);

	if (mh->mh_magic != cpu_to_be32(GFS2_MAGIC)) {
		if (found_rg)
			est_rg_size();
//		if (fsbsize)
//			check_rindex(buf, where, fsbsize);
		return;
	}

	if (!fsbsize && (mh_type != GFS2_METATYPE_SB))
	       return;

	switch(mh_type) {
	case GFS2_METATYPE_SB:
		if (where != (GFS2_SB_ADDR * GFS2_BASIC_BLOCK))
			break;
		if (be32_to_cpu(mh->mh_format) != GFS2_FORMAT_SB)
			break;
		fsbsize = be32_to_cpu(sb->sb_bsize);
		master_inode = be64_to_cpu(sb->sb_master_dir.no_addr);
		root_inode = be64_to_cpu(sb->sb_root_dir.no_addr);
//		printf("sb: bsize %u\n", fsbsize);
		break;
	case GFS2_METATYPE_RG:
		if (be32_to_cpu(mh->mh_format) != GFS2_FORMAT_RG)
			break;
		if (found_rg)
			break;
//		printf("rg: %lu flags %08x free %u dinodes %u\n",
//				where,
//				be32_to_cpu(rg->rg_flags),
//				be32_to_cpu(rg->rg_free),
//				be32_to_cpu(rg->rg_dinodes));
		new_rgrp(where);
		return;
	case GFS2_METATYPE_RB:
		if (be32_to_cpu(mh->mh_format) != GFS2_FORMAT_RB)
			break;
		new_rb(where);
//		printf("rb\n");
		return;
	case GFS2_METATYPE_DI:
		if (be32_to_cpu(mh->mh_format) != GFS2_FORMAT_DI)
			break;
		if (be64_to_cpu(di->di_num.no_addr) * fsbsize != where)
			break;
		new_inode(di, where);
		break;
	case GFS2_METATYPE_IN:
		if (be32_to_cpu(mh->mh_format) != GFS2_FORMAT_IN)
			break;
		new_indir((mh+1), where, fsbsize - sizeof(struct gfs2_meta_header));
		break;
	case GFS2_METATYPE_LF:
		if (be32_to_cpu(mh->mh_format) != GFS2_FORMAT_LF)
			break;
//		printf("lf\n");
		new_leaf(lf);
		break;
	case GFS2_METATYPE_EA:
		if (be32_to_cpu(mh->mh_format) != GFS2_FORMAT_EA)
			break;
//		printf("ea\n");
		break;
	case GFS2_METATYPE_ED:
		if (be32_to_cpu(mh->mh_format) != GFS2_FORMAT_QC)
			break;
//		printf("ed\n");
		break;
	case GFS2_METATYPE_JD:
		if (be32_to_cpu(mh->mh_format) != GFS2_FORMAT_JD)
			break;
//		printf("jd\n");
		break;
	}

//	fflush(stdout);
	if (found_rg)
		est_rg_size();
}

static double ts2sec(const struct timespec ts)
{
	return (double)ts.tv_sec + ((double)ts.tv_nsec / 1e9);
}

static int process_event(io_context_t ctx, const struct io_event *e)
{
	struct io_info *info = (struct io_info *)e->obj;
	struct iocb *cb = &info->cb;
	struct iocb **ncb;
	struct iocb **cblist;
	struct timespec now, pstart, pend;
	struct timespec df;
	unsigned nr_ios;
	double t, mbs;
	int i;

	assert(io_in_flight >= 1);
	io_in_flight--;

	if (e->res == info->bsize) {

		clock_gettime(CLOCK_MONOTONIC, &now);
		df = timespec_diff(info->ts, now);
		t = ts2sec(df);
		record_stats(io_buckets, t);
		mbs = info->bsize / t;
		mbs /= (1024*1024);
		bytes_read += e->res;

		if ((target_in_flight > io_in_flight) &&
		    (linear_offset < dev_info.size)) {
			nr_ios = target_in_flight - io_in_flight;
			cblist = alloca(nr_ios * sizeof(struct iocb *));
			ncb = cblist;
			for (i = 0; i < nr_ios; i++){
				*ncb = new_cb(cb->aio_fildes, linear_offset);
				linear_offset += io_size;
				if (*ncb == NULL)
					break;
				ncb++;
				if (linear_offset >= dev_info.size)
					break;
			}
			io_submit(ctx, i, cblist);
			io_in_flight += i;
		}
	}

	clock_gettime(CLOCK_MONOTONIC, &pstart);
	for (i = 0; i < info->bsize; i += (fsbsize ? fsbsize : 4096))
		process_block((char *)info->buf + i, cb->u.c.offset + i);

	free(info->buf);
	free(info);
	clock_gettime(CLOCK_MONOTONIC, &pend);
	df = timespec_diff(pstart, pend);
	t = ts2sec(df);
	record_stats(pr_buckets, t);

	return (io_in_flight == 0) ? 1 : 0;
}

int main(int argc, char *argv[])
{
	struct io_event events[NR_EVENTS];
	struct timespec begin, end, df;
	io_context_t ctx = 0;
	struct iocb *cb;
	int ret;
	double t;
	int fd;
	int i;

	zero_stats(io_buckets);
	zero_stats(pr_buckets);

	clock_gettime(CLOCK_MONOTONIC, &begin);

	fd = open(argv[1], O_RDONLY|O_EXCL|O_DIRECT|O_CLOEXEC|O_NOATIME);
	if (fd < 0) {
		perror(argv[1]);
		exit(-1);
	}

	lgfs2_get_dev_info(fd, &dev_info);

	io_setup(NR_EVENTS, &ctx);

	cb = new_cb(fd, linear_offset);
	linear_offset += io_size;
	io_submit(ctx, 1, &cb);
	io_in_flight++;

	do {
		ret = io_getevents(ctx, 1, NR_EVENTS, events, NULL);
		for (i = 0; i < ret; i++) {
			if (process_event(ctx, &events[i]))
				goto out;
		}

	} while (ret > 0);
out:
	io_destroy(ctx);

	if (ret < 0)
		perror("io_getevents");

	clock_gettime(CLOCK_MONOTONIC, &end);
	df = timespec_diff(begin, end);
	t = ts2sec(df);
	printf("%lf secs, %lfMB/sec\n", t, bytes_read / (1024*1024) / t);

	for (i = 0; i < nr_rgrps; i++) {
		printf("Rgrp: %i at %lu with %u rb\n", i, rgrps[i]->start, rgrps[i]->rb_count);
	}

	print_stats(io_buckets, "fscan-io.png");
	print_stats(pr_buckets, "fscan-pr.png");

	return 0;
}
