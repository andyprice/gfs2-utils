#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
#include <dirent.h>
#include <libintl.h>
#define _(String) gettext(String)

#define __user

#include "gfs2_tool.h"
#include "libgfs2.h"

#define SIZE (65536)

#define SYS_BASE "/sys/fs/gfs2" /* FIXME: Look in /proc/mounts for this */

/**
 * get_tune - print out the current tuneable parameters for a filesystem
 * @argc:
 * @argv:
 *
 */

void
get_tune(int argc, char **argv)
{
	char path[PATH_MAX];
	char *fs;
	DIR *d;
	struct dirent *de;
	double ratio;
	unsigned int num, den;
	struct gfs2_sbd sbd;
	char *value;

	if (optind == argc)
		die( _("Usage: gfs2_tool gettune <mountpoint>\n"));

	sbd.path_name = argv[optind];
	if (check_for_gfs2(&sbd)) {
		if (errno == EINVAL)
			fprintf(stderr, _("Not a valid GFS2 mount point: %s\n"),
					sbd.path_name);
		else
			fprintf(stderr, "%s\n", strerror(errno));
		exit(-1);
	}
	fs = mp2fsname(argv[optind]);
	if (!fs) {
		fprintf(stderr, _("Couldn't find GFS2 filesystem mounted at %s\n"),
				argv[optind]);
		exit(-1);
	}
	memset(path, 0, PATH_MAX);
	snprintf(path, PATH_MAX - 1, "%s/%s/tune", SYS_BASE, fs);

	d = opendir(path);
	if (!d)
		die( _("can't open %s: %s\n"), path, strerror(errno));

	while((de = readdir(d))) {
		if (de->d_name[0] == '.')
			continue;
		snprintf(path, PATH_MAX - 1, "tune/%s", de->d_name);
		if (strcmp(de->d_name, "quota_scale") == 0) {
			value = get_sysfs(fs, "tune/quota_scale");
			if (!value) {
				printf( _("quota_scale = (Not found: %s)\n"),
						strerror(errno));
				continue;
			}
			sscanf(value, "%u %u", &num, &den);
			ratio = (double)num / den;
			printf( _("quota_scale = %.4f   (%u, %u)\n"), ratio, num,
			       den);
		} else
			printf("%s = %s\n", de->d_name, get_sysfs(fs, path));
	}
	closedir(d);
}

/**
 * set_tune - set a tuneable parameter
 * @argc:
 * @argv:
 *
 */

void
set_tune(int argc, char **argv)
{
	char *param, *value;
	char tune_base[SIZE] = "tune/";
	char buf[256];
	char *fs;
	struct gfs2_sbd sbd;

	if (optind == argc)
		die( _("Usage: gfs2_tool settune <mountpoint> <parameter> <value>\n"));
	sbd.path_name = argv[optind++];
	if (optind == argc)
		die( _("Usage: gfs2_tool settune <mountpoint> <parameter> <value>\n"));
	param = argv[optind++];
	if (optind == argc)
		die( _("Usage: gfs2_tool settune <mountpoint> <parameter> <value>\n"));
	value = argv[optind++];

	if (check_for_gfs2(&sbd)) {
		if (errno == EINVAL)
			fprintf(stderr, _("Not a valid GFS2 mount point: %s\n"),
					sbd.path_name);
		else
			fprintf(stderr, "%s\n", strerror(errno));
		exit(-1);
	}
	fs = mp2fsname(sbd.path_name);
	if (!fs) {
		fprintf(stderr, _("Couldn't find GFS2 filesystem mounted at %s\n"),
				sbd.path_name);
		exit(-1);
	}

	if (strcmp(param, "quota_scale") == 0) {
		float s;
		sscanf(value, "%f", &s);
		sprintf(buf, "%u %u", (unsigned int)(s * 10000.0 + 0.5), 10000);
		value = buf;
	}
	if (set_sysfs(fs, strcat(tune_base, param), value)) {
		fprintf(stderr, _("Error writing to sysfs %s tune file: %s\n"),
				param, strerror(errno));
		exit(-1);
	}
}
