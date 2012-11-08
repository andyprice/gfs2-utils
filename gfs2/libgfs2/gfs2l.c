#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include "libgfs2.h"

static void usage(const char *cmd)
{
	fprintf(stderr, "Usage: %s -f <script_path> <fs_path>\n", cmd);
	fprintf(stderr, "Use -f - for stdin\n");
}

struct cmdopts {
	char *fspath;
	FILE *src;
};

static int getopts(int argc, char *argv[], struct cmdopts *opts)
{
	int opt;
	while ((opt = getopt(argc, argv, "f:")) != -1) {
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
		default:
			usage(argv[0]);
			return 1;
		}
	}

	if (argc - optind != 1) {
		usage(argv[0]);
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
