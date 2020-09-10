#include <check.h>

START_TEST(test_tune_stub)
{
	ck_assert(1);
}
END_TEST

static Suite *suite_tune(void)
{
	Suite *s = suite_create("main.c");
	TCase *tc_tune = tcase_create("tunegfs2");
	tcase_add_test(tc_tune, test_tune_stub);
	suite_add_tcase(s, tc_tune);
	return s;
}

int main(void)
{
	int failures;

	SRunner *runner = srunner_create(suite_tune());
	srunner_run_all(runner, CK_ENV);
	failures = srunner_ntests_failed(runner);
	srunner_free(runner);
	return failures ? 1 : 0;
}
