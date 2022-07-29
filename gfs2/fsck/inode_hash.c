#include "clusterautoconfig.h"

#include <stdint.h>
#include <unistd.h>
#include <libintl.h>
#include <string.h>

#include <logging.h>
#include "libgfs2.h"
#include "osi_list.h"
#include "inode_hash.h"
#include "fsck.h"
#define _(String) gettext(String)

struct inode_info *inodetree_find(struct fsck_cx *cx, uint64_t block)
{
	struct osi_node *node = cx->inodetree.osi_node;

	while (node) {
		struct inode_info *data = (struct inode_info *)node;

		if (block < data->num.in_addr)
			node = node->osi_left;
		else if (block > data->num.in_addr)
			node = node->osi_right;
		else
			return data;
	}
	return NULL;
}

struct inode_info *inodetree_insert(struct fsck_cx *cx, struct lgfs2_inum no)
{
	struct osi_node **newn = &cx->inodetree.osi_node, *parent = NULL;
	struct inode_info *data;

	/* Figure out where to put new node */
	while (*newn) {
		struct inode_info *cur = (struct inode_info *)*newn;

		parent = *newn;
		if (no.in_addr < cur->num.in_addr)
			newn = &((*newn)->osi_left);
		else if (no.in_addr > cur->num.in_addr)
			newn = &((*newn)->osi_right);
		else
			return cur;
	}

	data = calloc(1, sizeof(struct inode_info));
	if (!data) {
		log_crit( _("Unable to allocate inode_info structure\n"));
		return NULL;
	}
	/* Add new node and rebalance tree. */
	data->num = no;
	osi_link_node(&data->node, parent, newn);
	osi_insert_color(&data->node, &cx->inodetree);

	return data;
}

void inodetree_delete(struct fsck_cx *cx, struct inode_info *b)
{
	osi_erase(&b->node, &cx->inodetree);
	free(b);
}
