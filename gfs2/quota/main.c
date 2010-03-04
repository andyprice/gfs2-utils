#include "clusterautoconfig.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <ctype.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <time.h>
#include <limits.h>
#include <sys/param.h>
#include <sys/mount.h>
#include <stdint.h>
#include <inttypes.h>

#include <linux/types.h>
#include <linux/fiemap.h>
#include "gfs2_quota.h"

#define __user

#include "copyright.cf"


/*  Constants  */

#define OPTION_STRING ("bdf:g:hkl:mnsu:V")
#define FS_IOC_FIEMAP                   _IOWR('f', 11, struct fiemap)

/**
 * This function is for libgfs2's sake.
 */
void print_it(const char *label, const char *fmt, const char *fmt2, ...)
{
        va_list args;

        va_start(args, fmt2);
        printf("%s: ", label);
        vprintf(fmt, args);
        va_end(args);
}

/**
 * print_usage - print usage info to the user
 * @prog_name: The name of this program
 */

static void
print_usage(const char *prog_name)
{
	printf("Usage:\n");
	printf("\n");
	printf("%s <list|sync|get|limit|warn|check|init> [options]\n",
	       prog_name);
	printf("\n");
	printf("Actions:\n");
	printf("  list             list the whole quota file\n");
	printf("  sync             sync out unsynced quotas\n");
	printf("  get              get quota values for an ID\n");
	printf("  limit            set a quota limit value for an ID\n");
	printf("  warn             set a quota warning value for an ID\n");
	printf("  check            check the quota file\n");
	printf("  init             initialize the quota file\n");
	printf("  reset            reset the quota file\n");
	printf("\n");
	printf("Options:\n");
	printf("  -b               sizes are in FS blocks\n");
	printf("  -f <directory>   the filesystem to work on\n");
	printf("  -g <gid>         get/set a group ID\n");
	printf("  -h               Print this help, then exit\n");
	printf("  -k               sizes are in KB\n");
	printf("  -l <size>        the new limit or warn value\n");
	printf("  -m               sizes are in MB\n");
	printf("  -n               print out UID/GID numbers instead of names\n");
	printf("  -s               sizes are in 512-byte blocks\n");
	printf("  -u <uid>         get/set a user ID\n");
	printf("  -V               Print program version information, then exit\n");
}

/**
 * decode_arguments - parse command line arguments
 * @argc: well, it's argc...
 * @argv: well, it's argv...
 * @comline: the structure filled in with the parsed arguments
 *
 * Function description
 *
 * Returns: what is returned
 */

