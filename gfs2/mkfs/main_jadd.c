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
#include <sys/file.h>
#include <sys/vfs.h>
#include <sys/mount.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <stdarg.h>
#include <libintl.h>
#define _(String) gettext(String)

#include <linux/types.h>
#include "libgfs2.h"
#include "gfs2_mkfs.h"

#define BUF_SIZE 4096
#define RANDOM(values) ((values) * (random() / (RAND_MAX + 1.0)))

static int quiet = 0;
static int debug = 0;

static void
make_jdata(int fd, const char *value)
{
        int err;
        uint32_t val;

        err = ioctl(fd, FS_IOC_GETFLAGS, &val);
        if (err){
		perror("GETFLAGS");
		exit(EXIT_FAILURE);
	}

        if (strcmp(value, "set") == 0)
                val |= FS_JOURNAL_DATA_FL;
        if (strcmp(value, "clear") == 0)
                val &= ~FS_JOURNAL_DATA_FL;
        err = ioctl(fd, FS_IOC_SETFLAGS, &val);

        if (err){
		perror("SETFLAGS");
		exit(EXIT_FAILURE);
	}
}

static int
rename2system(struct gfs2_sbd *sdp, const char *new_dir, const char *new_name)
{
	char oldpath[PATH_MAX], newpath[PATH_MAX];
	int error = 0;
	error = snprintf(oldpath, PATH_MAX, "%s/new_inode", 
			 sdp->metafs_path);
	if (error >= PATH_MAX)
		die("rename2system (1)\n");

	error = snprintf(newpath, PATH_MAX, "%s/%s/%s",
			 sdp->metafs_path, new_dir, new_name);
	if (error >= PATH_MAX)
		die("rename2system (2)\n");
	
	return rename(oldpath, newpath);
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
		   <size> and <number> here are  to commandline parameters,
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

static void decode_arguments(int argc, char *argv[], struct gfs2_sbd *sdp)
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
			debug = 1;
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
			sdp->md.journals = atoi(optarg);
			break;
		case 'q':
			quiet = 1;
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
		sdp->path_name = argv[optind];
		optind++;
	} else
		die( _("no path specified (try -h for help)\n"));

	if (optind < argc)
		die( _("Unrecognized argument: %s\n"), argv[optind]);

	if (debug) {
		printf( _("Command Line Arguments:\n"));
		printf("  qcsize = %u\n", sdp->qcsize);
		printf("  jsize = %u\n", sdp->jsize);
		printf("  journals = %u\n", sdp->md.journals);
		printf("  quiet = %d\n", quiet);
		printf("  path = %s\n", sdp->path_name);
	}
}

static void verify_arguments(struct gfs2_sbd *sdp)
{
	if (!sdp->md.journals)
		die( _("no journals specified\n"));
	if (sdp->jsize < 32 || sdp->jsize > 1024)
		die( _("bad journal size\n"));
	if (!sdp->qcsize || sdp->qcsize > 64)
		die( _("bad quota change size\n"));
}

/**
 * print_results - print out summary information
 * @sdp: the command line
 *
 */

static void print_results(struct gfs2_sbd *sdp)
{
	if (debug)
		printf("\n");
	else if (quiet)
		return;

	printf( _("Filesystem: %s\n"), sdp->path_name);
	printf( _("Old Journals: %u\n"), sdp->orig_journals);
	printf( _("New Journals: %u\n"), sdp->md.journals);

}

