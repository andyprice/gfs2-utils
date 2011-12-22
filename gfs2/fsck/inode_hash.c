#include "clusterautoconfig.h"

#include <stdint.h>
#include <unistd.h>
#include <libintl.h>
#include <string.h>

#include "libgfs2.h"
#include "osi_list.h"
#include "hash.h"
#include "inode_hash.h"
#include "fsck.h"
#define _(String) gettext(String)

struct inode_info *inodetree_find(uint64_t block)
{
	struct osi_node *node = inodetree.osi_node;

	while (node) {
		struct inode_info *data = (struct inode_info *)node;

		if (block < data->inode)
			node = node->osi_left;
		else if (block > data->inode)
			node = node->osi_right;
		else
			return data;
	}
	return NULL;
}

struct inode_info *inodetree_insert(uint64_t dblock)
{
	struct osi_node **newn = &inodetree.osi_node, *parent = NULL;
	struct inode_info *data;

	/* Figure out where to put new node */
	while (*newn) {
		struct inode_info *cur = (struct inode_info *)*newn;

		parent = *newn;
		if (dblock < cur->inode)
			newn = &((*newn)->osi_left);
		else if (dblock > cur->inode)
			newn = &((*newn)->osi_right);
		else
			return cur;
	}

	data = malloc(sizeof(struct inode_info));
	if (!data) {
		log_crit( _("Unable to allocate inode_info structure\n"));
		return NULL;
	}
	if (!memset(data, 0, sizeof(struct inode_info))) {
		log_crit( _("Error while zeroing inode_info structure\n"));
		return NULL;
	}
	/* Add new node and rebalance tree. */
	data->inode = dblock;
	osi_link_node(&data->node, parent, newn);
	osi_insert_color(&data->node, &inodetree);

	return data;
}

void inodetree_delete(struct inode_info *b)
{
	osi_erase(&b->node, &inodetree);
	free(b);
}
