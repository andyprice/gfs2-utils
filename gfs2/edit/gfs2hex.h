#ifndef __GFS2HEX_DOT_H__
#define __GFS2HEX_DOT_H__

#include "hexedit.h"

extern void display_gfs2(void *buf);
extern int edit_gfs2(void);
extern void do_dinode_extended(char *buf);
extern void print_gfs2(const char *fmt, ...) __attribute__((format (printf, 1, 2)));
extern uint64_t do_leaf_extended(char *dlebuf, struct iinfo *indir);
extern void eol(int col);
extern uint64_t block;

#endif /*  __GFS2HEX_DOT_H__  */
