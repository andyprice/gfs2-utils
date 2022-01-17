/* pass1 checks inodes for format & type, duplicate blocks, & incorrect
 * block count.
 *
 * It builds up tables that contains the state of each block (free,
 * block in use, metadata type, etc), as well as bad blocks and
 * duplicate blocks.  (See block_list.[ch] for more info)
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <unistd.h>
#include <string.h>
#include <time.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <inttypes.h>
#include <libintl.h>
#define _(String) gettext(String)

#include <logging.h>
#include "libgfs2.h"
#include "fsck.h"
#include "inode_hash.h"
#include "util.h"
#include "link.h"
#include "metawalk.h"
#include "fs_recovery.h"

static struct special_blocks gfs1_rindex_blks;
static struct gfs2_bmap *bl = NULL;

struct block_count {
	uint64_t indir_count;
	uint64_t data_count;
	uint64_t ea_count;
};

static int p1check_leaf(struct gfs2_inode *ip, uint64_t block, void *private);
static int pass1_check_metalist(struct iptr iptr, struct gfs2_buffer_head **bh, int h,
                                int *is_valid, int *was_duplicate, void *private);
static int undo_check_metalist(struct gfs2_inode *ip, uint64_t block,
			       int h, void *private);
static int pass1_check_data(struct gfs2_inode *ip, uint64_t metablock,
		      uint64_t block, void *private,
		      struct gfs2_buffer_head *bh, __be64 *ptr);
static int undo_check_data(struct gfs2_inode *ip, uint64_t block,
			   void *private);
static int check_eattr_indir(struct gfs2_inode *ip, uint64_t indirect,
			     uint64_t parent, struct gfs2_buffer_head **bh,
			     void *private);
static int check_eattr_leaf(struct gfs2_inode *ip, uint64_t block,
			    uint64_t parent, struct gfs2_buffer_head **bh,
			    void *private);
static int check_eattr_entries(struct gfs2_inode *ip,
			       struct gfs2_buffer_head *leaf_bh,
			       struct gfs2_ea_header *ea_hdr,
			       struct gfs2_ea_header *ea_hdr_prev,
			       void *private);
static int check_extended_leaf_eattr(struct gfs2_inode *ip, int i,
				     __be64 *data_ptr,
				     struct gfs2_buffer_head *leaf_bh,
				     uint32_t tot_ealen,
				     struct gfs2_ea_header *ea_hdr,
				     struct gfs2_ea_header *ea_hdr_prev,
				     void *private);
static int finish_eattr_indir(struct gfs2_inode *ip, int leaf_pointers,
			      int leaf_pointer_errors, void *private);
static int handle_ip(struct gfs2_sbd *sdp, struct gfs2_inode *ip);
static int delete_block(struct gfs2_inode *ip, uint64_t block,
			struct gfs2_buffer_head **bh, const char *btype,
			void *private);

static int gfs2_blockmap_set(struct gfs2_bmap *bmap, uint64_t bblock, int mark)
{
	static unsigned char *byte;
	static uint64_t b;

	if (!bmap)
		return 0;
	if (bblock > bmap->size)
		return -1;

	byte = bmap->map + BLOCKMAP_SIZE2(bblock);
	b = BLOCKMAP_BYTE_OFFSET2(bblock);
	*byte &= ~(BLOCKMAP_MASK2 << b);
	*byte |= (mark & BLOCKMAP_MASK2) << b;
	return 0;
}

/*
 * _fsck_blockmap_set - Mark a block in the 4-bit blockmap and the 2-bit
 *                      bitmap, and adjust free space accordingly.
 */
static int _fsck_blockmap_set(struct gfs2_inode *ip, uint64_t bblock,
			      const char *btype, int mark, int error_on_dinode,
			      const char *caller, int fline)
{
	int error = _fsck_bitmap_set(ip, bblock, btype, mark, error_on_dinode,
				     caller, fline);
	if (error)
		return error;

	return gfs2_blockmap_set(bl, bblock, mark);
}

#define fsck_blockmap_set(ip, b, bt, m) \
	_fsck_blockmap_set(ip, b, bt, m, 0, __FUNCTION__, __LINE__)
#define fsck_blkmap_set_noino(ip, b, bt, m) \
	_fsck_blockmap_set(ip, b, bt, m, 1, __FUNCTION__, __LINE__)

/**
 * delete_block - delete a block associated with an inode
 */
static int delete_block(struct gfs2_inode *ip, uint64_t block,
			struct gfs2_buffer_head **bh, const char *btype,
			void *private)
{
	if (valid_block_ip(ip, block)) {
		fsck_blockmap_set(ip, block, btype, GFS2_BLKST_FREE);
		return 0;
	}
	return -1;
}

/* This is a pass1-specific leaf repair. Since we are not allowed to do
 * block allocations, we do what we can. */
static int pass1_repair_leaf(struct gfs2_inode *ip, uint64_t *leaf_no,
			     int lindex, int ref_count, const char *msg)
{
	uint64_t *cpyptr;
	char *padbuf;
	int pad_size, i;

	log_err(_("Directory Inode %"PRIu64" (0x%"PRIx64") points to leaf %"PRIu64" (0x%"PRIx64") %s.\n"),
	       ip->i_num.in_addr, ip->i_num.in_addr, *leaf_no, *leaf_no, msg);
	if (!query( _("Attempt to patch around it? (y/n) "))) {
		log_err( _("Bad leaf left in place.\n"));
		goto out;
	}

	padbuf = malloc(ref_count * sizeof(uint64_t));
	cpyptr = (uint64_t *)padbuf;
	for (i = 0; i < ref_count; i++) {
		*cpyptr = 0;
		cpyptr++;
	}
	pad_size = ref_count * sizeof(uint64_t);
	log_err(_("Writing zeros to the hash table of directory %"PRIu64
	          " (0x%"PRIx64") at index: 0x%x for 0x%x pointers.\n"),
	        ip->i_num.in_addr, ip->i_num.in_addr, lindex, ref_count);
	if (ip->i_sbd->gfs1)
		lgfs2_gfs1_writei(ip, padbuf, lindex * sizeof(uint64_t), pad_size);
	else
		lgfs2_writei(ip, padbuf, lindex * sizeof(uint64_t), pad_size);
	free(padbuf);
	log_err(_("Directory Inode %"PRIu64" (0x%"PRIx64") patched.\n"),
	        ip->i_num.in_addr, ip->i_num.in_addr);

out:
	*leaf_no = 0;
	return 0;
}

static struct metawalk_fxns pass1_fxns = {
	.private = NULL,
	.check_leaf = p1check_leaf,
	.check_metalist = pass1_check_metalist,
	.check_data = pass1_check_data,
	.check_eattr_indir = check_eattr_indir,
	.check_eattr_leaf = check_eattr_leaf,
	.check_dentry = NULL,
	.check_eattr_entry = check_eattr_entries,
	.check_eattr_extentry = check_extended_leaf_eattr,
	.big_file_msg = big_file_comfort,
	.repair_leaf = pass1_repair_leaf,
	.undo_check_meta = undo_check_metalist,
	.undo_check_data = undo_check_data,
	.delete_block = delete_block,
};

/*
 * resuscitate_metalist - make sure a system directory entry's metadata blocks
 *                        are marked "in use" in the bitmap.
 *
 * This function makes sure metadata blocks for system and root directories are
 * marked "in use" by the bitmap.  You don't want root's indirect blocks
 * deleted, do you? Or worse, reused for lost+found.
 */
static int resuscitate_metalist(struct iptr iptr, struct gfs2_buffer_head **bh, int h,
                                int *is_valid, int *was_duplicate, void *private)
{
	struct block_count *bc = (struct block_count *)private;
	struct gfs2_inode *ip = iptr.ipt_ip;
	uint64_t block = iptr_block(iptr);

	*is_valid = 1;
	*was_duplicate = 0;
	*bh = NULL;
	if (!valid_block_ip(ip, block)){ /* blk outside of FS */
		fsck_blockmap_set(ip, ip->i_num.in_addr, _("itself"), GFS2_BLKST_UNLINKED);
		log_err(_("Bad indirect block pointer (invalid or out of "
		          "range) found in system inode %"PRIu64" (0x%"PRIx64").\n"),
		        ip->i_num.in_addr, ip->i_num.in_addr);
		*is_valid = 0;
		return META_IS_GOOD;
	}
	if (fsck_system_inode(ip->i_sbd, block))
		fsck_blockmap_set(ip, block, _("system file"),
				  ip->i_sbd->gfs1 ?
				  GFS2_BLKST_DINODE : GFS2_BLKST_USED);
	else
		check_n_fix_bitmap(ip->i_sbd, ip->i_rgd, block, 0,
				   ip->i_sbd->gfs1 ?
				   GFS2_BLKST_DINODE : GFS2_BLKST_USED);
	bc->indir_count++;
	return META_IS_GOOD;
}

/*
 * resuscitate_dentry - make sure a system directory entry is alive
 *
 * This function makes sure directory entries in system directories are
 * kept alive.  You don't want journal0 deleted from jindex, do you?
 */
static int resuscitate_dentry(struct gfs2_inode *ip, struct gfs2_dirent *dent,
			      struct gfs2_dirent *prev_de,
			      struct gfs2_buffer_head *bh, char *filename,
			      uint32_t *count, int *lindex, void *priv)
{
	struct gfs2_sbd *sdp = ip->i_sbd;
	struct lgfs2_dirent d;
	char tmp_name[PATH_MAX];
	uint64_t block;

	lgfs2_dirent_in(&d, dent);
	block = d.dr_inum.in_addr;
	/* Start of checks */
	memset(tmp_name, 0, sizeof(tmp_name));
	if (d.dr_name_len < sizeof(tmp_name))
		strncpy(tmp_name, filename, d.dr_name_len);
	else
		strncpy(tmp_name, filename, sizeof(tmp_name) - 1);
	if (!valid_block_ip(ip, block)) {
		log_err(_("Block # referenced by system directory entry %s in inode "
		          "%"PRIu64" (0x%"PRIx64") is invalid or out of range; ignored.\n"),
		        tmp_name, ip->i_num.in_addr, ip->i_num.in_addr);
		return 0;
	}
	/* If this is a system dinode, we'll handle it later in
	   check_system_inodes.  If not, it'll be handled by pass1 but
	   since it's in a system directory we need to make sure it's
	   represented in the rgrp bitmap. */
	if (fsck_system_inode(sdp, block))
		fsck_blockmap_set(ip, block, _("system file"),
				  GFS2_BLKST_DINODE);
	else
		check_n_fix_bitmap(sdp, ip->i_rgd, block, 0,
				   GFS2_BLKST_DINODE);
	/* Return the number of leaf entries so metawalk doesn't flag this
	   leaf as having none. */
	*count = be16_to_cpu(((struct gfs2_leaf *)bh->b_data)->lf_entries);
	return 0;
}

