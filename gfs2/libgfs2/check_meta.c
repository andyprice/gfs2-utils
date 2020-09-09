#include <check.h>
#include "libgfs2.h"

Suite *suite_meta(void);

START_TEST(test_lgfs2_meta)
{
	ck_assert(lgfs2_selfcheck() == 0);
}
END_TEST

Suite *suite_meta(void)
{
	Suite *s = suite_create("meta.c");

	TCase *tc_meta = tcase_create("Metadata description self-check");
	tcase_add_test(tc_meta, test_lgfs2_meta);
	suite_add_tcase(s, tc_meta);

	return s;
}
