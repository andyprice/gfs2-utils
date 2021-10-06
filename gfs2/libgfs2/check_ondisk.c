#include <check.h>
#include "libgfs2.h"

Suite *suite_ondisk(void);

START_TEST(check_sb_in)
{
	char buf[sizeof(struct gfs2_sb)];
	char namechk[GFS2_LOCKNAME_LEN];
	struct gfs2_sbd sbd;
	char uuidchk[sizeof(sbd.sd_uuid)];

	memset(buf, 0x5a, sizeof(buf));
	memset(namechk, 0x5a, GFS2_LOCKNAME_LEN);
	memset(uuidchk, 0x5a, sizeof(sbd.sd_uuid));
	memset(&sbd, 0, sizeof(sbd));

	lgfs2_sb_in(&sbd, buf);
	/* Compare each field individually to find exactly where any bugs are */
	ck_assert(sbd.sd_fs_format == 0x5a5a5a5a);
	ck_assert(sbd.sd_multihost_format == 0x5a5a5a5a);
	ck_assert(sbd.sd_flags == 0x5a5a5a5a);
	ck_assert(sbd.sd_bsize == 0x5a5a5a5a);
	ck_assert(sbd.sd_bsize_shift == 0x5a5a5a5a);
	ck_assert(sbd.sd_seg_size == 0x5a5a5a5a);
	ck_assert(sbd.sd_meta_dir.in_formal_ino == 0x5a5a5a5a5a5a5a5a);
	ck_assert(sbd.sd_meta_dir.in_addr == 0x5a5a5a5a5a5a5a5a);
	ck_assert(sbd.sd_root_dir.in_formal_ino == 0x5a5a5a5a5a5a5a5a);
	ck_assert(sbd.sd_root_dir.in_addr == 0x5a5a5a5a5a5a5a5a);
	ck_assert(memcmp(sbd.sd_lockproto, namechk, GFS2_LOCKNAME_LEN) == 0);
	ck_assert(memcmp(sbd.sd_locktable, namechk, GFS2_LOCKNAME_LEN) == 0);
	ck_assert(sbd.sd_jindex_di.in_formal_ino == 0x5a5a5a5a5a5a5a5a);
	ck_assert(sbd.sd_jindex_di.in_addr == 0x5a5a5a5a5a5a5a5a);
	ck_assert(sbd.sd_rindex_di.in_formal_ino == 0x5a5a5a5a5a5a5a5a);
	ck_assert(sbd.sd_rindex_di.in_addr == 0x5a5a5a5a5a5a5a5a);
	ck_assert(sbd.sd_quota_di.in_formal_ino == 0x5a5a5a5a5a5a5a5a);
	ck_assert(sbd.sd_quota_di.in_addr == 0x5a5a5a5a5a5a5a5a);
	ck_assert(sbd.sd_license_di.in_formal_ino == 0x5a5a5a5a5a5a5a5a);
	ck_assert(sbd.sd_license_di.in_addr == 0x5a5a5a5a5a5a5a5a);
	ck_assert(memcmp(sbd.sd_uuid, uuidchk, sizeof(sbd.sd_uuid)) == 0);
}
END_TEST

START_TEST(check_sb1_out)
{
	char namechk[GFS2_LOCKNAME_LEN];
	char buf[sizeof(struct gfs_sb)];
	struct gfs2_sbd sbd;
	struct gfs_sb *sb;

	memset(namechk, 0x5a, GFS2_LOCKNAME_LEN);

	/* 1. If only the gfs1 fields are set, the sb must be filled */
	memset(buf, 0, sizeof(buf));
	memset(&sbd, 0, sizeof(sbd));

	sbd.sd_fs_format = 0x5a5a5a51;
	sbd.sd_multihost_format = 0x5a5a5a52;
	sbd.sd_flags = 0x5a5a5a53;
	sbd.sd_bsize = 0x5a5a5a54;
	sbd.sd_bsize_shift = 0x5a5a5a55;
	sbd.sd_seg_size = 0x5a5a5a56;
	sbd.sd_jindex_di.in_formal_ino = 0x5a5a5a5a5a5a5a57;
	sbd.sd_jindex_di.in_addr = 0x5a5a5a5a5a5a5a58;
	sbd.sd_rindex_di.in_formal_ino = 0x5a5a5a5a5a5a5a59;
	sbd.sd_rindex_di.in_addr = 0x5a5a5a5a5a5a5a5a;
	sbd.sd_root_dir.in_formal_ino = 0x5a5a5a5a5a5a5a5b;
	sbd.sd_root_dir.in_addr = 0x5a5a5a5a5a5a5a5c;
	memset(sbd.sd_lockproto, 0x5a, sizeof(sbd.sd_lockproto));
	memset(sbd.sd_locktable, 0x5a, sizeof(sbd.sd_locktable));
	sbd.sd_quota_di.in_formal_ino = 0x5a5a5a5a5a5a5a5d;
	sbd.sd_quota_di.in_addr = 0x5a5a5a5a5a5a5a5e;
	sbd.sd_license_di.in_formal_ino = 0x5a5a5a5a5a5a5a5f;
	sbd.sd_license_di.in_addr = 0x5a5a5a5a5a5a5a50;

	lgfs2_sb_out(&sbd, buf);

	sb = (struct gfs_sb *)buf;
	ck_assert(be32_to_cpu(sb->sb_fs_format) == 0x5a5a5a51);
	ck_assert(be32_to_cpu(sb->sb_multihost_format) == 0x5a5a5a52);
	ck_assert(be32_to_cpu(sb->sb_flags) == 0x5a5a5a53);
	ck_assert(be32_to_cpu(sb->sb_bsize) == 0x5a5a5a54);
	ck_assert(be32_to_cpu(sb->sb_bsize_shift) == 0x5a5a5a55);
	ck_assert(be32_to_cpu(sb->sb_seg_size) == 0x5a5a5a56);
	ck_assert(be64_to_cpu(sb->sb_jindex_di.no_formal_ino) == 0x5a5a5a5a5a5a5a57);
	ck_assert(be64_to_cpu(sb->sb_jindex_di.no_addr) == 0x5a5a5a5a5a5a5a58);
	ck_assert(be64_to_cpu(sb->sb_rindex_di.no_formal_ino) == 0x5a5a5a5a5a5a5a59);
	ck_assert(be64_to_cpu(sb->sb_rindex_di.no_addr) == 0x5a5a5a5a5a5a5a5a);
	ck_assert(be64_to_cpu(sb->sb_root_di.no_formal_ino) == 0x5a5a5a5a5a5a5a5b);
	ck_assert(be64_to_cpu(sb->sb_root_di.no_addr) == 0x5a5a5a5a5a5a5a5c);
	ck_assert(memcmp(sb->sb_lockproto, namechk, GFS2_LOCKNAME_LEN) == 0);
	ck_assert(memcmp(sb->sb_locktable, namechk, GFS2_LOCKNAME_LEN) == 0);
	ck_assert(be64_to_cpu(sb->sb_quota_di.no_formal_ino) == 0x5a5a5a5a5a5a5a5d);
	ck_assert(be64_to_cpu(sb->sb_quota_di.no_addr) == 0x5a5a5a5a5a5a5a5e);
	ck_assert(be64_to_cpu(sb->sb_license_di.no_formal_ino) == 0x5a5a5a5a5a5a5a5f);
	ck_assert(be64_to_cpu(sb->sb_license_di.no_addr) == 0x5a5a5a5a5a5a5a50);
}
END_TEST