static struct metawalk_fxns sysdir_fxns = {
	.private = NULL,
	.check_metalist = resuscitate_metalist,
	.check_dentry = resuscitate_dentry,
	.delete_block = delete_block,
};

static int p1check_leaf(struct gfs2_inode *ip, uint64_t block, void *private)
{
	struct block_count *bc = (struct block_count *) private;
	int q;

	/* Note if we've gotten this far, the block has already passed the
	   check in metawalk: lgfs2_check_meta(lbh, GFS2_METATYPE_LF).
	   So we know it's a leaf block. */
	bc->indir_count++;
	q = block_type(bl, block);
	if (q != GFS2_BLKST_FREE) {
		log_err(_("Found duplicate block #%"PRIu64" (0x%"PRIx64") referenced "
		          "as a directory leaf in dinode "
		          "%"PRIu64" (0x%"PRIx64") - was marked %d (%s)\n"),
		        block, block, ip->i_num.in_addr, ip->i_num.in_addr, q,
		        block_type_string(q));
		add_duplicate_ref(ip, block, REF_AS_META, 0, INODE_VALID);
		if (q == (ip->i_sbd->gfs1 ? GFS2_BLKST_DINODE :
			  GFS2_BLKST_USED))
			/* If the previous reference also saw this as a leaf,
			   it was already checked, so don't check again. */
			return EEXIST; /* non-fatal */
	}
	fsck_blockmap_set(ip, block, _("directory leaf"),
			  ip->i_sbd->gfs1 ? GFS2_BLKST_DINODE :
			  GFS2_BLKST_USED);
	return 0;
}

static int pass1_check_metalist(struct iptr iptr, struct gfs2_buffer_head **bh, int h,
                                int *is_valid, int *was_duplicate, void *private)
{
	struct block_count *bc = (struct block_count *)private;
	struct gfs2_inode *ip = iptr.ipt_ip;
	uint64_t block = iptr_block(iptr);
	struct gfs2_buffer_head *nbh;
	const char *blktypedesc;
	int iblk_type;
	int q;

	*bh = NULL;

	*was_duplicate = 0;
	*is_valid = 0;
	if (!valid_block_ip(ip, block)) { /* blk outside of FS */
		/* The bad dinode should be invalidated later due to
		   "unrecoverable" errors.  The inode itself should be
		   set "free" and removed from the inodetree by
		   undo_check_metalist. */
		fsck_blockmap_set(ip, ip->i_num.in_addr,
				  _("bad block referencing"), GFS2_BLKST_UNLINKED);
		log_debug(_("Bad indirect block (invalid/out of range) "
		            "found in inode %"PRIu64" (0x%"PRIx64").\n"),
		          ip->i_num.in_addr, ip->i_num.in_addr);

		return META_SKIP_FURTHER;
	}
	if (is_dir(ip, ip->i_sbd->gfs1) && h == ip->i_height) {
		iblk_type = GFS2_METATYPE_JD;
		blktypedesc = _("a directory hash table block");
	} else {
		iblk_type = GFS2_METATYPE_IN;
		blktypedesc = _("a journaled data block");
	}
	q = block_type(bl, block);
	if (q != GFS2_BLKST_FREE) {
		log_err(_("Found duplicate block #%"PRIu64" (0x%"PRIx64") referenced "
		          "as metadata in indirect block for dinode "
		          "%"PRIu64" (0x%"PRIx64") - was marked %d (%s)\n"),
			 block, block, ip->i_num.in_addr, ip->i_num.in_addr, q,
			 block_type_string(q));
		*was_duplicate = 1;
	}
	nbh = lgfs2_bread(ip->i_sbd, block);

	*is_valid = (lgfs2_check_meta(nbh->b_data, iblk_type) == 0);

	if (!(*is_valid)) {
		log_err(_("Inode %"PRIu64" (0x%"PRIx64") has a bad indirect block "
		          "pointer %"PRIu64" (0x%"PRIx64") (points to something "
		          "that is not %s).\n"),
			 ip->i_num.in_addr, ip->i_num.in_addr, block, block, blktypedesc);
		if (query(_("Zero the indirect block pointer? (y/n) "))){
			*iptr_ptr(iptr) = 0;
			lgfs2_bmodified(iptr.ipt_bh);
			*is_valid = 1;
			return META_SKIP_ONE;
		} else {
			lgfs2_brelse(nbh);
			return META_SKIP_FURTHER;
		}
	}

	bc->indir_count++;
	if (*was_duplicate) {
		add_duplicate_ref(ip, block, REF_AS_META, 0,
				  *is_valid ? INODE_VALID : INODE_INVALID);
		lgfs2_brelse(nbh);
	} else {
		*bh = nbh;
		fsck_blockmap_set(ip, block, _("indirect"), ip->i_sbd->gfs1 ?
				  GFS2_BLKST_DINODE : GFS2_BLKST_USED);
	}

	if (*is_valid)
		return META_IS_GOOD;
	return META_SKIP_FURTHER;
}

/* undo_reference - undo previously processed data or metadata
 * We've treated the metadata for this dinode as good so far, but not we
 * realize it's bad. So we need to undo what we've done.
 *
 * Returns: 0 - We need to process the block as metadata. In other words,
 *              we need to undo any blocks it refers to.
 *          1 - We can't process the block as metadata.
 */

static int undo_reference(struct gfs2_inode *ip, uint64_t block, int meta,
			  void *private)
{
	struct block_count *bc = (struct block_count *)private;
	struct duptree *dt;
	struct inode_with_dups *id;
	int old_bitmap_state = 0;
	struct rgrp_tree *rgd;

	if (!valid_block_ip(ip, block)) { /* blk outside of FS */
		fsck_blockmap_set(ip, ip->i_num.in_addr, _("bad block referencing"), GFS2_BLKST_FREE);
		return 1;
	}

	if (meta)
		bc->indir_count--;
	dt = dupfind(block);
	if (dt) {
		/* remove all duplicate reference structures from this inode */
		do {
			id = find_dup_ref_inode(dt, ip);
			if (!id)
				break;

			dup_listent_delete(dt, id);
		} while (id);

		if (dt->refs) {
			log_err(_("Block %"PRIu64" (0x%"PRIx64") is still referenced "
				  "from another inode; not freeing.\n"),
			        block, block);
			if (dt->refs == 1) {
				log_err(_("This was the only duplicate "
					  "reference so far; removing it.\n"));
				dup_delete(dt);
			}
			return 1;
		}
	}
	if (!meta) {
		rgd = lgfs2_blk2rgrpd(ip->i_sbd, block);
		old_bitmap_state = lgfs2_get_bitmap(ip->i_sbd, block, rgd);
		if (old_bitmap_state == GFS2_BLKST_DINODE)
			return -1;
	}
	fsck_blockmap_set(ip, block,
			  meta ? _("bad indirect") : _("referenced data"),
			  GFS2_BLKST_FREE);
	return 0;
}

static int undo_check_metalist(struct gfs2_inode *ip, uint64_t block,
			       int h, void *private)
{
	return undo_reference(ip, block, 1, private);
}

static int undo_check_data(struct gfs2_inode *ip, uint64_t block,
			   void *private)
{
	return undo_reference(ip, block, 0, private);
}

/* blockmap_set_as_data - set block as 'data' in the blockmap, if not dinode
 *
 * This function tries to set a block that's referenced as data as 'data'
 * in the fsck blockmap. But if that block is marked as 'dinode' in the
 * rgrp bitmap, it does additional checks to see if it looks like a dinode.
 * Note that previous checks were done for duplicate references, so this
 * is checking for dinodes that we haven't processed yet.
 */
static int blockmap_set_as_data(struct gfs2_inode *ip, uint64_t block)
{
	int error;
	struct gfs2_buffer_head *bh;
	struct gfs2_dinode *di;

	error = fsck_blkmap_set_noino(ip, block, "data", GFS2_BLKST_USED);
	if (!error)
		return 0;

	error = 0;
	/* The bitmap says it's a dinode, but a block reference begs to differ.
	   So which is it? */
	bh = lgfs2_bread(ip->i_sbd, block);
	if (lgfs2_check_meta(bh->b_data, GFS2_METATYPE_DI) != 0)
		goto out;

	/* The meta header agrees it's a dinode. But it might be data in
	   disguise, so do some extra checks. */
	di = (struct gfs2_dinode *)bh->b_data;
	if (be64_to_cpu(di->di_num.no_addr) != block)
		goto out;

	log_err(_("Inode %"PRIu64" (0x%"PRIx64") has a reference to block %"PRIu64" (0x%"PRIx64") "
	          "as a data block, but it appears to be a dinode we haven't checked yet.\n"),
	        ip->i_num.in_addr, ip->i_num.in_addr, block, block);
	error = -1;
out:
	if (!error)
		fsck_blockmap_set(ip, block, "data",  GFS2_BLKST_USED);
	lgfs2_brelse(bh);
	return error;
}

static int pass1_check_data(struct gfs2_inode *ip, uint64_t metablock,
		      uint64_t block, void *private,
		      struct gfs2_buffer_head *bbh, __be64 *ptr)
{
	int q;
	struct block_count *bc = (struct block_count *) private;

