#include "clusterautoconfig.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>
#include <sys/types.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/vfs.h>
#include <sys/mount.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <stdarg.h>
#include <libintl.h>
#include <locale.h>
#define _(String) gettext(String)

#include <linux/types.h>
#include <linux/fiemap.h>
#include <linux/fs.h>
#include "libgfs2.h"
#include "gfs2_mkfs.h"
#include "metafs.h"

#define RANDOM(values) ((values) * (random() / (RAND_MAX + 1.0)))

struct jadd_opts {
	char *path;
	char *new_inode;
	char *per_node;
	char *jindex;
	unsigned orig_journals;
	unsigned journals;
	unsigned quiet:1;
	unsigned debug:1;
};

#define JA_FL_SET   0
#define JA_FL_CLEAR 1
static int set_flags(int fd, int op, uint32_t flags)
{
	uint32_t val;

        if (ioctl(fd, FS_IOC_GETFLAGS, &val)) {
		perror("GETFLAGS");
		return -1;
	}

        if (op == JA_FL_SET)
                val |= flags;
        else if (op == JA_FL_CLEAR)
                val &= ~flags;

        if (ioctl(fd, FS_IOC_SETFLAGS, &val)) {
		perror("SETFLAGS");
		return -1;
	}
	return 0;
}

static int rename2system(struct jadd_opts *opts, const char *new_dir, const char *new_name)
{
	char *newpath;
	int ret;

	if (asprintf(&newpath, "%s/%s", new_dir, new_name) < 0) {
		perror(_("Failed to allocate new path"));
		return -1;
	}

	ret = rename(opts->new_inode, newpath);
	free(newpath);
	return ret;
}

static int build_paths(const char *metafs_path, struct jadd_opts *opts)
{
	int i = 0;
	struct {
		char **path;
		const char *tail;
	} p[] = {
		{ &opts->new_inode, "new_inode" },
		{ &opts->per_node, "per_node" },
		{ &opts->jindex, "jindex" },
		{ NULL, NULL}
	};

	while (p[i].path != NULL) {
		if (asprintf(p[i].path, "%s/%s", metafs_path, p[i].tail) < 0) {
			while (i > 0)
				free(*p[--i].path);
			return 1;
		}
		if (opts->debug)
			printf("%d: %s\n", i, *p[i].path);
		i++;
	}
	return 0;
}

/**
 * print_usage - print out usage information
 * @prog_name: The name of this program
 */

static void print_usage(const char *prog_name)
{
	int i;
	const char *option, *param, *desc;
	const char *options[] = {
		/* Translators: This is a usage string printed with --help.
		   <size> and <number> here are the commandline parameters,
		   e.g. gfs2_jadd -j <number> /dev/sda */
		"-c", "<size>",   _("Size of quota change file, in megabytes"),
		"-D", NULL,       _("Enable debugging code"),
		"-h", NULL,       _("Display this help, then exit"),
		"-J", "<size>",   _("Size of journals, in megabytes"),
		"-j", "<number>", _("Number of journals"),
		"-q", NULL,       _("Don't print anything"),
		"-V", NULL,       _("Display version information, then exit"),
		NULL, NULL, NULL /* Must be kept at the end */
	};

	printf("%s\n", _("Usage:"));
	printf("%s [%s] <%s>\n\n", prog_name, _("options"), _("device"));
	printf(_("Adds journals to a GFS2 file system."));
	printf("\n\n%s\n", _("Options:"));

	for (i = 0; options[i] != NULL; i += 3) {
		option = options[i];
		param = options[i+1];
		desc = options[i+2];
		printf("%3s %-15s %s\n", option, param ? param : "", desc);
	}
}

/**
 * decode_arguments - decode command line arguments and fill in the struct gfs2_sbd
 * @argc:
 * @argv:
 * @sdp: the decoded command line arguments
 *
 */

