#include "clusterautoconfig.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <sys/mount.h>
#include <linux/types.h>
#include <sys/file.h>
#include <dirent.h>
#include <linux/kdev_t.h>
#include <sys/sysmacros.h>
#include <mntent.h>
#include <sys/time.h>

#include "libgfs2.h"

#define PAGE_SIZE (4096)
#define SYS_BASE "/sys/fs/gfs2" /* FIXME: Look in /proc/mounts to find this */
#define DIV_RU(x, y) (((x) + (y) - 1) / (y))

static char sysfs_buf[PAGE_SIZE];

int compute_heightsize(struct gfs2_sbd *sdp, uint64_t *heightsize,
	uint32_t *maxheight, uint32_t bsize1, int diptrs, int inptrs)
{
	heightsize[0] = sdp->bsize - sizeof(struct gfs2_dinode);
	heightsize[1] = bsize1 * diptrs;
	for (*maxheight = 2;; (*maxheight)++) {
		uint64_t space, d;
		uint32_t m;

		space = heightsize[*maxheight - 1] * inptrs;
		m = space % inptrs;
		d = space / inptrs;

		if (d != heightsize[*maxheight - 1] || m)
			break;
		heightsize[*maxheight] = space;
	}
	if (*maxheight > GFS2_MAX_META_HEIGHT) {
		errno = EINVAL;
		return -1;
	}
	return 0;
}

int compute_constants(struct gfs2_sbd *sdp)
{
	uint32_t hash_blocks, ind_blocks, leaf_blocks;
	uint32_t tmp_blocks;

	sdp->md.next_inum = 1;

	sdp->sd_sb.sb_bsize_shift = ffs(sdp->bsize) - 1;
	sdp->sb_addr = GFS2_SB_ADDR * GFS2_BASIC_BLOCK / sdp->bsize;

	sdp->sd_fsb2bb_shift = sdp->sd_sb.sb_bsize_shift -
		GFS2_BASIC_BLOCK_SHIFT;
	sdp->sd_fsb2bb = 1 << sdp->sd_fsb2bb_shift;
	sdp->sd_diptrs = (sdp->bsize - sizeof(struct gfs2_dinode)) /
		sizeof(uint64_t);
	sdp->sd_inptrs = (sdp->bsize - sizeof(struct gfs2_meta_header)) /
		sizeof(uint64_t);
	sdp->sd_jbsize = sdp->bsize - sizeof(struct gfs2_meta_header);
	sdp->sd_hash_bsize = sdp->bsize / 2;
	sdp->sd_hash_bsize_shift = sdp->sd_sb.sb_bsize_shift - 1;
	sdp->sd_hash_ptrs = sdp->sd_hash_bsize / sizeof(uint64_t);

	/*  Compute maximum reservation required to add a entry to a directory  */

	hash_blocks = DIV_RU(sizeof(uint64_t) * (1 << GFS2_DIR_MAX_DEPTH),
			     sdp->sd_jbsize);

	ind_blocks = 0;
	for (tmp_blocks = hash_blocks; tmp_blocks > sdp->sd_diptrs;) {
		tmp_blocks = DIV_RU(tmp_blocks, sdp->sd_inptrs);
		ind_blocks += tmp_blocks;
	}

	leaf_blocks = 2 + GFS2_DIR_MAX_DEPTH;

	sdp->sd_max_dirres = hash_blocks + ind_blocks + leaf_blocks;

	if (compute_heightsize(sdp, sdp->sd_heightsize, &sdp->sd_max_height,
				sdp->bsize, sdp->sd_diptrs, sdp->sd_inptrs)) {
		return -1;
	}
	if (compute_heightsize(sdp, sdp->sd_jheightsize, &sdp->sd_max_jheight,
				sdp->sd_jbsize, sdp->sd_diptrs, sdp->sd_inptrs)) {
		return -1;
	}
	return 0;
}

