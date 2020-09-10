#include <check.h>

START_TEST(test_edit_stub)
{
	ck_assert(1);
}
END_TEST

static Suite *suite_edit(void)
{
	Suite *s = suite_create("hexedit.c");
	TCase *tc_edit = tcase_create("gfs2_edit");
	tcase_add_test(tc_edit, test_edit_stub);
	suite_add_tcase(s, tc_edit);
	return s;
}

int main(void)
{
	int failures;

	SRunner *runner = srunner_create(suite_edit());
	srunner_run_all(runner, CK_ENV);
	failures = srunner_ntests_failed(runner);
	srunner_free(runner);
	return failures ? 1 : 0;
}