	if (!valid_block_ip(ip, block)) {
		log_err(_("inode %"PRIu64" (0x%"PRIx64") has a bad data block pointer "
		          "%"PRIu64" (0x%"PRIx64") (invalid or out of range) "),
		        ip->i_num.in_addr, ip->i_num.in_addr, block, block);
		if (metablock == ip->i_num.in_addr)
			log_err("\n");
		else
			log_err(_("from metadata block %"PRIu64" (0x%"PRIx64")\n"),
			        metablock, metablock);
		/* Mark the owner of this block with the bad_block
		 * designator so we know to check it for out of range
		 * blocks later */
		fsck_blockmap_set(ip, ip->i_num.in_addr, _("bad (out of range) data"), GFS2_BLKST_UNLINKED);
		return -1;
	}
	bc->data_count++; /* keep the count sane anyway */
	q = block_type(bl, block);
	if (q != GFS2_BLKST_FREE) {
		struct gfs2_buffer_head *bh;
		struct gfs2_meta_header *mh;
		uint32_t mh_type;

		log_err(_("Found duplicate %s block %"PRIu64" (0x%"PRIx64") "
		          "referenced as data by dinode %"PRIu64" (0x%"PRIx64") "),
		        block_type_string(q),
		        block, block, ip->i_num.in_addr, ip->i_num.in_addr);
		if (metablock == ip->i_num.in_addr)
			log_err("\n");
		else
			log_err(_("from metadata block %"PRIu64" (0x%"PRIx64")\n"),
			        metablock, metablock);

		switch (q) {
		case GFS2_BLKST_DINODE:
			log_info(_("The block was processed earlier as an "
				   "inode, so it can't possibly be data.\n"));
			/* We still need to add a duplicate record here because
			   when check_metatree tries to delete the inode, we
			   can't have the "undo" functions freeing the block
			   out from other the original referencing inode. */
			add_duplicate_ref(ip, block, REF_AS_DATA, 0,
					  INODE_VALID);
			return 1;
		case GFS2_BLKST_USED: /* tough decision: May be data or meta */
			bh = lgfs2_bread(ip->i_sbd, block);
			mh = (struct gfs2_meta_header *)bh->b_data;
			mh_type = be32_to_cpu(mh->mh_type);
			lgfs2_brelse(bh);
			if (be32_to_cpu(mh->mh_magic) == GFS2_MAGIC &&
			    mh_type >= GFS2_METATYPE_RG &&
			    mh_type <= GFS2_METATYPE_QC &&
			    mh_type != GFS2_METATYPE_DI &&
			    be32_to_cpu(mh->mh_format) % 100 == 0) {
				log_info(_("The block was processed earlier "
					   "as valid metadata, so it can't "
					   "possibly be data.\n"));
				/* We still need to add a duplicate record here
				   because when check_metatree tries to delete
				   the inode, we can't have the "undo"
				   functions freeing the block out from other
				   the original referencing inode. */
				add_duplicate_ref(ip, block, REF_AS_DATA, 0,
						  INODE_VALID);
				return 1;
			}
			log_info( _("Seems to be a normal duplicate; I'll "
				    "sort it out in pass1b.\n"));
			add_duplicate_ref(ip, block, REF_AS_DATA, 0,
					  INODE_VALID);
			/* This inode references the block as data. So if this
			   all is validated, we want to keep this count. */
			return 0;
		case GFS2_BLKST_UNLINKED:
			log_info( _("The block was invalid as metadata but might be "
				    "okay as data.  I'll sort it out in pass1b.\n"));
			add_duplicate_ref(ip, block, REF_AS_DATA, 0, INODE_VALID);
			return 0;
		}
	}
	/* In gfs1, rgrp indirect blocks are marked in the bitmap as "meta".
	   In gfs2, "meta" is only for dinodes. So here we dummy up the
	   blocks so that the bitmap isn't changed improperly. */
	if (ip->i_sbd->gfs1 && ip == ip->i_sbd->md.riinode) {
		log_info(_("Block %"PRIu64" (0x%"PRIx64") is a GFS1 rindex block\n"),
		         block, block);
		gfs2_special_set(&gfs1_rindex_blks, block);
		fsck_blockmap_set(ip, block, "rgrp", GFS2_BLKST_DINODE);
	} else if (ip->i_sbd->gfs1 && ip->i_flags & GFS2_DIF_JDATA) {
		log_info(_("Block %"PRIu64" (0x%"PRIx64") is a GFS1 journaled data block\n"),
		         block, block);
		fsck_blockmap_set(ip, block, "jdata", GFS2_BLKST_DINODE);
	} else
		return blockmap_set_as_data(ip, block);
	return 0;
}

static int ask_remove_inode_eattr(struct gfs2_inode *ip,
				  struct block_count *bc)
{
	if (ip->i_eattr == 0)
		return 0; /* eattr was removed prior to this call */
	log_err(_("Inode %"PRIu64" (0x%"PRIx64") has unrecoverable Extended Attribute errors.\n"),
	        ip->i_num.in_addr, ip->i_num.in_addr);
	if (query( _("Clear all Extended Attributes from the inode? (y/n) "))){
		undo_reference(ip, ip->i_eattr, 0, bc);
		ip->i_eattr = 0;
		bc->ea_count = 0;
		ip->i_blocks = 1 + bc->indir_count + bc->data_count;
		ip->i_flags &= ~GFS2_DIF_EA_INDIRECT;
		lgfs2_bmodified(ip->i_bh);
		log_err( _("Extended attributes were removed.\n"));
	} else {
		log_err( _("Extended attributes were not removed.\n"));
	}
	return 0;
}

static int undo_eattr_indir_or_leaf(struct gfs2_inode *ip, uint64_t block,
				    uint64_t parent,
				    struct gfs2_buffer_head **bh,
				    void *private)
{
	struct gfs2_sbd *sdp = ip->i_sbd;
	int q;
	int error;
	struct block_count *bc = (struct block_count *) private;

	if (!valid_block_ip(ip, block))
		return META_ERROR;

	/* Need to check block_type before undoing the reference, which can
	   set it to free, which would cause the test below to fail. */
	q = block_type(bl, block);

	error = undo_reference(ip, block, 0, private);
	if (error)
		return error;

	bc->ea_count--;

	if (q != (sdp->gfs1 ? GFS2_BLKST_DINODE : GFS2_BLKST_USED))
		return 1;

	*bh = lgfs2_bread(sdp, block);
	return 0;
}

/* complain_eas - complain about extended attribute errors for an inode
 *
 * @ip       - in core inode pointer
 * block     - the block that had the problem
 * duplicate - if this is a duplicate block, don't set it "free"
 * emsg      - what to tell the user about the eas being checked
 * Returns: 1 if the EA is fixed, else 0 if it was not fixed.
 */
static void complain_eas(struct gfs2_inode *ip, uint64_t block,
			 const char *emsg)
{
	log_err(_("Inode #%"PRIu64" (0x%"PRIx64"): %s"), ip->i_num.in_addr, ip->i_num.in_addr, emsg);
	log_err(_(" at block #%"PRIu64" (0x%"PRIx64").\n"), block, block);
}

static int check_eattr_indir(struct gfs2_inode *ip, uint64_t indirect,
			     uint64_t parent, struct gfs2_buffer_head **bh,
			     void *private)
{
	struct gfs2_sbd *sdp = ip->i_sbd;
	int ret = 0;
	int q;
	struct block_count *bc = (struct block_count *) private;

	/* This inode contains an eattr - it may be invalid, but the
	 * eattr attributes points to a non-zero block */
	if (!valid_block_ip(ip, indirect)) {
		/* Doesn't help to mark this here - this gets checked
		 * in pass1c */
		return 1;
	}
	q = block_type(bl, indirect);

	/* Special duplicate processing:  If we have an EA block,
	   check if it really is an EA.  If it is, let duplicate
	   handling sort it out.  If it isn't, clear it but don't
	   count it as a duplicate. */
	*bh = lgfs2_bread(sdp, indirect);
	if (lgfs2_check_meta((*bh)->b_data, GFS2_METATYPE_IN)) {
		bc->ea_count++;
		if (q != GFS2_BLKST_FREE) { /* Duplicate? */
			add_duplicate_ref(ip, indirect, REF_AS_EA, 0,
					  INODE_VALID);
			complain_eas(ip, indirect,
				     _("Bad indirect Extended Attribute "
				       "duplicate found"));
			/* Return 0 here because if all that's wrong is a
			   duplicate block reference, we want pass1b to figure
			   it out. We don't want to delete all the extended
			   attributes as if they are in error. */
			return 0;
		}
		complain_eas(ip, indirect,
			     _("Extended Attribute indirect block has "
			       "incorrect type"));
		return 1;
	}
	if (q != GFS2_BLKST_FREE) { /* Duplicate? */
		add_duplicate_ref(ip, indirect, REF_AS_EA, 0, INODE_VALID);
		complain_eas(ip, indirect,
			     _("Duplicate Extended Attribute indirect block"));
		bc->ea_count++;
		ret = 0; /* For the same reason stated above. */
	} else {
		fsck_blockmap_set(ip, indirect,
				  _("indirect Extended Attribute"), sdp->gfs1 ?
				  GFS2_BLKST_DINODE : GFS2_BLKST_USED);
		bc->ea_count++;
	}
	return ret;
}

static int finish_eattr_indir(struct gfs2_inode *ip, int leaf_pointers,
			      int leaf_pointer_errors, void *private)
{
	struct block_count *bc = (struct block_count *) private;

	if (leaf_pointer_errors == leaf_pointers) /* All eas were bad */
		return ask_remove_inode_eattr(ip, bc);
	log_debug(_("Marking inode #%"PRIu64" (0x%"PRIx64") with extended attribute block\n"),
		   ip->i_num.in_addr, ip->i_num.in_addr);
	if (!leaf_pointer_errors)
		return 0;
	log_err(_("Inode %"PRIu64" (0x%"PRIx64") has recoverable indirect extended attribute errors.\n"),
	        ip->i_num.in_addr, ip->i_num.in_addr);
	if (query( _("Okay to fix the block count for the inode? (y/n) "))) {
		ip->i_blocks = 1 + bc->indir_count + bc->data_count + bc->ea_count;
		lgfs2_bmodified(ip->i_bh);
		log_err(_("Block count fixed: 1+%"PRIu64"+%"PRIu64"+%"PRIu64" = %"PRIu64".\n"),
		        bc->indir_count, bc->data_count, bc->ea_count, ip->i_blocks);
		return 1;
	}
	log_err( _("Block count not fixed.\n"));
	return 1;
}

/* check_ealeaf_block
 *      checks an extended attribute (not directory) leaf block
 */
static int check_ealeaf_block(struct gfs2_inode *ip, uint64_t block, int btype,
			      struct gfs2_buffer_head **bh, void *private)
{
	struct gfs2_buffer_head *leaf_bh = NULL;
	struct gfs2_sbd *sdp = ip->i_sbd;
	int q;
	struct block_count *bc = (struct block_count *) private;

