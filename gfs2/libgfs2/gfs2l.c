#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include "libgfs2.h"

static void usage(const char *cmd)
{
	printf("A language for modifying and querying a gfs2 file system.\n");
	printf("Usage: %s [options] <fs_path>\n", cmd);
	printf("Available options:\n");
	printf("  -h                Print this help message and exit\n");
	printf("  -f <script_path>  Path to script file or '-' for stdin\n");
	printf("  -T                Print a list of gfs2 structure types and exit\n");
	printf("  -F <type>         Print a list of fields belonging to a type and exit\n");
}

struct cmdopts {
	char *fspath;
	FILE *src;
	unsigned help:1;
};

static int metastrcmp(const void *a, const void *b)
{
	const struct lgfs2_metadata *m1 = *(struct lgfs2_metadata **)a;
	const struct lgfs2_metadata *m2 = *(struct lgfs2_metadata **)b;
	return strcmp(m1->name, m2->name);
}

static void print_structs(void)
{
	const struct lgfs2_metadata *mlist[lgfs2_metadata_size];
	int i;
	for (i = 0; i < lgfs2_metadata_size; i++)
		mlist[i] = &lgfs2_metadata[i];

	qsort(mlist, lgfs2_metadata_size, sizeof(struct lgfs2_metadata *), metastrcmp);
	for (i = 0; i < lgfs2_metadata_size; i++)
		if (mlist[i]->mh_type != GFS2_METATYPE_NONE)
			printf("%s\n", mlist[i]->name);
}

static void print_fields(const char *name)
{
	const struct lgfs2_metadata *m = lgfs2_find_mtype_name(name, LGFS2_MD_GFS1|LGFS2_MD_GFS2);
	if (m != NULL) {
		const struct lgfs2_metafield *fields = m->fields;
		const unsigned nfields = m->nfields;
		int i;
		for (i = 0; i < nfields; i++)
			printf("0x%.4x %s\n", fields[i].offset, fields[i].name);
	}
}

static int getopts(int argc, char *argv[], struct cmdopts *opts)
{
	int opt;
	while ((opt = getopt(argc, argv, "F:f:hT")) != -1) {
		switch (opt) {
		case 'f':
			if (!strcmp("-", optarg)) {
				opts->src = stdin;
			} else {
				opts->src = fopen(optarg, "r");
				if (opts->src == NULL) {
					perror("Failed to open source file");
					return 1;
				}
			}
			break;
		case 'T':
			print_structs();
			exit(0);
		case 'F':
			print_fields(optarg);
			exit(0);
		case 'h':
			opts->help = 1;
			return 0;
		default:
			fprintf(stderr, "Use -h for help\n");
			return 1;
		}
	}

	if (argc - optind != 1) {
		usage(argv[0]);
		fprintf(stderr, "Missing file system path. Use -h for help.\n");
		return 1;
	}

	opts->fspath = strdup(argv[optind]);
	if (opts->fspath == NULL) {
		perror("getopts");
		return 1;
	}
	return 0;
}

static struct gfs2_sbd *openfs(const char *path)
{
	int fd;
	int ret;
	int sane;
	int count;
	struct gfs2_sbd *sdp = calloc(1, sizeof(struct gfs2_sbd));
	if (sdp == NULL) {
		perror("calloc");
		return NULL;
	}

	fd = open(path, O_RDWR);
	if (fd < 0) {
		fprintf(stderr, "Failed to open %s\n", path);
		free(sdp);
		return NULL;
	}

	memset(sdp, 0, sizeof(*sdp));
	sdp->bsize = GFS2_BASIC_BLOCK;
	sdp->device_fd = fd;
	compute_constants(sdp);
	lgfs2_get_dev_info(fd, &sdp->dinfo);
	fix_device_geometry(sdp);

	ret = read_sb(sdp);
	if (ret != 0) {
		perror("Could not read sb");
		return NULL;
	}

	sdp->master_dir = lgfs2_inode_read(sdp, sdp->sd_sb.sb_master_dir.no_addr);
	gfs2_lookupi(sdp->master_dir, "rindex", 6, &sdp->md.riinode);
	sdp->fssize = sdp->device.length;
	if (sdp->md.riinode) {
		rindex_read(sdp, 0, &count, &sane);
	} else {
		perror("Failed to look up rindex");
		free(sdp);
		return NULL;
	}
	return sdp;
}

int main(int argc, char *argv[])
{
	int ret;
	struct cmdopts opts = {NULL, NULL};
	struct gfs2_sbd *sdp;
	struct lgfs2_lang_result *result;
	struct lgfs2_lang_state *state;

	if (getopts(argc, argv, &opts)) {
		exit(1);
	}

	if (opts.help) {
		usage(argv[0]);
		exit(0);
	}

	sdp = openfs(argv[optind]);
	if (sdp == NULL) {
		exit(1);
	}

	state = lgfs2_lang_init();
	if (state == NULL) {
		perror("lgfs2_lang_init failed");
		exit(1);
	}

	ret = lgfs2_lang_parsef(state, opts.src);
	if (ret != 0) {
		fprintf(stderr, "Parse failed\n");
		return ret;
	}

	for (result = lgfs2_lang_result_next(state, sdp);
	     result != NULL;
	     result = lgfs2_lang_result_next(state, sdp)) {
		if (result == NULL) {
			fprintf(stderr, "Failed to interpret script\n");
			return -1;
		}
		lgfs2_lang_result_print(result);
		lgfs2_lang_result_free(&result);
	}

	gfs2_rgrp_free(&sdp->rgtree);
	inode_put(&sdp->md.riinode);
	inode_put(&sdp->master_dir);
	lgfs2_lang_free(&state);
	free(opts.fspath);
	return 0;
}

// libgfs2 still requires an external print_it function
void print_it(const char *label, const char *fmt, const char *fmt2, ...) { return; }
