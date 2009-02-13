#include <sys/types.h>
#include <asm/types.h>
#include <sys/uio.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <stddef.h>
#include <stdint.h>
#include <fcntl.h>
#include <netdb.h>
#include <limits.h>
#include <unistd.h>
#include <dirent.h>

#include "gfs_daemon.h"
#include "config.h"
#include "ccs.h"

int ccs_handle;

/* was a config value set on command line?, 0 or 1. */

int optd_debug_logfile;
int optd_enable_withdraw;

/* actual config value from command line, cluster.conf, or default. */

int cfgd_debug_logfile		= DEFAULT_DEBUG_LOGFILE;
int cfgd_enable_withdraw	= DEFAULT_ENABLE_WITHDRAW;

void read_ccs_name(char *path, char *name)
{
	char *str;
	int error;

	error = ccs_get(ccs_handle, path, &str);
	if (error || !str)
		return;

	strcpy(name, str);

	free(str);
}

void read_ccs_yesno(char *path, int *yes, int *no)
{
	char *str;
	int error;

	*yes = 0;
	*no = 0;

	error = ccs_get(ccs_handle, path, &str);
	if (error || !str)
		return;

	if (!strcmp(str, "yes"))
		*yes = 1;

	else if (!strcmp(str, "no"))
		*no = 1;

	free(str);
}

int read_ccs_int(char *path, int *config_val)
{
	char *str;
	int val;
	int error;

	error = ccs_get(ccs_handle, path, &str);
	if (error || !str)
		return -1;

	val = atoi(str);

	if (val < 0) {
		log_error("ignore invalid value %d for %s", val, path);
		return -1;
	}

	*config_val = val;
	log_debug("%s is %u", path, val);
	free(str);
	return 0;
}

#define LOCKSPACE_NODIR "/cluster/dlm/lockspace[@name=\"%s\"]/@nodir"

void read_ccs_nodir(struct mountgroup *mg, char *buf)
{
	char path[PATH_MAX];
	char *str;
	int val;
	int error;

	memset(path, 0, PATH_MAX);
	sprintf(path, LOCKSPACE_NODIR, mg->name);

	error = ccs_get(ccs_handle, path, &str);
	if (error || !str)
		return;

	val = atoi(str);

	if (val < 0) {
		log_error("ignore invalid value %d for %s", val, path);
		return;
	}

	snprintf(buf, 32, ":nodir=%d", val);

	log_debug("%s is %u", path, val);
	free(str);
}

#define ENABLE_WITHDRAW_PATH "/cluster/gfs_controld/@enable_withdraw"

int setup_ccs(void)
{
	int cd;

	if (ccs_handle)
		return 0;

	cd = ccs_connect();
	if (cd < 0) {
		log_error("ccs_connect error %d %d", cd, errno);
		return -1;
	}
	ccs_handle = cd;

	if (!optd_enable_withdraw)
		read_ccs_int(ENABLE_WITHDRAW_PATH, &cfgd_enable_withdraw);

	read_ccs_name("/cluster/@name", clustername);
	log_debug("cluster name \"%s\"", clustername);

	return 0;
}

void close_ccs(void)
{
	ccs_disconnect(ccs_handle);
}

