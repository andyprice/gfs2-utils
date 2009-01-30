#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>
#include <inttypes.h>
#include <sys/types.h>
#include <linux/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <limits.h>
#include <errno.h>
#include <ctype.h>

#include <linux/gfs2_ondisk.h>

#include "gfs2_tool.h"
#include "libgfs2.h"

void print_it(const char *label, const char *fmt, const char *fmt2, ...)
{
	va_list args;

	va_start(args, fmt2);
	printf("%s = ", label);
	vprintf(fmt, args);
	printf("\n");
	va_end(args);
}

/**
 * str_to_hexchar - convert a string consisting of two isxdigits back to hex.
 * Returns: the hex character
 */
int str_to_hexchar(const char *estring)
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

/**
 * do_sb - examine/modify a unmounted FS' superblock
 * @argc:
 * @argv:
 *
 */

void
do_sb(int argc, char **argv)
{
	char *device, *field, *newval = NULL;
	int fd;
	unsigned char buf[GFS2_BASIC_BLOCK], input[256];
	struct gfs2_sb sb;

	if (optind == argc)
		die("Usage: gfs2_tool sb <device> <field> [newval]\n");

	device = argv[optind++];

	if (optind == argc)
		die("Usage: gfs2_tool sb <device> <field> [newval]\n");

	field = argv[optind++];

	if (optind < argc) {
		if (strcmp(field, "all") == 0)
			die("can't specify new value for \"all\"\n");
		newval = argv[optind++];
	}


	fd = open(device, (newval) ? O_RDWR : O_RDONLY);
	if (fd < 0)
		die("can't open %s: %s\n", device, strerror(errno));

	if (newval && !override) {
		printf("You shouldn't change any of these values if the filesystem is mounted.\n");
		printf("\nAre you sure? [y/n] ");
		if(!fgets((char*)input, 255, stdin))
			die("unable to read from stdin\n");

		if (input[0] != 'y')
			die("aborted\n");

		printf("\n");
	}

	if (lseek(fd, GFS2_SB_ADDR * GFS2_BASIC_BLOCK, SEEK_SET) !=
	    GFS2_SB_ADDR * GFS2_BASIC_BLOCK) {
		fprintf(stderr, "bad seek: %s from %s:%d: superblock\n",
			strerror(errno), __FUNCTION__, __LINE__);
		exit(-1);
	}
	if (read(fd, buf, GFS2_BASIC_BLOCK) != GFS2_BASIC_BLOCK) {
		fprintf(stderr, "bad read: %s from %s:%d: superblock\n",
			strerror(errno), __FUNCTION__, __LINE__);
		exit(-1);
	}

	gfs2_sb_in(&sb, (char*) buf);

	if (sb.sb_header.mh_magic != GFS2_MAGIC ||
	    sb.sb_header.mh_type != GFS2_METATYPE_SB)
		die("there isn't a GFS2 filesystem on %s\n", device);

	if (strcmp(field, "proto") == 0) {
		printf("current lock protocol name = \"%s\"\n",
		       sb.sb_lockproto);

		if (newval) {
			if (strlen(newval) >= GFS2_LOCKNAME_LEN)
				die("new lockproto name is too long\n");
			strcpy(sb.sb_lockproto, newval);
			printf("new lock protocol name = \"%s\"\n",
			       sb.sb_lockproto);
		}
	} else if (strcmp(field, "table") == 0) {
		printf("current lock table name = \"%s\"\n",
		       sb.sb_locktable);

		if (newval) {
			if (strlen(newval) >= GFS2_LOCKNAME_LEN)
				die("new locktable name is too long\n");
			strcpy(sb.sb_locktable, newval);
			printf("new lock table name = \"%s\"\n",
			       sb.sb_locktable);
		}
	} else if (strcmp(field, "ondisk") == 0) {
		printf("current ondisk format = %u\n",
		       sb.sb_fs_format);

		if (newval) {
			sb.sb_fs_format = atoi(newval);
			printf("new ondisk format = %u\n",
			       sb.sb_fs_format);
		}
	} else if (strcmp(field, "multihost") == 0) {
		printf("current multihost format = %u\n",
		       sb.sb_multihost_format);

		if (newval) {
			sb.sb_multihost_format = atoi(newval);
			printf("new multihost format = %u\n",
			       sb.sb_multihost_format);
		}
#ifdef GFS2_HAS_UUID
	} else if (strcmp(field, "uuid") == 0) {
		printf("current uuid = %s\n", str_uuid(sb.sb_uuid));

		if (newval) {
			int i;
			unsigned char uuid[16], *cp;

			if (strlen(newval) != 36)
				die("uuid %s is the wrong length; must be 36 "
				    "hex characters long.\n", newval);
			cp = uuid;
			for (i = 0; i < 36; i++) {
				if ((i == 8) || (i == 13) ||
				    (i == 18) || (i == 23)) {
					if (newval[i] == '-')
						continue;
					die("uuid %s has an invalid format.",
					    newval);
				}
				if (!isxdigit(newval[i]))
					die("uuid %s has an invalid hex "
					    "digit '%c' at offset %d.\n",
					    newval, newval[i], i + 1);
				*cp = str_to_hexchar(&newval[i++]);
				cp++;
			}
			memcpy(sb.sb_uuid, uuid, 16);
			printf("new uuid = %s\n", str_uuid(sb.sb_uuid));
		}
#endif
	} else if (strcmp(field, "all") == 0) {
		gfs2_sb_print(&sb);
		newval = FALSE;
	} else
		die("unknown field %s\n", field);

	if (newval) {
		gfs2_sb_out(&sb,(char*) buf);

		if (lseek(fd, GFS2_SB_ADDR * GFS2_BASIC_BLOCK, SEEK_SET) !=
		    GFS2_SB_ADDR * GFS2_BASIC_BLOCK) {
			fprintf(stderr, "bad seek: %s from %s:%d: superblock\n",
				strerror(errno), __FUNCTION__, __LINE__);
			exit(-1);
		}
		if (write(fd, buf, GFS2_BASIC_BLOCK) != GFS2_BASIC_BLOCK) {
			fprintf(stderr, "write error: %s from %s:%d: "
				"superblock\n", strerror(errno),
				__FUNCTION__, __LINE__);
			exit(-1);
		}

		fsync(fd);

		printf("Done\n");
	}

	close(fd);
}
