#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>
#include <endian.h>
#include <byteswap.h>

#define printk printf
#define pv(struct, member, fmt) printf("  "#member" = "fmt"\n", struct->member);

#define ENTER(x)
#define EXIT(x)
#define RET(x) return
#define RETURN(x, y) return y

#if __BYTE_ORDER == __BIG_ENDIAN

#define be16_to_cpu(x) (x)
#define be32_to_cpu(x) (x)
#define be64_to_cpu(x) (x)

#define cpu_to_be16(x) (x)
#define cpu_to_be32(x) (x)
#define cpu_to_be64(x) (x)

#define le16_to_cpu(x) (bswap_16((x)))
#define le32_to_cpu(x) (bswap_32((x)))
#define le64_to_cpu(x) (bswap_64((x)))

#define cpu_to_le16(x) (bswap_16((x)))
#define cpu_to_le32(x) (bswap_32((x)))
#define cpu_to_le64(x) (bswap_64((x)))

#endif  /*  __BYTE_ORDER == __BIG_ENDIAN  */

#if __BYTE_ORDER == __LITTLE_ENDIAN

#define be16_to_cpu(x) (bswap_16((x)))
#define be32_to_cpu(x) (bswap_32((x)))
#define be64_to_cpu(x) (bswap_64((x)))

#define cpu_to_be16(x) (bswap_16((x)))
#define cpu_to_be32(x) (bswap_32((x)))
#define cpu_to_be64(x) (bswap_64((x))) 

#define le16_to_cpu(x) (x)
#define le32_to_cpu(x) (x)
#define le64_to_cpu(x) (x)

#define cpu_to_le16(x) (x)
#define cpu_to_le32(x) (x)
#define cpu_to_le64(x) (x)

#endif  /*  __BYTE_ORDER == __LITTLE_ENDIAN  */

#define WANT_GFS_CONVERSION_FUNCTIONS
#include "gfs_ondisk.h"

