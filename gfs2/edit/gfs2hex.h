#ifndef __GFS2HEX_DOT_H__
#define __GFS2HEX_DOT_H__

#include "hexedit.h"

extern int display_gfs2(void);
extern int edit_gfs2(void);
extern void do_dinode_extended(struct gfs2_dinode *di,
			       struct gfs2_buffer_head *lbh);
extern void print_gfs2(const char *fmt, ...);
extern uint64_t do_leaf_extended(char *dlebuf, struct iinfo *indir);
extern void eol(int col);

#endif /*  __GFS2HEX_DOT_H__  */