	q = block_type(bl, block);
	/* Special duplicate processing:  If we have an EA block, check if it
	   really is an EA.  If it is, let duplicate handling sort it out.
	   If it isn't, clear it but don't count it as a duplicate. */
	leaf_bh = lgfs2_bread(sdp, block);
	if (lgfs2_check_meta(leaf_bh->b_data, btype)) {
		bc->ea_count++;
		if (q != GFS2_BLKST_FREE) { /* Duplicate? */
			add_duplicate_ref(ip, block, REF_AS_EA, 0,
					  INODE_VALID);
			complain_eas(ip, block, _("Extended attribute leaf "
						  "duplicate found"));
			/* Return 0 here because if all that's wrong is a
			   duplicate block reference, we want pass1b to figure
			   it out. We don't want to delete all the extended
			   attributes as if they are in error. */
			return 0;
		}
		complain_eas(ip, block, _("Extended Attribute leaf block has "
					  "incorrect type"));
		lgfs2_brelse(leaf_bh);
		return 1;
	}
	if (q != GFS2_BLKST_FREE) { /* Duplicate? */
		complain_eas(ip, block, _("Extended Attribute leaf "
					  "duplicate found"));
		add_duplicate_ref(ip, block, REF_AS_DATA, 0, INODE_VALID);
		bc->ea_count++;
		lgfs2_brelse(leaf_bh);
		/* Return 0 here because if all that's wrong is a duplicate
		   block reference, we want pass1b to figure it out. We don't
		   want to delete all the extended attributes as if they are
		   in error. */
		return 0;
	}
	/* Point of confusion: We've got to set the ea block itself to
	   GFS2_BLKST_USED here.  Elsewhere we mark the inode with
	   gfs2_eattr_block meaning it contains an eattr. */
	fsck_blockmap_set(ip, block, _("Extended Attribute"),
			  sdp->gfs1 ? GFS2_BLKST_DINODE : GFS2_BLKST_USED);
	bc->ea_count++;
	*bh = leaf_bh;
	return 0;
}

/**
 * check_extended_leaf_eattr
 * @ip
 * @el_blk: block number of the extended leaf
 *
 * An EA leaf block can contain EA's with pointers to blocks
 * where the data for that EA is kept.  Those blocks still
 * have the gfs2 meta header of type GFS2_METATYPE_EA
 *
 * Returns: 0 if correct[able], -1 if removal is needed
 */
static int check_extended_leaf_eattr(struct gfs2_inode *ip, int i,
				     __be64 *data_ptr,
				     struct gfs2_buffer_head *leaf_bh,
				     uint32_t tot_ealen,
				     struct gfs2_ea_header *ea_hdr,
				     struct gfs2_ea_header *ea_hdr_prev,
				     void *private)
{
	uint64_t el_blk = be64_to_cpu(*data_ptr);
	struct gfs2_sbd *sdp = ip->i_sbd;
	struct gfs2_buffer_head *bh = NULL;
	int error = 0;

	if (!valid_block_ip(ip, el_blk)) {
		log_err(_("Inode #%"PRIu64" (0x%"PRIx64"): extended attribute block "
		          "%"PRIu64" (0x%"PRIx64") has an extended leaf block #%"PRIu64" "
		          "(0x%"PRIx64") that is invalid or out of range.\n"),
		        ip->i_num.in_addr, ip->i_num.in_addr, ip->i_eattr, ip->i_eattr, el_blk, el_blk);
		fsck_blockmap_set(ip, ip->i_eattr, _("bad (out of range) Extended Attribute "),
		                  GFS2_BLKST_UNLINKED);
		error = 1;
	} else {
		error = check_ealeaf_block(ip, el_blk, GFS2_METATYPE_ED, &bh,
					   private);
	}
	if (bh)
		lgfs2_brelse(bh);
	if (error) {
		log_err(_("Bad extended attribute found at block %"PRIu64" (0x%"PRIx64")"),
		        be64_to_cpu(*data_ptr), be64_to_cpu(*data_ptr));
		if (query( _("Repair the bad Extended Attribute? (y/n) "))) {
			ea_hdr->ea_num_ptrs = i;
			ea_hdr->ea_data_len = cpu_to_be32(tot_ealen);
			*data_ptr = 0;
			lgfs2_bmodified(leaf_bh);
			/* Endianness doesn't matter in this case because it's
			   a single byte. */
			fsck_blockmap_set(ip, ip->i_eattr,
					  _("extended attribute"),
					  sdp->gfs1 ? GFS2_BLKST_DINODE :
					  GFS2_BLKST_USED);
			log_err( _("The EA was fixed.\n"));
			error = 0;
		} else {
			error = 1;
			log_err( _("The bad EA was not fixed.\n"));
		}
	}
	return error;
}

static int check_eattr_leaf(struct gfs2_inode *ip, uint64_t block,
			    uint64_t parent, struct gfs2_buffer_head **bh,
			    void *private)
{
	if (!valid_block_ip(ip, block)) {
		log_warn(_("Inode #%"PRIu64" (0x%"PRIx64"): extended attribute leaf "
		           "block #%"PRIu64" (0x%"PRIx64") is invalid or out of "
		           "range.\n"),
		         ip->i_num.in_addr, ip->i_num.in_addr, block, block);
		fsck_blockmap_set(ip, ip->i_eattr, _("bad (out of range) extended attribute leaf"),
		                  GFS2_BLKST_UNLINKED);
		return 1;
	}
	return check_ealeaf_block(ip, block, GFS2_METATYPE_EA, bh, private);
}

static int ask_remove_eattr_entry(struct gfs2_sbd *sdp,
				  struct gfs2_buffer_head *leaf_bh,
				  struct gfs2_ea_header *curr,
				  struct gfs2_ea_header *prev,
				  int fix_curr, int fix_curr_len)
{
	if (!query( _("Remove the bad Extended Attribute entry? (y/n) "))) {
		log_err( _("Bad Extended Attribute not removed.\n"));
		return 0;
	}
	if (fix_curr)
		curr->ea_flags |= GFS2_EAFLAG_LAST;
	if (fix_curr_len) {
		uint32_t max_size = sdp->sd_bsize;
		uint32_t offset = (uint32_t)(((unsigned long)curr) -
					     ((unsigned long)leaf_bh->b_data));
		curr->ea_rec_len = cpu_to_be32(max_size - offset);
	}
	if (!prev)
		curr->ea_type = GFS2_EATYPE_UNUSED;
	else {
		uint32_t tmp32 = be32_to_cpu(curr->ea_rec_len) +
			be32_to_cpu(prev->ea_rec_len);
		prev->ea_rec_len = cpu_to_be32(tmp32);
		if (curr->ea_flags & GFS2_EAFLAG_LAST)
			prev->ea_flags |= GFS2_EAFLAG_LAST;
	}
	log_err(_("Bad Extended Attribute at block %"PRIu64" (0x%"PRIx64") removed.\n"),
	        leaf_bh->b_blocknr, leaf_bh->b_blocknr);
	lgfs2_bmodified(leaf_bh);
	return 1;
}

static int eatype_max(unsigned fs_format)
{
	int max = 4;
	if (fs_format < 1802)
		max = 3;
	return max;
}

static int check_eattr_entries(struct gfs2_inode *ip,
			       struct gfs2_buffer_head *leaf_bh,
			       struct gfs2_ea_header *ea_hdr,
			       struct gfs2_ea_header *ea_hdr_prev,
			       void *private)
{
	struct gfs2_sbd *sdp = ip->i_sbd;
	char ea_name[256];
	uint32_t offset = (uint32_t)(((unsigned long)ea_hdr) -
				     ((unsigned long)leaf_bh->b_data));
	uint32_t max_size = sdp->sd_bsize;
	uint32_t avail_size;
	int max_ptrs;

	if (!ea_hdr->ea_name_len){
		log_err( _("EA has name length of zero\n"));
		return ask_remove_eattr_entry(sdp, leaf_bh, ea_hdr,
					      ea_hdr_prev, 1, 1);
	}
	if (offset + be32_to_cpu(ea_hdr->ea_rec_len) > max_size){
		log_err( _("EA rec length too long\n"));
		return ask_remove_eattr_entry(sdp, leaf_bh, ea_hdr,
					      ea_hdr_prev, 1, 1);
	}
	if (offset + be32_to_cpu(ea_hdr->ea_rec_len) == max_size &&
	   (ea_hdr->ea_flags & GFS2_EAFLAG_LAST) == 0){
		log_err( _("last EA has no last entry flag\n"));
		return ask_remove_eattr_entry(sdp, leaf_bh, ea_hdr,
					      ea_hdr_prev, 0, 0);
	}
	if (!ea_hdr->ea_name_len){
		log_err( _("EA has name length of zero\n"));
		return ask_remove_eattr_entry(sdp, leaf_bh, ea_hdr,
					      ea_hdr_prev, 0, 0);
	}

	memset(ea_name, 0, sizeof(ea_name));
	strncpy(ea_name, (char *)ea_hdr + sizeof(struct gfs2_ea_header),
		ea_hdr->ea_name_len);

	if (ea_hdr->ea_type > eatype_max(sdp->sd_fs_format) &&
	   ((ea_hdr_prev) || (!ea_hdr_prev && ea_hdr->ea_type))){
		/* Skip invalid entry */
		log_err(_("EA (%s) type is invalid (%d > %d).\n"),
			ea_name, ea_hdr->ea_type, eatype_max(sdp->sd_fs_format));
		return ask_remove_eattr_entry(sdp, leaf_bh, ea_hdr,
					      ea_hdr_prev, 0, 0);
	}

	if (!ea_hdr->ea_num_ptrs)
		return 0;

	avail_size = sdp->sd_bsize - sizeof(struct gfs2_meta_header);
	max_ptrs = (be32_to_cpu(ea_hdr->ea_data_len)+avail_size-1)/avail_size;

	if (max_ptrs > ea_hdr->ea_num_ptrs) {
		log_err(_("EA (%s) has incorrect number of pointers.\n"),
			ea_name);
		log_err(_("  Required:  %d\n  Reported:  %d\n"),
			max_ptrs, ea_hdr->ea_num_ptrs);
		return ask_remove_eattr_entry(sdp, leaf_bh, ea_hdr,
					      ea_hdr_prev, 0, 0);
	} else {
		log_debug( _("  Pointers Required: %d\n  Pointers Reported: %d\n"),
			   max_ptrs, ea_hdr->ea_num_ptrs);
	}
	return 0;
}

