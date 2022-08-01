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

#include <logging.h>
#include "libgfs2.h"
#include "metawalk.h"
#include "util.h"

const char *reftypes[REF_TYPES + 1] = {"data", "metadata",
				       "an extended attribute", "an inode",
				       "unimportant"};

void big_file_comfort(struct fsck_cx *cx, struct lgfs2_inode *ip, uint64_t blks_checked)
{
	static struct timeval tv;
	static uint32_t seconds = 0;
	static uint64_t percent, fsize, chksize;
	uint64_t one_percent = 0;
	int i, cs;
	const char *human_abbrev = " KMGTPE";

	one_percent = ip->i_blocks / 100;
	if (blks_checked - last_reported_fblock < one_percent)
		return;

	last_reported_fblock = blks_checked;
	gettimeofday(&tv, NULL);
	if (!seconds)
		seconds = tv.tv_sec;
	if (tv.tv_sec == seconds)
		return;

	fsize = ip->i_size;
	for (i = 0; i < 6 && fsize > 1024; i++)
		fsize /= 1024;
	chksize = blks_checked * ip->i_sbd->sd_bsize;
	for (cs = 0; cs < 6 && chksize > 1024; cs++)
		chksize /= 1024;
	seconds = tv.tv_sec;
	percent = (blks_checked * 100) / ip->i_blocks;
	log_notice(_("\rChecking %"PRIu64"%c of %"PRIu64"%c of file at %"PRIu64" (0x%"PRIx64")"
		      "- %"PRIu64" percent complete.                   \r"),
	           chksize, human_abbrev[cs], fsize, human_abbrev[i], ip->i_num.in_addr,
	           ip->i_num.in_addr, percent);
	fflush(stdout);
}

void display_progress(uint64_t block)
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
				log_notice(_("\r%"PRIu64" percent complete.\r"), percent);
				fflush(stdout);
			}
		}
	}
}

