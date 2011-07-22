#include <stdio.h>
#include <stdlib.h>
#include <sysexits.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <ctype.h>
#include <string.h>
#include <libintl.h>
#define _(String) gettext(String)
#include <linux_endian.h>
#include <linux/gfs2_ondisk.h>
#include "tunegfs2.h"

static int str_to_hexchar(const char *estring)
{
	int ch = 0;

	if (isdigit(*estring))
		ch = (*estring - '0') * 0x10;
	else if (*estring >= 'a' && *estring <= 'f')
		ch = (*estring - 'a' + 0x0a) * 0x10;
	else if (*estring >= 'A' && *estring <= 'F')
		ch = (*estring - 'A' + 0x0a) * 0x10;

	estring++;
	if (isdigit(*estring))
		ch += (*estring - '0');
	else if (*estring >= 'a' && *estring <= 'f')
		ch += (*estring - 'a' + 0x0a);
	else if (*estring >= 'A' && *estring <= 'F')
		ch += (*estring - 'A' + 0x0a);
	return ch;
}



static const char *uuid2str(const unsigned char *uuid)
{
	static char str[64];
	char *ch;
	int i;

	memset(str, 0, 64);
	ch = str;
	for (i = 0; i < 16; i++) {
		sprintf(ch, "%02x", uuid[i]);
		ch += 2;
		if ((i == 3) || (i == 5) || (i == 7) || (i == 9)) {
			*ch = '-';
			ch++;
		}
	}
	return str;
}

static int str2uuid(const char *newval, char *uuid)
{
	char *cp;
	int i;

	if (strlen(newval) != 36) {
		fprintf(stderr, _("Invalid UUID specified.\n"));
		return EX_DATAERR;
	}

	cp = uuid;
	for (i = 0; i < 36; i++) {
		if ((i == 8) || (i == 13) ||
				(i == 18) || (i == 23)) {
			if (newval[i] == '-')
				continue;
			fprintf(stderr, _("uuid %s has an invalid format."),
					newval);
			return EX_DATAERR;
		}
		if (!isxdigit(newval[i])) {
			fprintf(stderr, _("uuid %s has an invalid hex "
						"digit '%c' at offset %d.\n"),
					newval, newval[i], i + 1);
			return EX_DATAERR;
		}
		*cp = str_to_hexchar(&newval[i++]);
		cp++;
	}
	return 0;
}

int read_super(struct tunegfs2 *tfs)
{
	void *block;
	int n;
       	tfs->sb_start = GFS2_SB_ADDR << GFS2_BASIC_BLOCK_SHIFT;
	block = malloc(sizeof(char) * GFS2_DEFAULT_BSIZE);
	n = pread(tfs->fd, block, GFS2_DEFAULT_BSIZE, tfs->sb_start);
	if (n < 0) {
		perror("read_super: pread");
		return EX_IOERR;
	}
	tfs->sb = block;
	if (be32_to_cpu(tfs->sb->sb_header.mh_magic) != GFS2_MAGIC) {
		fprintf(stderr, _("Not a GFS/GFS2 device\n"));
		return EX_IOERR;
	}
	/* Ensure that table and proto are NULL terminated */
	tfs->sb->sb_lockproto[GFS2_LOCKNAME_LEN - 1] = '\0';
	tfs->sb->sb_locktable[GFS2_LOCKNAME_LEN - 1] = '\0';
	return 0;
}

static int is_gfs2(const struct tunegfs2 *tfs)
{
	return be32_to_cpu(tfs->sb->sb_fs_format) == GFS2_FORMAT_FS;
}

int print_super(const struct tunegfs2 *tfs)
{
	printf(_("Filesystem volume name: %s\n"), tfs->sb->sb_locktable);
	if (is_gfs2(tfs))
		printf(_("Filesystem UUID: %s\n"), uuid2str(tfs->sb->sb_uuid));
	printf( _("Filesystem magic number: 0x%X\n"), be32_to_cpu(tfs->sb->sb_header.mh_magic));
	printf(_("Block size: %d\n"), be32_to_cpu(tfs->sb->sb_bsize));
	printf(_("Block shift: %d\n"), be32_to_cpu(tfs->sb->sb_bsize_shift));
	printf(_("Root inode: %llu\n"), (unsigned long long)be64_to_cpu(tfs->sb->sb_root_dir.no_addr));
	if (is_gfs2(tfs))
		printf(_("Master inode: %llu\n"), (unsigned long long)be64_to_cpu(tfs->sb->sb_master_dir.no_addr));
	printf(_("Lock Protocol: %s\n"), tfs->sb->sb_lockproto);
	printf(_("Lock table: %s\n"), tfs->sb->sb_locktable);

	return 0;
}

int write_super(const struct tunegfs2 *tfs)
{
	int n;
	n = pwrite(tfs->fd, tfs->sb, GFS2_DEFAULT_BSIZE, tfs->sb_start);
	if (n < 0) {
		perror("write_super: pwrite");
		return EX_IOERR;
	}
	return 0;
}

int change_uuid(struct tunegfs2 *tfs, const char *str)
{
	char uuid[16];
	int status = 0;
	if (be32_to_cpu(tfs->sb->sb_header.mh_magic) != GFS2_MAGIC) {
		fprintf(stderr, _("UUID can be changed for a GFS2"));
		fprintf(stderr, _(" device only\n"));
		return EX_IOERR;
	}
	status = str2uuid(str, uuid);
	if (!status)
		memcpy(tfs->sb->sb_uuid, uuid, 16);
	return status;
}

int change_lockproto(struct tunegfs2 *tfs, const char *lockproto)
{
	int l = strlen(lockproto);

	if (l >= GFS2_LOCKNAME_LEN) {
		fprintf(stderr, _("Lock protocol name too long\n"));
		return EX_DATAERR;
	}

	if (strncmp(lockproto, "lock_dlm", 8) &&
	    strncmp(lockproto, "lock_nolock", 11)) {
		fprintf(stderr, _("Incorrect lock protocol specified\n"));
		return EX_DATAERR;
	}
	memset(tfs->sb->sb_lockproto, '\0', GFS2_LOCKNAME_LEN);
	strncpy(tfs->sb->sb_lockproto, lockproto, l);
	return 0;
}

int change_locktable(struct tunegfs2 *tfs, const char *locktable)
{
	if (strlen(locktable) >= GFS2_LOCKNAME_LEN) {
		fprintf(stderr, _("Lock table name too long\n"));
		return EX_DATAERR;
	}

	if (strcmp(tfs->sb->sb_lockproto, "lock_dlm") == 0) {
		char *fsname = strchr(locktable, ':');
		if (fsname == NULL) {
			fprintf(stderr, _("locktable error: mising colon in the locktable\n"));
			return EX_DATAERR;
		}
		if (strlen(++fsname) > 16) {
			fprintf(stderr, _("locktable error: fsname too long\n"));
			return EX_DATAERR;
		}
		if (strchr(fsname, ':')) {
			fprintf(stderr, _("locktable error: more than one colon present\n"));
			return EX_DATAERR;
		}
	}

	strcpy(tfs->sb->sb_locktable, locktable);
	return 0;
}

