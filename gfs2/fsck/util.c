#include "clusterautoconfig.h"

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/time.h>
#include <stdio.h>
#include <stdarg.h>
#include <termios.h>
#include <libintl.h>
#include <ctype.h>
#define _(String) gettext(String)

#include "libgfs2.h"
#include "metawalk.h"
#include "util.h"

const char *reftypes[ref_types + 1] = {"data", "metadata",
				       "extended attribute", "itself",
				       "unimportant"};

void big_file_comfort(struct gfs2_inode *ip, uint64_t blks_checked)
{
	static struct timeval tv;
	static uint32_t seconds = 0;
	static uint64_t percent, fsize, chksize;
	uint64_t one_percent = 0;
	int i, cs;
	const char *human_abbrev = " KMGTPE";

	one_percent = ip->i_di.di_blocks / 100;
	if (blks_checked - last_reported_fblock < one_percent)
		return;

	last_reported_fblock = blks_checked;
	gettimeofday(&tv, NULL);
	if (!seconds)
		seconds = tv.tv_sec;
	if (tv.tv_sec == seconds)
		return;

	fsize = ip->i_di.di_size;
	for (i = 0; i < 6 && fsize > 1024; i++)
		fsize /= 1024;
	chksize = blks_checked * ip->i_sbd->bsize;
	for (cs = 0; cs < 6 && chksize > 1024; cs++)
		chksize /= 1024;
	seconds = tv.tv_sec;
	percent = (blks_checked * 100) / ip->i_di.di_blocks;
	log_notice( _("\rChecking %lld%c of %lld%c of file at %lld (0x%llx)"
		      "- %llu percent complete.                   \r"),
		    (long long)chksize, human_abbrev[cs],
		    (unsigned long long)fsize, human_abbrev[i],
		    (unsigned long long)ip->i_di.di_num.no_addr,
		    (unsigned long long)ip->i_di.di_num.no_addr,
		    (unsigned long long)percent);
	fflush(stdout);
}

/* Put out a warm, fuzzy message every second so the user     */
/* doesn't think we hung.  (This may take a long time).       */
void warm_fuzzy_stuff(uint64_t block)
{
	static uint64_t one_percent = 0;
	static struct timeval tv;
	static uint32_t seconds = 0;

	if (!one_percent)
		one_percent = last_fs_block / 100;
	if (!last_reported_block ||
	    block - last_reported_block >= one_percent) {
		last_reported_block = block;
		gettimeofday(&tv, NULL);
		if (!seconds)
			seconds = tv.tv_sec;
		if (tv.tv_sec - seconds) {
			static uint64_t percent;

			seconds = tv.tv_sec;
			if (last_fs_block) {
				percent = (block * 100) / last_fs_block;
				log_notice( _("\r%llu percent complete.\r"),
					   (unsigned long long)percent);
				fflush(stdout);
			}
		}
	}
}

char gfs2_getch(void)
{
	struct termios termattr, savetermattr;
	char ch;
	ssize_t size;

	tcgetattr (STDIN_FILENO, &termattr);
	savetermattr = termattr;
	termattr.c_lflag &= ~(ICANON | IEXTEN | ISIG);
	termattr.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
	termattr.c_cflag &= ~(CSIZE | PARENB);
	termattr.c_cflag |= CS8;
	termattr.c_oflag &= ~(OPOST);
   	termattr.c_cc[VMIN] = 0;
	termattr.c_cc[VTIME] = 0;

	tcsetattr (STDIN_FILENO, TCSANOW, &termattr);
	do {
		size = read(STDIN_FILENO, &ch, 1);
		if (size)
			break;
		usleep(50000);
	} while (!size);

	tcsetattr (STDIN_FILENO, TCSANOW, &savetermattr);
	return ch;
}

