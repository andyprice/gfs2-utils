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

START_TEST(check_flag_sym_value)
{
	const struct lgfs2_metadata *di_meta = &lgfs2_metadata[LGFS2_MT_GFS2_DINODE];
	const struct lgfs2_metafield *flags_field = lgfs2_find_mfield_name("di_flags", di_meta);

	ck_assert(lgfs2_flag_sym_value(NULL, flags_field) == 0);
	ck_assert(lgfs2_flag_sym_value("", flags_field) == 0);
	ck_assert(lgfs2_flag_sym_value("invalid_flag", flags_field) == 0);
	ck_assert(lgfs2_flag_sym_value("GFS2_DIF_JDATA", flags_field) == GFS2_DIF_JDATA);
	ck_assert(lgfs2_flag_sym_value("GFS2_DIF_EXHASH", flags_field) == GFS2_DIF_EXHASH);
	ck_assert(lgfs2_flag_sym_value("GFS2_DIF_UNUSED", flags_field) == GFS2_DIF_UNUSED);
	ck_assert(lgfs2_flag_sym_value("GFS2_DIF_EA_INDIRECT", flags_field) == GFS2_DIF_EA_INDIRECT);
	ck_assert(lgfs2_flag_sym_value("GFS2_DIF_DIRECTIO", flags_field) == GFS2_DIF_DIRECTIO);
	ck_assert(lgfs2_flag_sym_value("GFS2_DIF_IMMUTABLE", flags_field) == GFS2_DIF_IMMUTABLE);
	ck_assert(lgfs2_flag_sym_value("GFS2_DIF_APPENDONLY", flags_field) == GFS2_DIF_APPENDONLY);
	ck_assert(lgfs2_flag_sym_value("GFS2_DIF_NOATIME", flags_field) == GFS2_DIF_NOATIME);
	ck_assert(lgfs2_flag_sym_value("GFS2_DIF_SYNC", flags_field) == GFS2_DIF_SYNC);
	ck_assert(lgfs2_flag_sym_value("GFS2_DIF_SYSTEM", flags_field) == GFS2_DIF_SYSTEM);
	ck_assert(lgfs2_flag_sym_value("GFS2_DIF_TRUNC_IN_PROG", flags_field) == GFS2_DIF_TRUNC_IN_PROG);
	ck_assert(lgfs2_flag_sym_value("GFS2_DIF_INHERIT_DIRECTIO", flags_field) == GFS2_DIF_INHERIT_DIRECTIO);
	ck_assert(lgfs2_flag_sym_value("GFS2_DIF_INHERIT_JDATA", flags_field) == GFS2_DIF_INHERIT_JDATA);
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
	tcase_add_test(tc_meta, check_flag_sym_value);
	suite_add_tcase(s, tc_meta);

	return s;
}
