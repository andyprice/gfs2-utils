#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <check.h>
#include "libgfs2.h"
#include "rgrp.h"

/* Keep this size small enough to run on most build machines and large enough
   that we can create several resource groups. */
#define MOCK_DEV_SIZE (1 << 30)

Suite *suite_rgrp(void);

lgfs2_rgrps_t tc_rgrps;

static void mockup_rgrps(void)
{
	struct lgfs2_sbd *sdp;
	lgfs2_rgrps_t rgs;
	uint64_t addr;
	struct gfs2_rindex ri = {0};
	lgfs2_rgrp_t rg;
	uint32_t rgsize = (100 << 20) / 4096;
	char tmpnam[] = "mockdev-XXXXXX";
	int ret;

	sdp = calloc(1, sizeof(*sdp));
	ck_assert(sdp != NULL);

	sdp->device.length = MOCK_DEV_SIZE / 4096;

	sdp->device_fd = mkstemp(tmpnam);
	ck_assert(sdp->device_fd >= 0);
	ck_assert(unlink(tmpnam) == 0);
	ck_assert(ftruncate(sdp->device_fd, MOCK_DEV_SIZE) == 0);

	sdp->sd_bsize = 4096;
	lgfs2_compute_constants(sdp);

	rgs = lgfs2_rgrps_init(sdp, 0, 0);
	ck_assert(rgs != NULL);

	lgfs2_rgrps_plan(rgs, sdp->device.length - 16, rgsize);

	addr = lgfs2_rindex_entry_new(rgs, &ri, 16, rgsize);
	ck_assert(addr != 0);

	rg = lgfs2_rgrps_append(rgs, &ri, 0);
	ck_assert(rg != NULL);

	ret = lgfs2_rgrp_bitbuf_alloc(rg);
	ck_assert(ret == 0);
	ck_assert(rg->bits[0].bi_data != NULL);

	tc_rgrps = rgs;
}

static void teardown_rgrps(void)
{
	close(tc_rgrps->sdp->device_fd);
	free(tc_rgrps->sdp);
	lgfs2_rgrp_bitbuf_free(lgfs2_rgrp_first(tc_rgrps));
	lgfs2_rgrps_free(&tc_rgrps);
}

START_TEST(test_rbm_find_good)
{
	uint32_t minext;
	struct lgfs2_rbm rbm = {0};
	lgfs2_rgrps_t rgs = tc_rgrps;
	rbm.rgd = lgfs2_rgrp_first(rgs);

	/* Check that extent sizes up to the whole rg can be found */
	for (minext = 1; minext <= rbm.rgd->rt_data; minext++) {
		int err;
		uint64_t addr;

		rbm.offset = rbm.bii = 0;

		err = lgfs2_rbm_find(&rbm, GFS2_BLKST_FREE, &minext);
		ck_assert_int_eq(err, 0);

		addr = lgfs2_rbm_to_block(&rbm);
		ck_assert(addr == rbm.rgd->rt_data0);
	}
}
END_TEST

START_TEST(test_rbm_find_bad)
{
	int err;
	uint32_t minext;
	struct lgfs2_rbm rbm = {0};
	lgfs2_rgrps_t rgs = tc_rgrps;

	rbm.rgd = lgfs2_rgrp_first(rgs);
	minext = rbm.rgd->rt_data + 1;

	err = lgfs2_rbm_find(&rbm, GFS2_BLKST_FREE, &minext);
	ck_assert_int_eq(err, 1);
}
END_TEST

START_TEST(test_rbm_find_lastblock)
{
	int err;
	unsigned i;
	uint64_t addr;
	uint32_t minext = 1; /* Only looking for one block */
	struct lgfs2_rbm rbm = {0};
	lgfs2_rgrp_t rg;
	lgfs2_rgrps_t rgs = tc_rgrps;

	rbm.rgd = rg = lgfs2_rgrp_first(rgs);

	/* Flag all blocks as allocated... */
	for (i = 0; i < rg->rt_length; i++)
		memset(rg->bits[i].bi_data, 0xff, rgs->sdp->sd_bsize);

	/* ...except the final one */
	err = lgfs2_set_bitmap(rg, rg->rt_data0 + rg->rt_data - 1, GFS2_BLKST_FREE);
	ck_assert_int_eq(err, 0);

	err = lgfs2_rbm_find(&rbm, GFS2_BLKST_FREE, &minext);
	ck_assert_int_eq(err, 0);

	addr = lgfs2_rbm_to_block(&rbm);
	ck_assert(addr == (rg->rt_data0 + rg->rt_data - 1));
}
END_TEST

START_TEST(test_rgrps_write_final)
{
	lgfs2_rgrp_t rg = lgfs2_rgrp_last(tc_rgrps);
	struct lgfs2_sbd *sdp = tc_rgrps->sdp;
	struct gfs2_rindex ri;
	struct gfs2_rgrp rgrp;
	uint64_t addr;
	char *buf;

	lgfs2_rindex_out(rg, &ri);
	addr = be64_to_cpu(ri.ri_addr);

	buf = calloc(1, 4096);
	ck_assert(buf != NULL);
	memset(buf, 0xff, sizeof(rgrp));
	ck_assert(pwrite(sdp->device_fd, buf, 4096, addr * 4096) == 4096);

	ck_assert(lgfs2_rgrps_write_final(sdp->device_fd, tc_rgrps) == 0);
	ck_assert(pread(sdp->device_fd, &rgrp, sizeof(rgrp), addr * 4096) == sizeof(rgrp));

	ck_assert(be32_to_cpu(rgrp.rg_header.mh_magic) == GFS2_MAGIC);
	ck_assert(be32_to_cpu(rgrp.rg_header.mh_type) == GFS2_METATYPE_RG);
	ck_assert(rgrp.rg_skip == 0);
	free(buf);

	ck_assert(lgfs2_rgrps_write_final(-1, tc_rgrps) == -1);
}
END_TEST

Suite *suite_rgrp(void)
{

	Suite *s = suite_create("rgrp.c");
	TCase *tc;

	tc = tcase_create("rbm_find");
	tcase_add_checked_fixture(tc, mockup_rgrps, teardown_rgrps);
	tcase_add_test(tc, test_rbm_find_good);
	tcase_add_test(tc, test_rbm_find_bad);
	tcase_add_test(tc, test_rbm_find_lastblock);
	tcase_set_timeout(tc, 0);
	suite_add_tcase(s, tc);

	tc = tcase_create("lgfs2_rgrps_write_final");
	tcase_add_checked_fixture(tc, mockup_rgrps, teardown_rgrps);
	tcase_add_test(tc, test_rgrps_write_final);
	suite_add_tcase(s, tc);

	return s;
}
