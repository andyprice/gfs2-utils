#include "gfs_daemon.h"
#include "config.h"
#include <corosync/corotypes.h>
#include <corosync/cfg.h>

static corosync_cfg_handle_t ch;

void kick_node_from_cluster(int nodeid)
{
	if (!nodeid) {
		log_error("telling corosync to shut down cluster locally");
		corosync_cfg_try_shutdown(ch,
				COROSYNC_CFG_SHUTDOWN_FLAG_IMMEDIATE);
	} else {
		log_error("telling corosync to remove nodeid %d from cluster",
			  nodeid);
		corosync_cfg_kill_node(ch, nodeid, "gfs_controld");
	}
}

static void shutdown_callback(corosync_cfg_handle_t h,
			      corosync_cfg_shutdown_flags_t flags)
{
	if (flags & COROSYNC_CFG_SHUTDOWN_FLAG_REQUEST) {
		if (list_empty(&mountgroups))
			corosync_cfg_replyto_shutdown(ch,
					COROSYNC_CFG_SHUTDOWN_FLAG_YES);
		else {
			log_debug("no to corosync shutdown");
			corosync_cfg_replyto_shutdown(ch,
					COROSYNC_CFG_SHUTDOWN_FLAG_NO);
		}
	}
}

static corosync_cfg_callbacks_t cfg_callbacks =
{
	.corosync_cfg_shutdown_callback = shutdown_callback,
	.corosync_cfg_state_track_callback = NULL,
};

void process_cluster_cfg(int ci)
{
	cs_error_t err;

	err = corosync_cfg_dispatch(ch, CS_DISPATCH_ALL);
	if (err != CS_OK)
		cluster_dead(0);
}

int setup_cluster_cfg(void)
{
	cs_error_t err;
	unsigned int nodeid;
	int fd;

	err = corosync_cfg_initialize(&ch, &cfg_callbacks);
	if (err != CS_OK) {
		log_error("corosync cfg init error %d", err);
		return -1;
	}

	err = corosync_cfg_fd_get(ch, &fd);
	if (err != CS_OK) {
		log_error("corosync cfg fd_get error %d", err);
		corosync_cfg_finalize(ch);
		return -1;
	}

	err = corosync_cfg_local_get(ch, &nodeid);
	if (err != CS_OK) {
		log_error("corosync cfg local_get error %d", err);
		corosync_cfg_finalize(ch);
		return -1;
	}
	our_nodeid = nodeid;
	log_debug("our_nodeid %d", our_nodeid);

	return fd;
}

void close_cluster_cfg(void)
{
	corosync_cfg_finalize(ch);
}

/* what's the replacement for this? */
#if 0
	case CMAN_REASON_CONFIG_UPDATE:
		setup_logging();
		setup_ccs();
		break;
	}
#endif

