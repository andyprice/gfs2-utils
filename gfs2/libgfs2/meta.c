#include <stdint.h>
#include "libgfs2.h"


#define F(f,...)  { .name = #f, \
		    .offset = offsetof(struct STRUCT, f), \
		    .length = sizeof(((struct STRUCT *)(0))->f), \
		    __VA_ARGS__ },
#define FP(f) F(f, .pointer=1)
#define RF(f) F(f, .reserved=1)
#define RFP(f) F(f, .pointer=1, .reserved=1)

#define ARRAY_SIZE(x) (sizeof(x)/sizeof(x[0]))

#define MH(f) F(f.mh_magic) \
	      F(f.mh_type) \
	      RF(f.__pad0) \
	      F(f.mh_format) \
	      F(f.mh_jid)

#define IN(f) F(f.no_formal_ino) \
	      FP(f.no_addr)

#define INR(f) RF(f.no_formal_ino) \
	       RFP(f.no_addr)


#undef STRUCT
#define STRUCT gfs2_sb

static const struct lgfs2_metafield gfs2_sb_fields[] = {
MH(sb_header)
F(sb_fs_format)
F(sb_multihost_format)
RF(__pad0)
F(sb_bsize)
F(sb_bsize_shift)
RF(__pad1)
IN(sb_master_dir)
INR(__pad2)
IN(sb_root_dir)
F(sb_lockproto)
F(sb_locktable)
IN( __pad3)
IN( __pad4)
F(sb_uuid)
};

#undef STRUCT
#define STRUCT gfs_sb

static const struct lgfs2_metafield gfs_sb_fields[] = {
MH(sb_header)
F(sb_fs_format)
F(sb_multihost_format)
F(sb_flags)
F(sb_bsize)
F(sb_bsize_shift)
F(sb_seg_size)
IN(sb_jindex_di)
IN(sb_rindex_di)
IN(sb_root_di)
F(sb_lockproto)
F(sb_locktable)
IN(sb_quota_di)
IN(sb_license_di)
RF(sb_reserved)
};

#undef STRUCT
#define STRUCT gfs2_rindex

static const struct lgfs2_metafield gfs2_rindex_fields[] = {
FP(ri_addr)
F(ri_length)
RF(__pad)
FP(ri_data0)
F(ri_data)
F(ri_bitbytes)
F(ri_reserved)
};

#undef STRUCT
#define STRUCT gfs2_rgrp

static const struct lgfs2_metafield gfs2_rgrp_fields[] = {
MH(rg_header)
F(rg_flags)
F(rg_free)
F(rg_dinodes)
RF(__pad)
F(rg_igeneration)
RF(rg_reserved)
};

#undef STRUCT
#define STRUCT gfs_rgrp

static const struct lgfs2_metafield gfs_rgrp_fields[] = {
MH(rg_header)
F(rg_flags)
F(rg_free)
F(rg_useddi)
F(rg_freedi)
IN(rg_freedi_list)
F(rg_usedmeta)
F(rg_freemeta)
RF(rg_reserved)
};

#undef STRUCT
#define STRUCT gfs2_meta_header

static const struct lgfs2_metafield gfs2_rgrp_bitmap_fields[] = {
F(mh_magic)
F(mh_type)
RF(__pad0)
F(mh_format)
F(mh_jid)
};

#undef STRUCT
#define STRUCT gfs2_dinode

static const struct lgfs2_metafield gfs2_dinode_fields[] = {
MH(di_header)
IN(di_num)
F(di_mode)
F(di_uid)
F(di_gid)
F(di_nlink)
F(di_size)
F(di_blocks)
F(di_atime)
F(di_mtime)
F(di_ctime)
F(di_major)
F(di_minor)
FP(di_goal_meta)
FP(di_goal_data)
F(di_generation)
F(di_flags)
F(di_payload_format)
RF(__pad1)
F(di_height)
RF(__pad2)
RF(__pad3)
F(di_depth)
F(di_entries)
INR(__pad4)
FP(di_eattr)
F(di_atime_nsec)
F(di_mtime_nsec)
F(di_ctime_nsec)
RF(di_reserved)
};

#undef STRUCT
#define STRUCT gfs_dinode

