#include "clusterautoconfig.h"

#include <inttypes.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <libintl.h>
#define _(String) gettext(String)

#include <logging.h>
#include "libgfs2.h"
#include "fsck.h"
#include "inode_hash.h"
#include "link.h"

int set_di_nlink(struct gfs2_inode *ip)
{
	struct inode_info *ii;

	/*log_debug( _("Setting link count to %u for %" PRIu64
	  " (0x%" PRIx64 ")\n"), count, inode_no, inode_no);*/
	/* If the list has entries, look for one that matches inode_no */
	ii = inodetree_find(ip->i_di.di_num.no_addr);
	if (!ii)
		ii = inodetree_insert(ip->i_di.di_num);
	if (ii)
		ii->di_nlink = ip->i_di.di_nlink;
	else
		return -1;
	return 0;
}

int incr_link_count(struct gfs2_inum no, struct gfs2_inode *ip,
		    const char *why)
{
	struct inode_info *ii = NULL;
	uint64_t referenced_from = ip ? ip->i_di.di_num.no_addr : 0;

	ii = inodetree_find(no.no_addr);
	/* If the list has entries, look for one that matches inode_no */
	if (ii) {
		if (ii->di_num.no_formal_ino != no.no_formal_ino)
			return 1;

		ii->counted_links++;
		log_debug( _("Dir (0x%llx) incremented counted "
			     "links to %u for (0x%llx) via %s\n"),
			   (unsigned long long)referenced_from,
			   ii->counted_links, (unsigned long long)no.no_addr,
			   why);
		return 0;
	}
	log_debug( _("Ref: (0x%llx) No match found when incrementing "
		     "link for (0x%llx)!\n"),
		   (unsigned long long)referenced_from,
		   (unsigned long long)no.no_addr);
	/* If no match was found, add a new entry and set its
	 * counted links to 1 */
	ii = inodetree_insert(no);
	if (ii)
		ii->counted_links = 1;
	else
		return -1;
	return 0;
}

int decr_link_count(uint64_t inode_no, uint64_t referenced_from,
		    const char *why)
{
	struct inode_info *ii = NULL;

	ii = inodetree_find(inode_no);
	/* If the list has entries, look for one that matches
	 * inode_no */
	if (ii) {
		if (!ii->counted_links) {
			log_debug( _("Dir (0x%llx)'s link to "
			     "(0x%llx) via %s is zero!\n"),
			   (unsigned long long)referenced_from,
			   (unsigned long long)inode_no, why);
			return 0;
		}
		ii->counted_links--;
		log_debug( _("Dir (0x%llx) decremented counted "
			     "links to %u for (0x%llx) via %s\n"),
			   (unsigned long long)referenced_from,
			   ii->counted_links, (unsigned long long)inode_no,
			   why);
		return 0;
	}
	log_debug( _("No match found when decrementing link for (0x%llx)!\n"),
		   (unsigned long long)inode_no);
	return -1;

}


