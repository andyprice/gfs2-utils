#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include "lang.h"
#include "libgfs2.h"

static void usage(const char *cmd)
{
	printf("A language for modifying and querying a gfs2 file system.\n");
	printf("Usage: %s [options] <fs_path>\n", cmd);
	printf("Available options:\n");
	printf("  -h                Print this help message and exit\n");
	printf("  -f <script_path>  Path to script file or '-' for stdin (the default)\n");
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

static int print_structs(void)
{
	const struct lgfs2_metadata **mlist;
	int i;

	mlist = calloc(lgfs2_metadata_size, sizeof(struct lgfs2_metadata *));
	if (mlist == NULL) {
		perror("Failed to create metadata type array");
		return 1;
	}
	for (i = 0; i < lgfs2_metadata_size; i++)
		mlist[i] = &lgfs2_metadata[i];

	qsort(mlist, lgfs2_metadata_size, sizeof(struct lgfs2_metadata *), metastrcmp);
	for (i = 0; i < lgfs2_metadata_size; i++)
		if (mlist[i]->mh_type != GFS2_METATYPE_NONE)
			printf("%s\n", mlist[i]->name);
	free(mlist);
	return 0;
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
	opts->src = stdin;
	while ((opt = getopt(argc, argv, "F:f:hT")) != -1) {
		switch (opt) {
		case 'f':
			if (strcmp("-", optarg)) {
				opts->src = fopen(optarg, "r");
				if (opts->src == NULL) {
					perror("Failed to open source file");
					return 1;
				}
			}
			break;
		case 'T':
			exit(print_structs());
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

static int openfs(const char *path, struct lgfs2_sbd *sdp)
{
	int fd;
	int ret;
	int ok;
	uint64_t count;

	fd = open(path, O_RDWR);
	if (fd < 0) {
		fprintf(stderr, "Failed to open %s\n", path);
		return 1;
	}

	memset(sdp, 0, sizeof(*sdp));
	sdp->sd_bsize = GFS2_BASIC_BLOCK;
	sdp->device_fd = fd;
	ret = lgfs2_compute_constants(sdp);
	if (ret != 0) {
		perror("Bad constants");
		return 1;
	}
	ret = lgfs2_get_dev_info(fd, &sdp->dinfo);
	if (ret != 0) {
		perror("Failed to gather device info");
		return 1;
	}
	lgfs2_fix_device_geometry(sdp);

	ret = lgfs2_read_sb(sdp);
	if (ret != 0) {
		perror("Could not read sb");
		return 1;
	}

	sdp->master_dir = lgfs2_inode_read(sdp, sdp->sd_meta_dir.in_addr);
	sdp->md.riinode = lgfs2_lookupi(sdp->master_dir, "rindex", 6);
	sdp->fssize = sdp->device.length;
	if (sdp->md.riinode) {
		lgfs2_rindex_read(sdp, &count, &ok);
	} else {
		perror("Failed to look up rindex");
		return 1;
	}
	return 0;
}

int main(int argc, char *argv[])
{
	int ret;
	struct cmdopts opts = {NULL, NULL};
	struct lgfs2_sbd sbd;
	struct lgfs2_lang_result *result;
	struct lgfs2_lang_state *state;

	if (getopts(argc, argv, &opts)) {
		exit(1);
	}

	if (opts.help) {
		usage(argv[0]);
		exit(0);
	}

	if (openfs(argv[optind], &sbd))
		exit(1);

	state = lgfs2_lang_init();
	if (state == NULL) {
		perror("lgfs2_lang_init failed");
		exit(1);
	}

	ret = lgfs2_lang_parsef(state, opts.src);
	if (ret != 0) {
		fprintf(stderr, "Parse failed\n");
		free(opts.fspath);
		return ret;
	}

	for (result = lgfs2_lang_result_next(state, &sbd);
	     result != NULL;
	     result = lgfs2_lang_result_next(state, &sbd)) {
		lgfs2_lang_result_print(result);
		lgfs2_lang_result_free(&result);
	}

	lgfs2_rgrp_free(&sbd, &sbd.rgtree);
	lgfs2_inode_put(&sbd.md.riinode);
	lgfs2_inode_put(&sbd.master_dir);
	lgfs2_lang_free(&state);
	free(opts.fspath);
	return 0;
}
