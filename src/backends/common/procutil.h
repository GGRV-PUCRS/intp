/* SPDX-License-Identifier: MIT */
/*
 * procutil.h — procfs/sysfs parsing helpers.
 *
 * Ported from across-intp:
 *   variants/v2.1-c-abi-cgroup/include/procutil.h @ 2e87b30fc762
 *
 * Each function is side-effect-free and reads the entire target file.
 * Intended for use by the metric backends and the unit tests.
 */

#ifndef INTP_PROCUTIL_H
#define INTP_PROCUTIL_H

#include <sys/types.h>

#include <stddef.h>

int procutil_read_file(const char *path, char *buf, size_t bufsize);
long procutil_read_long(const char *path); /* -1 on failure */

struct diskstats_entry {
	char name[32];
	unsigned long reads_completed;
	unsigned long writes_completed;
	unsigned long read_ms;
	unsigned long write_ms;
	unsigned long io_ticks; /* field 13: weighted busy time (ms) */
	unsigned long time_in_queue;
};

/* Returns count of entries, or -1 on error. */
int procutil_read_diskstats(struct diskstats_entry *entries, size_t max);

struct netdev_entry {
	char iface[32];
	unsigned long rx_bytes, tx_bytes;
	unsigned long rx_packets, tx_packets;
};

int procutil_read_netdev(struct netdev_entry *entries, size_t max);

/* Same parser as procutil_read_netdev but against an explicit path, so
 * the caller can read another task's network namespace via
 * /proc/<pid>/net/dev (per-container netp attribution). */
int procutil_read_netdev_at(const char *path, struct netdev_entry *entries,
			    size_t max);

/* Sum NET_TX and NET_RX softirq columns across all CPUs. */
int procutil_read_net_softirqs(unsigned long *net_tx, unsigned long *net_rx);

/* Read /proc/<pid>/stat utime + stime in jiffies. */
int procutil_read_proc_stat(pid_t pid, unsigned long *utime,
			    unsigned long *stime);

/* Read aggregate /proc/stat first line. total includes user+nice+sys+
 * idle+iowait+irq+softirq+steal+guest+guest_nice. */
int procutil_read_stat_total(unsigned long *total_jiffies,
			     unsigned long *idle_jiffies);

/* /proc/<pid>/io for read_bytes / write_bytes. */
int procutil_read_proc_io(pid_t pid, unsigned long long *read_bytes,
			  unsigned long long *write_bytes);

/* Read PIDs from <cgroup_path>/cgroup.procs into out[] (up to max).
 * Returns the count. Used to (re-)populate a resctrl mon_group from a
 * cgroup's live member set. */
int procutil_read_cgroup_procs(const char *cgroup_path, pid_t *out, size_t max);

/* Like procutil_read_cgroup_procs, but recurses into descendant cgroups
 * too. cgroup v2's "no internal processes" rule places a container's
 * payload in LEAF cgroups (incus/lxc, nested k8s pods), so a
 * non-recursive read of the targeted cgroup misses them. A task lives
 * in exactly one cgroup, so the subtree union is disjoint (no dedup
 * needed). Used for target resolution and the resctrl rescan so a
 * container whose top cgroup holds no processes is still enrolled. */
int procutil_read_cgroup_procs_rec(const char *cgroup_path, pid_t *out,
				   size_t max);

/* Like procutil_read_cgroup_procs_rec, but reads cgroup.threads: every
 * TID in the subtree, not only thread-group leaders.
 * /proc/<pid>/schedstat and the *_ctxt_switches lines of
 * /proc/<pid>/status describe ONE task_struct (fs/proc/base.c
 * proc_pid_schedstat, fs/proc/array.c task_context_switch_counts), so
 * per-task scheduler metrics must be read per TID or the worker threads
 * of a multi-threaded workload are never counted (across-intp C38). */
int procutil_read_cgroup_threads_rec(const char *cgroup_path, pid_t *out,
				     size_t max);

/* List the TIDs of process `pid` from /proc/<pid>/task into out[] (up to
 * max). Returns the count; 0 if the process is gone. */
int procutil_read_proc_tasks(pid_t pid, pid_t *out, size_t max);

#endif /* INTP_PROCUTIL_H */
