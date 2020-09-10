#include <check.h>

START_TEST(test_mkfs_stub)
{
	ck_assert(1);
}
END_TEST

static Suite *suite_mkfs(void)
{
	Suite *s = suite_create("main_mkfs.c");
	TCase *tc_mkfs = tcase_create("mkfs.gfs2");
	tcase_add_test(tc_mkfs, test_mkfs_stub);
	suite_add_tcase(s, tc_mkfs);
	return s;
}

int main(void)
{
	int failures;

	SRunner *runner = srunner_create(suite_mkfs());
	srunner_run_all(runner, CK_ENV);
	failures = srunner_ntests_failed(runner);
	srunner_free(runner);
	return failures ? 1 : 0;
}