char fsck_getch(void)
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
	while (1) {
		printf("\n%s interrupted during %s:  ", caller, where);
		if (progress)
			printf("%s.\n", progress);
		printf("%s", question);

		/* Make sure query is printed out */
		fflush(NULL);
		response = fsck_getch();
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
int fsck_query(struct fsck_cx *cx, const char *format, ...)
{
	va_list args;
	char response;
	int ret = 0;

	errors_found++;
	fsck_abort = 0;
	if (cx->opts->yes) {
		errors_corrected++;
		return 1;
	}
	if (cx->opts->no)
		return 0;

	while (1) {
		va_start(args, format);
		vprintf(format, args);
		va_end(args);

		/* Make sure query is printed out */
		fflush(NULL);
		response = fsck_getch();

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
	return ret;
}

/*
 * dup_set - Flag a block as a duplicate
 * We keep the references in a red/black tree.  We can't keep track of every
 * single inode in the file system, so the first time this function is called
 * will actually be for the second reference to the duplicated block.
 * This will return the number of references to the block.
 *
 * create - will be set if the call is supposed to create the reference. */
static struct duptree *dup_set(struct fsck_cx *cx, uint64_t dblock, int create)
{
	struct osi_node **newn = &cx->dup_blocks.osi_node, *parent = NULL;
	struct duptree *dt;

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
	dt = malloc(sizeof(struct duptree));
	if (dt == NULL) {
		log_crit( _("Unable to allocate duptree structure\n"));
		return NULL;
	}
	dups_found++;
	memset(dt, 0, sizeof(struct duptree));
	/* Add new node and rebalance tree. */
	dt->block = dblock;
	dt->refs = 1; /* reference 1 is actually the reference we need to
			 discover in pass1b. */
	osi_list_init(&dt->ref_inode_list);
	osi_list_init(&dt->ref_invinode_list);
	osi_link_node(&dt->node, parent, newn);
	osi_insert_color(&dt->node, &cx->dup_blocks);

	return dt;
}

/**
 * find_dup_ref_inode - find a duplicate reference inode entry for an inode
 */
struct inode_with_dups *find_dup_ref_inode(struct duptree *dt,
					   struct lgfs2_inode *ip)
{
	osi_list_t *ref;
	struct inode_with_dups *id;

	osi_list_foreach(ref, &dt->ref_invinode_list) {
		id = osi_list_entry(ref, struct inode_with_dups, list);

		if (id->block_no == ip->i_num.in_addr)
			return id;
	}
	osi_list_foreach(ref, &dt->ref_inode_list) {
		id = osi_list_entry(ref, struct inode_with_dups, list);

		if (id->block_no == ip->i_num.in_addr)
			return id;
	}
	return NULL;
}

/**
 * count_dup_meta_refs - count the number of remaining references as metadata
 */
int count_dup_meta_refs(struct duptree *dt)
{
	osi_list_t *ref;
	struct inode_with_dups *id;
	int metarefs = 0;

	osi_list_foreach(ref, &dt->ref_invinode_list) {
		id = osi_list_entry(ref, struct inode_with_dups, list);
		if (id->reftypecount[REF_AS_META])
			metarefs++;
	}
	osi_list_foreach(ref, &dt->ref_inode_list) {
		id = osi_list_entry(ref, struct inode_with_dups, list);
		if (id->reftypecount[REF_AS_META])
			metarefs++;
	}
	return metarefs;
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
int add_duplicate_ref(struct fsck_cx *cx, struct lgfs2_inode *ip, uint64_t block,
		      enum dup_ref_type reftype, int first, int inode_valid)
{
	struct inode_with_dups *id;
	struct duptree *dt;

	if (!valid_block_ip(ip, block))
		return META_IS_GOOD;
	/* If this is not the first reference (i.e. all calls from pass1) we
	   need to create the duplicate reference. If this is pass1b, we want
	   to ignore references that aren't found. */
	dt = dup_set(cx, block, !first);
	if (!dt)        /* If this isn't a duplicate */
		return META_IS_GOOD;

	/* If we found the duplicate reference but we've already discovered
	   the first reference (in pass1b) and the other references in pass1,
	   we don't need to count it, so just return. */
	if (dt->dup_flags & DUPFLAG_REF1_FOUND)
		return META_IS_GOOD;

	/* Check for a previous reference to this duplicate */
	id = find_dup_ref_inode(dt, ip);

	/* We have to be careful here. The original referencing dinode may have
	   deemed to be bad and deleted/freed in pass1. In that case, pass1b
	   wouldn't discover the correct [deleted] original reference. In
	   that case, we don't want to be confused and consider this second
	   reference the same as the first. If we do, we'll never be able to
	   resolve it. The first reference can't be the second reference. */
	if (id && first && !(dt->dup_flags & DUPFLAG_REF1_FOUND)) {
		log_info(_("Original reference to block %"PRIu64" (0x%"PRIx64") was "
			   "either found to be bad and deleted, or else "
			   "a duplicate within the same inode.\n"),
		         block, block);
		log_info(_("I'll consider the reference from inode %"PRIu64
			   " (0x%"PRIx64") the first reference.\n"),
		         ip->i_num.in_addr, ip->i_num.in_addr);
		dt->dup_flags |= DUPFLAG_REF1_IS_DUPL;
		dt->refs++;
	}

	/* The first time this is called from pass1 is actually the second
	   reference.  When we go back in pass1b looking for the original
	   reference, we don't want to increment the reference count because
	   it's already accounted for. */
	if (first) {
		dt->dup_flags |= DUPFLAG_REF1_FOUND;
		dups_found_first++; /* We found another first ref. */
	} else {
		dt->refs++;
	}

	if (id == NULL) {
		/* Check for the inode on the invalid inode reference list. */
		int q;

		id = calloc(1, sizeof(*id));
		if (!id) {
			log_crit( _("Unable to allocate inode_with_dups structure\n"));
			return META_ERROR;
		}
		id->block_no = ip->i_num.in_addr;
		q = bitmap_type(ip->i_sbd, ip->i_num.in_addr);
		/* If it's an invalid dinode, put it first on the invalid
		   inode reference list otherwise put it on the normal list. */
		if (!inode_valid || q == GFS2_BLKST_UNLINKED)
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
	log_info(_("Found %d reference(s) to block %"PRIu64" (0x%"PRIx64") "
	           "as %s in %s inode #%"PRIu64" (0x%"PRIx64")\n"),
		  id->dup_count, block, block, reftypes[reftype],
		  inode_valid ? _("valid") : _("invalid"),
		  ip->i_num.in_addr, ip->i_num.in_addr);
	if (first)
		log_info( _("This is the original reference.\n"));
	else {
		/* Check for duplicate refs to the same block in one inode. */
		if (id->dup_count > 1)
			dt->dup_flags |= DUPFLAG_REF1_FOUND;
		log_info( _("This brings the total to: %d inode references, "
			    "%d from this inode.\n"),
			  dt->refs, id->dup_count);
	}
	return META_IS_GOOD;
}

struct dir_info *dirtree_insert(struct fsck_cx *cx, struct lgfs2_inum inum)
{
	struct osi_node **newn = &cx->dirtree.osi_node, *parent = NULL;
	struct dir_info *data;

	/* Figure out where to put new node */
	while (*newn) {
		struct dir_info *cur = (struct dir_info *)*newn;

		parent = *newn;
		if (inum.in_addr < cur->dinode.in_addr)
			newn = &((*newn)->osi_left);
		else if (inum.in_addr > cur->dinode.in_addr)
			newn = &((*newn)->osi_right);
		else
			return cur;
	}

	data = calloc(1, sizeof(struct dir_info));
	if (!data) {
		log_crit( _("Unable to allocate dir_info structure\n"));
		return NULL;
	}
	/* Add new node and rebalance tree. */
	data->dinode.in_addr = inum.in_addr;
	data->dinode.in_formal_ino = inum.in_formal_ino;
	osi_link_node(&data->node, parent, newn);
	osi_insert_color(&data->node, &cx->dirtree);

	return data;
}

struct dir_info *dirtree_find(struct fsck_cx *cx, uint64_t block)
{
	struct osi_node *node = cx->dirtree.osi_node;

	while (node) {
		struct dir_info *data = (struct dir_info *)node;

		if (block < data->dinode.in_addr)
			node = node->osi_left;
		else if (block > data->dinode.in_addr)
			node = node->osi_right;
		else
			return data;
	}
	return NULL;
}

/* get_ref_type - figure out if all duplicate references from this inode
   are the same type, and if so, return the type. */
enum dup_ref_type get_ref_type(struct inode_with_dups *id)
{
	enum dup_ref_type t, i;
	int found_type_with_ref;
	int found_other_types;

	for (t = REF_AS_DATA; t < REF_TYPES; t++) {
		found_type_with_ref = 0;
		found_other_types = 0;
		for (i = REF_AS_DATA; i < REF_TYPES; i++) {
			if (id->reftypecount[i]) {
				if (t == i)
					found_type_with_ref = 1;
				else
					found_other_types = 1;
			}
		}
		if (found_type_with_ref)
			return found_other_types ? REF_TYPES : t;
	}
	return REF_TYPES;
}

void dup_listent_delete(struct duptree *dt, struct inode_with_dups *id)
{
	log_err(_("Removing duplicate reference to block %"PRIu64" (0x%"PRIx64") "
	          "referenced as %s by dinode %"PRIu64" (0x%"PRIx64")\n"),
	        dt->block, dt->block, reftypes[get_ref_type(id)], id->block_no, id->block_no);
	dt->refs--;
	if (id->name)
		free(id->name);
	osi_list_del(&id->list);
	free(id);
}

void dup_delete(struct fsck_cx *cx, struct duptree *dt)
{
	struct inode_with_dups *id;
	osi_list_t *tmp;

	while (!osi_list_empty(&dt->ref_invinode_list)) {
		tmp = (&dt->ref_invinode_list)->next;
		id = osi_list_entry(tmp, struct inode_with_dups, list);
		dup_listent_delete(dt, id);
	}
	while (!osi_list_empty(&dt->ref_inode_list)) {
		tmp = (&dt->ref_inode_list)->next;
		id = osi_list_entry(tmp, struct inode_with_dups, list);
		dup_listent_delete(dt, id);
	}
	osi_erase(&dt->node, &cx->dup_blocks);
	free(dt);
}

void dirtree_delete(struct fsck_cx *cx, struct dir_info *b)
{
	osi_erase(&b->node, &cx->dirtree);
	free(b);
}

uint64_t find_free_blk(struct lgfs2_sbd *sdp)
{
	struct osi_node *n, *next = NULL;
	struct lgfs2_rgrp_tree *rl = NULL;
	struct gfs2_rgrp *rg;
	unsigned int block, bn = 0, x = 0, y = 0;
	unsigned int state;

	memset(&rg, 0, sizeof(rg));
	for (n = osi_first(&sdp->rgtree); n; n = next) {
		next = osi_next(n);
		rl = (struct lgfs2_rgrp_tree *)n;
		if (rl->rt_free)
			break;
	}

	if (n == NULL)
		return 0;

	for (block = 0; block < rl->rt_length; block++) {
		char *buf = rl->bits[block].bi_data;
		x = (block) ? sizeof(struct gfs2_meta_header) : sizeof(struct gfs2_rgrp);

		for (; x < sdp->sd_bsize; x++)
			for (y = 0; y < GFS2_NBBY; y++) {
				state = (buf[x] >> (GFS2_BIT_SIZE * y)) & 0x03;
				if (state == GFS2_BLKST_FREE)
					return rl->rt_data0 + bn;
				bn++;
			}
	}
	return 0;
}

__be64 *get_dir_hash(struct lgfs2_inode *ip)
{
	unsigned hsize = (1 << ip->i_depth) * sizeof(uint64_t);
	int ret;
	__be64 *tbl = malloc(hsize);

	if (tbl == NULL)
		return NULL;

	ret = lgfs2_readi(ip, tbl, 0, hsize);
	if (ret != hsize) {
		free(tbl);
		return NULL;
	}

	return tbl;
}

void delete_all_dups(struct fsck_cx *cx, struct lgfs2_inode *ip)
{
	struct osi_node *n, *next;
	struct duptree *dt;
	osi_list_t *tmp, *x;
	struct inode_with_dups *id;
	int found;

	for (n = osi_first(&cx->dup_blocks); n; n = next) {
		next = osi_next(n);
		dt = (struct duptree *)n;

		found = 0;
		id = NULL;

		osi_list_foreach_safe(tmp, &dt->ref_invinode_list, x) {
			id = osi_list_entry(tmp, struct inode_with_dups, list);
			if (id->block_no == ip->i_num.in_addr) {
				dup_listent_delete(dt, id);
				found = 1;
			}
		}
		osi_list_foreach_safe(tmp, &dt->ref_inode_list, x) {
			id = osi_list_entry(tmp, struct inode_with_dups, list);
			if (id->block_no == ip->i_num.in_addr) {
				dup_listent_delete(dt, id);
				found = 1;
			}
		}
		if (!found)
			continue;

		if (dt->refs == 0) {
			log_debug(_("This was the last reference: 0x%"PRIx64" is "
				    "no longer a duplicate.\n"),
			          dt->block);
			dup_delete(cx, dt);
		} else {
			log_debug(_("%d references remain to 0x%"PRIx64"\n"),
				  dt->refs, dt->block);
			if (dt->refs > 1)
				continue;

			id = NULL;
			osi_list_foreach(tmp, &dt->ref_invinode_list)
				id = osi_list_entry(tmp,
						    struct inode_with_dups,
						    list);
			osi_list_foreach(tmp, &dt->ref_inode_list)
				id = osi_list_entry(tmp,
						    struct inode_with_dups,
						    list);
			if (id)
				log_debug("Last reference is from inode 0x%"PRIx64"\n",
				          id->block_no);
		}
	}
}

void print_pass_duration(const char *name, struct timeval *start)
{
	char duration[17] = ""; /* strlen("XXdXXhXXmXX.XXXs") + 1 */
	struct timeval end, diff;
	unsigned d, h, m, s;
	char *p = duration;

	gettimeofday(&end, NULL);
	timersub(&end, start, &diff);

	s = diff.tv_sec % 60;
	diff.tv_sec /= 60;
	m = diff.tv_sec % 60;
	diff.tv_sec /= 60;
	h = diff.tv_sec % 24;
	d = diff.tv_sec / 24;

	if (d)
		p += snprintf(p, 4, "%ud", d > 99 ? 99U : d);
	if (h)
		p += snprintf(p, 4, "%uh", h);
	if (m)
		p += snprintf(p, 4, "%um", m);

	snprintf(p, 8, "%u.%03lus", s, diff.tv_usec / 1000);
	log_notice(_("%s completed in %s\n"), name, duration);
}