char generic_interrupt(const char *caller, const char *where,
		       const char *progress, const char *question,
		       const char *answers)
{
	fd_set rfds;
	struct timeval tv;
	char response;
	int err, i;

	FD_ZERO(&rfds);
	FD_SET(STDIN_FILENO, &rfds);

	tv.tv_sec = 0;
	tv.tv_usec = 0;
	/* Make sure there isn't extraneous input before asking the
	 * user the question */
	while((err = select(STDIN_FILENO + 1, &rfds, NULL, NULL, &tv))) {
		if(err < 0) {
			log_debug("Error in select() on stdin\n");
			break;
		}
		if(read(STDIN_FILENO, &response, sizeof(char)) < 0) {
			log_debug("Error in read() on stdin\n");
			break;
		}
	}
	while (TRUE) {
		printf("\n%s interrupted during %s:  ", caller, where);
		if (progress)
			printf("%s.\n", progress);
		printf("%s", question);

		/* Make sure query is printed out */
		fflush(NULL);
		response = gfs2_getch();
		printf("\n");
		fflush(NULL);
		if (strchr(answers, response))
			break;
		printf("Bad response, please type ");
		for (i = 0; i < strlen(answers) - 1; i++)
			printf("'%c', ", answers[i]);
		printf(" or '%c'.\n", answers[i]);
	}
	return response;
}

/* fsck_query: Same as gfs2_query except it adjusts errors_found and
   errors_corrected. */
int fsck_query(const char *format, ...)
{
	va_list args;
	const char *transform;
	char response;
	int ret = 0;

	errors_found++;
	fsck_abort = 0;
	if (opts.yes) {
		errors_corrected++;
		return 1;
	}
	if (opts.no)
		return 0;

	opts.query = TRUE;
	while (1) {
		va_start(args, format);
		transform = _(format);
		vprintf(transform, args);
		va_end(args);

		/* Make sure query is printed out */
		fflush(NULL);
		response = gfs2_getch();

		printf("\n");
		fflush(NULL);
		if (response == 0x3) { /* if interrupted, by ctrl-c */
			response = generic_interrupt("Question", "response",
						     NULL,
						     "Do you want to abort " \
						     "or continue (a/c)?",
						     "ac");
			if (response == 'a') {
				ret = 0;
				fsck_abort = 1;
				break;
			}
			printf("Continuing.\n");
		} else if (tolower(response) == 'y') {
			errors_corrected++;
                        ret = 1;
                        break;
		} else if (tolower(response) == 'n') {
			ret = 0;
			break;
		} else {
			printf("Bad response %d, please type 'y' or 'n'.\n",
			       response);
		}
	}

	opts.query = FALSE;
	return ret;
}

/*
 * gfs2_dup_set - Flag a block as a duplicate
 * We keep the references in a red/black tree.  We can't keep track of every
 * single inode in the file system, so the first time this function is called
 * will actually be for the second reference to the duplicated block.
 * This will return the number of references to the block.
 *
 * create - will be set if the call is supposed to create the reference. */
static struct duptree *gfs2_dup_set(uint64_t dblock, int create)
{
	struct osi_node **newn = &dup_blocks.osi_node, *parent = NULL;
	struct duptree *data;

	/* Figure out where to put new node */
	while (*newn) {
		struct duptree *cur = (struct duptree *)*newn;

		parent = *newn;
		if (dblock < cur->block)
			newn = &((*newn)->osi_left);
		else if (dblock > cur->block)
			newn = &((*newn)->osi_right);
		else
			return cur;
	}

	if (!create)
		return NULL;
	data = malloc(sizeof(struct duptree));
	if (data == NULL) {
		log_crit( _("Unable to allocate duptree structure\n"));
		return NULL;
	}
	dups_found++;
	memset(data, 0, sizeof(struct duptree));
	/* Add new node and rebalance tree. */
	data->block = dblock;
	data->refs = 1; /* reference 1 is actually the reference we need to
			   discover in pass1b. */
	data->first_ref_found = 0;
	osi_list_init(&data->ref_inode_list);
	osi_list_init(&data->ref_invinode_list);
	osi_link_node(&data->node, parent, newn);
	osi_insert_color(&data->node, &dup_blocks);

	return data;
}

/**
 * find_dup_ref_inode - find a duplicate reference inode entry for an inode
 */
struct inode_with_dups *find_dup_ref_inode(struct duptree *dt,
					   struct gfs2_inode *ip)
{
	osi_list_t *ref;
	struct inode_with_dups *id;