int is_pathname_mounted(struct gfs2_sbd *sdp, int *ro_mount)
{
	FILE *fp;
	struct mntent *mnt;
	dev_t file_dev=0, file_rdev=0;
	ino_t file_ino=0;
	struct stat st_buf;

	*ro_mount = 0;
	if ((fp = setmntent("/proc/mounts", "r")) == NULL) {
		perror("open: /proc/mounts");
		return 0;
	}
	if (stat(sdp->path_name, &st_buf) == 0) {
		if (S_ISBLK(st_buf.st_mode)) {
#ifndef __GNU__ /* The GNU hurd is broken with respect to stat devices */
			file_rdev = st_buf.st_rdev;
#endif  /* __GNU__ */
		} else {
			file_dev = st_buf.st_dev;
			file_ino = st_buf.st_ino;
		}
	}
	while ((mnt = getmntent (fp)) != NULL) {
		/* Check if they specified the device instead of mnt point */
		if (strcmp(sdp->device_name, mnt->mnt_fsname) == 0) {
			strcpy(sdp->path_name, mnt->mnt_dir); /* fix it */
			break;
		}
		if (strcmp(sdp->path_name, mnt->mnt_dir) == 0) {
			strcpy(sdp->device_name, mnt->mnt_fsname); /* fix it */
			break;
		}
		if (stat(mnt->mnt_fsname, &st_buf) == 0) {
			if (S_ISBLK(st_buf.st_mode)) {
#ifndef __GNU__
				if (file_rdev && (file_rdev == st_buf.st_rdev))
					break;
#endif  /* __GNU__ */
			} else {
				if (file_dev && ((file_dev == st_buf.st_dev) &&
						 (file_ino == st_buf.st_ino)))
					break;
			}
		}
	}
	endmntent (fp);
	if (mnt == NULL)
		return 0;
	if (stat(mnt->mnt_dir, &st_buf) < 0) {
		if (errno == ENOENT)
			return 0;
	}
	/* Can't trust fstype because / has "rootfs". */
	if (file_rdev && (st_buf.st_dev != file_rdev))
		return 0;
	if (hasmntopt(mnt, MNTOPT_RO))
               *ro_mount = 1;
	return 1; /* mounted */
}

int is_gfs2(struct gfs2_sbd *sdp)
{
	int fd, rc;
	struct gfs2_sb sb;

	fd = open(sdp->device_name, O_RDWR);
	if (fd < 0)
		return 0;

	rc = 0;
	if (lseek(fd, GFS2_SB_ADDR * GFS2_BASIC_BLOCK, SEEK_SET) >= 0 &&
	    read(fd, &sb, sizeof(sb)) == sizeof(sb) &&
	    be32_to_cpu(sb.sb_header.mh_magic) == GFS2_MAGIC &&
	    be32_to_cpu(sb.sb_header.mh_type) == GFS2_METATYPE_SB)
		rc = 1;
	close(fd);
	return rc;
}

int check_for_gfs2(struct gfs2_sbd *sdp)
{
	int ro;

	if (!is_pathname_mounted(sdp, &ro))
		return -1;
	if (!is_gfs2(sdp))
		return -1;
	return 0;
}

static int lock_for_admin(struct gfs2_sbd *sdp)
{
	int error;

	if (sdp->debug)
		printf("\nTrying to get admin lock...\n");

	sdp->metafs_fd = open(sdp->metafs_path, O_RDONLY | O_NOFOLLOW);
	if (sdp->metafs_fd < 0)
		return -1;
	
	error = flock(sdp->metafs_fd, LOCK_EX);
	if (error)
		return -1;
	if (sdp->debug)
		printf("Got it.\n");
	return 0;
}


int mount_gfs2_meta(struct gfs2_sbd *sdp)
{
	int ret;

	memset(sdp->metafs_path, 0, PATH_MAX);
	snprintf(sdp->metafs_path, PATH_MAX - 1, "/tmp/.gfs2meta.XXXXXX");

	if(!mkdtemp(sdp->metafs_path))
		return -1;

	ret = mount(sdp->path_name, sdp->metafs_path, "gfs2meta", 0, NULL);
	if (ret) {
		rmdir(sdp->metafs_path);
		return -1;
	}
	if (lock_for_admin(sdp))
		return -1;
	return 0;
}

void cleanup_metafs(struct gfs2_sbd *sdp)
{
	int ret;

	if (sdp->metafs_fd <= 0)
		return;

	fsync(sdp->metafs_fd);
	close(sdp->metafs_fd);
	ret = umount(sdp->metafs_path);
	if (ret)
		fprintf(stderr, "Couldn't unmount %s : %s\n",
			sdp->metafs_path, strerror(errno));
	else
		rmdir(sdp->metafs_path);
}

static char *__get_sysfs(const char *fsname, const char *filename)
{
	char path[PATH_MAX];
	int fd, rv;

	memset(path, 0, PATH_MAX);
	memset(sysfs_buf, 0, PAGE_SIZE);
	snprintf(path, PATH_MAX - 1, "%s/%s/%s", SYS_BASE, fsname, filename);

	fd = open(path, O_RDONLY);
	if (fd < 0)
		return NULL;
	rv = read(fd, sysfs_buf, PAGE_SIZE);
	if (rv < 0) {
		close(fd);
		return NULL;
	}

	close(fd);
	return sysfs_buf;
}

char *get_sysfs(const char *fsname, const char *filename)
{
	char *s;
	char *p;

	s = __get_sysfs(fsname, filename);
	if (!s)
		return NULL;
	p = strchr(s, '\n');
	if (p)
		*p = '\0';
	return sysfs_buf;
}

