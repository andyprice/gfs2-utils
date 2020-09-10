#include <check.h>

START_TEST(test_grow_stub)
{
	ck_assert(1);
}
END_TEST

static Suite *suite_grow(void)
{
	Suite *s = suite_create("main_grow.c");
	TCase *tc_grow = tcase_create("grow.gfs2");
	tcase_add_test(tc_grow, test_grow_stub);
	suite_add_tcase(s, tc_grow);
	return s;
}

int main(void)
{
	int failures;

	SRunner *runner = srunner_create(suite_grow());
	srunner_run_all(runner, CK_ENV);
	failures = srunner_ntests_failed(runner);
	srunner_free(runner);
	return failures ? 1 : 0;
}