static void decode_arguments(int argc, char *argv[], struct gfs2_sbd *sdp, struct jadd_opts *opts)
{
	int cont = TRUE;
	int optchar;

	while (cont) {
		optchar = getopt(argc, argv, "c:DhJ:j:qu:V");

		switch (optchar) {
		case 'c':
			sdp->qcsize = atoi(optarg);
			break;
		case 'D':
			opts->debug = 1;
			lgfs2_set_debug(1);
			break;
		case 'h':
			print_usage(argv[0]);
			exit(0);
			break;
		case 'J':
			sdp->jsize = atoi(optarg);
			break;
		case 'j':
			opts->journals = atoi(optarg);
			break;
		case 'q':
			opts->quiet = 1;
			break;
		case 'V':
			printf("gfs2_jadd %s (built %s %s)\n", VERSION,
			       __DATE__, __TIME__);
			printf(REDHAT_COPYRIGHT "\n");
			exit(0);
			break;
		case ':':
		case '?':
			fprintf(stderr, _("Please use '-h' for help.\n"));
			exit(EXIT_FAILURE);
			break;
		case EOF:
			cont = FALSE;
			break;
		default:
			die( _("Invalid option: %c\n"), optchar);
			break;
		};
	}

	if (optind < argc) {
		opts->path = argv[optind];
		optind++;
	} else
		die( _("no path specified (try -h for help)\n"));

	if (optind < argc)
		die( _("Unrecognized argument: %s\n"), argv[optind]);

	if (opts->debug) {
		printf( _("Command line arguments:\n"));
		printf("  qcsize = %u\n", sdp->qcsize);
		printf("  jsize = %u\n", sdp->jsize);
		printf("  journals = %u\n", sdp->md.journals);
		printf("  quiet = %u\n", opts->quiet);
		printf("  path = %s\n", opts->path);
	}
}

static void verify_arguments(struct gfs2_sbd *sdp, struct jadd_opts *opts)
{
	if (!opts->journals)
		die( _("no journals specified\n"));
	if (sdp->jsize < 32 || sdp->jsize > 1024)
		die( _("bad journal size\n"));
	if (!sdp->qcsize || sdp->qcsize > 64)
		die( _("bad quota change size\n"));
}

static void print_results(struct jadd_opts *opts)
{
	if (opts->debug)
		printf("\n");
	else if (opts->quiet)
		return;

	printf( _("Filesystem: %s\n"), opts->path);
	printf( _("Old journals: %u\n"), opts->orig_journals);
	printf( _("New journals: %u\n"), opts->journals);
}

