
#ifndef __GFS2_TUNE_DOT_H__
#define __GFS2_TUNE_DOT_H__

#define GFS2_DEFAULT_BSIZE	4096
#define GFS_MAGIC               (0x01161970) /* GFS1 magic, because we are */ 
					/* not including any GFS1 headers */
struct tunegfs2 {
	char *devicename;
	int fd;
	unsigned long sb_start;
	struct gfs2_sb *sb;
	char *uuid;
	char *label;
	char *table;
	char *proto;
	char *mount_options;
	int opt_list;
	int opt_label;
	int opt_uuid;
	int opt_proto;
	int opt_table;
};

int print_super(struct tunegfs2 *);
int read_super(struct tunegfs2 *);
int write_super(struct tunegfs2 *);
int change_uuid(struct tunegfs2 *, char *uuid);
int change_label(struct tunegfs2 *, char *label);
int change_lockproto(struct tunegfs2 *, char *label);
int change_locktable(struct tunegfs2 *, char *label);

#endif