static void
decode_arguments(int argc, char *argv[], commandline_t *comline)
{
	int cont = TRUE;
	int optchar;

	while (cont) {
		optchar = getopt(argc, argv, OPTION_STRING);

		switch (optchar) {
		case 'u':
			comline->id_type = GQ_ID_USER;
			comline->id = name_to_id(TRUE, optarg, comline->numbers);
			break;

		case 'g':
			comline->id_type = GQ_ID_GROUP;
			comline->id = name_to_id(FALSE, optarg, comline->numbers);
			break;

		case 'l':
			if (!isdigit(*optarg))
				die("argument to -l must be a number\n");
			sscanf(optarg, "%"SCNu64, &comline->new_value);
			comline->new_value_set = TRUE;
			break;

		case 'f':
			if (!realpath(optarg, comline->filesystem))
				die("can't find %s: %s\n", optarg,
				    strerror(errno));
			break;

		case 'm':
			comline->units = GQ_UNITS_MEGABYTE;
			break;

		case 'k':
			comline->units = GQ_UNITS_KILOBYTE;
			break;

		case 'b':
			comline->units = GQ_UNITS_FSBLOCK;
			break;

		case 's':
			comline->units = GQ_UNITS_BASICBLOCK;
			break;

		case 'n':
			comline->numbers = TRUE;
			break;

		case 'V':
			printf("gfs2_quota %s (built %s %s)\n", VERSION,
			       __DATE__, __TIME__);
			printf("%s\n", REDHAT_COPYRIGHT);
			exit(EXIT_SUCCESS);
			break;

		case 'h':
			print_usage(argv[0]);
			exit(EXIT_SUCCESS);
			break;

		case ':':
		case '?':
			fprintf(stderr, "Please use '-h' for usage.\n");
			exit(EXIT_FAILURE);
			break;

		case EOF:
			cont = FALSE;
			break;

		default:
			die("unknown option: %c\n", optchar);
			break;
		};
	}

	while (optind < argc) {
		if (strcmp(argv[optind], "list") == 0 ||
		    strcmp(argv[optind], "dump") == 0) {
			if (comline->operation)
				die("can't specify two operations\n");
			comline->operation = GQ_OP_LIST;
		} else if (strcmp(argv[optind], "sync") == 0) {
			if (comline->operation)
				die("can't specify two operations\n");
			comline->operation = GQ_OP_SYNC;
		} else if (strcmp(argv[optind], "get") == 0) {
			if (comline->operation)
				die("can't specify two operations\n");
			comline->operation = GQ_OP_GET;
		} else if (strcmp(argv[optind], "limit") == 0) {
			if (comline->operation)
				die("can't specify two operations\n");
			comline->operation = GQ_OP_LIMIT;
		} else if (strcmp(argv[optind], "warn") == 0) {
			if (comline->operation)
				die("can't specify two operations\n");
			comline->operation = GQ_OP_WARN;
		} else if (strcmp(argv[optind], "check") == 0) {
			if (comline->operation)
				die("can't specify two operations\n");
			comline->operation = GQ_OP_CHECK;
		} else if (strcmp(argv[optind], "init") == 0) {
			if (comline->operation)
				die("can't specify two operations\n");
			comline->operation = GQ_OP_INIT;
		} else if (strcmp(argv[optind], "reset") == 0) {
			if (comline->operation)
				die("can't specify two operations\n");
			comline->operation = GQ_OP_RESET;
		} else
			die("unknown option %s\n", argv[optind]);

		optind++;
	}
}

/**
 * print_quota - Print out a quota entry
 * @comline: the struct containing the parsed command line arguments
 * @user: TRUE if this is a user quota, FALSE if it's a group quota
 * @id: the ID
 * @q: the quota value
 * @sb: the superblock of the filesystem this quota belongs to
 *
 */

static void
print_quota(commandline_t *comline,
	    int user, uint32_t id,
	    struct gfs2_quota *q,
	    struct gfs2_sb *sb)
{
	printf("%-5s %10s:  ", (user) ? "user" : "group",
	       id_to_name(user, id, comline->numbers));

	switch (comline->units) {
	case GQ_UNITS_MEGABYTE:
		printf("limit: %-10.1f warn: %-10.1f value: %-10.1f\n",
		       (double) q->qu_limit * sb->sb_bsize / 1048576,
		       (double) q->qu_warn * sb->sb_bsize / 1048576,
		       (double) q->qu_value * sb->sb_bsize / 1048576);
		break;

	case GQ_UNITS_KILOBYTE:
		if (sb->sb_bsize == 512)
			printf("limit: %-10llu warn: %-10lluvalue: %-10llu\n",
			       (unsigned long long)q->qu_limit / 2,
			       (unsigned long long)q->qu_warn / 2,
			       (unsigned long long)q->qu_value / 2);
		else
			printf("limit: %-10llu warn: %-10lluvalue: %-10llu\n",
			       (unsigned long long)
			       q->qu_limit << (sb->sb_bsize_shift - 10),
			       (unsigned long long)
			       q->qu_warn << (sb->sb_bsize_shift - 10),
			       (unsigned long long)
			       q->qu_value << (sb->sb_bsize_shift - 10));
		break;

	case GQ_UNITS_FSBLOCK:
		printf("limit: %-10llu warn: %-10llu value: %-10llu\n",
		       (unsigned long long)q->qu_limit,
		       (unsigned long long)q->qu_warn,
		       (unsigned long long)q->qu_value);
		break;

	case GQ_UNITS_BASICBLOCK:
		printf("limit: %-10llu warn: %-10llu value: %-10llu\n",
		       (unsigned long long)
		       q->qu_limit << (sb->sb_bsize_shift - 9),
		       (unsigned long long)
		       q->qu_warn << (sb->sb_bsize_shift - 9),
		       (unsigned long long)
		       q->qu_value << (sb->sb_bsize_shift - 9));
		break;

	default:
		die("bad units\n");
		break;
	}
}

