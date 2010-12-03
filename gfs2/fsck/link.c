#include "clusterautoconfig.h"

#include <inttypes.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <libintl.h>
#define _(String) gettext(String)

#include "libgfs2.h"
#include "fsck.h"
#include "inode_hash.h"
#include "link.h"

int set_link_count(uint64_t inode_no, uint32_t count)
{
	struct inode_info *ii;
	/* If the list has entries, look for one that matches inode_no */
	ii = inodetree_find(inode_no);
	if (!ii)
		ii = inodetree_insert(inode_no);
	if (ii)
		ii->link_count = count;
	else
		return -1;
	return 0;
}

int increment_link(uint64_t inode_no, uint64_t referenced_from,
		   const char *why)
{
	struct inode_info *ii = NULL;

	ii = inodetree_find(inode_no);
	/* If the list has entries, look for one that matches
	 * inode_no */
	if (ii) {
		ii->counted_links++;
		log_debug( _("Directory %lld (0x%llx) incremented counted "
			     "links to %u for %llu (0x%llx) via %s\n"),
			   (unsigned long long)referenced_from,
			   (unsigned long long)referenced_from,
			   ii->counted_links, (unsigned long long)inode_no,
			   (unsigned long long)inode_no, why);
		return 0;
	}
	log_debug( _("Ref: %llu (0x%llx) No match found when incrementing "
		     "link for %llu (0x%llx)!\n"),
		   (unsigned long long)referenced_from,
		   (unsigned long long)referenced_from,
		   (unsigned long long)inode_no,
		   (unsigned long long)inode_no);
	/* If no match was found, add a new entry and set its
	 * counted links to 1 */
	ii = inodetree_insert(inode_no);
	if (ii)
		ii->counted_links = 1;
	else
		return -1;
	return 0;
}

int decrement_link(uint64_t inode_no, uint64_t referenced_from,
		   const char *why)
{
	struct inode_info *ii = NULL;

	ii = inodetree_find(inode_no);
	/* If the list has entries, look for one that matches
	 * inode_no */
	if(ii) {
		if (!ii->counted_links) {
			log_debug( _("Directory %llu (0x%llx)'s link to "
			     " %llu (0x%llx) via %s is zero!\n"),
			   (unsigned long long)referenced_from,
			   (unsigned long long)referenced_from,
			   (unsigned long long)inode_no,
			   (unsigned long long)inode_no, why);
			return 0;
		}
		ii->counted_links--;
		log_debug( _("Directory %llu (0x%llx) decremented counted "
			     "links to %u for %llu (0x%llx) via %s\n"),
			   (unsigned long long)referenced_from,
			   (unsigned long long)referenced_from,
			   ii->counted_links, (unsigned long long)inode_no,
			   (unsigned long long)inode_no, why);
		return 0;
	}
	log_debug( _("No match found when decrementing link for %llu"
		     " (0x%llx)!\n"), (unsigned long long)inode_no,
		  (unsigned long long)inode_no);
	return -1;

}


