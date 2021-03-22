#include <check.h>
#include "libgfs2.h"

Suite *suite_meta(void);

START_TEST(check_metadata_sizes)
{
	unsigned offset;
	int i, j;

	for (i = 0; i < lgfs2_metadata_size; i++) {
		const struct lgfs2_metadata *m = &lgfs2_metadata[i];
		offset = 0;
		for (j = 0; j < m->nfields; j++) {
			const struct lgfs2_metafield *f = &m->fields[j];
			ck_assert(f->offset == offset);
			offset += f->length;
		}
		ck_assert(offset == m->size);
	}
}
END_TEST

START_TEST(check_symtab)
{
	int i, j;

	for (i = 0; i < lgfs2_metadata_size; i++) {
		const struct lgfs2_metadata *m = &lgfs2_metadata[i];
		for (j = 0; j < m->nfields; j++) {
			const struct lgfs2_metafield *f = &m->fields[j];
			if (f->flags & (LGFS2_MFF_MASK|LGFS2_MFF_ENUM))
				ck_assert(f->symtab != NULL);
			if (f->symtab)
				ck_assert(f->flags & (LGFS2_MFF_MASK|LGFS2_MFF_ENUM));
		}
	}
}
END_TEST

START_TEST(check_ptrs)
{
	int i, j;

	for (i = 0; i < lgfs2_metadata_size; i++) {
		const struct lgfs2_metadata *m = &lgfs2_metadata[i];
		for (j = 0; j < m->nfields; j++) {
			const struct lgfs2_metafield *f = &m->fields[j];
			if (f->flags & LGFS2_MFF_POINTER) {
				ck_assert(f->points_to != 0);
			} else {
				ck_assert(f->points_to == 0);
			}
		}
	}
}
END_TEST

Suite *suite_meta(void)
{
	Suite *s = suite_create("meta.c");

	TCase *tc_meta = tcase_create("Metadata description checks");
	tcase_add_test(tc_meta, check_metadata_sizes);
	tcase_add_test(tc_meta, check_symtab);
	tcase_add_test(tc_meta, check_ptrs);
	suite_add_tcase(s, tc_meta);

	return s;
}
