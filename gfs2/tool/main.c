#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <limits.h>
#include <errno.h>
#include <linux/types.h>
#include <libintl.h>
#include <locale.h>
#define _(String) gettext(String)

#include "copyright.cf"

#include "gfs2_tool.h"
#include "libgfs2.h"

char *action = NULL;
int override = FALSE;
int expert = FALSE;
int debug = FALSE;
int continuous = FALSE;
int interval = 1;
int output_type = OUTPUT_BLOCKS;

static const char *usage = {
	"Clear a flag on a inode\n"
	"  gfs2_tool clearflag flag <filenames>\n"
	"Freeze a GFS2 cluster:\n"
	"  gfs2_tool freeze <mountpoint>\n"
	"Get tuneable parameters for a filesystem\n"
	"  gfs2_tool gettune <mountpoint>\n"
	"List the file system's journals:\n"
	"  gfs2_tool journals <mountpoint>\n"
	"Have GFS2 dump its lock state:\n"
	"  gfs2_tool lockdump <mountpoint> [buffersize]\n"
	"Tune a GFS2 superblock\n"
	"  gfs2_tool sb <device> proto [newval]\n"
	"  gfs2_tool sb <device> table [newval]\n"
	"  gfs2_tool sb <device> ondisk [newval]\n"
	"  gfs2_tool sb <device> multihost [newval]\n"
	"  gfs2_tool sb <device> all\n"
	"Set a flag on a inode\n"
	"  gfs2_tool setflag flag <filenames>\n"
	"Tune a running filesystem\n"
	"  gfs2_tool settune <mountpoint> <parameter> <value>\n"
	"Unfreeze a GFS2 cluster:\n"
	"  gfs2_tool unfreeze <mountpoint>\n"
	"Print tool version information\n"
	"  gfs2_tool version\n"
	"Withdraw this machine from participating in a filesystem:\n"
	"  gfs2_tool withdraw <mountpoint>\n"
};

/**
 * print_usage - print out usage information
 *
 */

void print_usage(void)
{
	puts( _(usage) );
}

/**
 * print_version -
 *
 */

static void print_version(void)
{
	printf( _("gfs2_tool " RELEASE_VERSION " (built " __DATE__ " " __TIME__ ")\n"));
	puts( _(REDHAT_COPYRIGHT "\n") );
}

/**
 * decode_arguments -
 * @argc:
 * @argv:
 *
 */

static void decode_arguments(int argc, char *argv[])
{
	int cont = TRUE;
	int optchar;

	output_type = OUTPUT_BLOCKS;
	while (cont) {
		optchar = getopt(argc, argv, "cDhHki:OVX");

		switch (optchar) {
		case 'c':
			continuous = TRUE;
			break;

		case 'D':
			debug = TRUE;
			break;

		case 'h':
			print_usage();
			exit(0);

		case 'H':
			output_type = OUTPUT_HUMAN;
			break;

		case 'i':
			sscanf(optarg, "%u", &interval);
			break;

		case 'k':
			output_type = OUTPUT_K;
			break;

		case 'O':
			override = TRUE;
			break;

		case 'V':
			print_version();
			exit(0);

		case 'X':
			expert = TRUE;
			break;

		case EOF:
			cont = FALSE;
			break;

		default:
			die( _("unknown option: %c\n"), optchar);
		};
	}

	if (optind < argc) {
		action = argv[optind];
		optind++;
	} else {
		die( _("no action specified\n"));
	}
}

/**
 * main - Do everything
 * @argc:
 * @argv:
 *
 */

int main(int argc, char *argv[])
{
	setlocale(LC_ALL, "");
	textdomain("gfs2-utils");

	if (argc < 2) {
		print_usage();
		return 0;
	}

	decode_arguments(argc, argv);

	if (strcmp(action, "clearflag") == 0)
		set_flag(argc, argv);
	else if (strcmp(action, "freeze") == 0)
		do_freeze(argc, argv);
	else if (strcmp(action, "gettune") == 0)
		get_tune(argc, argv);
	else if (strcmp(action, "journals") == 0)
		print_journals(argc, argv);
	else if (strcmp(action, "lockdump") == 0)
		print_lockdump(argc, argv);
	else if (strcmp(action, "sb") == 0)
		do_sb(argc, argv);
	else if (strcmp(action, "setflag") == 0)
		set_flag(argc, argv);
	else if (strcmp(action, "settune") == 0)
		set_tune(argc, argv);
	else if (strcmp(action, "unfreeze") == 0)
		do_freeze(argc, argv);
	else if (strcmp(action, "version") == 0)
		print_version();
	else if (strcmp(action, "withdraw") == 0)
		do_withdraw(argc, argv);
	else
		die( _("unknown action: %s\n"), action);

	return 0;
}