	osi_list_foreach(ref, &dt->ref_invinode_list) {
		id = osi_list_entry(ref, struct inode_with_dups, list);

		if (id->block_no == ip->i_di.di_num.no_addr)
			return id;
	}
	osi_list_foreach(ref, &dt->ref_inode_list) {
		id = osi_list_entry(ref, struct inode_with_dups, list);

		if (id->block_no == ip->i_di.di_num.no_addr)
			return id;
	}
	return NULL;
}

/*
 * add_duplicate_ref - Add a duplicate reference to the duplicates tree list
 * A new element of the tree will be created as needed
 * When the first reference is discovered in pass1, it realizes it's a
 * duplicate but it has already forgotten where the first reference was.
 * So we need to recreate the duplicate reference structure if it's not there.
 * Later, in pass1b, it has to go back through the file system
 * and figure out those original references in order to resolve them.
 *
 * first - if 1, we're being called from pass1b, in which case we're trying
 *         to find the first reference to this block.  If 0, we're being
 *         called from pass1, which is the second reference, which determined
 *         it was a duplicate..
 */
int add_duplicate_ref(struct gfs2_inode *ip, uint64_t block,
		      enum dup_ref_type reftype, int first, int inode_valid)
{
	struct inode_with_dups *id;
	struct duptree *dt;

	if (!valid_block(ip->i_sbd, block))
		return 0;
	/* If this is not the first reference (i.e. all calls from pass1) we
	   need to create the duplicate reference. If this is pass1b, we want
	   to ignore references that aren't found. */
	dt = gfs2_dup_set(block, !first);
	if (!dt)        /* If this isn't a duplicate */
		return 0;

	/* If we found the duplicate reference but we've already discovered
	   the first reference (in pass1b) and the other references in pass1,
	   we don't need to count it, so just return. */
	if (dt->first_ref_found)
		return 0;

	/* The first time this is called from pass1 is actually the second
	   reference.  When we go back in pass1b looking for the original
	   reference, we don't want to increment the reference count because
	   it's already accounted for. */
	if (first) {
		dt->first_ref_found = 1;
		dups_found_first++; /* We found another first ref. */
	} else {
		dt->refs++;
	}

	/* Check for a previous reference to this duplicate */
	id = find_dup_ref_inode(dt, ip);
	if (id == NULL) {
		/* Check for the inode on the invalid inode reference list. */
		uint8_t q;

		if (!(id = malloc(sizeof(*id)))) {
			log_crit( _("Unable to allocate "
				    "inode_with_dups structure\n"));
			return -1;
		}
		if (!(memset(id, 0, sizeof(*id)))) {
			log_crit( _("Unable to zero inode_with_dups "
				    "structure\n"));
			return -1;
		}
		id->block_no = ip->i_di.di_num.no_addr;
		q = block_type(ip->i_di.di_num.no_addr);
		/* If it's an invalid dinode, put it first on the invalid
		   inode reference list otherwise put it on the normal list. */
		if (!inode_valid || q == gfs2_inode_invalid)
			osi_list_add_prev(&id->list, &dt->ref_invinode_list);
		else {
			/* If this is a system dinode, we want the duplicate
			   processing to find it first. That way references
			   from inside journals, et al, will take priority.
			   We don't want to delete journals in favor of dinodes
			   that reference a block inside a journal. */
			if (fsck_system_inode(ip->i_sbd, id->block_no))
				osi_list_add(&id->list, &dt->ref_inode_list);
			else
				osi_list_add_prev(&id->list,
						  &dt->ref_inode_list);
		}
	}
	id->reftypecount[reftype]++;
	id->dup_count++;
	log_info( _("Found %d reference(s) to block %llu"
		    " (0x%llx) as %s in inode #%llu (0x%llx)\n"),
		  id->dup_count, (unsigned long long)block,
		  (unsigned long long)block, reftypes[reftype],
		  (unsigned long long)ip->i_di.di_num.no_addr,
		  (unsigned long long)ip->i_di.di_num.no_addr);
	if (first)
		log_info( _("This is the original reference.\n"));
	else
		log_info( _("This brings the total to: %d duplicate "
			    "references\n"), dt->refs);
	return 0;
}

struct dir_info *dirtree_insert(struct gfs2_inum inum)
{
	struct osi_node **newn = &dirtree.osi_node, *parent = NULL;
	struct dir_info *data;

