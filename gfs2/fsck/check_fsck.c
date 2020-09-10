#include <check.h>

START_TEST(test_fsck_stub)
{
	ck_assert(1);
}
END_TEST

static Suite *suite_fsck(void)
{
	Suite *s = suite_create("main.c");
	TCase *tc_fsck = tcase_create("fsck.gfs2");
	tcase_add_test(tc_fsck, test_fsck_stub);
	suite_add_tcase(s, tc_fsck);
	return s;
}

int main(void)
{
	int failures;

	SRunner *runner = srunner_create(suite_fsck());
	srunner_run_all(runner, CK_ENV);
	failures = srunner_ntests_failed(runner);
	srunner_free(runner);
	return failures ? 1 : 0;
}
