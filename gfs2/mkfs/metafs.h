#ifndef METAFS_H
#define METAFS_H

extern int metafs_interrupted;

struct metafs {
	int fd;
	char *path;
};

extern int mount_gfs2_meta(struct metafs *mfs, const char *path, int debug);
extern void cleanup_metafs(struct metafs *mfs);

#endif /* METAFS_H */