static const struct lgfs2_metafield gfs_dinode_fields[] = {
MH(di_header)
IN(di_num)
F(di_mode)
F(di_uid)
F(di_gid)
F(di_nlink)
F(di_size)
F(di_blocks)
F(di_atime)
F(di_mtime)
F(di_ctime)
F(di_major)
F(di_minor)
FP(di_rgrp)
FP(di_goal_rgrp)
F(di_goal_dblk)
F(di_goal_mblk)
F(di_flags)
F(di_payload_format)
F(di_type)
F(di_height)
F(di_incarn)
F(di_pad)
F(di_depth)
F(di_entries)
INR(di_next_unused)
FP(di_eattr)
F(di_reserved)
};

#undef STRUCT
#define STRUCT gfs2_meta_header

static const struct lgfs2_metafield gfs2_indirect_fields[] = {
F(mh_magic)
F(mh_type)
RF(__pad0)
F(mh_format)
F(mh_jid)
};

#undef STRUCT
#define STRUCT gfs_indirect

static const struct lgfs2_metafield gfs_indirect_fields[] = {
MH(in_header)
RF(in_reserved)
};

#undef STRUCT
#define STRUCT gfs2_leaf

static const struct lgfs2_metafield gfs2_leaf_fields[] = {
MH(lf_header)
F(lf_depth)
F(lf_entries)
F(lf_dirent_format)
F(lf_next)
RF(lf_reserved)
};

#undef STRUCT
#define STRUCT gfs2_log_header

static const struct lgfs2_metafield gfs2_log_header_fields[] = {
MH(lh_header)
F(lh_sequence)
F(lh_flags)
F(lh_tail)
F(lh_blkno)
F(lh_hash)
};

#undef STRUCT
#define STRUCT gfs2_log_descriptor

static const struct lgfs2_metafield gfs2_log_desc_fields[] = {
MH(ld_header)
F(ld_type)
F(ld_length)
F(ld_data1)
F(ld_data2)
RF(ld_reserved)
};

#undef STRUCT
#define STRUCT gfs2_meta_header

static const struct lgfs2_metafield gfs2_log_block_fields[] = {
F(mh_magic)
F(mh_type)
RF(__pad0)
F(mh_format)
F(mh_jid)
};

#undef STRUCT
#define STRUCT gfs2_meta_header

static const struct lgfs2_metafield gfs2_ea_attr_fields[] = {
F(mh_magic)
F(mh_type)
RF(__pad0)
F(mh_format)
F(mh_jid)
};

#undef STRUCT
#define STRUCT gfs2_meta_header

static const struct lgfs2_metafield gfs2_ea_data_fields[] = {
F(mh_magic)
F(mh_type)
RF(__pad0)
F(mh_format)
F(mh_jid)
};

#undef STRUCT
#define STRUCT gfs2_quota_change

static const struct lgfs2_metafield gfs2_quota_change_fields[] = {
F(qc_change)
F(qc_flags)
F(qc_id)
};

#undef STRUCT
#define STRUCT gfs2_dirent

static const struct lgfs2_metafield gfs2_dirent_fields[] = {
IN(de_inum)
F(de_hash)
F(de_rec_len)
F(de_name_len)
F(de_type)
RF(__pad)
};

#undef STRUCT
#define STRUCT gfs2_ea_header

static const struct lgfs2_metafield gfs2_ea_header_fields[] = {
F(ea_rec_len)
F(ea_data_len)
F(ea_name_len)
F(ea_type)
F(ea_flags)
F(ea_num_ptrs)
RF(__pad)
};

#undef STRUCT
#define STRUCT gfs2_inum_range

static const struct lgfs2_metafield gfs2_inum_range_fields[] = {
F(ir_start)
F(ir_length)
};

#undef STRUCT
#define STRUCT gfs2_statfs_change

static const struct lgfs2_metafield gfs2_statfs_change_fields[] = {
F(sc_total)
F(sc_free)
F(sc_dinodes)
};

