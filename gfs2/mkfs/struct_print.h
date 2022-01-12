#ifndef STRUCT_PRINT_H
#define STRUCT_PRINT_H

/* Printing functions. These expect on-disk data */
extern void inum_print(void *nop);
extern void meta_header_print(void *mhp);
extern void sb_print(void *sbp);
extern void dinode_print(void *dip);
extern void log_header_print(void *lhp);
extern void log_descriptor_print(void *ldp);
extern void quota_print(void *qp);
extern void quota_change_print(void *qcp);
extern void statfs_change_print(void *scp);
extern void ea_header_print(void *eap);
extern void leaf_print(void *lfp);
extern void rindex_print(void *rip);
extern void rgrp_print(void *rgp);

#endif /* STRUCT_PRINT_H */
