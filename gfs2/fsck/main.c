#include "clusterautoconfig.h"

#include <unistd.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <libgen.h>
#include <string.h>
#include <stdarg.h>
#include <ctype.h>
#include <signal.h>
#include <libintl.h>
#include <locale.h>
#include <sys/time.h>
#include <time.h>
#define _(String) gettext(String)
#include <syslog.h>

#include <logging.h>
#include "copyright.cf"
#include "libgfs2.h"
#include "fsck.h"
#include "link.h"
#include "osi_list.h"
#include "metawalk.h"
#include "util.h"

struct gfs2_options opts = {0};
struct lgfs2_inode *lf_dip = NULL; /* Lost and found directory inode */
int lf_was_created = 0;
uint64_t last_fs_block, last_reported_block = -1;
int64_t last_reported_fblock = -1000000;
int skip_this_pass = 0, fsck_abort = 0;
int errors_found = 0, errors_corrected = 0;
uint64_t last_data_block;
uint64_t first_data_block;
struct osi_root dup_blocks;
struct osi_root dirtree;
struct osi_root inodetree;
int dups_found = 0, dups_found_first = 0;
int sb_fixed = 0;
int print_level = MSG_NOTICE;

static int preen = 0;
static int force_check = 0;
static const char *pass_name = "";

static void usage(char *name)
{
	printf("Usage: %s [-afhnpqvVy] <device> \n", basename(name));
}

static void version(void)
{
	printf( _("GFS2 fsck %s (built %s %s)\n"),
	       VERSION, __DATE__, __TIME__);
	printf(REDHAT_COPYRIGHT "\n");
}

static int read_cmdline(int argc, char **argv, struct gfs2_options *gopts)
{
	int c;

	while ((c = getopt(argc, argv, "afhnpqvyV")) != -1) {
		switch(c) {

		case 'a':
		case 'p':
			if (gopts->yes || gopts->no) {
				fprintf(stderr, _("Options -p/-a, -y and -n may not be used together\n"));
				return FSCK_USAGE;
			}
			preen = 1;
			gopts->yes = 1;
			break;
		case 'f':
			force_check = 1;
			break;
		case 'h':
			usage(argv[0]);
			exit(FSCK_OK);
			break;
		case 'n':
			if (gopts->yes || preen) {
				fprintf(stderr, _("Options -p/-a, -y and -n may not be used together\n"));
				return FSCK_USAGE;
			}
			gopts->no = 1;
			break;
		case 'q':
			decrease_verbosity();
			break;
		case 'v':
			increase_verbosity();
			break;
		case 'V':
			version();
			exit(FSCK_OK);
			break;
		case 'y':
			if (gopts->no || preen) {
				fprintf(stderr, _("Options -p/-a, -y and -n may not be used together\n"));
				return FSCK_USAGE;
			}
			gopts->yes = 1;
			break;
		case ':':
		case '?':
			fprintf(stderr, _("Please use '-h' for help.\n"));
			return FSCK_USAGE;
		default:
			fprintf(stderr, _("Invalid option %c\n"), c);
			return FSCK_USAGE;

		}
	}
	if (argc > optind) {
		gopts->device = (argv[optind]);
		if (!gopts->device) {
			fprintf(stderr, _("Please use '-h' for help.\n"));
			return FSCK_USAGE;
		}
	} else {
		fprintf(stderr, _("No device specified (Please use '-h' for help)\n"));
		return FSCK_USAGE;
	}
	return 0;
}

static void interrupt(int sig)
{
	char response;
	char progress[PATH_MAX];

	if (!last_reported_block || last_reported_block == last_fs_block)
		sprintf(progress, _("progress unknown.\n"));
	else
		sprintf(progress, _("processing block %"PRIu64" out of %"PRIu64"\n"),
		        last_reported_block, last_fs_block);

	response = generic_interrupt("fsck.gfs2", pass_name, progress,
				     _("Do you want to abort fsck.gfs2, skip " \
				     "the rest of this pass or continue " \
				     "(a/s/c)?"), "asc");
	if (tolower(response) == 's') {
		skip_this_pass = 1;
		return;
	}
	else if (tolower(response) == 'a') {
		fsck_abort = 1;
		return;
	}
}