void 
read_superblock(struct gfs2_sb *sb, struct gfs2_sbd *sdp)
{
	int fd;
	char buf[PATH_MAX];
	struct gfs2_buffer_head dummy_bh;

	dummy_bh.b_data = buf;
	fd = open(sdp->device_name, O_RDONLY);
	if (fd < 0) {
		die("Could not open the block device %s: %s\n",
			sdp->device_name, strerror(errno));
	}
	if (lseek(fd, 0x10 * 4096, SEEK_SET) != 0x10 * 4096) {
		fprintf(stderr, "bad seek: %s from %s:%d: "
			"superblock\n",
			strerror(errno), __FUNCTION__, __LINE__);

		exit(-1);
	}
	if (read(fd, buf, PATH_MAX) != PATH_MAX) {
		fprintf(stderr, "bad read: %s from %s:%d: superblock\n",
			strerror(errno), __FUNCTION__, __LINE__);
		exit(-1);
	}
	gfs2_sb_in(sb, &dummy_bh);

	close(fd);
}

void
read_quota_internal(int fd, uint32_t id, int id_type, struct gfs2_quota *q)
{
	/* seek to the appropriate offset in the quota file and read the 
	   quota info */
	uint64_t offset;
	char buf[256];
	int error;
	if (id_type == GQ_ID_USER)
		offset = (2 * (uint64_t)id) * sizeof(struct gfs2_quota);
	else
		offset = (2 * (uint64_t)id + 1) * sizeof(struct gfs2_quota);
	lseek(fd, offset, SEEK_SET);
	error = read(fd, buf, sizeof(struct gfs2_quota));
	if (error < 0)
		die("failed to read from quota file: %s\n", strerror(errno));
	if (error != sizeof(struct gfs2_quota))
		die("Couldn't read %lu bytes from quota file at offset %llu\n",
		    (unsigned long)sizeof(struct gfs2_quota),
		    (unsigned long long)offset);
	gfs2_quota_in(q, buf);
}

static inline void
write_quota_internal(int fd, uint32_t id, int id_type, struct gfs2_quota *q)
{
	/* seek to the appropriate offset in the quota file and read the
	   quota info */
	uint64_t offset;
	char buf[256];
	int error;
	if (id_type == GQ_ID_USER)
		offset = (2 * (uint64_t)id) * sizeof(struct gfs2_quota);
	else
		offset = (2 * (uint64_t)id + 1) * sizeof(struct gfs2_quota);
	lseek(fd, offset, SEEK_SET);
	gfs2_quota_out(q, buf);
	error = write(fd, buf, sizeof(struct gfs2_quota));
	if (error != sizeof(struct gfs2_quota))
		die("failed to write to quota file: %s\n", strerror(errno));
}

/**
 * do_reset - Reset all the quota data for a filesystem
 * @comline: the struct containing the parsed command line arguments
 */

