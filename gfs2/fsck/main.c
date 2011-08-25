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
#define _(String) gettext(String)

#include "copyright.cf"
#include "libgfs2.h"
#include "fsck.h"
#include "osi_list.h"
#include "metawalk.h"
#include "util.h"

struct gfs2_options opts = {0};
struct gfs2_inode *lf_dip = NULL; /* Lost and found directory inode */
struct gfs2_bmap *bl = NULL;
uint64_t last_fs_block, last_reported_block = -1;
int64_t last_reported_fblock = -1000000;
int skip_this_pass = FALSE, fsck_abort = FALSE;
int errors_found = 0, errors_corrected = 0;
const char *pass = "";
uint64_t last_data_block;
uint64_t first_data_block;
int preen = 0, force_check = 0;
struct osi_root dup_blocks = (struct osi_root) { NULL, };
struct osi_root dirtree = (struct osi_root) { NULL, };
struct osi_root inodetree = (struct osi_root) { NULL, };
int dups_found = 0, dups_found_first = 0;
struct gfs_sb *sbd1 = NULL;

/* This function is for libgfs2's sake.                                      */
void print_it(const char *label, const char *fmt, const char *fmt2, ...)
{
	va_list args;

	va_start(args, fmt2);
	printf("%s: ", label);
	vprintf(fmt, args);
	va_end(args);
}

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
			gopts->no = 1;
			break;
		case 'p':
			preen = 1;
			gopts->yes = 1;
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
		sprintf(progress, _("processing block %llu out of %llu\n"),
			(unsigned long long)last_reported_block,
			(unsigned long long)last_fs_block);
	
	response = generic_interrupt("gfs2_fsck", pass, progress,
				     _("Do you want to abort gfs2_fsck, skip " \
				     "the rest of this pass or continue " \
				     "(a/s/c)?"), "asc");
	if (tolower(response) == 's') {
		skip_this_pass = TRUE;
		return;
	}
	else if (tolower(response) == 'a') {
		fsck_abort = TRUE;
		return;
	}
}

static void check_statfs(struct gfs2_sbd *sdp)
{
	struct osi_node *n, *next = NULL;
	struct rgrp_tree *rgd;
	struct gfs2_rindex *ri;
	struct gfs2_statfs_change sc;
	char buf[sizeof(struct gfs2_statfs_change)];
	int count;

	if (sdp->gfs1 && !sdp->md.statfs->i_di.di_size) {
		log_info("This GFS1 file system is not using fast_statfs.\n");
		return;
	}
	/* Read the current statfs values */
	count = gfs2_readi(sdp->md.statfs, buf, 0,
			   sdp->md.statfs->i_di.di_size);
	if (count == sizeof(struct gfs2_statfs_change))
		gfs2_statfs_change_in(&sc, buf);

	/* Calculate the real values from the rgrp information */
	sdp->blks_total = 0;
	sdp->blks_alloced = 0;
	sdp->dinodes_alloced = 0;

	for (n = osi_first(&sdp->rgtree); n; n = next) {
		next = osi_next(n);
		rgd = (struct rgrp_tree *)n;
		ri = &rgd->ri;
		sdp->blks_total += ri->ri_data;
		sdp->blks_alloced += (ri->ri_data - rgd->rg.rg_free);
		sdp->dinodes_alloced += rgd->rg.rg_dinodes;
	}

	/* See if they match */
	if (sc.sc_total == sdp->blks_total &&
	    sc.sc_free == (sdp->blks_total - sdp->blks_alloced) &&
	    sc.sc_dinodes == sdp->dinodes_alloced) {
		log_info( _("The statfs file is accurate.\n"));
		return;
	}
	log_err( _("The statfs file is wrong:\n\n"));
	log_err( _("Current statfs values:\n"));
	log_err( _("blocks:  %lld (0x%llx)\n"),
		 (unsigned long long)sc.sc_total,
		 (unsigned long long)sc.sc_total);
	log_err( _("free:    %lld (0x%llx)\n"),
		 (unsigned long long)sc.sc_free,
		 (unsigned long long)sc.sc_free);
	log_err( _("dinodes: %lld (0x%llx)\n\n"),
		 (unsigned long long)sc.sc_dinodes,
		 (unsigned long long)sc.sc_dinodes);

	log_err( _("Calculated statfs values:\n"));
	log_err( _("blocks:  %lld (0x%llx)\n"),
		 (unsigned long long)sdp->blks_total,
		 (unsigned long long)sdp->blks_total);
	log_err( _("free:    %lld (0x%llx)\n"),
		 (unsigned long long)(sdp->blks_total - sdp->blks_alloced),
		 (unsigned long long)(sdp->blks_total - sdp->blks_alloced));
	log_err( _("dinodes: %lld (0x%llx)\n"),
		 (unsigned long long)sdp->dinodes_alloced,
		 (unsigned long long)sdp->dinodes_alloced);

	errors_found++;
	if (!query( _("Okay to fix the master statfs file? (y/n)"))) {
		log_err( _("The statfs file was not fixed.\n"));
		return;
	}

	do_init_statfs(sdp);
	log_err( _("The statfs file was fixed.\n"));
	errors_corrected++;
}