static int check_statfs(struct fsck_cx *cx)
{
	struct osi_node *n, *next = NULL;
	struct lgfs2_rgrp_tree *rgd;
	struct gfs2_statfs_change sc;
	struct lgfs2_sbd *sdp = cx->sdp;
	uint64_t sc_total;
	uint64_t sc_free;
	uint64_t sc_dinodes;
	int count;

	if (sdp->gfs1 && !sdp->md.statfs->i_size) {
		log_info("This GFS1 file system is not using fast_statfs.\n");
		return 0;
	}
	/* Read the current statfs values */
	count = lgfs2_readi(sdp->md.statfs, &sc, 0, sdp->md.statfs->i_size);
	if (count != sizeof(struct gfs2_statfs_change)) {
		log_err(_("Failed to read statfs values (%d of %"PRIu64" read)\n"),
		        count, sdp->md.statfs->i_size);
		return FSCK_ERROR;
	}
	sc_total = be64_to_cpu(sc.sc_total);
	sc_free = be64_to_cpu(sc.sc_free);
	sc_dinodes = be64_to_cpu(sc.sc_dinodes);

	/* Calculate the real values from the rgrp information */
	sdp->blks_total = 0;
	sdp->blks_alloced = 0;
	sdp->dinodes_alloced = 0;

	for (n = osi_first(&sdp->rgtree); n; n = next) {
		next = osi_next(n);
		rgd = (struct lgfs2_rgrp_tree *)n;
		sdp->blks_total += rgd->rt_data;
		sdp->blks_alloced += (rgd->rt_data - rgd->rt_free);
		sdp->dinodes_alloced += rgd->rt_dinodes;
	}

	/* See if they match */
	if (sc_total == sdp->blks_total &&
	    sc_free == (sdp->blks_total - sdp->blks_alloced) &&
	    sc_dinodes == sdp->dinodes_alloced) {
		log_info( _("The statfs file is accurate.\n"));
		return 0;
	}
	log_err( _("The statfs file is wrong:\n\n"));
	log_err( _("Current statfs values:\n"));
	log_err( _("blocks:  %"PRId64" (0x%"PRIx64")\n"), sc_total, sc_total);
	log_err( _("free:    %"PRId64" (0x%"PRIx64")\n"), sc_free, sc_free);
	log_err( _("dinodes: %"PRId64" (0x%"PRIx64")\n\n"), sc_dinodes, sc_dinodes);
	log_err( _("Calculated statfs values:\n"));
	log_err( _("blocks:  %"PRIu64" (0x%"PRIx64")\n"),
	        sdp->blks_total, sdp->blks_total);
	log_err( _("free:    %"PRIu64" (0x%"PRIx64")\n"),
	        (sdp->blks_total - sdp->blks_alloced),
	        (sdp->blks_total - sdp->blks_alloced));
	log_err( _("dinodes: %"PRIu64" (0x%"PRIx64")\n"),
	        sdp->dinodes_alloced, sdp->dinodes_alloced);

	errors_found++;
	if (!query( _("Okay to fix the master statfs file? (y/n)"))) {
		log_err( _("The statfs file was not fixed.\n"));
		return 0;
	}

	lgfs2_init_statfs(sdp, NULL);
	log_err( _("The statfs file was fixed.\n"));
	errors_corrected++;
	return 0;
}

static const struct fsck_pass passes[] = {
	{ .name = "pass1",  .f = pass1 },
	{ .name = "pass1b", .f = pass1b },
	{ .name = "pass2",  .f = pass2 },
	{ .name = "pass3",  .f = pass3 },
	{ .name = "pass4",  .f = pass4 },
	{ .name = "check_statfs", .f = check_statfs },
	{ .name = NULL, }
};

static int fsck_pass(const struct fsck_pass *p, struct fsck_cx *cx)
{
	int ret;
	struct timeval timer;

	if (fsck_abort)
		return FSCK_CANCELED;
	pass_name = p->name;

	log_notice( _("Starting %s\n"), p->name);
	gettimeofday(&timer, NULL);

	ret = p->f(cx);
	if (ret)
		exit(ret);
	if (skip_this_pass || fsck_abort) {
		skip_this_pass = 0;
		log_notice( _("%s interrupted   \n"), p->name);
		return FSCK_CANCELED;
	}

	print_pass_duration(p->name, &timer);
	return 0;
}