START_TEST(check_sb2_out)
{
	char buf[sizeof(struct gfs2_sb)];
	char namechk[GFS2_LOCKNAME_LEN];
	struct gfs2_sbd sbd;
	struct gfs2_sb *sb;
	char uuidchk[sizeof(sbd.sd_uuid)];

	memset(namechk, 0x5a, GFS2_LOCKNAME_LEN);
	memset(uuidchk, 0x5a, sizeof(sbd.sd_uuid));

	/* 2. If only the gfs2 fields are set, the sb must be filled */
	memset(buf, 0, sizeof(buf));
	memset(&sbd, 0, sizeof(sbd));

	sbd.sd_fs_format = 0x5a5a5a50;
	sbd.sd_multihost_format = 0x5a5a5a51;
	sbd.sd_bsize = 0x5a5a5a52;
	sbd.sd_bsize_shift = 0x5a5a5a53;
	sbd.sd_meta_dir.in_formal_ino = 0x5a5a5a5a5a5a5a54;
	sbd.sd_meta_dir.in_addr = 0x5a5a5a5a5a5a5a55;
	sbd.sd_root_dir.in_formal_ino = 0x5a5a5a5a5a5a5a56;
	sbd.sd_root_dir.in_addr = 0x5a5a5a5a5a5a5a57;
	memset(sbd.sd_lockproto, 0x5a, sizeof(sbd.sd_lockproto));
	memset(sbd.sd_locktable, 0x5a, sizeof(sbd.sd_locktable));
	memset(sbd.sd_uuid, 0x5a, sizeof(sbd.sd_uuid));

	lgfs2_sb_out(&sbd, buf);

	sb = (struct gfs2_sb *)buf;
	ck_assert(be32_to_cpu(sb->sb_fs_format) == 0x5a5a5a50);
	ck_assert(be32_to_cpu(sb->sb_multihost_format) == 0x5a5a5a51);
	ck_assert(be32_to_cpu(sb->sb_bsize) == 0x5a5a5a52);
	ck_assert(be32_to_cpu(sb->sb_bsize_shift) == 0x5a5a5a53);
	ck_assert(be64_to_cpu(sb->sb_master_dir.no_formal_ino) == 0x5a5a5a5a5a5a5a54);
	ck_assert(be64_to_cpu(sb->sb_master_dir.no_addr) == 0x5a5a5a5a5a5a5a55);
	ck_assert(be64_to_cpu(sb->sb_root_dir.no_formal_ino) == 0x5a5a5a5a5a5a5a56);
	ck_assert(be64_to_cpu(sb->sb_root_dir.no_addr) == 0x5a5a5a5a5a5a5a57);
	ck_assert(memcmp(sb->sb_lockproto, namechk, GFS2_LOCKNAME_LEN) == 0);
	ck_assert(memcmp(sb->sb_locktable, namechk, GFS2_LOCKNAME_LEN) == 0);
	ck_assert(memcmp(sb->sb_uuid, uuidchk, sizeof(sb->sb_uuid)) == 0);
}
END_TEST

Suite *suite_ondisk(void)
{
	Suite *s = suite_create("ondisk.c");

	TCase *tc_meta = tcase_create("On-disk structure parsing checks");
	tcase_add_test(tc_meta, check_sb_in);
	tcase_add_test(tc_meta, check_sb1_out);
	tcase_add_test(tc_meta, check_sb2_out);
	suite_add_tcase(s, tc_meta);

	return s;
}