static void
do_reset(struct gfs2_sbd *sdp, commandline_t *comline)
{
	int fd;
	char quota_file[BUF_SIZE], c;
	struct gfs2_quota q;

	if (!*comline->filesystem)
		die("need a filesystem to work on\n");

	printf("This operation will permanently erase all quota information.\n"
	       "You will have to re-assign all quota limit/warn values.\n"
	       "Proceed [y/N]? ");
	c = getchar();
	if (c != 'y' && c != 'Y')
		return;

	strcpy(sdp->path_name, comline->filesystem);
	if (check_for_gfs2(sdp)) {
		if (errno == EINVAL)
			fprintf(stderr, "Not a valid GFS2 mount point: %s\n",
					sdp->path_name);
		else
			fprintf(stderr, "%s\n", strerror(errno));
		exit(-1);
	}
	read_superblock(&sdp->sd_sb, sdp);
	if (mount_gfs2_meta(sdp)) {
		fprintf(stderr, "Error mounting GFS2 metafs: %s\n",
			strerror(errno));
		exit(-1);
	}

	strcpy(quota_file, sdp->metafs_path);
	strcat(quota_file, "/quota");

	fd = open(quota_file, O_RDWR);
	if (fd < 0) {
		close(sdp->metafs_fd);
		cleanup_metafs(sdp);
		die("can't open file %s: %s\n", quota_file,
		    strerror(errno));
	}

	read_quota_internal(fd, 0, GQ_ID_USER, &q);
	write_quota_internal(fd, 0, GQ_ID_USER, &q);

	read_quota_internal(fd, 0, GQ_ID_GROUP, &q);
	write_quota_internal(fd, 0, GQ_ID_GROUP, &q);

	/* truncate the quota file such that only the first
	 * two quotas(uid=0 and gid=0) remain.
	 */
	if (ftruncate(fd, (sizeof(struct gfs2_quota)) * 2))
		die("couldn't truncate quota file %s\n", strerror(errno));
	
	close(fd);
	close(sdp->metafs_fd);
	cleanup_metafs(sdp);
}

/**
 * do_list - List all the quota data for a filesystem
 * @comline: the struct containing the parsed command line arguments
 *
 */

static void 
do_list(struct gfs2_sbd *sdp, commandline_t *comline)
{
	int fd;
	struct gfs2_quota q;
	char buf[sizeof(struct gfs2_quota)];
	uint64_t offset;
	uint32_t id, startid;
	int pass = 0;
	int error = 0;
	char quota_file[BUF_SIZE];
	uint64_t quota_file_size = 0;
	struct fiemap fmap = { 0, }, *fmap2;
	struct stat statbuf;
	
	if (!*comline->filesystem)
		die("need a filesystem to work on\n");

	strcpy(sdp->path_name, comline->filesystem);
	if (check_for_gfs2(sdp)) {
		if (errno == EINVAL)
			fprintf(stderr, "Not a valid GFS2 mount point: %s\n",
					sdp->path_name);
		else
			fprintf(stderr, "%s\n", strerror(errno));
		exit(-1);
	}
	read_superblock(&sdp->sd_sb, sdp);
	if (mount_gfs2_meta(sdp)) {
		fprintf(stderr, "Error mounting GFS2 metafs: %s\n",
			strerror(errno));
		exit(-1);
	}

	strcpy(quota_file, sdp->metafs_path);
	strcat(quota_file, "/quota");

	fd = open(quota_file, O_RDONLY);
	if (fd < 0) {
		close(sdp->metafs_fd);
		cleanup_metafs(sdp);
		die("can't open file %s: %s\n", quota_file,
		    strerror(errno));
	}
	if (fstat(fd, &statbuf) < 0) {
		close(fd);
		close(sdp->metafs_fd);
		cleanup_metafs(sdp);
		die("can't stat file %s: %s\n", quota_file,
		    strerror(errno));
	}
	quota_file_size = statbuf.st_size;
	/* First find the number of extents in the quota file */
	fmap.fm_flags = 0;
	fmap.fm_start = 0;
	fmap.fm_length = (~0ULL);
	error = ioctl(fd, FS_IOC_FIEMAP, &fmap);
	if (error == -1) {
		fprintf(stderr, "fiemap error (%d): %s\n", errno, strerror(errno));
		goto out;
	}
	fmap2 = malloc(sizeof(struct fiemap) + 
		       fmap.fm_mapped_extents * sizeof(struct fiemap_extent));
	if (fmap2 == NULL) {
		fprintf(stderr, "malloc error (%d): %s\n", errno, strerror(errno));
		goto out;
	}
	fmap2->fm_flags = 0;
	fmap2->fm_start = 0;
	fmap2->fm_length = (~0ULL);
	fmap2->fm_extent_count = fmap.fm_mapped_extents;
	
	error = ioctl(fd, FS_IOC_FIEMAP, fmap2);
	if (error == -1) {
		fprintf(stderr, "fiemap error (%d): %s\n", errno, strerror(errno));
		goto fmap2_free;
	}
	if (fmap2->fm_mapped_extents) {
		int i;
	again:
		for (i=0; i<fmap2->fm_mapped_extents; i++) {
			struct fiemap_extent *fe = &fmap2->fm_extents[i];
			uint64_t end = fe->fe_logical + fe->fe_length;

			end = end > quota_file_size ? quota_file_size : end;
			startid = DIV_RU(fe->fe_logical, sizeof(struct gfs2_quota));
			if (startid % 2 != pass)
				startid++;
			offset = startid * sizeof(struct gfs2_quota);
			do {
				memset(buf, 0, sizeof(struct gfs2_quota));
				/* read hidden quota file here */
				lseek(fd, offset, SEEK_SET);
				error = read(fd, buf, sizeof(struct gfs2_quota));
				if (error < 0) {
					fprintf(stderr, "read error (%d): %s\n",
						errno, strerror(errno));
					goto fmap2_free;
				}
				gfs2_quota_in(&q, buf);
				id = (offset / sizeof(struct gfs2_quota)) >> 1;
				if (q.qu_limit || q.qu_warn || q.qu_value)
					print_quota(comline, (pass) ? FALSE : TRUE, id,
						    &q, &sdp->sd_sb);
				offset += 2 * sizeof(struct gfs2_quota);
			} while (offset < end);
		}
		if (!pass) {
			pass = 1;
			goto again;
		}
	}

fmap2_free:
	free(fmap2);
out:
	close(fd);
	close(sdp->metafs_fd);
	cleanup_metafs(sdp);
}