	/* Figure out where to put new node */
	while (*newn) {
		struct dir_info *cur = (struct dir_info *)*newn;

		parent = *newn;
		if (inum.no_addr < cur->dinode.no_addr)
			newn = &((*newn)->osi_left);
		else if (inum.no_addr > cur->dinode.no_addr)
			newn = &((*newn)->osi_right);
		else
			return cur;
	}

	data = malloc(sizeof(struct dir_info));
	if (!data) {
		log_crit( _("Unable to allocate dir_info structure\n"));
		return NULL;
	}
	if (!memset(data, 0, sizeof(struct dir_info))) {
		log_crit( _("Error while zeroing dir_info structure\n"));
		return NULL;
	}
	/* Add new node and rebalance tree. */
	data->dinode.no_addr = inum.no_addr;
	data->dinode.no_formal_ino = inum.no_formal_ino;
	osi_link_node(&data->node, parent, newn);
	osi_insert_color(&data->node, &dirtree);

	return data;
}

struct dir_info *dirtree_find(uint64_t block)
{
	struct osi_node *node = dirtree.osi_node;

	while (node) {
		struct dir_info *data = (struct dir_info *)node;

		if (block < data->dinode.no_addr)
			node = node->osi_left;
		else if (block > data->dinode.no_addr)
			node = node->osi_right;
		else
			return data;
	}
	return NULL;
}

void dup_listent_delete(struct inode_with_dups *id)
{
	if (id->name)
		free(id->name);
	osi_list_del(&id->list);
	free(id);
}

void dup_delete(struct duptree *b)
{
	struct inode_with_dups *id;
	osi_list_t *tmp;

	while (!osi_list_empty(&b->ref_invinode_list)) {
		tmp = (&b->ref_invinode_list)->next;
		id = osi_list_entry(tmp, struct inode_with_dups, list);
		dup_listent_delete(id);
	}
	while (!osi_list_empty(&b->ref_inode_list)) {
		tmp = (&b->ref_inode_list)->next;
		id = osi_list_entry(tmp, struct inode_with_dups, list);
		dup_listent_delete(id);
	}
	osi_erase(&b->node, &dup_blocks);
	free(b);
}

void dirtree_delete(struct dir_info *b)
{
	osi_erase(&b->node, &dirtree);
	free(b);
}

static int gfs2_blockmap_create(struct gfs2_bmap *bmap, uint64_t size)
{
	bmap->size = size;

	/* Have to add 1 to BLOCKMAP_SIZE since it's 0-based and mallocs
	 * must be 1-based */
	bmap->mapsize = BLOCKMAP_SIZE4(size);

	if (!(bmap->map = malloc(sizeof(char) * bmap->mapsize)))
		return -ENOMEM;
	if (!memset(bmap->map, 0, sizeof(char) * bmap->mapsize)) {
		free(bmap->map);
		bmap->map = NULL;
		return -ENOMEM;
	}
	return 0;
}

static void gfs2_blockmap_destroy(struct gfs2_bmap *bmap)
{
	if (bmap->map)
		free(bmap->map);
	bmap->size = 0;
	bmap->mapsize = 0;
}

struct gfs2_bmap *gfs2_bmap_create(struct gfs2_sbd *sdp, uint64_t size,
				   uint64_t *addl_mem_needed)
{
	struct gfs2_bmap *il;

	*addl_mem_needed = 0L;
	il = malloc(sizeof(*il));
	if (!il || !memset(il, 0, sizeof(*il)))
		return NULL;

	if (gfs2_blockmap_create(il, size)) {
		*addl_mem_needed = il->mapsize;
		free(il);
		il = NULL;
	}
	osi_list_init(&sdp->eattr_blocks.list);
	return il;
}

int gfs2_blockmap_set(struct gfs2_bmap *bmap, uint64_t bblock,
		      enum gfs2_mark_block mark)
{
	static unsigned char *byte;
	static uint64_t b;

	if (bblock > bmap->size)
		return -1;

	byte = bmap->map + BLOCKMAP_SIZE4(bblock);
	b = BLOCKMAP_BYTE_OFFSET4(bblock);
	*byte &= ~(BLOCKMAP_MASK4 << b);
	*byte |= (mark & BLOCKMAP_MASK4) << b;
	return 0;
}