/**
 * Check for massive amounts of pointer corruption.  If the block has
 * lots of out-of-range pointers, we can't trust any of the pointers.
 * For example, a stray pointer with a value of 0x1d might be
 * corruption/nonsense, and if so, we don't want to delete an
 * important file (like master or the root directory) because of it.
 * We need to check for a large number of bad pointers BEFORE we start
 * messing with them because we don't want to mark a block as a
 * duplicate (for example) until we know if the pointers in general can
 * be trusted. Thus it needs to be in a separate loop.
 * Returns: 0 if good range, otherwise != 0
 */
enum b_types { BTYPE_META, BTYPE_LEAF, BTYPE_DATA, BTYPE_IEATTR, BTYPE_EATTR};
static const char *btypes[5] = {
	"metadata", "leaf", "data", "indirect extended attribute",
	"extended attribute" };

static int rangecheck_block(struct gfs2_inode *ip, uint64_t block,
			    struct gfs2_buffer_head **bh, enum b_types btype,
			    void *private)
{
	long *bad_pointers = (long *)private;
	int q;

	if (!valid_block_ip(ip, block)) {
		(*bad_pointers)++;
		log_info(_("Bad %s block pointer (invalid or out of range "
		           "#%ld) found in inode %"PRIu64" (0x%"PRIx64").\n"),
		         btypes[btype], *bad_pointers, ip->i_num.in_addr, ip->i_num.in_addr);
		if ((*bad_pointers) <= BAD_POINTER_TOLERANCE)
			return META_IS_GOOD;
		else
			return META_ERROR; /* Exits check_metatree quicker */
	}
	/* See how many duplicate blocks it has */
	q = block_type(bl, block);
	if (q != GFS2_BLKST_FREE) {
		(*bad_pointers)++;
		log_info(_("Duplicated %s block pointer (violation %ld, block %"PRIu64
		           " (0x%"PRIx64")) found in inode %"PRIu64" (0x%"PRIx64").\n"),
			  btypes[btype], *bad_pointers, block, block, ip->i_num.in_addr, ip->i_num.in_addr);
		if ((*bad_pointers) <= BAD_POINTER_TOLERANCE)
			return META_IS_GOOD;
		else {
			log_debug(_("Inode 0x%"PRIx64" bad pointer tolerance "
			            "exceeded: block 0x%"PRIx64".\n"),
			          ip->i_num.in_addr, block);
			return META_ERROR; /* Exits check_metatree quicker */
		}
	}
	return META_IS_GOOD;
}

static int rangecheck_metadata(struct iptr iptr, struct gfs2_buffer_head **bh, int h,
                               int *is_valid, int *was_duplicate, void *private)
{
	struct gfs2_inode *ip = iptr.ipt_ip;
	uint64_t block = iptr_block(iptr);

	*is_valid = 1;
	*was_duplicate = 0;
	return rangecheck_block(ip, block, bh, BTYPE_META, private);
}

static int rangecheck_leaf(struct gfs2_inode *ip, uint64_t block,
			   void *private)
{
	return rangecheck_block(ip, block, NULL, BTYPE_LEAF, private);
}

static int rangecheck_data(struct gfs2_inode *ip, uint64_t metablock,
			   uint64_t block, void *private,
			   struct gfs2_buffer_head *bh, __be64 *ptr)
{
	return rangecheck_block(ip, block, NULL, BTYPE_DATA, private);
}

static int rangecheck_eattr_indir(struct gfs2_inode *ip, uint64_t block,
				  uint64_t parent,
				  struct gfs2_buffer_head **bh, void *private)
{
	return rangecheck_block(ip, block, NULL, BTYPE_IEATTR, private);
}

static int rangecheck_eattr_leaf(struct gfs2_inode *ip, uint64_t block,
				 uint64_t parent, struct gfs2_buffer_head **bh,
				 void *private)
{
	return rangecheck_block(ip, block, NULL, BTYPE_EATTR, private);
}

static struct metawalk_fxns rangecheck_fxns = {
        .private = NULL,
	.readahead = 1,
        .check_metalist = rangecheck_metadata,
        .check_data = rangecheck_data,
        .check_leaf = rangecheck_leaf,
        .check_eattr_indir = rangecheck_eattr_indir,
        .check_eattr_leaf = rangecheck_eattr_leaf,
	.delete_block = delete_block,
};

static struct metawalk_fxns eattr_undo_fxns = {
	.private = NULL,
	.check_eattr_indir = undo_eattr_indir_or_leaf,
	.check_eattr_leaf = undo_eattr_indir_or_leaf,
	.finish_eattr_indir = finish_eattr_indir,
	.delete_block = delete_block,
};
/* set_ip_blockmap - set the blockmap for a dinode
 *
 * returns: 0 if no error, -EINVAL if dinode has a bad mode, -EPERM on error
 */
static int set_ip_blockmap(struct gfs2_inode *ip)
{
	uint64_t block = ip->i_bh->b_blocknr;
	struct lgfs2_inum no;
	uint32_t mode;
	const char *ty;

	if (ip->i_sbd->gfs1)
		mode = gfs_to_gfs2_mode(ip);
	else
		mode = ip->i_mode & S_IFMT;

	switch (mode) {
	case S_IFDIR:
		ty = "directory";
		break;
	case S_IFREG:
		ty = "file";
		break;
	case S_IFLNK:
		ty = "symlink";
		break;
	case S_IFBLK:
		ty = "block device";
		break;
	case S_IFCHR:
		ty = "character device";
		break;
	case S_IFIFO:
		ty = "fifo";
		break;
	case S_IFSOCK:
		ty = "socket";
		break;
	default:
		return -EINVAL;
	}
	no = ip->i_num;
	if (fsck_blockmap_set(ip, block, ty, GFS2_BLKST_DINODE) ||
	    (mode == S_IFDIR && !dirtree_insert(no))) {
		stack;
		return -EPERM;
	}
	return 0;
}

static int alloc_metalist(struct iptr iptr, struct gfs2_buffer_head **bh, int h,
                          int *is_valid, int *was_duplicate, void *private)
{
	const char *desc = (const char *)private;
	struct gfs2_inode *ip = iptr.ipt_ip;
	uint64_t block = iptr_block(iptr);
	int q;

	/* No need to range_check here--if it was added, it's in range. */
	/* We can't check the bitmap here because this function is called
	   after the bitmap has been set but before the blockmap has. */
	*is_valid = 1;
	*was_duplicate = 0;
	*bh = lgfs2_bread(ip->i_sbd, block);
	q = bitmap_type(ip->i_sbd, block);
	if (q == GFS2_BLKST_FREE) {
		log_debug(_("%s reference to new metadata block %"PRIu64" (0x%"PRIx64") is now marked as indirect.\n"),
		          desc, block, block);
		gfs2_blockmap_set(bl, block, ip->i_sbd->gfs1 ?
				  GFS2_BLKST_DINODE : GFS2_BLKST_USED);
	}
	return META_IS_GOOD;
}

static int alloc_data(struct gfs2_inode *ip, uint64_t metablock,
		      uint64_t block, void *private,
		      struct gfs2_buffer_head *bh, __be64 *ptr)
{
	int q;
	const char *desc = (const char *)private;

	/* No need to range_check here--if it was added, it's in range. */
	/* We can't check the bitmap here because this function is called
	   after the bitmap has been set but before the blockmap has. */
	q = bitmap_type(ip->i_sbd, block);
	if (q == GFS2_BLKST_FREE) {
		log_debug(_("%s reference to new data block %"PRIu64" (0x%"PRIx64") is now marked as data.\n"),
		          desc, block, block);
		gfs2_blockmap_set(bl, block, GFS2_BLKST_USED);
	}
	return 0;
}

static int alloc_leaf(struct gfs2_inode *ip, uint64_t block, void *private)
{
	int q;

	/* No need to range_check here--if it was added, it's in range. */
	/* We can't check the bitmap here because this function is called
	   after the bitmap has been set but before the blockmap has. */
	q = bitmap_type(ip->i_sbd, block);
	if (q == GFS2_BLKST_FREE)
		fsck_blockmap_set(ip, block, "newly allocated leaf",
				  ip->i_sbd->gfs1 ? GFS2_BLKST_DINODE :
				  GFS2_BLKST_USED);
	return 0;
}

static struct metawalk_fxns alloc_fxns = {
	.private = NULL,
	.check_leaf = alloc_leaf,
	.check_metalist = alloc_metalist,
	.check_data = alloc_data,
	.check_eattr_indir = NULL,
	.check_eattr_leaf = NULL,
	.check_dentry = NULL,
	.check_eattr_entry = NULL,
	.check_eattr_extentry = NULL,
	.finish_eattr_indir = NULL,
	.delete_block = delete_block,
};

/*
 * pass1_check_metatree - wrapper function for check_metatree
 *
 * Generic function check_metatree sets the bitmap values, but not the
 * corresponding values in the blockmap. If we get an error, the inode will
 * have been freed in the bitmap. We need to set the inode address as free
 * as well.
 */
static int pass1_check_metatree(struct gfs2_inode *ip,
				struct metawalk_fxns *pass)
{
	int error;

	error = check_metatree(ip, pass);
	if (error)
		gfs2_blockmap_set(bl, ip->i_num.in_addr, GFS2_BLKST_FREE);
	return error;
}

/*
 * reprocess_inode - fixes the blockmap to match the bitmap due to an
 *                   unexpected block allocation via libgfs2.
 *
 * The problem we're trying to overcome here is when a new block must be
 * added to a dinode because of a write.  This will happen when lost+found
 * needs a new indirect block for its hash table.  In that case, the write
 * causes a new block to be assigned in the bitmap but that block is not yet
 * accurately reflected in the fsck blockmap.  We need to compensate here.
 *
 * We can't really use fsck_blockmap_set here because the new block
 * was already allocated by libgfs2 and therefore it took care of
 * the rgrp free space variable.  fsck_blockmap_set adjusts the free space
 * in the rgrp according to the change, which has already been done.
 * So it's only our blockmap that now disagrees with the rgrp bitmap, so we
 * need to fix only that.
 */
static void reprocess_inode(struct gfs2_inode *ip, const char *desc)
{
	int error;

	alloc_fxns.private = (void *)desc;
	log_info(_("%s inode %"PRIu64" (0x%"PRIx64") had blocks added; reprocessing "
	           "its metadata tree at height=%d.\n"), desc,
	         ip->i_num.in_addr, ip->i_num.in_addr, ip->i_height);
	error = pass1_check_metatree(ip, &alloc_fxns);
	if (error)
		log_err( _("Error %d reprocessing the %s metadata tree.\n"),
			 error, desc);
}

