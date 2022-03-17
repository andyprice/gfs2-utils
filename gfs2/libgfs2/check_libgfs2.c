#include <check.h>
#include "libgfs2.h"

extern Suite *suite_meta(void);
extern Suite *suite_ondisk(void);
extern Suite *suite_rgrp(void);
extern Suite *suite_fs_ops(void);

int main(void)
{
	int failures;

	SRunner *runner = srunner_create(suite_meta());
	srunner_add_suite(runner, suite_ondisk());
	srunner_add_suite(runner, suite_rgrp());
	srunner_add_suite(runner, suite_fs_ops());

	srunner_run_all(runner, CK_ENV);
	failures = srunner_ntests_failed(runner);
	srunner_free(runner);
	return failures ? 1 : 0;
}