void *gfs2_bmap_destroy(struct gfs2_sbd *sdp, struct gfs2_bmap *il)
{
	if (il) {
		gfs2_blockmap_destroy(il);
		free(il);
		il = NULL;
	}
	gfs2_special_free(&sdp->eattr_blocks);
	return il;
}

/* set_ip_blockmap - set the blockmap for a dinode
 *
 * instree: Set to 1 if directories should be inserted into the directory tree
 *          otherwise 0.
 * returns: 0 if no error, -EINVAL if dinode has a bad mode, -EPERM on error
 */
int set_ip_blockmap(struct gfs2_inode *ip, int instree)
{
	uint64_t block = ip->i_bh->b_blocknr;
	struct gfs2_sbd *sdp = ip->i_sbd;
	uint32_t mode;

	if (sdp->gfs1)
		mode = gfs_to_gfs2_mode(ip);
	else
		mode = ip->i_di.di_mode & S_IFMT;

	switch (mode) {
	case S_IFDIR:
		if (fsck_blockmap_set(ip, block, _("directory"),
				      gfs2_inode_dir))
			goto bad_dinode;
		if (instree && !dirtree_insert(ip->i_di.di_num))
			goto bad_dinode;
		break;
	case S_IFREG:
		if (fsck_blockmap_set(ip, block, _("file"), gfs2_inode_file))
			goto bad_dinode;
		break;
	case S_IFLNK:
		if (fsck_blockmap_set(ip, block, _("symlink"),
				      gfs2_inode_lnk))
			goto bad_dinode;
		break;
	case S_IFBLK:
		if (fsck_blockmap_set(ip, block, _("block device"),
				      gfs2_inode_device))
			goto bad_dinode;
		break;
	case S_IFCHR:
		if (fsck_blockmap_set(ip, block, _("character device"),
				      gfs2_inode_device))
			goto bad_dinode;
		break;
	case S_IFIFO:
		if (fsck_blockmap_set(ip, block, _("fifo"),
				      gfs2_inode_fifo))
			goto bad_dinode;
		break;
	case S_IFSOCK:
		if (fsck_blockmap_set(ip, block, _("socket"),
				      gfs2_inode_sock))
			goto bad_dinode;
		break;
	default:
		fsck_blockmap_set(ip, block, _("invalid mode"),
				  gfs2_inode_invalid);
		return -EINVAL;
	}
	return 0;

bad_dinode:
	stack;
	return -EPERM;
}

uint64_t find_free_blk(struct gfs2_sbd *sdp)
{
	struct osi_node *n, *next = NULL;
	struct rgrp_tree *rl = NULL;
	struct gfs2_rindex *ri;
	struct gfs2_rgrp *rg;
	unsigned int block, bn = 0, x = 0, y = 0;
	unsigned int state;
	struct gfs2_buffer_head *bh;

	memset(&rg, 0, sizeof(rg));
	for (n = osi_first(&sdp->rgtree); n; n = next) {
		next = osi_next(n);
		rl = (struct rgrp_tree *)n;
		if (rl->rg.rg_free)
			break;
	}

	if (n == NULL)
		return 0;

	ri = &rl->ri;
	rg = &rl->rg;

	for (block = 0; block < ri->ri_length; block++) {
		bh = rl->bh[block];
		x = (block) ? sizeof(struct gfs2_meta_header) : sizeof(struct gfs2_rgrp);

		for (; x < sdp->bsize; x++)
			for (y = 0; y < GFS2_NBBY; y++) {
				state = (bh->b_data[x] >> (GFS2_BIT_SIZE * y)) & 0x03;
				if (state == GFS2_BLKST_FREE)
					return ri->ri_data0 + bn;
				bn++;
			}
	}
	return 0;
}

uint64_t *get_dir_hash(struct gfs2_inode *ip)
{
	unsigned hsize = (1 << ip->i_di.di_depth) * sizeof(uint64_t);
	int ret;
	uint64_t *tbl = malloc(hsize);

	if (tbl == NULL)
		return NULL;

	ret = gfs2_readi(ip, tbl, 0, hsize);
	if (ret != hsize) {
		free(tbl);
		return NULL;
	}

	return tbl;
}