static int create_new_inode(struct jadd_opts *opts, uint64_t *addr)
{
	char *name = opts->new_inode;
	int fd, error = 0;

	for (;;) {
		fd = open(name, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
		if (fd >= 0)
			break;
		if (errno == EEXIST) {
			if (unlink(name)) {
				perror("unlink");
				return -1;
			}
			continue;
		}
		perror("create");
		return -1;
	}

	if (addr != NULL) {
		struct stat st;

		if ((error = fstat(fd, &st))) {
			perror("fstat");
			return close(fd);
		}
		*addr = st.st_ino;
	}

	return fd;
}

static int add_ir(struct jadd_opts *opts)
{
	int fd, error = 0;
	char new_name[256];
	struct gfs2_inum_range ir;

	if ((fd = create_new_inode(opts, NULL)) < 0)
		return fd;

	if ((error = set_flags(fd, JA_FL_SET, FS_JOURNAL_DATA_FL)))
		goto close_fd;

	memset(&ir, 0, sizeof(struct gfs2_inum_range));
	if (write(fd, (void*)&ir, sizeof(struct gfs2_inum_range)) !=
	    sizeof(struct gfs2_inum_range)) {
		perror("add_ir write");
		error = -1;
		goto close_fd;
	}

	if ((error = fsync(fd))) {
		perror("add_ir fsync");
		goto close_fd;
	}


	sprintf(new_name, "inum_range%u", opts->journals);
	error = rename2system(opts, opts->per_node, new_name);
	if (error < 0 && errno != EEXIST) {
		perror("add_ir rename2system");
		goto close_fd;
	}
close_fd:
	return close(fd) || error;
}

static int add_sc(struct jadd_opts *opts)
{
	int fd, error = 0;
	char new_name[256];
	struct gfs2_statfs_change sc;

	if ((fd = create_new_inode(opts, NULL)) < 0)
		return fd;

	if ((error = set_flags(fd, JA_FL_SET, FS_JOURNAL_DATA_FL)))
		goto close_fd;

	memset(&sc, 0, sizeof(struct gfs2_statfs_change));
	if (write(fd, (void*)&sc, sizeof(struct gfs2_statfs_change)) !=
	    sizeof(struct gfs2_statfs_change)) {
		perror("add_sc write");
		error = -1;
		goto close_fd;
	}

	if ((error = fsync(fd))) {
		perror("add_sc fsync");
		goto close_fd;
	}

	sprintf(new_name, "statfs_change%u", opts->journals);
	error = rename2system(opts, opts->per_node, new_name);
	if (error < 0 && errno != EEXIST){
		perror("add_sc rename2system");
		goto close_fd;
	}
close_fd:
	return close(fd) || error;
}

static int add_qc(struct gfs2_sbd *sdp, struct jadd_opts *opts)
{
	int fd, error = 0;
	char new_name[256], buf[sdp->bsize];
	unsigned int blocks =
		sdp->qcsize << (20 - sdp->sd_sb.sb_bsize_shift);
	unsigned int x;
	struct gfs2_meta_header mh;

	if ((fd = create_new_inode(opts, NULL)) < 0)
		return fd;

	if ((error = set_flags(fd, JA_FL_CLEAR, FS_JOURNAL_DATA_FL)))
		goto close_fd;

	memset(buf, 0, sdp->bsize);
	for (x=0; x<blocks; x++) {
		if (write(fd, buf, sdp->bsize) != sdp->bsize) {
			perror("add_qc write");
			error = -1;
			goto close_fd;
		}
	}

	if ((error = lseek(fd, 0, SEEK_SET)) < 0) {
		perror("add_qc lseek");
		goto close_fd;
	}

	memset(&mh, 0, sizeof(struct gfs2_meta_header));
	mh.mh_magic = GFS2_MAGIC;
	mh.mh_type = GFS2_METATYPE_QC;
	mh.mh_format = GFS2_FORMAT_QC;
	gfs2_meta_header_out(&mh, buf);

	for (x=0; x<blocks; x++) {
		if (write(fd, buf, sdp->bsize) != sdp->bsize) {
			perror("add_qc write");
			error = 1;
			goto close_fd;
		}
		if ((error = fsync(fd))) {
			perror("add_qc fsync");
			goto close_fd;
		}
	}

	sprintf(new_name, "quota_change%u", opts->journals);
	error = rename2system(opts, opts->per_node, new_name);
	if (error < 0 && errno != EEXIST){
		perror("add_qc rename2system");
		goto close_fd;
	}
close_fd:
	return close(fd) || error;
}

static int gather_info(struct gfs2_sbd *sdp, struct jadd_opts *opts)
{
	struct statfs statbuf;

	if (statfs(opts->path, &statbuf) < 0) {
		perror(opts->path);
		return -1;
	}

	sdp->bsize = statbuf.f_bsize;
	sdp->blks_total = statbuf.f_blocks;
	sdp->blks_alloced = sdp->blks_total - statbuf.f_bfree;

	return 0;
}

static int find_current_journals(struct jadd_opts *opts)
{
	struct dirent *dp;
	DIR *dirp;
	unsigned existing_journals = 0;
	int ret = 0;

	dirp = opendir(opts->jindex);
	if (!dirp) {
		perror("jindex");
		ret = -1;
		goto out;
	}
	while (dirp) {
		if ((dp = readdir(dirp)) != NULL) {
			if (strncmp(dp->d_name, "journal", 7) == 0)
				existing_journals++;
		} else
			goto close_fd;
	}
close_fd:
	if ((ret = closedir(dirp)))
		goto out;

	if (existing_journals == 0) {
		errno = EINVAL;
		perror("No journals found. Did you run mkfs.gfs2 correctly?\n");
		ret = -1;
		goto out;
	}

	opts->orig_journals = existing_journals;
out:
	return ret;
}

#ifdef GFS2_HAS_LH_V2
static uint64_t find_block_address(int fd, off_t offset, unsigned bsize)
{
	struct {
		struct fiemap fm;
		struct fiemap_extent fe;
	} fme;
	int ret;

	fme.fm.fm_start = offset;
	fme.fm.fm_length = 1;
	fme.fm.fm_flags = FIEMAP_FLAG_SYNC;
	fme.fm.fm_extent_count = 1;

	ret = ioctl(fd, FS_IOC_FIEMAP, &fme.fm);
	if (ret != 0 || fme.fm.fm_mapped_extents != 1) {
		fprintf(stderr, "Failed to find log header block address\n");
		return 0;
	}
	return fme.fe.fe_physical / bsize;
}
#endif

static int alloc_new_journal(int fd, unsigned bytes)
{
#define ALLOC_BUF_SIZE (4 << 20)
	unsigned left = bytes;
	int error;
	char *buf;

	error = fallocate(fd, 0, 0, bytes);
	if (error == 0)
	       return 0;
	if (errno != EOPNOTSUPP)
		goto out_errno;

	/* No fallocate support, fall back to writes */
	buf = calloc(1, ALLOC_BUF_SIZE);
	if (buf == NULL)
		goto out_errno;

	while (left > 0) {
		unsigned sz = ALLOC_BUF_SIZE;

		if (left < ALLOC_BUF_SIZE)
			sz = left;

		if (pwrite(fd, buf, sz, bytes - left) != sz) {
			free(buf);
			goto out_errno;
		}
		left -= sz;
	}
	free(buf);
	return 0;
out_errno:
	perror("Failed to allocate space for new journal");
	return -1;
}

static int add_j(struct gfs2_sbd *sdp, struct jadd_opts *opts)
{
	int fd, error = 0;
	char new_name[256], buf[sdp->bsize];
	unsigned int x, blocks =
		sdp->jsize << (20 - sdp->sd_sb.sb_bsize_shift);
	struct gfs2_log_header lh;
	uint64_t seq = RANDOM(blocks), addr;
	off_t off = 0;

	if ((fd = create_new_inode(opts, &addr)) < 0)
		return fd;

	if ((error = set_flags(fd, JA_FL_CLEAR, FS_JOURNAL_DATA_FL)))
		goto close_fd;

	error = alloc_new_journal(fd, sdp->jsize << 20);
	if (error != 0)
		goto close_fd;

	if ((error = lseek(fd, 0, SEEK_SET)) < 0) {
		perror("add_j lseek");
		goto close_fd;
	}

	memset(&lh, 0, sizeof(struct gfs2_log_header));
	lh.lh_header.mh_magic = GFS2_MAGIC;
	lh.lh_header.mh_type = GFS2_METATYPE_LH;
	lh.lh_header.mh_format = GFS2_FORMAT_LH;
	lh.lh_flags = GFS2_LOG_HEAD_UNMOUNT;
#ifdef GFS2_HAS_LH_V2
	lh.lh_flags |= GFS2_LOG_HEAD_USERSPACE;
	lh.lh_jinode = addr;
#endif
	for (x=0; x<blocks; x++) {
		uint32_t hash;
#ifdef GFS2_HAS_LH_V2
		uint64_t blk_addr = 0;
#endif
		lh.lh_sequence = seq;
		lh.lh_blkno = x;
		gfs2_log_header_out(&lh, buf);
		hash = lgfs2_log_header_hash(buf);
		((struct gfs2_log_header *)buf)->lh_hash = cpu_to_be32(hash);
#ifdef GFS2_HAS_LH_V2
		if (!(blk_addr = find_block_address(fd, off, sdp->bsize))) {
			error = -1;
			goto close_fd;
		}
		((struct gfs2_log_header *)buf)->lh_addr = cpu_to_be64(blk_addr);
		hash = lgfs2_log_header_crc(buf, sdp->bsize);
		((struct gfs2_log_header *)buf)->lh_crc = cpu_to_be32(hash);
#endif
		if (write(fd, buf, sdp->bsize) != sdp->bsize) {
			perror("add_j write");
			error = -1;
			goto close_fd;
		}

		if (++seq == blocks)
			seq = 0;
		off += sdp->bsize;

		if ((error = fsync(fd))) {
			perror("add_j fsync");
			goto close_fd;
		}
	}

	sprintf(new_name, "journal%u", opts->journals);
	error = rename2system(opts, opts->jindex, new_name);
	if (error < 0 && errno != EEXIST){
		perror("add_j rename2system");
		goto close_fd;
	}
close_fd:
	return close(fd) || error;
}

static int check_fit(struct gfs2_sbd *sdp, struct jadd_opts *opts)
{
	/* Compute how much space we'll need for the new journals
	 * Number of blocks needed per added journal:
	 * 1 block for the ir inode
	 * 1 block for the sc inode
	 * for sizes of the qc and journal inodes, use lgfs2_space_for_data()
	 * to calculate.
	 */
	uint64_t blks_per_j, total_blks;

	blks_per_j = 1 + 1 +
		lgfs2_space_for_data(sdp, sdp->bsize, sdp->qcsize << 20) +
		lgfs2_space_for_data(sdp, sdp->bsize, sdp->jsize << 20);
	total_blks = opts->journals * blks_per_j;

	if (total_blks > (sdp->blks_total - sdp->blks_alloced)) {
		printf( _("\nInsufficient space on the device to add %u %uMB "
			  "journals (%uMB QC size)\n\n"),
			opts->journals, sdp->jsize, sdp->qcsize);
		printf( _("Required space  : %*lu blks (%lu blks per "
			  "journal)\n"), 10, total_blks, blks_per_j);
		printf( _("Available space : %*lu blks\n\n"), 10,
			sdp->blks_total - sdp->blks_alloced);
		errno = ENOSPC;
		return -1;
	}
	return 0;
}

int main(int argc, char *argv[])
{
	struct jadd_opts opts = {0};
	struct gfs2_sbd sbd, *sdp = &sbd;
	struct metafs mfs = {0};
	struct mntent *mnt;
	unsigned int total, ret = 0;

	setlocale(LC_ALL, "");
	textdomain("gfs2-utils");
	srandom(time(NULL) ^ getpid());

	memset(sdp, 0, sizeof(struct gfs2_sbd));
	sdp->jsize = GFS2_DEFAULT_JSIZE;
	sdp->qcsize = GFS2_DEFAULT_QCSIZE;
	opts.journals = 1;

	decode_arguments(argc, argv, sdp, &opts);
	verify_arguments(sdp, &opts);

	sbd.path_fd = lgfs2_open_mnt_dir(opts.path, O_RDONLY|O_CLOEXEC, &mnt);
	if (sbd.path_fd < 0) {
		fprintf(stderr, "Error looking up mount '%s': %s\n",
			opts.path, strerror(errno));
		ret = -1;
		goto out;
	}
	if (mnt == NULL) {
		fprintf(stderr, "%s: not a mounted gfs2 file system\n", opts.path);
		ret = -1;
		goto close_sb;
	}

	if ((ret = gather_info(sdp, &opts)))
		goto close_sb;

	mfs.context = copy_context_opt(mnt);
	if ((ret = mount_gfs2_meta(&mfs, mnt->mnt_dir, opts.debug))) {
		perror("GFS2 metafs");
		goto close_sb;
	}

	if ((ret = build_paths(mfs.path, &opts))) {
		perror(_("Failed to build paths"));
		goto umount_meta;
	}

	if ((ret = compute_constants(sdp))) {
		perror(_("Failed to compute file system constants"));
		goto free_paths;
	}

	if ((ret = find_current_journals(&opts)))
		goto free_paths;

	if ((ret = check_fit(sdp, &opts))) {
		perror(_("Failed to add journals"));
		goto free_paths;
	}

	total = opts.orig_journals + opts.journals;
	for (opts.journals = opts.orig_journals;
	     opts.journals < total;
	     opts.journals++) {
		if (metafs_interrupted) {
			errno = 130;
			goto free_paths;
		}
		if ((ret = add_ir(&opts)))
			goto free_paths;
		if ((ret = add_sc(&opts)))
			goto free_paths;
		if ((ret = add_qc(sdp, &opts)))
			goto free_paths;
		if ((ret = add_j(sdp, &opts)))
			goto free_paths;
	}

free_paths:
	free(opts.new_inode);
	free(opts.per_node);
	free(opts.jindex);
umount_meta:
	sync();
	cleanup_metafs(&mfs);
close_sb:
	close(sdp->path_fd);
out:
	if (!ret)
		print_results(&opts);

	return ret;
}
