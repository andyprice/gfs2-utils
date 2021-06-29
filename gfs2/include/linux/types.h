#ifndef LINUX_TYPES_H
#define LINUX_TYPES_H

#include <asm/types.h>

/* Satisfy gfs2_ondisk.h with userspace definitions of kernel types */

#include <stdint.h>

#ifdef __CHECKER__
#define __bitwise__ __attribute__((bitwise))
#define __force__ __attribute__((force))
#else
#define __bitwise__
#define __force__
#endif
#define __bitwise __bitwise__
#define __force __force__

typedef uint16_t __bitwise __le16;
typedef uint16_t __bitwise __be16;
typedef uint32_t __bitwise __le32;
typedef uint32_t __bitwise __be32;
typedef uint64_t __bitwise __le64;
typedef uint64_t __bitwise __be64;

#endif /* LINUX_TYPES_H */
