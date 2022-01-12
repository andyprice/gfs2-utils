#ifndef __GFS2_MKFS_DOT_H__
#define __GFS2_MKFS_DOT_H__

#include <stdarg.h>
#include "copyright.cf"

/*
 * The following inode IOCTL macros and inode flags 
 * are copied from linux/fs.h, because we have duplicate 
 * definition of symbols when we include both linux/fs.h and 
 * sys/mount.h in our program
 */

#define FS_IOC_GETFLAGS                 _IOR('f', 1, long)
#define FS_IOC_SETFLAGS                 _IOW('f', 2, long)
#define FS_IOC_FIEMAP                   _IOWR('f', 11, struct fiemap)

/*
 * Inode flags (FS_IOC_GETFLAGS / FS_IOC_SETFLAGS)
 */
#define FS_JOURNAL_DATA_FL              0x00004000 /* Reserved for ext3 */

#endif /* __GFS2_MKFS_DOT_H__ */
