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
#define _(String) gettext(String)

#include "copyright.cf"
#include "libgfs2.h"
#include "fsck.h"
#include "osi_list.h"

struct gfs2_options opts = {0};
struct gfs2_inode *lf_dip; /* Lost and found directory inode */
osi_list_t dir_hash[FSCK_HASH_SIZE];
osi_list_t inode_hash[FSCK_HASH_SIZE];
struct gfs2_block_list *bl;
uint64_t last_fs_block, last_reported_block = -1;
int skip_this_pass = FALSE, fsck_abort = FALSE;
int errors_found = 0, errors_corrected = 0;
const char *pass = "";
uint64_t last_data_block;
uint64_t first_data_block;
const char *prog_name = "gfs2_fsck"; /* needed by libgfs2 */

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
	printf( _("Usage: %s [-hnqvVy] <device> \n"), basename(name));
}

static void version(void)
{
	printf( _("GFS2 fsck %s (built %s %s)\n"),
	       RELEASE_VERSION, __DATE__, __TIME__);
	printf( _(REDHAT_COPYRIGHT "\n"));
}

static int read_cmdline(int argc, char **argv, struct gfs2_options *gopts)
{
	int c;

	while((c = getopt(argc, argv, "hnqvyV")) != -1) {
		switch(c) {

		case 'h':
			usage(argv[0]);
			exit(FSCK_OK);
			break;
		case 'n':
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
			gopts->yes = 1;
			break;
		case ':':
		case '?':
			fprintf(stderr, _("Please use '-h' for usage.\n"));
			return FSCK_USAGE;
		default:
			fprintf(stderr, _("Bad programmer! You forgot to catch"
				" the %c flag\n"), c);
			return FSCK_USAGE;

		}
	}
	if(argc > optind) {
		gopts->device = (argv[optind]);
		if(!gopts->device) {
			fprintf(stderr, _("Please use '-h' for usage.\n"));
			return FSCK_USAGE;
		}
	} else {
		fprintf(stderr, _("No device specified.  Use '-h' for usage.\n"));
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
		sprintf(progress, _("processing block %" PRIu64 " out of %"
			PRIu64 "\n"), last_reported_block, last_fs_block);
	
	response = generic_interrupt("gfs2_fsck", pass, progress,
				     _("Do you want to abort gfs2_fsck, skip " \
				     "the rest of this pass or continue " \
				     "(a/s/c)?"), "asc");
	if(tolower(response) == 's') {
		skip_this_pass = TRUE;
		return;
	}
	else if (tolower(response) == 'a') {
		fsck_abort = TRUE;
		return;
	}
}

/* Check system inode and verify it's marked "in use" in the bitmap:       */
/* Should work for all system inodes: root, master, jindex, per_node, etc. */
static int check_system_inode(struct gfs2_inode *sysinode, const char *filename,
		       int builder(struct gfs2_sbd *sbp),
		       enum gfs2_mark_block mark)
{
	uint64_t iblock = 0;
	struct dir_status ds = {0};

	log_info( _("Checking system inode '%s'\n"), filename);
	if (sysinode) {
		/* Read in the system inode, look at its dentries, and start
		 * reading through them */
		iblock = sysinode->i_di.di_num.no_addr;
		log_info( _("System inode for '%s' is located at block %"
			 PRIu64 " (0x%" PRIx64 ")\n"), filename,
			 iblock, iblock);
		
		/* FIXME: check this block's validity */

		if(gfs2_block_check(sysinode->i_sbd, bl, iblock, &ds.q)) {
			log_crit( _("Can't get %s inode block %" PRIu64 " (0x%"
				 PRIx64 ") from block list\n"), filename,
				 iblock, iblock);
			return -1;
		}
		/* If the inode exists but the block is marked      */
		/* free, we might be recovering from a corrupt      */
		/* bitmap.  In that case, don't rebuild the inode.  */
		/* Just reuse the inode and fix the bitmap.         */
		if (ds.q.block_type == gfs2_block_free) {
			log_info( _("The inode exists but the block is not marked 'in use'; fixing it.\n"));
			gfs2_block_set(sysinode->i_sbd, bl,
				       sysinode->i_di.di_num.no_addr,
				       mark);
			ds.q.block_type = mark;
			if (mark == gfs2_inode_dir)
				add_to_dir_list(sysinode->i_sbd,
						sysinode->i_di.di_num.no_addr);
		}
	}
	else
		log_info( _("System inode for '%s' is missing.\n"), filename);
	/* If there are errors with the inode here, we need to
	 * create a new inode and get it all setup - of course,
	 * everything will be in lost+found then, but we *need* our
	 * system inodes before we can do any of that. */
	if(!sysinode || ds.q.block_type != mark) {
		log_err( _("Invalid or missing %s system inode.\n"), filename);
		errors_found++;
		if ((errors_corrected +=
		    query(&opts, _("Create new %s system inode? (y/n) "),
			  filename))) {
			builder(sysinode->i_sbd);
			gfs2_block_set(sysinode->i_sbd, bl,
				       sysinode->i_di.di_num.no_addr,
				       mark);
			ds.q.block_type = mark;
			if (mark == gfs2_inode_dir)
				add_to_dir_list(sysinode->i_sbd,
						sysinode->i_di.di_num.no_addr);
		}
		else {
			log_err( _("Cannot continue without valid %s inode\n"),
				filename);
			return -1;
		}
	}

	return 0;
}

static int check_system_inodes(struct gfs2_sbd *sdp)
{
	/*******************************************************************
	 *******  Check the system inode integrity             *************
	 *******************************************************************/
	if (check_system_inode(sdp->master_dir, "master", build_master,
			       gfs2_inode_dir)) {
		stack;
		return -1;
	}
	if (check_system_inode(sdp->md.rooti, "root", build_root,
			       gfs2_inode_dir)) {
		stack;
		return -1;
	}
	if (check_system_inode(sdp->md.inum, "inum", build_inum,
			       gfs2_inode_file)) {
		stack;
		return -1;
	}
	if (check_system_inode(sdp->md.statfs, "statfs", build_statfs,
			       gfs2_inode_file)) {
		stack;
		return -1;
	}
	if (check_system_inode(sdp->md.jiinode, "jindex", build_jindex,
			       gfs2_inode_dir)) {
		stack;
		return -1;
	}
	if (check_system_inode(sdp->md.riinode, "rindex", build_rindex,
			       gfs2_inode_file)) {
		stack;
		return -1;
	}
	if (check_system_inode(sdp->md.qinode, "quota", build_quota,
			       gfs2_inode_file)) {
		stack;
		return -1;
	}
	if (check_system_inode(sdp->md.pinode, "per_node", build_per_node,
			       gfs2_inode_dir)) {
		stack;
		return -1;
	}
	return 0;
}

int main(int argc, char **argv)
{
	struct gfs2_sbd sb;
	struct gfs2_sbd *sbp = &sb;
	int j;
	enum update_flags update_sys_files;
	int error = 0;

	setlocale(LC_ALL, "");
	textdomain("fsck.gfs2");

	memset(sbp, 0, sizeof(*sbp));

	if((error = read_cmdline(argc, argv, &opts)))
		exit(error);
	setbuf(stdout, NULL);
	log_notice( _("Initializing fsck\n"));
	if ((error = initialize(sbp)))
		exit(error);

	signal(SIGINT, interrupt);
	log_notice( _("Starting pass1\n"));
	pass = "pass 1";
	last_reported_block = 0;
	if ((error = pass1(sbp)))
		exit(error);
	if (skip_this_pass || fsck_abort) {
		skip_this_pass = FALSE;
		log_notice( _("Pass1 interrupted   \n"));
	}
	else
		log_notice( _("Pass1 complete      \n"));

	/* Make sure the system inodes are okay & represented in the bitmap. */
	check_system_inodes(sbp);

	if (!fsck_abort) {
		last_reported_block = 0;
		pass = "pass 1b";
		log_notice( _("Starting pass1b\n"));
		if((error = pass1b(sbp)))
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
		if((error = pass1c(sbp)))
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
		if ((error = pass2(sbp)))
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
		if ((error = pass3(sbp)))
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
		if ((error = pass4(sbp)))
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
		if ((error = pass5(sbp)))
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
	update_sys_files = (opts.no ? not_updated : updated);
	/* Free up our system inodes */
	inode_put(sbp->md.inum, update_sys_files);
	inode_put(sbp->md.statfs, update_sys_files);
	for (j = 0; j < sbp->md.journals; j++)
		inode_put(sbp->md.journal[j], update_sys_files);
	inode_put(sbp->md.jiinode, update_sys_files);
	inode_put(sbp->md.riinode, update_sys_files);
	inode_put(sbp->md.qinode, update_sys_files);
	inode_put(sbp->md.pinode, update_sys_files);
	inode_put(sbp->md.rooti, update_sys_files);
	inode_put(sbp->master_dir, update_sys_files);
	if (lf_dip)
		inode_put(lf_dip, update_sys_files);

	if (!opts.no)
		log_notice( _("Writing changes to disk\n"));
	bsync(&sbp->buf_list);
	bsync(&sbp->nvbuf_list);
	destroy(sbp);
	log_notice( _("gfs2_fsck complete    \n"));

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