/**
 * do_get_one - Get a quota value from one FS
 * @comline: the struct containing the parsed command line arguments
 * @filesystem: the filesystem to get from
 *
 */

static void 
do_get_one(struct gfs2_sbd *sdp, commandline_t *comline, char *filesystem)
{
	int fd;
	char buf[256];
	struct gfs2_quota q;
	uint64_t offset;
	int error;
	char quota_file[BUF_SIZE];

	strcpy(sdp->path_name, filesystem);
	if (check_for_gfs2(sdp)) {
		if (errno == EINVAL)
			fprintf(stderr, "Not a valid GFS2 mount point: %s\n",
					sdp->path_name);
		else
			fprintf(stderr, "%s\n", strerror(errno));
		exit(-1);
	}
	read_superblock(&sdp->sd_sb, sdp);
	if (mount_gfs2_meta(sdp)) {
		fprintf(stderr, "Error mounting GFS2 metafs: %s\n",
			strerror(errno));
		exit(-1);
	}

	strcpy(quota_file, sdp->metafs_path);
	strcat(quota_file, "/quota");

	fd = open(quota_file, O_RDONLY);
	if (fd < 0) {
		close(sdp->metafs_fd);
		cleanup_metafs(sdp);
		die("can't open file %s: %s\n", quota_file,
		    strerror(errno));
	}

	if (comline->id_type == GQ_ID_USER)
		offset = (2 * (uint64_t)comline->id) * sizeof(struct gfs2_quota);
	else
		offset = (2 * (uint64_t)comline->id + 1) * sizeof(struct gfs2_quota);

	memset(&q, 0, sizeof(struct gfs2_quota));
	
	lseek(fd, offset, SEEK_SET);
	error = read(fd, buf, sizeof(struct gfs2_quota));
	if (error < 0) {
		close(fd);
		close(sdp->metafs_fd);
		cleanup_metafs(sdp);
		die("can't get quota info (%d): %s\n",
		    error, strerror(errno));
	}

	gfs2_quota_in(&q, buf);
	print_quota(comline,
		    (comline->id_type == GQ_ID_USER), comline->id,
		    &q, &sdp->sd_sb);

	close(fd);
	close(sdp->metafs_fd);
	cleanup_metafs(sdp);	
}