/*
 * handle_ip - process an incore structure representing a dinode.
 */
static int handle_ip(struct gfs2_sbd *sdp, struct gfs2_inode *ip)
{
	int error;
	struct block_count bc = {0};
	long bad_pointers;
	uint64_t lf_blks = 0;

	bad_pointers = 0L;

	/* First, check the metadata for massive amounts of pointer corruption.
	   Such corruption can only lead us to ruin trying to clean it up,
	   so it's better to check it up front and delete the inode if
	   there is corruption. */
	rangecheck_fxns.private = &bad_pointers;
	error = pass1_check_metatree(ip, &rangecheck_fxns);
	if (bad_pointers > BAD_POINTER_TOLERANCE) {
		log_err(_("Error: inode %"PRIu64" (0x%"PRIx64") has more than %d bad pointers.\n"),
		        ip->i_num.in_addr, ip->i_num.in_addr, BAD_POINTER_TOLERANCE);
		fsck_blockmap_set(ip, ip->i_num.in_addr, _("badly corrupt"), GFS2_BLKST_FREE);
		return 0;
	}

	error = set_ip_blockmap(ip);
	if (error == -EINVAL) {
		/* We found a dinode that has an invalid mode. At this point
		   set_ip_blockmap returned an error, which means it never
		   got inserted into the inode tree. Since we haven't even
		   processed its metadata with pass1_fxns, none of its
		   metadata will be flagged as metadata or data blocks yet.
		   Therefore, we don't need to invalidate anything. */
		fsck_blockmap_set(ip, ip->i_num.in_addr, _("invalid mode"), GFS2_BLKST_FREE);
		return 0;
	} else if (error)
		goto bad_dinode;

	if (set_di_nlink(ip))
		goto bad_dinode;

	if (lf_dip)
		lf_blks = lf_dip->i_blocks;

	pass1_fxns.private = &bc;
	error = pass1_check_metatree(ip, &pass1_fxns);

	/* Pass1 may have added some blocks to lost+found by virtue of leafs
	   that were misplaced. If it did, we need to reprocess lost+found
	   to correctly account for its blocks. */
	if (lf_dip && lf_dip->i_blocks != lf_blks)
		reprocess_inode(lf_dip, "lost+found");

	/* We there was an error, we return 0 because we want fsck to continue
	   and analyze the other dinodes as well. */
	if (fsck_abort)
		return 0;

	if (!error) {
		error = check_inode_eattr(ip, &pass1_fxns);

		if (error) {
			if (!query(_("Clear the bad Extended Attributes? "
				    "(y/n) "))) {
				log_err( _("The bad Extended Attributes were "
					   "not fixed.\n"));
				return 0;
			}
			log_err(_("Clearing the bad Extended Attributes in "
			          "inode %"PRIu64" (0x%"PRIx64").\n"),
			        ip->i_num.in_addr, ip->i_num.in_addr);
			eattr_undo_fxns.private = &bc;
			check_inode_eattr(ip, &eattr_undo_fxns);
			ask_remove_inode_eattr(ip, &bc);
			return 1;
		}
	}

	if (ip->i_blocks != (1 + bc.indir_count + bc.data_count + bc.ea_count)) {
		log_err(_("Inode #%"PRIu64" (0x%"PRIx64"): Ondisk block count (%"PRIu64
			") does not match what fsck found (%"PRIu64")\n"),
		        ip->i_num.in_addr, ip->i_num.in_addr, ip->i_blocks, 1 + bc.indir_count +
		        bc.data_count + bc.ea_count);
		log_info(_("inode has: %"PRIu64", but fsck counts: Dinode:1 + "
		           "indir:%"PRIu64" + data: %"PRIu64" + ea: %"PRIu64"\n"),
		         ip->i_blocks, bc.indir_count, bc.data_count, bc.ea_count);
		if (query( _("Fix ondisk block count? (y/n) "))) {
			ip->i_blocks = 1 + bc.indir_count + bc.data_count + bc.ea_count;
			lgfs2_bmodified(ip->i_bh);
			log_err(_("Block count for #%"PRIu64" (0x%"PRIx64") fixed\n"),
			        ip->i_num.in_addr, ip->i_num.in_addr);
		} else
			log_err(_("Bad block count for #%"PRIu64" (0x%"PRIx64") not fixed\n"),
			        ip->i_num.in_addr, ip->i_num.in_addr);
	}

	return 0;
bad_dinode:
	stack;
	return -1;
}

static void check_i_goal(struct gfs2_sbd *sdp, struct gfs2_inode *ip)
{
	if (sdp->gfs1 || ip->i_flags & GFS2_DIF_SYSTEM)
		return;

	if (ip->i_goal_meta <= LGFS2_SB_ADDR(sdp) ||
	    ip->i_goal_meta > sdp->fssize) {
		log_err(_("Inode #%"PRIu64" (0x%"PRIx64"): Bad allocation goal block "
		          "found: %"PRIu64" (0x%"PRIx64")\n"),
		        ip->i_num.in_addr, ip->i_num.in_addr, ip->i_goal_meta, ip->i_goal_meta);
		if (query(_("Fix goal block in inode #%"PRIu64" (0x%"PRIx64")? (y/n) "),
		          ip->i_num.in_addr, ip->i_num.in_addr)) {
			ip->i_goal_meta = ip->i_num.in_addr;
			lgfs2_bmodified(ip->i_bh);
		} else
			log_err(_("Allocation goal block in inode #%"PRIu64
			          " (0x%"PRIx64") not fixed\n"),
			        ip->i_num.in_addr, ip->i_num.in_addr);
	}
}

/*
 * handle_di - This is now a wrapper function that takes a gfs2_buffer_head
 *             and calls handle_ip, which takes an in-code dinode structure.
 */
static int handle_di(struct gfs2_sbd *sdp, struct rgrp_tree *rgd,
		     struct gfs2_buffer_head *bh)
{
	int error = 0;
	uint64_t block = bh->b_blocknr;
	struct gfs2_inode *ip;

	ip = fsck_inode_get(sdp, rgd, bh);

	if (ip->i_num.in_addr != block) {
		log_err(_("Inode #%"PRIu64" (0x%"PRIx64"): Bad inode address found: %"PRIu64
		          " (0x%"PRIx64")\n"),
		        block, block, ip->i_num.in_addr, ip->i_num.in_addr);
		if (query(_("Fix address in inode at block #%"PRIu64" (0x%"PRIx64")? (y/n) "),
		          block, block)) {
			ip->i_num.in_addr = ip->i_num.in_formal_ino = block;
			lgfs2_bmodified(ip->i_bh);
		} else
			log_err(_("Address in inode at block #%"PRIu64" (0x%"PRIx64" not fixed\n"),
			        block, block);
	}
	if (sdp->gfs1 && ip->i_num.in_formal_ino != block) {
		log_err(_("Inode #%"PRIu64" (0x%"PRIx64"): GFS1 formal inode number "
		          "mismatch: was %"PRIu64" (0x%"PRIx64")\n"),
		        block, block, ip->i_num.in_formal_ino, ip->i_num.in_formal_ino);
		if (query(_("Fix formal inode number in inode #%"PRIu64" (0x%"PRIx64")? (y/n) "),
		          block, block)) {
			ip->i_num.in_formal_ino = block;
			lgfs2_bmodified(ip->i_bh);
		} else
			log_err(_("Inode number in inode at block %"PRIu64" (0x%"PRIx64") not fixed\n"),
			        block, block);
	}
	check_i_goal(sdp, ip);
	error = handle_ip(sdp, ip);
	fsck_inode_put(&ip);
	return error;
}

/* Check system inode and verify it's marked "in use" in the bitmap:       */
/* Should work for all system inodes: root, master, jindex, per_node, etc. */
/* We have to pass the sysinode as ** because the pointer may change out from
   under the reference by way of the builder() function.  */
static int check_system_inode(struct gfs2_sbd *sdp,
			      struct gfs2_inode **sysinode,
			      const char *filename,
			      int builder(struct gfs2_sbd *sdp), int isdir,
			      struct gfs2_inode *sysdir, int needs_sysbit)
{
	uint64_t iblock = 0;
	struct dir_status ds = {0};
	int error, err = 0;

