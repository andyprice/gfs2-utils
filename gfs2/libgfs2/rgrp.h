#ifndef __RGRP_DOT_H__
#define __RGRP_DOT_H__

#include "libgfs2.h"

struct lgfs2_rbm {
	lgfs2_rgrp_t rgd;
	uint32_t offset;    /* The offset is bitmap relative */
	unsigned bii;       /* Bitmap index */
};

static inline struct gfs2_bitmap *rbm_bi(const struct lgfs2_rbm *rbm)
{
	return rbm->rgd->bits + rbm->bii;
}

static inline uint64_t lgfs2_rbm_to_block(const struct lgfs2_rbm *rbm)
{
	return rbm->rgd->ri.ri_data0 + (rbm_bi(rbm)->bi_start * GFS2_NBBY) +
	        rbm->offset;
}

static inline int lgfs2_rbm_eq(const struct lgfs2_rbm *rbm1, const struct lgfs2_rbm *rbm2)
{
	return (rbm1->rgd == rbm2->rgd) && (rbm1->bii == rbm2->bii) &&
	        (rbm1->offset == rbm2->offset);
}

#endif /* __RGRP_DOT_H__ */
