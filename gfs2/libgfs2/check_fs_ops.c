#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <check.h>
#include <errno.h>
#include "libgfs2.h"

Suite *suite_fs_ops(void);

/* Doesn't need to be big, we just need some blocks to do i/o on */
#define MOCK_DEV_SIZE ((LGFS2_MAX_RGSIZE * 2ull) << 20)
#define MOCK_BSIZE (512)
#define MOCK_RGSIZE ((LGFS2_MIN_RGSIZE + 2) << 20)

struct lgfs2_sbd *mock_sdp;

static void mockup_fs(void)
{
	struct lgfs2_sbd *sdp;
	char tmpnam[] = "mockdev-XXXXXX";

	sdp = calloc(1, sizeof(*sdp));
	ck_assert(sdp != NULL);

	sdp->device.length = MOCK_DEV_SIZE / MOCK_BSIZE;

	sdp->device_fd = mkstemp(tmpnam);
	ck_assert(sdp->device_fd >= 0);
	ck_assert(unlink(tmpnam) == 0);
	ck_assert(ftruncate(sdp->device_fd, MOCK_DEV_SIZE) == 0);

	sdp->sd_bsize = MOCK_BSIZE;
	lgfs2_compute_constants(sdp);

	mock_sdp = sdp;
}

static void teardown_mock_fs(void)
{
	close(mock_sdp->device_fd);
	free(mock_sdp);
}

lgfs2_rgrps_t mock_rgs;

/* This fixture must be added after mockup_fs */
static void mockup_rgs(void)
{
	lgfs2_rgrps_t rgs;
	uint32_t rgcount, rgcount_check = 0;
	uint32_t tgtsize;
	uint64_t addr = GFS2_SB_ADDR + 1;

	ck_assert(mock_sdp != NULL);

	rgs = lgfs2_rgrps_init(mock_sdp, 0, 0);
	ck_assert(rgs != NULL);

	tgtsize = MOCK_RGSIZE / mock_sdp->sd_bsize;
	rgcount = lgfs2_rgrps_plan(rgs, mock_sdp->device.length - GFS2_SB_ADDR, tgtsize);
	ck_assert(rgcount > 0);
	while (1) {
		uint64_t nextaddr = 0;
		lgfs2_rgrp_t rg;
		struct gfs2_rindex ri;
		int err;

		nextaddr = lgfs2_rindex_entry_new(rgs, &ri, addr, 0);
		if (nextaddr == 0)
			break;
		rg = lgfs2_rgrps_append(rgs, &ri, addr - nextaddr);
		ck_assert(rg != NULL);
		err = lgfs2_rgrp_bitbuf_alloc(rg);
		ck_assert(err == 0);
		mock_sdp->fssize = be64_to_cpu(ri.ri_data0) + be32_to_cpu(ri.ri_data);
		addr = nextaddr;
		rgcount_check++;
	}
	ck_assert(rgcount_check == rgcount);
	mock_rgs = rgs;
	lgfs2_attach_rgrps(mock_sdp, mock_rgs);
}

static void teardown_mock_rgs(void)
{
	lgfs2_rgrps_free(&mock_rgs);
	ck_assert(mock_rgs == NULL);
}

START_TEST(test_lookupi_bad_name_size)
{
	struct lgfs2_inode idir;
	struct lgfs2_inode *ret = NULL;
	int e;

	e = lgfs2_lookupi(&idir, ".", 0, &ret);
	ck_assert(e == -ENAMETOOLONG);
	ck_assert(ret == NULL);

	e = lgfs2_lookupi(&idir, ".", GFS2_FNAMESIZE + 1, &ret);
	ck_assert(e == -ENAMETOOLONG);
	ck_assert(ret == NULL);
}
END_TEST

START_TEST(test_lookupi_dot)
{
	struct lgfs2_inode idir;
	struct lgfs2_inode *ret;
	int e;

	/* The contents of idir shouldn't matter, a "." lookup should just return it */
	e = lgfs2_lookupi(&idir, ".", 1, &ret);
	ck_assert(e == 0);
	ck_assert(ret == &idir);
}
END_TEST