/**
 * do_get - Get a quota value
 * @comline: the struct containing the parsed command line arguments
 *
 */

static void
do_get(struct gfs2_sbd *sdp, commandline_t *comline)
{
	int first = TRUE;

	if (*comline->filesystem)
		do_get_one(sdp, comline, comline->filesystem);
	else {
		char buf[256], device[256], path[256], type[256];
		FILE *file;

		file = fopen("/proc/mounts", "r");
		if (!file)
			die("can't open /proc/mounts: %s\n", strerror(errno));

		while (fgets(buf, 256, file)) {
			if (sscanf(buf, "%s %s %s", device, path, type) != 3)
				continue;
			if (strcmp(type, "gfs2") != 0)
				continue;

			if (first)
				first = FALSE;
			else
				printf("\n");

			printf("%s\n", path);
			do_get_one(sdp, comline, path);
		}

		fclose(file);
	}
}

/**
 * do_sync_one - sync the quotas on one GFS2 filesystem
 * @path: a file/directory in the filesystem
 *
 */
static void 
do_sync_one(struct gfs2_sbd *sdp, char *filesystem)
{
	char *fsname;

	fsname = mp2fsname(filesystem);
	if (!fsname) {
		fprintf(stderr, "Couldn't find GFS2 filesystem mounted at %s\n",
				filesystem);
		exit(-1);
	}
	if (set_sysfs(fsname, "quota_sync", "1")) {
		fprintf(stderr, "Error writing to sysfs quota sync file: %s\n",
				strerror(errno));
		exit(-1);
	}
}

/**
 * do_sync - sync out unsyned quotas
 * @comline: the struct containing the parsed command line arguments
 *
 */

void
do_sync(struct gfs2_sbd *sdp, commandline_t *comline)
{
	sync();

	if (*comline->filesystem)
		do_sync_one(sdp, comline->filesystem);
	else {
		char buf[256], device[256], path[256], type[256];
		FILE *file;

		file = fopen("/proc/mounts", "r");
		if (!file)
			die("can't open /proc/mounts: %s\n", strerror(errno));

		while (fgets(buf, 256, file)) {
			if (sscanf(buf, "%s %s %s", device, path, type) != 3)
				continue;
			if (strcmp(type, "gfs2") != 0)
				continue;

			do_sync_one(sdp, path);
		}

		fclose(file);
	}
}

/**
 * do_set - Set a quota value
 * @comline: the struct containing the parsed command line arguments
 *
 */