int main(int argc, char **argv)
{
	struct gfs2_sbd sb;
	struct gfs2_sbd *sdp = &sb;
	int j;
	int error = 0;
	int all_clean = 0;

	setlocale(LC_ALL, "");
	textdomain("gfs2-utils");

	memset(sdp, 0, sizeof(*sdp));

	if ((error = read_cmdline(argc, argv, &opts)))
		exit(error);
	setbuf(stdout, NULL);
	log_notice( _("Initializing fsck\n"));
	if ((error = initialize(sdp, force_check, preen, &all_clean)))
		exit(error);

	if (!force_check && all_clean && preen) {
		log_err( _("%s: clean.\n"), opts.device);
		destroy(sdp);
		exit(FSCK_OK);
	}

	signal(SIGINT, interrupt);
	log_notice( _("Starting pass1\n"));
	pass = "pass 1";
	last_reported_block = 0;
	if ((error = pass1(sdp)))
		exit(error);
	if (skip_this_pass || fsck_abort) {
		skip_this_pass = FALSE;
		log_notice( _("Pass1 interrupted   \n"));
	}
	else
		log_notice( _("Pass1 complete      \n"));

	if (!fsck_abort) {
		last_reported_block = 0;
		pass = "pass 1b";
		log_notice( _("Starting pass1b\n"));
		if ((error = pass1b(sdp)))
			exit(error);
		if (skip_this_pass || fsck_abort) {
			skip_this_pass = FALSE;
			log_notice( _("Pass1b interrupted   \n"));
		}
		else
			log_notice( _("Pass1b complete\n"));
	}
	if (!fsck_abort) {
		last_reported_block = 0;
		pass = "pass 1c";
		log_notice( _("Starting pass1c\n"));
		if ((error = pass1c(sdp)))
			exit(error);
		if (skip_this_pass || fsck_abort) {
			skip_this_pass = FALSE;
			log_notice( _("Pass1c interrupted   \n"));
		}
		else
			log_notice( _("Pass1c complete\n"));
	}
	if (!fsck_abort) {
		last_reported_block = 0;
		pass = "pass 2";
		log_notice( _("Starting pass2\n"));
		if ((error = pass2(sdp)))
			exit(error);
		if (skip_this_pass || fsck_abort) {
			skip_this_pass = FALSE;
			log_notice( _("Pass2 interrupted   \n"));
		}
		else
			log_notice( _("Pass2 complete      \n"));
	}
	if (!fsck_abort) {
		last_reported_block = 0;
		pass = "pass 3";
		log_notice( _("Starting pass3\n"));
		if ((error = pass3(sdp)))
			exit(error);
		if (skip_this_pass || fsck_abort) {
			skip_this_pass = FALSE;
			log_notice( _("Pass3 interrupted   \n"));
		}
		else
			log_notice( _("Pass3 complete      \n"));
	}
	if (!fsck_abort) {
		last_reported_block = 0;
		pass = "pass 4";
		log_notice( _("Starting pass4\n"));
		if ((error = pass4(sdp)))
			exit(error);
		if (skip_this_pass || fsck_abort) {
			skip_this_pass = FALSE;
			log_notice( _("Pass4 interrupted   \n"));
		}
		else
			log_notice( _("Pass4 complete      \n"));
	}
	if (!fsck_abort) {
		last_reported_block = 0;
		pass = "pass 5";
		log_notice( _("Starting pass5\n"));
		if ((error = pass5(sdp)))
			exit(error);
		if (skip_this_pass || fsck_abort) {
			skip_this_pass = FALSE;
			log_notice( _("Pass5 interrupted   \n"));
			error = FSCK_CANCELED;
		}
		else
			log_notice( _("Pass5 complete      \n"));
	} else {
		error = FSCK_CANCELED;
	}

	if (!fsck_abort)
		check_statfs(sdp);

	/* Free up our system inodes */
	if (!sdp->gfs1)
		inode_put(&sdp->md.inum);
	inode_put(&sdp->md.statfs);
	for (j = 0; j < sdp->md.journals; j++)
		inode_put(&sdp->md.journal[j]);
	inode_put(&sdp->md.jiinode);
	inode_put(&sdp->md.riinode);
	inode_put(&sdp->md.qinode);
	if (!sdp->gfs1)
		inode_put(&sdp->md.pinode);
	inode_put(&sdp->md.rooti);
	if (!sdp->gfs1)
		inode_put(&sdp->master_dir);
	if (lf_dip)
		inode_put(&lf_dip);

	if (!opts.no && errors_corrected)
		log_notice( _("Writing changes to disk\n"));
	fsync(sdp->device_fd);
	destroy(sdp);
	log_notice( _("gfs2_fsck complete\n"));

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