	log_info( _("Checking system inode '%s'\n"), filename);
	if (*sysinode) {
		/* Read in the system inode, look at its dentries, and start
		 * reading through them */
		iblock = (*sysinode)->i_num.in_addr;
		log_info(_("System inode for '%s' is located at block %"PRIu64" (0x%"PRIx64")\n"),
		         filename, iblock, iblock);
		if (lgfs2_check_meta((*sysinode)->i_bh->b_data, GFS2_METATYPE_DI)) {
			log_err(_("Found invalid system dinode at block %"PRIu64" (0x%"PRIx64")\n"),
			        iblock, iblock);
			gfs2_blockmap_set(bl, iblock, GFS2_BLKST_FREE);
			check_n_fix_bitmap(sdp, (*sysinode)->i_rgd, iblock, 0,
					   GFS2_BLKST_FREE);
			lgfs2_inode_put(sysinode);
		}
	}
	if (*sysinode) {
		ds.q = block_type(bl, iblock);
		/* If the inode exists but the block is marked free, we might
		   be recovering from a corrupt bitmap.  In that case, don't
		   rebuild the inode.  Just reuse the inode and fix the
		   bitmap. */
		if (ds.q == GFS2_BLKST_FREE) {
			log_info( _("The inode exists but the block is not "
				    "marked 'in use'; fixing it.\n"));
			fsck_blockmap_set(*sysinode, (*sysinode)->i_num.in_addr,
					  filename, GFS2_BLKST_DINODE);
			ds.q = GFS2_BLKST_DINODE;
			if (isdir) {
				struct lgfs2_inum no = (*sysinode)->i_num;
				dirtree_insert(no);
			}
		}
		/* Make sure it's marked as a system file/directory */
		if (needs_sysbit &&
		    !((*sysinode)->i_flags & GFS2_DIF_SYSTEM)) {
			log_err( _("System inode %s is missing the 'system' "
				   "flag. It should be rebuilt.\n"), filename);
			if (sysdir && query(_("Delete the corrupt %s system "
					      "inode? (y/n) "), filename)) {
				lgfs2_inode_put(sysinode);
				lgfs2_dirent_del(sysdir, filename,
						strlen(filename));
				/* Set the blockmap (but not bitmap) back to
				   'free' so that it gets checked like any
				   normal dinode. */
				gfs2_blockmap_set(bl, iblock, GFS2_BLKST_FREE);
				log_err( _("Removed system inode \"%s\".\n"),
					 filename);
			}
		}
	} else
		log_info( _("System inode for '%s' is corrupt or missing.\n"),
			  filename);
	/* If there are errors with the inode here, we need to create a new
	   inode and get it all setup - of course, everything will be in
	   lost+found then, but we *need* our system inodes before we can
	   do any of that. */
	if (!(*sysinode) || ds.q != GFS2_BLKST_DINODE) {
		log_err(_("Invalid or missing %s system inode (is '%s', "
			  "should be '%s').\n"), filename,
			block_type_string(ds.q),
			block_type_string(GFS2_BLKST_DINODE));
		if (query(_("Create new %s system inode? (y/n) "), filename)) {
			log_err( _("Rebuilding system file \"%s\"\n"),
				 filename);
			error = builder(sdp);
			if (error) {
				log_err( _("Error rebuilding system "
					   "inode %s: Cannot continue\n"),
					 filename);
				return error;
			}
			if (*sysinode == sdp->md.jiinode)
				ji_update(sdp);
			fsck_blockmap_set(*sysinode, (*sysinode)->i_num.in_addr,
					  filename, GFS2_BLKST_DINODE);
			ds.q = GFS2_BLKST_DINODE;
			if (isdir) {
				struct lgfs2_inum no = (*sysinode)->i_num;
				dirtree_insert(no);
			}
		} else {
			log_err( _("Cannot continue without valid %s inode\n"),
				filename);
			return -1;
		}
	}
	if (is_dir(*sysinode, sdp->gfs1)) {
		struct block_count bc = {0};

		sysdir_fxns.private = &bc;
		if ((*sysinode)->i_flags & GFS2_DIF_EXHASH)
			pass1_check_metatree(*sysinode, &sysdir_fxns);
		else {
			err = check_linear_dir(*sysinode, (*sysinode)->i_bh,
					       &sysdir_fxns);
			/* If we encountered an error in our directory check
			   we should still call handle_ip, but return the
			   error later. */
			if (err)
				log_err(_("Error found in %s while checking "
					  "directory entries.\n"), filename);
		}
	}
	check_i_goal(sdp, *sysinode);
	error = handle_ip(sdp, *sysinode);
	return error ? error : err;
}

static int build_a_journal(struct gfs2_sbd *sdp)
{
	char name[256];
	int err = 0;

	/* First, try to delete the journal if it's in jindex */
	sprintf(name, "journal%u", sdp->md.journals);
	lgfs2_dirent_del(sdp->md.jiinode, name, strlen(name));
	/* Now rebuild it */
	err = lgfs2_build_journal(sdp, sdp->md.journals, sdp->md.jiinode);
	if (err) {
		log_crit(_("Error %d building journal\n"), err);
		exit(FSCK_ERROR);
	}
	return 0;
}

int build_per_node(struct gfs2_sbd *sdp)
{
	struct gfs2_inode *per_node;
	unsigned int j;

	per_node = lgfs2_createi(sdp->master_dir, "per_node", S_IFDIR | 0700,
			   GFS2_DIF_SYSTEM);
	if (per_node == NULL) {
		log_err(_("Error building '%s': %s\n"), "per_node", strerror(errno));
		return -1;
	}
	for (j = 0; j < sdp->md.journals; j++) {
		struct gfs2_inode *ip;

		ip = lgfs2_build_inum_range(per_node, j);
		if (ip == NULL) {
			log_err(_("Error building '%s': %s\n"), "inum_range",
			        strerror(errno));
			return 1;
		}
		lgfs2_inode_put(&ip);

		ip = lgfs2_build_statfs_change(per_node, j);
		if (ip == NULL) {
			log_err(_("Error building '%s': %s\n"), "statfs_change",
			        strerror(errno));
			return 1;
		}
		lgfs2_inode_put(&ip);

		ip = lgfs2_build_quota_change(per_node, j);
		if (ip == NULL) {
			log_err(_("Error building '%s': %s\n"), "quota_change",
			        strerror(errno));
			return 1;
		}
		lgfs2_inode_put(&ip);
	}
	lgfs2_inode_put(&per_node);
	return 0;
}

static int build_inum(struct gfs2_sbd *sdp)
{
	struct gfs2_inode *ip = lgfs2_build_inum(sdp);
	if (ip == NULL)
		return -1;
	lgfs2_inode_put(&ip);
	return 0;
}

static int build_statfs(struct gfs2_sbd *sdp)
{
	struct gfs2_inode *ip = lgfs2_build_statfs(sdp);
	if (ip == NULL)
		return -1;
	lgfs2_inode_put(&ip);
	return 0;
}

static int build_rindex(struct gfs2_sbd *sdp)
{
	struct gfs2_inode *ip = lgfs2_build_rindex(sdp);
	if (ip == NULL)
		return -1;
	lgfs2_inode_put(&ip);
	return 0;
}

static int build_quota(struct gfs2_sbd *sdp)
{
	struct gfs2_inode *ip = lgfs2_build_quota(sdp);
	if (ip == NULL)
		return -1;
	lgfs2_inode_put(&ip);
	return 0;
}

static int check_system_inodes(struct gfs2_sbd *sdp)
{
	int journal_count;

	/*******************************************************************
	 *******  Check the system inode integrity             *************
	 *******************************************************************/
	/* Mark the master system dinode as a "dinode" in the block map.
	   All other system dinodes in master will be taken care of by function
	   resuscitate_metalist.  But master won't since it has no parent.*/
	if (!sdp->gfs1) {
		fsck_blockmap_set(sdp->master_dir, sdp->master_dir->i_num.in_addr,
				  "master", GFS2_BLKST_DINODE);
		if (check_system_inode(sdp, &sdp->master_dir, "master",
				       lgfs2_build_master, 1, NULL, 1)) {
			stack;
			return -1;
		}
	}
	/* Mark the root dinode as a "dinode" in the block map as we did
	   for master, since it has no parent. */
	fsck_blockmap_set(sdp->md.rooti, sdp->md.rooti->i_num.in_addr,
			  "root", GFS2_BLKST_DINODE);
	if (check_system_inode(sdp, &sdp->md.rooti, "root", lgfs2_build_root, 1,
			       NULL, 0)) {
		stack;
		return -1;
	}
	if (!sdp->gfs1 &&
	    check_system_inode(sdp, &sdp->md.inum, "inum", build_inum, 0,
			       sdp->master_dir, 1)) {
		stack;
		return -1;
	}
	if (check_system_inode(sdp, &sdp->md.statfs, "statfs", build_statfs, 0,
			       sdp->master_dir, !sdp->gfs1)) {
		stack;
		return -1;
	}
	if (check_system_inode(sdp, &sdp->md.jiinode, "jindex", build_jindex,
			       (sdp->gfs1 ? 0 : 1), sdp->master_dir,
			       !sdp->gfs1)) {
		stack;
		return -1;
	}
	if (check_system_inode(sdp, &sdp->md.riinode, "rindex", build_rindex,
			       0, sdp->master_dir, !sdp->gfs1)) {
		stack;
		return -1;
	}
	if (check_system_inode(sdp, &sdp->md.qinode, "quota", build_quota,
			       0, sdp->master_dir, !sdp->gfs1)) {
		stack;
		return -1;
	}
	if (!sdp->gfs1 &&
	    check_system_inode(sdp, &sdp->md.pinode, "per_node",
			       build_per_node, 1, sdp->master_dir, 1)) {
		stack;
		return -1;
	}
	/* We have to play a trick on lgfs2_build_journal:  We swap md.journals
	   in order to keep a count of which journal we need to build. */
	journal_count = sdp->md.journals;
	/* gfs1's journals aren't dinode, they're just a bunch of blocks. */
	if (sdp->gfs1) {
		struct lgfs2_inum no;
		/* gfs1 has four dinodes that are set in the superblock and
		   therefore not linked to anything else. We need to adjust
		   the link counts so pass4 doesn't get confused. */
		no = sdp->md.statfs->i_num;
		incr_link_count(no, NULL, _("gfs1 statfs inode"));
		no = sdp->md.jiinode->i_num;
		incr_link_count(no, NULL, _("gfs1 jindex inode"));
		no = sdp->md.riinode->i_num;
		incr_link_count(no, NULL, _("gfs1 rindex inode"));
		no = sdp->md.qinode->i_num;
		incr_link_count(no, NULL, _("gfs1 quota inode"));
		return 0;
	}
	for (sdp->md.journals = 0; sdp->md.journals < journal_count;
	     sdp->md.journals++) {
		char jname[16];

		sprintf(jname, "journal%d", sdp->md.journals);
		if (check_system_inode(sdp, &sdp->md.journal[sdp->md.journals],
				       jname, build_a_journal, 0,
				       sdp->md.jiinode, 1)) {
			stack;
			return -1;
		}
	}

	return 0;
}

