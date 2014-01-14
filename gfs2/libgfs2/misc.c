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
#include <sys/sysmacros.h>
#include <mntent.h>
#include <signal.h>

#include "libgfs2.h"

#define PAGE_SIZE (4096)
#define SYS_BASE "/sys/fs/gfs2" /* FIXME: Look in /proc/mounts to find this */
#define DIV_RU(x, y) (((x) + (y) - 1) / (y))

int metafs_interrupted = 0;

int compute_heightsize(unsigned bsize, uint64_t *heightsize,
	uint32_t *maxheight, uint32_t bsize1, int diptrs, int inptrs)
{
	heightsize[0] = bsize - sizeof(struct gfs2_dinode);
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
	sdp->sd_blocks_per_bitmap = (sdp->sd_sb.sb_bsize - sizeof(struct gfs2_meta_header))
	                             * GFS2_NBBY;

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

	if (compute_heightsize(sdp->bsize, sdp->sd_heightsize, &sdp->sd_max_height,
				sdp->bsize, sdp->sd_diptrs, sdp->sd_inptrs)) {
		return -1;
	}
	if (compute_heightsize(sdp->bsize, sdp->sd_jheightsize, &sdp->sd_max_jheight,
				sdp->sd_jbsize, sdp->sd_diptrs, sdp->sd_inptrs)) {
		return -1;
	}
	return 0;
}

/* Returns 0 if fd1 and fd2 refer to the same device/file, 1 otherwise, or -1 on error */
static int fdcmp(int fd1, int fd2)
{
	struct stat st1, st2;
	if ((fstat(fd1, &st1) != 0) || (fstat(fd2, &st2) != 0))
		return -1;
	if (S_ISBLK(st1.st_mode) && S_ISBLK(st2.st_mode)) {
		if (st1.st_rdev == st2.st_rdev) {
			return 0;
		}
	} else if ((st1.st_dev == st2.st_dev) && (st1.st_ino == st2.st_ino)) {
			return 0;
	}
	return 1;
}

int lgfs2_open_mnt(const char *path, int dirflags, int *dirfd, int devflags, int *devfd, struct mntent **mnt)
{
	FILE *fp = setmntent("/proc/mounts", "r");
	if (fp == NULL) {
		perror("open: /proc/mounts");
		return 1;
	}
	/* Assume path is mount point until we know better. */
	*dirfd = open(path, dirflags);
	if (*dirfd < 0)
		return 1;

	while ((*mnt = getmntent(fp)) != NULL) {
		int fd;
		if (strcmp((*mnt)->mnt_type, "gfs2") != 0)
			continue;
		*devfd = open((*mnt)->mnt_fsname, devflags);
		/* Defer checking *devfd until later: whether it's ok to ignore
		 * the error depends on whether we find the mount point. */

		if (strcmp(path, (*mnt)->mnt_dir) == 0)
			break;
		if (strcmp(path, (*mnt)->mnt_fsname) == 0 || fdcmp(*dirfd, *devfd) == 0) {
			/* We have a match but our above assumption was
			   incorrect and *dirfd is actually the device. */
			close(*dirfd);
			*dirfd = open((*mnt)->mnt_dir, dirflags);
			break;
		}

		fd = open((*mnt)->mnt_dir, dirflags);
		if (fd >= 0) {
			int diff = fdcmp(*dirfd, fd);
			close(fd);
			if (diff == 0)
				break;
		}
		if (*devfd >= 0)
			close(*devfd);
	}
	endmntent(fp);
	if (*mnt == NULL) {
		close(*dirfd);
		return 0; /* Success. Answer is no. Both fds closed. */
	}
	if (*dirfd < 0) {
		close(*devfd);
		return 1;
	}
	if (*devfd < 0) {
		close(*dirfd);
		return 1;
	}
	return 0; /* Success. Answer is yes. Both fds open. */
}

int lgfs2_open_mnt_dev(const char *path, int flags, struct mntent **mnt)
{
	int dirfd = -1;
	int devfd = -1;
	if (lgfs2_open_mnt(path, O_RDONLY, &dirfd, flags, &devfd, mnt) != 0)
		return -1;
	if (*mnt != NULL)
		close(dirfd);
	return devfd;
}

int lgfs2_open_mnt_dir(const char *path, int flags, struct mntent **mnt)
{
	int dirfd = -1;
	int devfd = -1;
	if (lgfs2_open_mnt(path, flags, &dirfd, O_RDONLY, &devfd, mnt) != 0)
		return -1;
	if (*mnt != NULL)
		close(devfd);
	return dirfd;
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

static void sighandler(int error)
{
	metafs_interrupted = 1;
}

int mount_gfs2_meta(struct gfs2_sbd *sdp)
{
	int ret;
	struct sigaction sa = {	.sa_handler = &sighandler };

	memset(sdp->metafs_path, 0, PATH_MAX);
	snprintf(sdp->metafs_path, PATH_MAX - 1, "/tmp/.gfs2meta.XXXXXX");

	if(!mkdtemp(sdp->metafs_path))
		return -1;

	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGILL, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGHUP, &sa, NULL);
	sigaction(SIGABRT, &sa, NULL);
	sigaction(SIGSEGV, &sa, NULL);
	sigaction(SIGCONT, &sa, NULL);
	sigaction(SIGUSR1, &sa, NULL);
	sigaction(SIGUSR2, &sa, NULL);
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
	struct sigaction sa = {	.sa_handler = SIG_DFL };

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
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGILL, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGHUP, &sa, NULL);
	sigaction(SIGABRT, &sa, NULL);
	sigaction(SIGSEGV, &sa, NULL);
	sigaction(SIGCONT, &sa, NULL);
	sigaction(SIGUSR1, &sa, NULL);
	sigaction(SIGUSR2, &sa, NULL);
	metafs_interrupted = 0;
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
