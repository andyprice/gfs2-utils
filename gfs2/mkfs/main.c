#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <libgen.h>
#include <libintl.h>
#include <locale.h>
#define _(String) gettext(String)

#include <linux/types.h>
#include "libgfs2.h"
#include "gfs2_mkfs.h"

/**
 * main - do everything
 * @argc:
 * @argv:
 *
 * Returns: 0 on success, non-0 on failure
 */

int
main(int argc, char *argv[])
{
	char *p, *whoami;

	setlocale(LC_ALL, "");
	textdomain("gfs2-utils");

	srandom(time(NULL) ^ getpid());

	p = strdup(argv[0]);
	whoami = basename(p);
	
	if (!strcmp(whoami, "gfs2_jadd"))
		main_jadd(argc, argv);
	else if (!strcmp(whoami, "gfs2_mkfs") || !strcmp(whoami, "mkfs.gfs2"))
		main_mkfs(argc, argv);
	else if (!strcmp(whoami, "gfs2_grow"))
		main_grow(argc, argv);
	else
		die( _("I don't know who I am!\n"));

	free(p);

	return 0;
}