/*
 * on_exit() is non-standard but useful for reporting the exit status if it's
 * available.
 */
#ifdef HAVE_ON_EXIT
static void exitlog(int status, void *unused)
{
	syslog(LOG_INFO, "exit: %d", status);
}
#else
static void exitlog(void)
{
	syslog(LOG_INFO, "exit.");
}
#endif

static void startlog(int argc, char **argv)
{
	int i;
	char *cmd, *p;
	size_t len;

	for (len = i = 0; i < argc; i++)
		len += strlen(argv[i]);
	len += argc; /* Add spaces and '\0' */

	cmd = malloc(len);
	if (cmd == NULL) {
		perror(argv[0]);
		exit(FSCK_ERROR);
	}
	p = cmd;
	for (i = 0; i < argc; i++, p++) {
		p = stpcpy(p, argv[i]);
		*p = ' ';
	}
	*(--p) = '\0';
	syslog(LOG_INFO, "started: %s", cmd);
	free(cmd);
}

#ifndef UNITTESTS
int main(int argc, char **argv)
{
	struct lgfs2_sbd sb;
	struct fsck_cx cx = {
		.sdp = &sb
	};
	int j;
	int i;
	int error = 0;
	int all_clean = 0;
	struct sigaction act = { .sa_handler = interrupt, };

	setlocale(LC_ALL, "");
	textdomain("gfs2-utils");

	openlog("fsck.gfs2", LOG_CONS|LOG_PID, LOG_USER);
	startlog(argc - 1, &argv[1]);
#ifdef HAVE_ON_EXIT
	on_exit(exitlog, NULL);
#else
	atexit(exitlog);
#endif

	memset(&sb, 0, sizeof(sb));

	if ((error = read_cmdline(argc, argv, &opts)))
		exit(error);
	setbuf(stdout, NULL);
	log_notice( _("Initializing fsck\n"));
	if ((error = initialize(&cx, force_check, preen, &all_clean)))
		exit(error);

	if (!force_check && all_clean && preen) {
		log_err( _("%s: clean.\n"), opts.device);
		destroy(&sb);
		exit(FSCK_OK);
	}

	sigaction(SIGINT, &act, NULL);

	for (i = 0; passes[i].name; i++)
		error = fsck_pass(passes + i, &cx);

	/* Free up our system inodes */
	if (!sb.gfs1)
		lgfs2_inode_put(&sb.md.inum);
	lgfs2_inode_put(&sb.md.statfs);
	for (j = 0; j < sb.md.journals; j++)
		lgfs2_inode_put(&sb.md.journal[j]);
	free(sb.md.journal);
	sb.md.journal = NULL;
	lgfs2_inode_put(&sb.md.jiinode);
	lgfs2_inode_put(&sb.md.riinode);
	lgfs2_inode_put(&sb.md.qinode);
	if (!sb.gfs1)
		lgfs2_inode_put(&sb.md.pinode);
	lgfs2_inode_put(&sb.md.rooti);
	if (!sb.gfs1)
		lgfs2_inode_put(&sb.master_dir);
	if (lf_dip)
		lgfs2_inode_put(&lf_dip);

	if (!opts.no && errors_corrected)
		log_notice( _("Writing changes to disk\n"));
	fsync(sb.device_fd);
	link1_destroy(&nlink1map);
	link1_destroy(&clink1map);
	destroy(&sb);
	if (sb_fixed)
		log_warn(_("Superblock was reset. Use tunegfs2 to manually "
		           "set lock table before mounting.\n"));
	log_notice( _("fsck.gfs2 complete\n"));

	if (!error) {
		if (!errors_found)
			error = FSCK_OK;
		else if (errors_found == errors_corrected)
			error = FSCK_NONDESTRUCT;
		else
			error = FSCK_UNCORRECTED;
	}
	exit(error);
}
#endif /* UNITTESTS */