const struct lgfs2_metadata lgfs2_metadata[] = {
	[LGFS2_MT_GFS2_SB] = {
		.gfs2 = 1,
		.header = 1,
		.mh_type = GFS2_METATYPE_SB,
		.mh_format = GFS2_FORMAT_SB,
		.name = "gfs2_sb",
		.fields = gfs2_sb_fields,
		.nfields = ARRAY_SIZE(gfs2_sb_fields),
		.size = sizeof(struct gfs2_sb),
	},
	[LGFS2_MT_GFS_SB] = {
		.gfs1 = 1,
		.header = 1,
		.mh_type = GFS2_METATYPE_SB,
		.mh_format = GFS_FORMAT_SB,
		.name = "gfs_sb",
		.fields = gfs_sb_fields,
		.nfields = ARRAY_SIZE(gfs_sb_fields),
		.size = sizeof(struct gfs_sb),
	},
	[LGFS2_MT_RINDEX] = {
		.gfs1 = 1,
		.gfs2 = 1,
		.name = "rindex",
		.fields = gfs2_rindex_fields,
		.nfields = ARRAY_SIZE(gfs2_rindex_fields),
		.size = sizeof(struct gfs2_rindex),
	},
	[LGFS2_MT_GFS2_RGRP] = {
		.gfs2 = 1,
		.header = 1,
		.mh_type = GFS2_METATYPE_RG,
		.mh_format = GFS2_FORMAT_RG,
		.name = "gfs2_rgrp",
		.fields = gfs2_rgrp_fields,
		.nfields = ARRAY_SIZE(gfs2_rgrp_fields),
		.size = sizeof(struct gfs2_rgrp),
	},
	[LGFS2_MT_GFS_RGRP] = {
		.gfs1 = 1,
		.header = 1,
		.mh_type = GFS2_METATYPE_RG,
		.mh_format = GFS2_FORMAT_RG,
		.name = "gfs_rgrp",
		.fields = gfs_rgrp_fields,
		.nfields = ARRAY_SIZE(gfs_rgrp_fields),
		.size = sizeof(struct gfs_rgrp),
	},
	[LGFS2_MT_RGRP_BITMAP] = {
		.gfs1 = 1,
		.gfs2 = 1,
		.header = 1,
		.mh_type = GFS2_METATYPE_RB,
		.mh_format = GFS2_FORMAT_RB,
		.name = "gfs2_metaheader",
		.fields = gfs2_rgrp_bitmap_fields,
		.nfields = ARRAY_SIZE(gfs2_rgrp_bitmap_fields),
		.size = sizeof(struct gfs2_meta_header),
	},
	[LGFS2_MT_GFS2_DINODE] = {
		.gfs2 = 1,
		.header = 1,
		.mh_type = GFS2_METATYPE_DI,
		.mh_format = GFS2_FORMAT_DI,
		.name = "gfs2_dinode",
		.fields = gfs2_dinode_fields,
		.nfields = ARRAY_SIZE(gfs2_dinode_fields),
		.size = sizeof(struct gfs2_dinode),
	},
	[LGFS2_MT_GFS_DINODE] = {
		.gfs1 = 1,
		.header = 1,
		.mh_type = GFS2_METATYPE_DI,
		.mh_format = GFS2_FORMAT_DI,
		.name = "gfs_dinode",
		.fields = gfs_dinode_fields,
		.nfields = ARRAY_SIZE(gfs_dinode_fields),
		.size = sizeof(struct gfs_dinode),
	},
	[LGFS2_MT_GFS2_INDIRECT] = {
		.gfs2 = 1,
		.header = 1,
		.mh_type = GFS2_METATYPE_IN,
		.mh_format = GFS2_FORMAT_IN,
		.name = "gfs2_meta_header",
		.fields = gfs2_indirect_fields,
		.nfields = ARRAY_SIZE(gfs2_indirect_fields),
		.size = sizeof(struct gfs2_meta_header),
	},
	[LGFS2_MT_GFS_INDIRECT] = {
		.gfs1 = 1,
		.header = 1,
		.mh_type = GFS2_METATYPE_IN,
		.mh_format = GFS2_FORMAT_IN,
		.name = "gfs_indirect",
		.fields = gfs_indirect_fields,
		.nfields = ARRAY_SIZE(gfs_indirect_fields),
		.size = sizeof(struct gfs_indirect),
	},
	[LGFS2_MT_DIR_LEAF] = {
		.gfs1 = 1,
		.gfs2 = 1,
		.header = 1,
		.mh_type = GFS2_METATYPE_LF,
		.mh_format = GFS2_FORMAT_LF,
		.name = "gfs2_leaf",
		.fields = gfs2_leaf_fields,
		.nfields = ARRAY_SIZE(gfs2_leaf_fields),
		.size = sizeof(struct gfs2_leaf),
	},
	[LGFS2_MT_GFS2_LOG_HEADER] = {
		.gfs2 = 1,
		.header = 1,
		.mh_type = GFS2_METATYPE_LH,
		.mh_format = GFS2_FORMAT_LH,
		.name = "gfs2_log_header",
		.fields = gfs2_log_header_fields,
		.nfields = ARRAY_SIZE(gfs2_log_header_fields),
		.size = sizeof(struct gfs2_log_header),
	},
	[LGFS2_MT_GFS2_LOG_DESC] = {
		.gfs2 = 1,
		.header = 1,
		.mh_type = GFS2_METATYPE_LD,
		.mh_format = GFS2_FORMAT_LD,
		.name = "gfs2_log_desc",
		.fields = gfs2_log_desc_fields,
		.nfields = ARRAY_SIZE(gfs2_log_desc_fields),
		.size = sizeof(struct gfs2_log_descriptor),
	},
	[LGFS2_MT_GFS2_LOG_BLOCK] = {
		.gfs2 = 1,
		.header = 1,
		.mh_type = GFS2_METATYPE_LB,
		.mh_format = GFS2_FORMAT_LB,
		.name = "gfs2_meta_header",
		.fields = gfs2_log_block_fields,
		.nfields = ARRAY_SIZE(gfs2_log_block_fields),
		.size = sizeof(struct gfs2_meta_header),
	},
	[LGFS2_MT_EA_ATTR] = {
		.gfs2 = 1,
		.gfs1 = 1,
		.header = 1,
		.mh_type = GFS2_METATYPE_EA,
		.mh_format = GFS2_FORMAT_EA,
		.name = "gfs2_meta_header",
		.fields = gfs2_ea_attr_fields,
		.nfields = ARRAY_SIZE(gfs2_ea_attr_fields),
		.size = sizeof(struct gfs2_meta_header),
	},
	[LGFS2_MT_EA_DATA] = {
		.gfs2 = 1,
		.gfs1 = 1,
		.header = 1,
		.mh_type = GFS2_METATYPE_ED,
		.mh_format = GFS2_FORMAT_ED,
		.name = "gfs2_meta_header",
		.fields = gfs2_ea_data_fields,
		.nfields = ARRAY_SIZE(gfs2_ea_data_fields),
		.size = sizeof(struct gfs2_meta_header),
	},
	[LGFS2_MT_GFS2_QUOTA_CHANGE] = {
		.gfs2 = 1,
		.name = "gfs2_quota_change",
		.fields = gfs2_quota_change_fields,
		.nfields = ARRAY_SIZE(gfs2_quota_change_fields),
		.size = sizeof(struct gfs2_quota_change),
	},
	[LGFS2_MT_DIRENT] = {
		.gfs1 = 1,
		.gfs2 = 1,
		.name = "gfs2_dirent",
		.fields = gfs2_dirent_fields,
		.nfields = ARRAY_SIZE(gfs2_dirent_fields),
		.size = sizeof(struct gfs2_dirent),
	},
	[LGFS2_MT_EA_HEADER] = {
		.gfs1 = 1,
		.gfs2 = 1,
		.name = "gfs2_ea_header",
		.fields = gfs2_ea_header_fields,
		.nfields = ARRAY_SIZE(gfs2_ea_header_fields),
		.size = sizeof(struct gfs2_ea_header),
	},
	[LGFS2_MT_GFS2_INUM_RANGE] = {
		.gfs2 = 1,
		.name = "gfs2_inum_range",
		.fields = gfs2_inum_range_fields,
		.nfields = ARRAY_SIZE(gfs2_inum_range_fields),
		.size = sizeof(struct gfs2_inum_range),
	},
	[LGFS2_MT_STATFS_CHANGE] = {
		.gfs1 = 1,
		.gfs2 = 1,
		.name = "gfs2_statfs_change",
		.fields = gfs2_statfs_change_fields,
		.nfields = ARRAY_SIZE(gfs2_statfs_change_fields),
		.size = sizeof(struct gfs2_statfs_change),
	},
};

const unsigned lgfs2_metadata_size = ARRAY_SIZE(lgfs2_metadata);

static int check_metadata_sizes(void)
{
	unsigned offset;
	int i, j;

	for (i = 0; i < lgfs2_metadata_size; i++) {
		const struct lgfs2_metadata *m = &lgfs2_metadata[i];
		offset = 0;
		for (j = 0; j < m->nfields; j++) {
			const struct lgfs2_metafield *f = &m->fields[j];
			if (f->offset != offset) {
				fprintf(stderr, "%s: %s: offset is %u, expected %u\n", m->name, f->name, f->offset, offset);
				return -1;
			}
			offset += f->length;
		}
		if (offset != m->size) {
			fprintf(stderr, "%s: size mismatch between struct %u and fields %u\n", m->name, m->size, offset);
			return -1;
		}
	}

	return 0;
}

int lgfs2_selfcheck(void)
{
	int ret = 0;

	ret |= check_metadata_sizes();

	return ret;
}