START_TEST(test_lookupi_dotdot)
{
	struct lgfs2_sbd *sdp = mock_sdp;
	char buf[512] = {0};
	struct lgfs2_buffer_head bh = {
		.b_data = buf,
		.b_blocknr = 42,
	};
	struct lgfs2_inode idir = {
		.i_sbd = sdp,
		.i_mode = S_IFDIR,
		.i_bh = &bh,
		.i_entries = 2,

	};
	struct gfs2_dirent *dent = (void *)(buf + sizeof(struct gfs2_dinode));
	struct lgfs2_inode *ret;
	int e;

	/* "." */
	dent->de_inum.no_addr = cpu_to_be64(42);
	dent->de_inum.no_formal_ino = cpu_to_be64(1);
	dent->de_rec_len = cpu_to_be16(GFS2_DIRENT_SIZE(1));
	dent->de_name_len = cpu_to_be16(1);
	dent->de_hash = cpu_to_be32(lgfs2_disk_hash(".", 1));
	*(char *)(dent + 1) = '.';

	/* ".." */
	dent = (void *)(buf + sizeof(struct gfs2_dinode) + GFS2_DIRENT_SIZE(1));
	dent->de_inum.no_addr = cpu_to_be64(43);
	dent->de_inum.no_formal_ino = cpu_to_be64(2);
	dent->de_rec_len = cpu_to_be16(MOCK_BSIZE - GFS2_DIRENT_SIZE(1) - sizeof(struct gfs2_dinode));
	dent->de_name_len = cpu_to_be16(2);
	dent->de_hash = cpu_to_be32(lgfs2_disk_hash("..", 2));
	*(char *)(dent + 1) = '.';
	*((char *)(dent + 1) + 1) = '.';

	e = lgfs2_lookupi(&idir, "..", 2, &ret);
	ck_assert(e == 0);
	ck_assert(ret != &idir);
	lgfs2_inode_put(&ret);
}
END_TEST

START_TEST(test_dinode_alloc)
{
	struct lgfs2_sbd *sdp = mock_sdp;
	uint64_t newblock = 0;
	uint64_t expected_dinodes_alloced = sdp->dinodes_alloced;
	uint64_t expected_blks_alloced = sdp->blks_alloced;
	int err;

	/* Fail an allocation by requesting too many blocks */
	err = lgfs2_dinode_alloc(sdp, ~(uint64_t)0, &newblock);
	ck_assert(err != 0);
	ck_assert(sdp->dinodes_alloced == expected_dinodes_alloced);
	ck_assert(sdp->blks_alloced == expected_blks_alloced);

	/* Allocate a dinode, request 1 block of space in the rg */
	err = lgfs2_dinode_alloc(sdp, 1, &newblock);
	expected_dinodes_alloced += 1;
	expected_blks_alloced += 1;
	ck_assert(err == 0);
	ck_assert(newblock > GFS2_SB_ADDR);
	ck_assert(newblock < sdp->fssize);
	ck_assert(sdp->dinodes_alloced == expected_dinodes_alloced);
	ck_assert(sdp->blks_alloced == expected_blks_alloced);
	ck_assert(lgfs2_get_bitmap(sdp, newblock, NULL) == GFS2_BLKST_DINODE);
}
END_TEST

START_TEST(test_meta_alloc)
{
	struct lgfs2_sbd *sdp = mock_sdp;
	uint64_t newblock = 0;
	struct lgfs2_buffer_head bh = {0};
	struct lgfs2_inode ip = { .i_bh = &bh, .i_sbd = sdp };
	uint64_t expected_blks_alloced = sdp->blks_alloced;
	int err;

	err = lgfs2_meta_alloc(&ip, &newblock);
	expected_blks_alloced += 1;
	ck_assert(err == 0);
	ck_assert(newblock > GFS2_SB_ADDR);
	ck_assert(newblock < sdp->fssize);
	ck_assert(sdp->blks_alloced == expected_blks_alloced);
	ck_assert(lgfs2_get_bitmap(sdp, newblock, NULL) == GFS2_BLKST_USED);
}
END_TEST


Suite *suite_fs_ops(void)
{
	Suite *s = suite_create("fs_ops.c");
	TCase *tc;

	tc = tcase_create("lgfs2_lookupi basic");
	tcase_add_test(tc, test_lookupi_bad_name_size);
	tcase_add_test(tc, test_lookupi_dot);
	suite_add_tcase(s, tc);

	tc = tcase_create("lgfs2_lookupi with fixture");
	tcase_add_checked_fixture(tc, mockup_fs, teardown_mock_fs);
	tcase_add_test(tc, test_lookupi_dotdot);
	suite_add_tcase(s, tc);

	tc = tcase_create("lgfs2_dinode_alloc");
	tcase_add_checked_fixture(tc, mockup_fs, teardown_mock_fs);
	tcase_add_checked_fixture(tc, mockup_rgs, teardown_mock_rgs);
	tcase_add_test(tc, test_dinode_alloc);
	suite_add_tcase(s, tc);

	tc = tcase_create("lgfs2_meta_alloc");
	tcase_add_checked_fixture(tc, mockup_fs, teardown_mock_fs);
	tcase_add_checked_fixture(tc, mockup_rgs, teardown_mock_rgs);
	tcase_add_test(tc, test_meta_alloc);
	suite_add_tcase(s, tc);

	return s;
}
