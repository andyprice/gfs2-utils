#include <check.h>

START_TEST(test_jadd_stub)
{
	ck_assert(1);
}
END_TEST

static Suite *suite_jadd(void)
{
	Suite *s = suite_create("main_jadd.c");
	TCase *tc_jadd = tcase_create("jadd.gfs2");
	tcase_add_test(tc_jadd, test_jadd_stub);
	suite_add_tcase(s, tc_jadd);
	return s;
}

int main(void)
{
	int failures;

	SRunner *runner = srunner_create(suite_jadd());
	srunner_run_all(runner, CK_ENV);
	failures = srunner_ntests_failed(runner);
	srunner_free(runner);
	return failures ? 1 : 0;
}
