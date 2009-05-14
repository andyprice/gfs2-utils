#ifndef __GFS2HEX_DOT_H__
#define __GFS2HEX_DOT_H__

#include "hexedit.h"

int display_gfs2(void);
int edit_gfs2(void);
void do_dinode_extended(struct gfs2_dinode *di, char *buf);
void print_gfs2(const char *fmt, ...);
int do_indirect_extended(char *diebuf, struct iinfo *iinf);
void do_leaf_extended(char *dlebuf, struct iinfo *indir);
void eol(int col);

#endif /*  __GFS2HEX_DOT_H__  */
