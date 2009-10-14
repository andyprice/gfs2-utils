#ifndef __GFS_DAEMON_DOT_H__
#define __GFS_DAEMON_DOT_H__

#include "clusterautoconfig.h"

#include <sys/types.h>
#include <asm/types.h>
#include <sys/uio.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <sys/poll.h>
#include <sys/wait.h>
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
#include <time.h>
#include <syslog.h>
#include <sched.h>
#include <signal.h>
#include <sys/time.h>
#include <dirent.h>
#include <openais/saAis.h>
#include <openais/saCkpt.h>
#include <corosync/cpg.h>
#include <liblogthread.h>

#include <linux/dlmconstants.h>
#include "libgfscontrol.h"
#include "gfs_controld.h"
#include "list.h"
#include "linux_endian.h"

/* TODO: warn if
   DLM_LOCKSPACE_LEN (from dlmconstants.h) !=
   GFS_MOUNTGROUP_LEN (from libgfscontrol.h)
*/

/* Maximum members of a mountgroup, should match CPG_MEMBERS_MAX in
   corosync/cpg.h.  There are no max defines in gfs-kernel for
   mountgroup members. (FIXME verify gfs-kernel/lock_dlm) */

#define MAX_NODES       128

/* Max string length printed on a line, for debugging/dump output. */

#define MAXLINE         256

extern int daemon_debug_opt;
extern int daemon_quit;
extern int cluster_down;
extern int poll_dlm;
extern struct list_head mountgroups;
extern int our_nodeid;
extern char clustername[1024]; /* actual limit is sure to be smaller */
extern char daemon_debug_buf[256];
extern char dump_buf[GFSC_DUMP_SIZE];
extern int dump_point;
extern int dump_wrap;
extern int dmsetup_wait;
extern cpg_handle_t cpg_handle_daemon;
extern int libcpg_flow_control_on;
extern struct list_head withdrawn_mounts;

void daemon_dump_save(void);

#define log_level(lvl, fmt, args...) \
do { \
	snprintf(daemon_debug_buf, 255, "%ld " fmt "\n", time(NULL), ##args); \
	daemon_dump_save(); \
	logt_print(lvl, fmt "\n", ##args); \
	if (daemon_debug_opt) \
		fprintf(stderr, "%s", daemon_debug_buf); \
} while (0)

#define log_debug(fmt, args...) log_level(LOG_DEBUG, fmt, ##args)
#define log_error(fmt, args...) log_level(LOG_ERR, fmt, ##args)

#define log_group(g, fmt, args...) \
do { \
	snprintf(daemon_debug_buf, 255, "%ld %s " fmt "\n", time(NULL), \
		 (g)->name, ##args); \
	daemon_dump_save(); \
	logt_print(LOG_DEBUG, "%s " fmt "\n", (g)->name, ##args); \
	if (daemon_debug_opt) \
		fprintf(stderr, "%s", daemon_debug_buf); \
} while (0)

#define log_plock(g, fmt, args...) \
do { \
	snprintf(daemon_debug_buf, 255, "%ld %s " fmt "\n", time(NULL), \
		 (g)->name, ##args); \
	if (daemon_debug_opt && cfgd_plock_debug) \
		fprintf(stderr, "%s", daemon_debug_buf); \
} while (0)

struct mountgroup {
	struct list_head	list;
	uint32_t		id;
	struct gfsc_mount_args	mount_args;
	char			name[GFS_MOUNTGROUP_LEN+1];

	int			mount_client;
	int			mount_client_result;
	int			mount_client_notified;
	int			mount_client_delay;
	int			remount_client;

	int			withdraw_uevent;
	int			withdraw_suspend;
	int			dmsetup_wait;
	pid_t			dmsetup_pid;
	int			our_jid;
	int			spectator;
	int			ro;
	int			rw;
	int                     joining;
	int                     leaving;
	int			kernel_mount_error;
	int			kernel_mount_done;
	int			first_mounter;

	/* cpg-new stuff */

	cpg_handle_t            cpg_handle;
	int                     cpg_client;
	int                     cpg_fd;
	int                     kernel_stopped;
	uint32_t                change_seq;
	uint32_t                started_count;
	struct change           *started_change;
	struct list_head        changes;
	struct list_head        node_history;
	struct list_head	journals;
	int			dlm_registered;
	int			dlm_notify_nodeid;
	int			first_done_uevent;
	int			first_recovery_needed;
	int			first_recovery_master;
	int			first_recovery_msg;
	int			local_recovery_jid;
	int			local_recovery_busy;
};

/* these need to match the kernel defines of the same name in lm_interface.h */

#define LM_RD_GAVEUP 308
#define LM_RD_SUCCESS 309

/* config.c */
int setup_ccs(void);
void close_ccs(void);
void read_ccs_name(const char *path, char *name);
void read_ccs_yesno(const char *path, int *yes, int *no);
int read_ccs_int(const char *path, int *config_val);
void read_ccs_nodir(struct mountgroup *mg, char *buf);

/* cpg-new.c */
int setup_cpg_daemon(void);
void close_cpg_daemon(void);
void process_cpg_daemon(int ci);
int setup_dlmcontrol(void);
void process_dlmcontrol(int ci);
int set_protocol(void);
void process_recovery_uevent(struct mountgroup *mg, int jid, int status);
void process_first_mount(struct mountgroup *mg);
void process_mountgroups(void);
int gfs_join_mountgroup(struct mountgroup *mg);
void do_leave(struct mountgroup *mg, int mnterr);
void gfs_mount_done(struct mountgroup *mg);
void send_remount(struct mountgroup *mg, struct gfsc_mount_args *ma);
void send_withdraw(struct mountgroup *mg);
int set_mountgroup_info(struct mountgroup *mg, struct gfsc_mountgroup *out);
int set_node_info(struct mountgroup *mg, int nodeid, struct gfsc_node *node);
int set_mountgroups(int *count, struct gfsc_mountgroup **mgs_out);
int set_mountgroup_nodes(struct mountgroup *mg, int option, int *node_count,
	struct gfsc_node **nodes_out);
void free_mg(struct mountgroup *mg);

/* main.c */
int do_read(int fd, void *buf, size_t count);
int do_write(int fd, void *buf, size_t count);
void client_dead(int ci);
int client_add(int fd, void (*workfn)(int ci), void (*deadfn)(int ci));
int client_fd(int ci);
void client_ignore(int ci, int fd);
void client_back(int ci, int fd);
struct mountgroup *create_mg(char *name);
struct mountgroup *find_mg(char *name);
void client_reply_remount(struct mountgroup *mg, int ci, int result);
void client_reply_join(int ci, struct gfsc_mount_args *ma, int result);
void client_reply_join_full(struct mountgroup *mg, int result);
void query_lock(void);
void query_unlock(void);
void process_connection(int ci);
void cluster_dead(int ci);

/* member_cman.c */
int setup_cluster_cfg(void);
void close_cluster_cfg(void);
void process_cluster_cfg(int ci);
void kick_node_from_cluster(int nodeid);

/* util.c */
int we_are_in_fence_domain(void);
int set_sysfs(struct mountgroup *mg, const char *field, int val);
int run_dmsetup_suspend(struct mountgroup *mg, char *dev);
void update_dmsetup_wait(void);
void update_flow_control_status(void);
int check_uncontrolled_filesystems(void);

/* logging.c */

void init_logging(void);
void setup_logging(void);
void close_logging(void);

/* crc.c */
uint32_t cpgname_to_crc(const char *data, int len);

#endif
