#include "clusterautoconfig.h"

#include <stdint.h>
#include <string.h>
#include <libintl.h>
#define _(String) gettext(String)

#include "libgfs2.h"
#include "fsck.h"
#include "metawalk.h"
#include "eattr.h"

int clear_eattr_entry (struct gfs2_inode *ip,
		       struct gfs2_buffer_head *leaf_bh,
		       struct gfs2_ea_header *ea_hdr,
		       struct gfs2_ea_header *ea_hdr_prev,
		       void *private)
{
	struct gfs2_sbd *sdp = ip->i_sbd;

	if(!ea_hdr->ea_name_len){
		/* Skip this entry for now */
		return 1;
	}

	if(!GFS2_EATYPE_VALID(ea_hdr->ea_type) &&
	   ((ea_hdr_prev) || (!ea_hdr_prev && ea_hdr->ea_type))){
		/* Skip invalid entry */
		return 1;
	}

	if(ea_hdr->ea_num_ptrs){
		uint32_t avail_size;
		int max_ptrs;

		avail_size = sdp->sd_sb.sb_bsize - sizeof(struct gfs2_meta_header);
		max_ptrs = (be32_to_cpu(ea_hdr->ea_data_len)+avail_size-1)/avail_size;

		if(max_ptrs > ea_hdr->ea_num_ptrs) {
			return 1;
		} else {
			log_debug( _("  Pointers Required: %d\n"
				  "  Pointers Reported: %d\n"),
				  max_ptrs, ea_hdr->ea_num_ptrs);
		}


	}
	return 0;
}

int clear_eattr_extentry(struct gfs2_inode *ip, uint64_t *ea_data_ptr,
			 struct gfs2_buffer_head *leaf_bh,
			 struct gfs2_ea_header *ea_hdr,
			 struct gfs2_ea_header *ea_hdr_prev, void *private)
{
	uint64_t block = be64_to_cpu(*ea_data_ptr);

	return delete_eattr_leaf(ip, block, 0, &leaf_bh, private);
}



