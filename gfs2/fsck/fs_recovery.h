#ifndef __FS_RECOVERY_H__
#define __FS_RECOVERY_H__

#include "libgfs2.h"

extern int replay_journals(struct fsck_cx *cx, int *clean_journals);
extern int preen_is_safe(struct lgfs2_sbd *sdp, const struct fsck_options * const _opts);

extern int ji_update(struct lgfs2_sbd *sdp);
extern int build_jindex(struct lgfs2_sbd *sdp);
extern int init_jindex(struct fsck_cx *cx, int allow_ji_rebuild);
#endif /* __FS_RECOVERY_H__ */

