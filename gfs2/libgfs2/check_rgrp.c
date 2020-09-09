#include <check.h>
#include "libgfs2.h"
#include "rgrp.h" /* Private header libgfs2/rgrp.h for convenience */

Suite *suite_rgrp(void);

lgfs2_rgrps_t tc_rgrps;

static void mockup_rgrps(void)
{
	struct gfs2_sbd *sdp;
	lgfs2_rgrps_t rgs;
	uint64_t addr;
	struct gfs2_rindex ri = {0};
	lgfs2_rgrp_t rg;
	uint32_t rgsize = (1024 << 20) / 4096;
	int ret;

	sdp = calloc(1, sizeof(*sdp));
	ck_assert(sdp != NULL);

	sdp->device.length = rgsize + 20;
	sdp->device_fd = -1;
	sdp->bsize = sdp->sd_sb.sb_bsize = 4096;
	compute_constants(sdp);

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
	for (minext = 1; minext <= rbm.rgd->ri.ri_data; minext++) {
		int err;
		uint64_t addr;

		rbm.offset = rbm.bii = 0;

		err = lgfs2_rbm_find(&rbm, GFS2_BLKST_FREE, &minext);
		ck_assert_int_eq(err, 0);

		addr = lgfs2_rbm_to_block(&rbm);
		ck_assert(addr == rbm.rgd->ri.ri_data0);
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
	minext = rbm.rgd->ri.ri_data + 1;

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
	for (i = 0; i < rg->ri.ri_length; i++)
		memset(rg->bits[i].bi_data, 0xff, rgs->sdp->bsize);

	/* ...except the final one */
	err = gfs2_set_bitmap(rg, rg->ri.ri_data0 + rg->ri.ri_data - 1, GFS2_BLKST_FREE);
	ck_assert_int_eq(err, 0);

	err = lgfs2_rbm_find(&rbm, GFS2_BLKST_FREE, &minext);
	ck_assert_int_eq(err, 0);

	addr = lgfs2_rbm_to_block(&rbm);
	ck_assert(addr == (rg->ri.ri_data0 + rg->ri.ri_data - 1));
}
END_TEST

Suite *suite_rgrp(void)
{

	Suite *s = suite_create("rgrp.c");

	TCase *tc_rbm_find = tcase_create("rbm_find");
	tcase_add_checked_fixture(tc_rbm_find, mockup_rgrps, teardown_rgrps);
	tcase_add_test(tc_rbm_find, test_rbm_find_good);
	tcase_add_test(tc_rbm_find, test_rbm_find_bad);
	tcase_add_test(tc_rbm_find, test_rbm_find_lastblock);
	tcase_set_timeout(tc_rbm_find, 0);
	suite_add_tcase(s, tc_rbm_find);

	return s;
}
