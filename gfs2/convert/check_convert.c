#include <check.h>

START_TEST(test_convert_stub)
{
	ck_assert(1);
}
END_TEST

static Suite *suite_convert(void)
{
	Suite *s = suite_create("gfs2_convert.c");
	TCase *tc_convert = tcase_create("gfs2_convert");
	tcase_add_test(tc_convert, test_convert_stub);
	suite_add_tcase(s, tc_convert);
	return s;
}

int main(void)
{
	int failures;

	SRunner *runner = srunner_create(suite_convert());
	srunner_run_all(runner, CK_ENV);
	failures = srunner_ntests_failed(runner);
	srunner_free(runner);
	return failures ? 1 : 0;
}
