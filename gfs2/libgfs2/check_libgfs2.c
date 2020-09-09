#include <check.h>
#include "libgfs2.h"

// TODO: Remove this when the extern is removed from libgfs2
void print_it(const char *label, const char *fmt, const char *fmt2, ...) {}

extern Suite *suite_meta(void);
extern Suite *suite_rgrp(void);

int main(void)
{
	int failures;

	SRunner *runner = srunner_create(suite_meta());
	srunner_add_suite(runner, suite_rgrp());

	srunner_run_all(runner, CK_ENV);
	failures = srunner_ntests_failed(runner);
	srunner_free(runner);
	return failures ? 1 : 0;
}
