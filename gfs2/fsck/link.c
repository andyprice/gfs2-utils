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
	/*log_debug( _("Setting link count to %u for %" PRIu64
	  " (0x%" PRIx64 ")\n"), count, inode_no, inode_no);*/
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
			     "links to %u for %"PRIu64" (0x%" PRIx64 ") "
			     "via %s\n"),
			   (unsigned long long)referenced_from,
			   (unsigned long long)referenced_from,
			   ii->counted_links, inode_no, inode_no, why);
		return 0;
	}
	log_debug( _("Ref: %lld (0x%llx) No match found when incrementing "
		     "link for %" PRIu64 " (0x%" PRIx64 ")!\n"),
		   (unsigned long long)referenced_from,
		   (unsigned long long)referenced_from, inode_no, inode_no);
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
		ii->counted_links--;
		log_debug( _("Directory %lld (0x%llx) decremented counted "
			     "links to %u for %"PRIu64" (0x%" PRIx64 ") "
			     "via %s\n"),
			   (unsigned long long)referenced_from,
			   (unsigned long long)referenced_from,
			   ii->counted_links, inode_no, inode_no, why);
		return 0;
	}
	log_debug( _("No match found when decrementing link for %" PRIu64
			  " (0x%" PRIx64 ")!\n"), inode_no, inode_no);
	return -1;

}