static int pass1_process_bitmap(struct gfs2_sbd *sdp, struct rgrp_tree *rgd, uint64_t *ibuf, unsigned n)
{
	struct gfs2_buffer_head *bh;
	unsigned i;
	uint64_t block;
	struct gfs2_inode *ip;
	int q;
	/* Readahead numbers arrived at by experiment */
	unsigned rawin = 50;
	unsigned ralen = 100 * sdp->sd_bsize;
	unsigned r = 0;

	for (i = 0; i < n; i++) {
		int is_inode;
		__be32 check_magic;

		block = ibuf[i];

		if (r++ == rawin) {
			posix_fadvise(sdp->device_fd, block * sdp->sd_bsize, ralen, POSIX_FADV_WILLNEED);
			r = 0;
		}

		/* skip gfs1 rindex indirect blocks */
		if (sdp->gfs1 && blockfind(&gfs1_rindex_blks, block)) {
			log_debug(_("Skipping rindex indir block %"PRIu64" (0x%"PRIx64")\n"),
			          block, block);
			continue;
		}
		display_progress(block);

		if (fsck_abort) { /* if asked to abort */
			gfs2_special_free(&gfs1_rindex_blks);
			return FSCK_OK;
		}
		if (skip_this_pass) {
			printf( _("Skipping pass 1 is not a good idea.\n"));
			skip_this_pass = 0;
			fflush(stdout);
		}
		if (fsck_system_inode(sdp, block)) {
			log_debug(_("Already processed system inode %"PRIu64" (0x%"PRIx64")\n"),
			          block, block);
			continue;
		}

		bh = lgfs2_bread(sdp, block);

		is_inode = 0;
		if (lgfs2_check_meta(bh->b_data, GFS2_METATYPE_DI) == 0)
			is_inode = 1;

		check_magic = ((struct gfs2_meta_header *)
			       (bh->b_data))->mh_magic;

		q = block_type(bl, block);
		if (q != GFS2_BLKST_FREE) {
			if (be32_to_cpu(check_magic) == GFS2_MAGIC &&
			    sdp->gfs1 && !is_inode) {
				log_debug(_("Block 0x%"PRIx64" assumed to be "
					    "previously processed GFS1 "
					    "non-dinode metadata.\n"),
				          block);
				lgfs2_brelse(bh);
				continue;
			}
			log_err(_("Found a duplicate inode block at %"PRIu64" (0x%"PRIx64") "
			          "previously marked as a %s\n"),
			         block, block, block_type_string(q));
			ip = fsck_inode_get(sdp, rgd, bh);
			if (is_inode && ip->i_num.in_addr == block)
				add_duplicate_ref(ip, block, REF_IS_INODE, 0,
						  INODE_VALID);
			else
				log_info(_("dinum.no_addr is wrong, so I "
					   "assume the bitmap is just "
					   "wrong.\n"));
			fsck_inode_put(&ip);
			lgfs2_brelse(bh);
			continue;
		}

		if (!is_inode) {
			if (be32_to_cpu(check_magic) == GFS2_MAGIC) {
			/* In gfs2, a bitmap mark of 2 means an inode,
			   but in gfs1 it means any metadata.  So if
			   this is gfs1 and not an inode, it may be
			   okay.  If it's non-dinode metadata, it will
			   be referenced by an inode, so we need to
			   skip it here and it will be sorted out
			   when the referencing inode is checked. */
				if (sdp->gfs1) {
					log_debug( _("Deferring GFS1 "
						     "metadata block #"
						     "%" PRIu64" (0x%"
						     PRIx64 ")\n"),
						   block, block);
					lgfs2_brelse(bh);
					continue;
				}
			}
			log_err(_("Found invalid inode at block %"PRIu64" (0x%"PRIx64")\n"),
			        block, block);
			check_n_fix_bitmap(sdp, rgd, block, 0, GFS2_BLKST_FREE);
		} else if (handle_di(sdp, rgd, bh) < 0) {
			stack;
			lgfs2_brelse(bh);
			gfs2_special_free(&gfs1_rindex_blks);
			return FSCK_ERROR;
		}
		/* Ignore everything else - they should be hit by the
		   handle_di step.  Don't check NONE either, because
		   check_meta passes everything if GFS2_METATYPE_NONE
		   is specified.  Hopefully, other metadata types such
		   as indirect blocks will be handled when the inode
		   itself is processed, and if it's not, it should be
		   caught in pass5. */
		lgfs2_brelse(bh);
	}

	return 0;
}

static int pass1_process_rgrp(struct gfs2_sbd *sdp, struct rgrp_tree *rgd)
{
	unsigned k, n, i;
	uint64_t *ibuf = malloc(sdp->sd_bsize * GFS2_NBBY * sizeof(uint64_t));
	int ret = 0;

	if (ibuf == NULL)
		return FSCK_ERROR;

	for (k = 0; k < rgd->rt_length; k++) {
		n = lgfs2_bm_scan(rgd, k, ibuf, GFS2_BLKST_DINODE);

		if (n) {
			ret = pass1_process_bitmap(sdp, rgd, ibuf, n);
			if (ret)
				goto out;
		}

		if (fsck_abort)
			goto out;
		/*
		  For GFS1, we have to count the "free meta" blocks in the
		  resource group and mark them specially so we can count them
		  properly in pass5.
		 */
		if (!sdp->gfs1)
			continue;

		n = lgfs2_bm_scan(rgd, k, ibuf, GFS2_BLKST_UNLINKED);
		for (i = 0; i < n; i++) {
			gfs2_blockmap_set(bl, ibuf[i], GFS2_BLKST_UNLINKED);
			if (fsck_abort)
				goto out;
		}
	}

out:
	free(ibuf);
	return ret;
}

static int gfs2_blockmap_create(struct gfs2_bmap *bmap, uint64_t size)
{
	bmap->size = size;

	/* Have to add 1 to BLOCKMAP_SIZE since it's 0-based and mallocs
	 * must be 1-based */
	bmap->mapsize = BLOCKMAP_SIZE2(size) + 1;

	if (!(bmap->map = calloc(bmap->mapsize, sizeof(char))))
		return -ENOMEM;
	return 0;
}


static int link1_create(struct gfs2_bmap *bmap, uint64_t size)
{
	bmap->size = size;

	/* Have to add 1 to BLOCKMAP_SIZE since it's 0-based and mallocs
	 * must be 1-based */
	bmap->mapsize = BLOCKMAP_SIZE1(size) + 1;

	if (!(bmap->map = calloc(bmap->mapsize, sizeof(char))))
		return -ENOMEM;
	return 0;
}

static struct gfs2_bmap *gfs2_bmap_create(struct gfs2_sbd *sdp, uint64_t size,
					  uint64_t *addl_mem_needed)
{
	struct gfs2_bmap *il;

	*addl_mem_needed = 0L;
	il = calloc(1, sizeof(*il));
	if (!il)
		return NULL;

	if (gfs2_blockmap_create(il, size)) {
		*addl_mem_needed = il->mapsize;
		free(il);
		il = NULL;
	}
	return il;
}

static void gfs2_blockmap_destroy(struct gfs2_bmap *bmap)
{
	if (bmap->map)
		free(bmap->map);
	bmap->size = 0;
	bmap->mapsize = 0;
}

static void *gfs2_bmap_destroy(struct gfs2_sbd *sdp, struct gfs2_bmap *il)
{
	if (il) {
		gfs2_blockmap_destroy(il);
		free(il);
		il = NULL;
	}
	return il;
}

static void enomem(uint64_t addl_mem_needed)
{
	log_crit( _("This system doesn't have enough memory and swap space to fsck this file system.\n"));
	log_crit( _("Additional memory needed is approximately: %"PRIu64"MB\n"),
	         (uint64_t)(addl_mem_needed / 1048576ULL));
	log_crit( _("Please increase your swap space by that amount and run fsck.gfs2 again.\n"));
}

/**
 * pass1 - walk through inodes and check inode state
 *
 * this walk can be done using root inode and depth first search,
 * watching for repeat inode numbers
 *
 * format & type
 * link count
 * duplicate blocks
 * bad blocks
 * inodes size
 * dir info
 */
int pass1(struct gfs2_sbd *sdp)
{
	struct osi_node *n, *next = NULL;
	struct rgrp_tree *rgd;
	uint64_t i;
	uint64_t rg_count = 0;
	struct timeval timer;
	int ret = FSCK_OK;
	uint64_t addl_mem_needed;

	bl = gfs2_bmap_create(sdp, last_fs_block+1, &addl_mem_needed);
	if (!bl) {
		enomem(addl_mem_needed);
		return FSCK_ERROR;
	}
	addl_mem_needed = link1_create(&nlink1map, last_fs_block+1);
	if (addl_mem_needed) {
		enomem(addl_mem_needed);
		gfs2_bmap_destroy(sdp, bl);
		return FSCK_ERROR;
	}
	addl_mem_needed = link1_create(&clink1map, last_fs_block+1);
	if (addl_mem_needed) {
		enomem(addl_mem_needed);
		link1_destroy(&nlink1map);
		gfs2_bmap_destroy(sdp, bl);
		return FSCK_ERROR;
	}
	osi_list_init(&gfs1_rindex_blks.list);

	/* FIXME: In the gfs fsck, we had to mark things like the
	 * journals and indices and such as 'other_meta' - in gfs2,
	 * the journals are files and are found in the normal file
	 * sweep - is there any metadata we need to mark here before
	 * the sweeps start that we won't find otherwise? */

	/* Make sure the system inodes are okay & represented in the bitmap. */
	check_system_inodes(sdp);

	/* So, do we do a depth first search starting at the root
	 * inode, or use the rg bitmaps, or just read every fs block
	 * to find the inodes?  If we use the depth first search, why
	 * have pass3 at all - if we use the rg bitmaps, pass5 is at
	 * least partially invalidated - if we read every fs block,
	 * things will probably be intolerably slow.  The current fsck
	 * uses the rg bitmaps, so maybe that's the best way to start
	 * things - we can change the method later if necessary.
	 */
	for (n = osi_first(&sdp->rgtree); n; n = next, rg_count++) {
		if (fsck_abort) {
			ret = FSCK_CANCELED;
			goto out;
		}
		next = osi_next(n);
		log_debug("Checking metadata in resource group #%"PRIu64"\n", rg_count);
		rgd = (struct rgrp_tree *)n;
		for (i = 0; i < rgd->rt_length; i++) {
			log_debug("rgrp block %"PRIu64" (0x%"PRIx64") is now marked as 'rgrp data'\n",
				   rgd->rt_addr + i, rgd->rt_addr + i);
			if (gfs2_blockmap_set(bl, rgd->rt_addr + i, GFS2_BLKST_USED)) {
				stack;
				gfs2_special_free(&gfs1_rindex_blks);
				ret = FSCK_ERROR;
				goto out;
			}
			/* rgrps and bitmaps don't have bits to represent
			   their blocks, so don't do this:
			check_n_fix_bitmap(sdp, rgd, rgd->ri.ri_num.in_addr + i, 0,
			gfs2_meta_rgrp);*/
		}

		ret = pass1_process_rgrp(sdp, rgd);
		if (ret)
			goto out;
	}
	log_notice(_("Reconciling bitmaps.\n"));
	gettimeofday(&timer, NULL);
	pass5(sdp, bl);
	print_pass_duration("reconcile_bitmaps", &timer);
out:
	gfs2_special_free(&gfs1_rindex_blks);
	if (bl)
		gfs2_bmap_destroy(sdp, bl);
	return ret;
}