int get_sysfs_uint(const char *fsname, const char *filename, unsigned int *val)
{
	char *s = __get_sysfs(fsname, filename);
	int ret;
	if (!s)
		return -1;
	ret = sscanf(s, "%u", val);
	if (1 != ret) {
		errno = ENOMSG;
		return -1;
	}
	return 0;
}

int set_sysfs(const char *fsname, const char *filename, const char *val)
{
	char path[PATH_MAX];
	int fd, rv, len;

	len = strlen(val) + 1;
	if (len > PAGE_SIZE) {
		errno = EINVAL;
		return -1;
	}

	memset(path, 0, PATH_MAX);
	snprintf(path, PATH_MAX - 1, "%s/%s/%s", SYS_BASE, fsname, filename);

	fd = open(path, O_WRONLY);
	if (fd < 0)
		return -1;

	rv = write(fd, val, len);
	if (rv != len) {
		close(fd);
		return -1;
	}
	close(fd);
	return 0;
}

char *find_debugfs_mount(void)
{
	FILE *file;
	char line[PATH_MAX];
	char device[PATH_MAX], type[PATH_MAX];
	char *path;

	file = fopen("/proc/mounts", "rt");
	if (!file)
		return NULL;

	path = malloc(PATH_MAX);
	if (!path) {
		fclose(file);
		return NULL;
	}
	while (fgets(line, PATH_MAX, file)) {

		if (sscanf(line, "%s %s %s", device, path, type) != 3)
			continue;
		if (!strcmp(type, "debugfs")) {
			fclose(file);
			return path;
		}
	}

	free(path);
	fclose(file);
	return NULL;
}

/**
 * mp2fsname - Find the name for a filesystem given its mountpoint
 *
 * We do this by iterating through gfs2 dirs in /sys/fs/gfs2/ looking for
 * one where the "id" attribute matches the device id returned by stat for
 * the mount point.  The reason we go through all this is simple: the
 * kernel's sysfs is named after the VFS s_id, not the device name.
 * So it's perfectly legal to do something like this to simulate user
 * conditions without the proper hardware:
 *    # rm /dev/sdb1
 *    # mkdir /dev/cciss
 *    # mknod /dev/cciss/c0d0p1 b 8 17
 *    # mount -tgfs2 /dev/cciss/c0d0p1 /mnt/gfs2
 *    # gfs2_tool gettune /mnt/gfs2
 * In this example the tuning variables are in a directory named after the
 * VFS s_id, which in this case would be /sys/fs/gfs2/sdb1/
 *
 * Returns: the fsname
 */

char *mp2fsname(char *mp)
{
	char device_id[PATH_MAX], *fsname = NULL;
	struct stat statbuf;
	DIR *d;
	struct dirent *de;
	char *id;

	if (stat(mp, &statbuf))
		return NULL;

	memset(device_id, 0, sizeof(device_id));
	sprintf(device_id, "%i:%i", major(statbuf.st_dev),
		minor(statbuf.st_dev));

	d = opendir(SYS_BASE);
	if (!d)
		return NULL;

	while ((de = readdir(d))) {
		if (de->d_name[0] == '.')
			continue;

		id = get_sysfs(de->d_name, "id");
		if (!id)
			continue;
		if (strcmp(id, device_id) == 0) {
			fsname = strdup(de->d_name);
			break;
		}
	}

	closedir(d);

	return fsname;
}

/*
 * get_random_bytes - Generate a series of random bytes using /dev/urandom.
 *
 * Modified from original code in gen_uuid.c in e2fsprogs/lib
 */
void get_random_bytes(void *buf, int nbytes)
{
	int i, n = nbytes, fd;
	int lose_counter = 0;
	unsigned char *cp = (unsigned char *) buf;
	struct timeval	tv;

	gettimeofday(&tv, 0);
	fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
	srand((getpid() << 16) ^ getuid() ^ tv.tv_sec ^ tv.tv_usec);
	/* Crank the random number generator a few times */
	gettimeofday(&tv, 0);
	for (i = (tv.tv_sec ^ tv.tv_usec) & 0x1F; i > 0; i--)
		rand();
	if (fd >= 0) {
		while (n > 0) {
			i = read(fd, cp, n);
			if (i <= 0) {
				if (lose_counter++ > 16)
					break;
				continue;
			}
			n -= i;
			cp += i;
			lose_counter = 0;
		}
		close(fd);
	}

	/*
	 * We do this all the time, but this is the only source of
	 * randomness if /dev/random/urandom is out to lunch.
	 */
	for (cp = buf, i = 0; i < nbytes; i++)
		*cp++ ^= (rand() >> 7) & 0xFF;

	return;
}
