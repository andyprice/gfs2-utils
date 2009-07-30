#ifndef __FS_RECOVERY_H__
#define __FS_RECOVERY_H__

#include "libgfs2.h"

int replay_journals(struct gfs2_sbd *sdp, int preen, int force_check,
		    int *clean_journals);
int preen_is_safe(struct gfs2_sbd *sdp, int preen, int force_check);

#endif /* __FS_RECOVERY_H__ */

