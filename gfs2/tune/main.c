#include <stdio.h>
#include <stdlib.h>
#include <libgen.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>


#include <libintl.h>
#define _(String) gettext(String)
//#include <libgfs2.h>
//
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include "tunegfs2.h"

struct tunegfs2 tunegfs2_struct;
struct tunegfs2 *tfs = &tunegfs2_struct;


void parse_mount_options(char *arg)
{
	struct opt_map *m;
	char *s, *c;
	int l;
	struct opt_map {
		char *tag;
		int *flag;
		char **val;
	} map[]= {
		{ "lockproto=", &tfs->opt_proto, &tfs->proto },
		{ "locktable=", &tfs->opt_table, &tfs->table },
		{ NULL, 0, NULL }
	};

	s = arg;
	for (m = &map[0]; m->tag; m++) {
		l = strlen(m->tag);
		if (!strncmp(s, m->tag, l)) {
			*(m->flag) = 1;
			*(m->val) = s + l;
			c = strchr(*(m->val), ',');
			if (!c)
				break;
			*c='\0';
			s = c+1;
		}
	}
}

static void usage(char *name)
{
	printf("Usage: %s -L <volume label> -U <UUID> -l -o "
		"<mount options> <device> \n", basename(name));
}

static void version(void)
{
	printf( _("GFS2 tunefs (built %s %s)\n"),
	       __DATE__, __TIME__);
}


int main(int argc, char **argv)
{
	int c, status = 0;

	memset(tfs, 0, sizeof(struct tunegfs2));
	while((c = getopt(argc, argv, "hL:U:lo:V")) != -1) {
		switch(c) {
		case 'h':
			usage(argv[0]);
			break;
		case 'L':
			tfs->opt_label = 1;
			tfs->label = optarg;
			break;
		case 'U':
			tfs->opt_uuid = 1;
			tfs->uuid = optarg;
			break;
		case 'l':
			tfs->opt_list = 1;
			break;
		case 'o':
			parse_mount_options(optarg);
			break;
		case 'V':
			version();
			break;
		default:
			fprintf(stderr, _("Invalid option.\n"));
			usage(argv[0]);
			status = -EINVAL;
			goto out;
		}
	}

	tfs->devicename = argv[optind];
	tfs->fd = open(tfs->devicename, O_RDWR); 

	if (tfs->fd < 0) {
		fprintf(stderr, _("Unable to open device %s\n"),
				tfs->devicename);
		status = -EIO;
		goto out;
	}

	status = read_super(tfs);
	if (status)
		goto out;

	if (tfs->opt_uuid) {
		status = change_uuid(tfs, tfs->uuid);
		if (status)
			goto out;
	}

	/* Keep label and table together because they are the same field
	 * in the superblock */

	if (tfs->opt_label) {
		status = change_label(tfs, tfs->label);
		if (status)
			goto out;
	}

	if (tfs->opt_table) {
		status = change_locktable(tfs, tfs->table);
		if (status)
			goto out;
	}

	if (tfs->opt_proto) {
		status = change_lockproto(tfs, tfs->proto);
		if (status)
			goto out;
	}

	if (tfs->opt_label || tfs->opt_uuid ||
			tfs->opt_table || tfs->opt_proto) {
		status = write_super(tfs);
		if (status)
			goto out;
	}

	if (tfs->opt_list)
		print_super(tfs);

	close(tfs->fd);
out:
	return status;
}