static int
create_new_inode(struct gfs2_sbd *sdp)
{
	char name[PATH_MAX];
	int fd;
	int error;

	error = snprintf(name, PATH_MAX, "%s/new_inode", sdp->metafs_path);
	if (error >= PATH_MAX)
		die("create_new_inode (1)\n");

	for (;;) {
		fd = open(name, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
		if (fd >= 0)
			break;
		if (errno == EEXIST) {
			error = unlink(name);
			if (error){
				perror("unlink");
				exit(EXIT_FAILURE);
			}
		} else{
			perror("create");
			exit(EXIT_FAILURE);
		}
	}
	
	return fd;
}

static void
add_ir(struct gfs2_sbd *sdp)
{
	int fd;
	char new_name[256];
	int error;

	fd = create_new_inode(sdp);

	{
		struct gfs2_inum_range ir;
		make_jdata(fd, "set");
		memset(&ir, 0, sizeof(struct gfs2_inum_range));
		if (write(fd, (void*)&ir, sizeof(struct gfs2_inum_range)) !=
		    sizeof(struct gfs2_inum_range)) {
			perror("add_ir");
			exit(EXIT_FAILURE);
		}
	}
	
	close(fd);
	
	sprintf(new_name, "inum_range%u", sdp->md.journals);
	error = rename2system(sdp, "per_node", new_name);
	if (error < 0 && errno != EEXIST){
		perror("add_ir rename2system");
		exit(EXIT_FAILURE);
	}
}

static void 
add_sc(struct gfs2_sbd *sdp)
{
	int fd;
	char new_name[256];
	int error;
	
	fd = create_new_inode(sdp);
	
	{
		struct gfs2_statfs_change sc;
		make_jdata(fd, "set");

		memset(&sc, 0, sizeof(struct gfs2_statfs_change));
		if (write(fd, (void*)&sc, sizeof(struct gfs2_statfs_change)) !=
		    sizeof(struct gfs2_statfs_change)) {
			perror("add_sc");
			exit(EXIT_FAILURE);
		}
	}

	close(fd);
	
	sprintf(new_name, "statfs_change%u", sdp->md.journals);
	error = rename2system(sdp, "per_node", new_name);
	if (error < 0 && errno != EEXIST){
		perror("add_sc rename2system");
		exit(EXIT_FAILURE);
	}
}

static void 
add_qc(struct gfs2_sbd *sdp)
{
	int fd;
	char new_name[256];
	int error;

	fd = create_new_inode(sdp);

	{
		char buf[sdp->bsize];
		unsigned int blocks =
			sdp->qcsize << (20 - sdp->sd_sb.sb_bsize_shift);
		unsigned int x;
		struct gfs2_meta_header mh;
		struct gfs2_buffer_head dummy_bh;

		dummy_bh.b_data = buf;
		make_jdata(fd, "clear");
		memset(buf, 0, sdp->bsize);

		for (x=0; x<blocks; x++) {
			if (write(fd, buf, sdp->bsize) != sdp->bsize) {
				perror("add_qc");
				exit(EXIT_FAILURE);
			}
		}

		lseek(fd, 0, SEEK_SET);
		
		memset(&mh, 0, sizeof(struct gfs2_meta_header));
		mh.mh_magic = GFS2_MAGIC;
		mh.mh_type = GFS2_METATYPE_QC;
		mh.mh_format = GFS2_FORMAT_QC;
		gfs2_meta_header_out_bh(&mh, &dummy_bh);

		for (x=0; x<blocks; x++) {
			if (write(fd, buf, sdp->bsize) != sdp->bsize) {
				perror("add_qc");
				exit(EXIT_FAILURE);
			}
		}

		error = fsync(fd);
		if (error){
			perror("add_qc fsync");
			exit(EXIT_FAILURE);
		}
	}

	close(fd);
	
	sprintf(new_name, "quota_change%u", sdp->md.journals);
	error = rename2system(sdp, "per_node", new_name);
	if (error < 0 && errno != EEXIST){
		perror("add_qc rename2system");
		exit(EXIT_FAILURE);
	}
}

static void 
gather_info(struct gfs2_sbd *sdp)
{
	struct statfs statbuf;
	if (statfs(sdp->path_name, &statbuf) < 0) {
		perror(sdp->path_name);
		exit(EXIT_FAILURE);
	}
	sdp->bsize = statbuf.f_bsize;
}

static void 
find_current_journals(struct gfs2_sbd *sdp)
{
	char jindex[PATH_MAX];
	struct dirent *dp;
	DIR *dirp;
	int existing_journals = 0;

	sprintf(jindex, "%s/jindex", sdp->metafs_path);
	dirp = opendir(jindex);
	if (!dirp) {
		perror("jindex");
		exit(EXIT_FAILURE);
	}
	while (dirp) {
		if ((dp = readdir(dirp)) != NULL) {
			if (strncmp(dp->d_name, "journal", 7) == 0)
				existing_journals++;
		} else
			goto close;
	}
close:
	closedir(dirp);
	if (existing_journals <= 0) {
		die( _("No journals found. Did you run mkfs.gfs2 correctly?\n"));
	}

	sdp->orig_journals = existing_journals;
}

static void 
add_j(struct gfs2_sbd *sdp)
{
	int fd;
	char new_name[256];
	int error;

	fd = create_new_inode(sdp);

	{
		char buf[sdp->bsize];
		unsigned int blocks =
			sdp->jsize << (20 - sdp->sd_sb.sb_bsize_shift);
		unsigned int x;
		struct gfs2_log_header lh;
		uint64_t seq = RANDOM(blocks);

		make_jdata(fd, "clear");
		memset(buf, 0, sdp->bsize);
		for (x=0; x<blocks; x++) {
			if (write(fd, buf, sdp->bsize) != sdp->bsize) {
				perror("add_j");
				exit(EXIT_FAILURE);
			}
		}

		lseek(fd, 0, SEEK_SET);

		memset(&lh, 0, sizeof(struct gfs2_log_header));
		lh.lh_header.mh_magic = GFS2_MAGIC;
		lh.lh_header.mh_type = GFS2_METATYPE_LH;
		lh.lh_header.mh_format = GFS2_FORMAT_LH;
		lh.lh_flags = GFS2_LOG_HEAD_UNMOUNT;

		for (x=0; x<blocks; x++) {
			uint32_t hash;
			struct gfs2_buffer_head dummy_bh;

			dummy_bh.b_data = buf;
			lh.lh_sequence = seq;
			lh.lh_blkno = x;
			gfs2_log_header_out(&lh, &dummy_bh);
			hash = gfs2_disk_hash(buf, sizeof(struct gfs2_log_header));
			((struct gfs2_log_header *)buf)->lh_hash = cpu_to_be32(hash);

			if (write(fd, buf, sdp->bsize) != sdp->bsize) {
				perror("add_j");
				exit(EXIT_FAILURE);
			}

			if (++seq == blocks)
				seq = 0;
		}

		error = fsync(fd);
		if (error){
			perror("add_j fsync");
			exit(EXIT_FAILURE);
		}
	}

	close(fd);
	
	sprintf(new_name, "journal%u", sdp->md.journals);
	error = rename2system(sdp, "jindex", new_name);
	if (error < 0 && errno != EEXIST){
		perror("add_j rename2system");
		exit(EXIT_FAILURE);
	}
}

/**
 * main_jadd - do everything
 * @argc:
 * @argv:
 *
 */

void main_jadd(int argc, char *argv[])
{
	struct gfs2_sbd sbd, *sdp = &sbd;
	struct mntent *mnt;
	unsigned int total;

	memset(sdp, 0, sizeof(struct gfs2_sbd));
	sdp->jsize = GFS2_DEFAULT_JSIZE;
	sdp->qcsize = GFS2_DEFAULT_QCSIZE;
	sdp->md.journals = 1;

	decode_arguments(argc, argv, sdp);
	verify_arguments(sdp);

	sbd.path_fd = lgfs2_open_mnt_dir(sbd.path_name, O_RDONLY|O_CLOEXEC, &mnt);
	if (sbd.path_fd < 0) {
		fprintf(stderr, _("Error looking up mount '%s': %s\n"), sbd.path_name, strerror(errno));
		exit(EXIT_FAILURE);
	}
	if (mnt == NULL) {
		fprintf(stderr, _("%s: not a mounted gfs2 file system\n"), sbd.path_name);
		exit(EXIT_FAILURE);
	}
	sbd.path_name = mnt->mnt_dir;
	strncpy(sbd.device_name, mnt->mnt_fsname, PATH_MAX - 1);

	gather_info(sdp);

	if (mount_gfs2_meta(sdp)) {
		perror("GFS2 metafs");
		exit(EXIT_FAILURE);
	}

	if (compute_constants(sdp)) {
		perror(_("Bad constants (1)"));
		exit(EXIT_FAILURE);
	}
	find_current_journals(sdp);

	total = sdp->orig_journals + sdp->md.journals;
	for (sdp->md.journals = sdp->orig_journals; 
	     sdp->md.journals < total;
	     sdp->md.journals++) {
		if (metafs_interrupted) {
			cleanup_metafs(&sbd);
			exit(130);
		}
		add_ir(sdp);
		add_sc(sdp);
		add_qc(sdp);
		add_j(sdp);
	}

	close(sdp->path_fd);
	cleanup_metafs(sdp);
	sync();
	print_results(sdp);
}