static void
do_set(struct gfs2_sbd *sdp, commandline_t *comline)
{
	int fd;
	uint64_t offset;
	uint64_t new_value;
	char quota_file[BUF_SIZE];
	char id_str[16];
	struct stat stat_buf;
	struct gfs2_quota q;
	char *fs;
	
	if (!*comline->filesystem)
		die("need a filesystem to work on\n");
	if (!comline->new_value_set)
		die("need a new value\n");

	strcpy(sdp->path_name, comline->filesystem);
	if (check_for_gfs2(sdp)) {
		if (errno == EINVAL)
			fprintf(stderr, "Not a valid GFS2 mount point: %s\n",
					sdp->path_name);
		else
			fprintf(stderr, "%s\n", strerror(errno));
		exit(-1);
	}
	read_superblock(&sdp->sd_sb, sdp);
	if (mount_gfs2_meta(sdp)) {
		fprintf(stderr, "Error mounting GFS2 metafs: %s\n",
			strerror(errno));
		exit(-1);
	}

	strcpy(quota_file, sdp->metafs_path);
	strcat(quota_file, "/quota");

	fd = open(quota_file, O_RDWR);
	if (fd < 0) {
		close(sdp->metafs_fd);
		cleanup_metafs(sdp);
		die("can't open file %s: %s\n", quota_file,
		    strerror(errno));
	}
	
	switch (comline->id_type) {
	case GQ_ID_USER:
		offset = (2 * (uint64_t)comline->id) * sizeof(struct gfs2_quota);
		break;

	case GQ_ID_GROUP:
		offset = (2 * (uint64_t)comline->id + 1) * sizeof(struct gfs2_quota);
		break;

	default:
		fprintf(stderr, "invalid user/group ID\n");
		goto out;
	}

	switch (comline->units) {
	case GQ_UNITS_MEGABYTE:
		new_value =
			comline->new_value << (20 - sdp->sd_sb.sb_bsize_shift);
		break;

	case GQ_UNITS_KILOBYTE:
		if (sdp->sd_sb.sb_bsize == 512)
			new_value = comline->new_value * 2;
		else
			new_value = comline->new_value >>
				(sdp->sd_sb.sb_bsize_shift - 10);
		break;

	case GQ_UNITS_FSBLOCK:
		new_value = comline->new_value;
		break;

	case GQ_UNITS_BASICBLOCK:
		new_value = comline->new_value >>
			(sdp->sd_sb.sb_bsize_shift - 9);
		break;

	default:
		fprintf(stderr, "bad units\n");
		goto out;
	}

	memset(&q, 0, sizeof(struct gfs2_quota));
	if (fstat(fd, &stat_buf)) {
		fprintf(stderr, "stat failed: %s\n", strerror(errno));
		goto out;
	}
	if (stat_buf.st_size >= (offset + sizeof(struct gfs2_quota)))
		read_quota_internal(fd, comline->id, comline->id_type, &q);

	switch (comline->operation) {
	case GQ_OP_LIMIT:
		q.qu_limit = new_value; break;
	case GQ_OP_WARN:
		q.qu_warn = new_value; break;
	}

	write_quota_internal(fd, comline->id, comline->id_type, &q);
	fs = mp2fsname(comline->filesystem);
	if (!fs) {
		fprintf(stderr, "Couldn't find GFS2 filesystem mounted at %s\n",
				comline->filesystem);
		exit(-1);
	}
	sprintf(id_str, "%d", comline->id);
	if (set_sysfs(fs, comline->id_type == GQ_ID_USER ?
		  "quota_refresh_user" : "quota_refresh_group", id_str)) {
		fprintf(stderr, "Error writing to sysfs quota refresh file: %s\n",
				strerror(errno));
		exit(-1);
	}
	
out:
	close(fd);
	close(sdp->metafs_fd);
	cleanup_metafs(sdp);
}

/**
 * main - Do everything
 * @argc: well, it's argc...
 * @argv: well, it's argv...
 *
 * Returns: exit status
 */

int
main(int argc, char *argv[])
{
    struct gfs2_sbd sbd, *sdp = &sbd;
	commandline_t comline;

	memset(sdp, 0, sizeof(struct gfs2_sbd));
	memset(&comline, 0, sizeof(commandline_t));

	decode_arguments(argc, argv, &comline);
	sdp->path_name = (char*) malloc(512);
	if (!sdp->path_name)
		die("Can't malloc! %s\n", strerror(errno));

	switch (comline.operation) {
	case GQ_OP_LIST:
		do_list(sdp, &comline);
		break;

	case GQ_OP_GET:
		do_get(sdp, &comline);
		break;

	case GQ_OP_LIMIT:
	case GQ_OP_WARN:
		do_set(sdp, &comline);
		break;

	case GQ_OP_SYNC:
		do_sync(sdp, &comline);
		break;

	case GQ_OP_CHECK:
		do_sync(sdp, &comline);
		do_check(sdp, &comline);
		break;

	case GQ_OP_INIT:
		do_sync(sdp, &comline);
		do_quota_init(sdp, &comline);
		break;

	case GQ_OP_RESET:
		do_reset(sdp, &comline);
		break;
	default:
		if (!comline.id_type) {
			comline.id_type = GQ_ID_USER;
			comline.id = geteuid();
		}
		do_get(sdp, &comline);
		break;
	}
	
	free(sdp->path_name);

	exit(EXIT_SUCCESS);
}
