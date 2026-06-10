/* SPDX-License-Identifier: MIT */
/*
 * resctrl.h — thin wrapper over /sys/fs/resctrl for IntP backends.
 *
 * Ported from intp-comparison:
 *   variants/v2.1-c-abi-cgroup/include/resctrl.h @ 2e87b30fc762
 *
 * resctrl is the cross-vendor cache/bandwidth monitoring API:
 *   Intel CMT/MBM (4.10+), AMD QoS (5.1+ for Rome+), ARM MPAM (6.19+).
 *
 * RMID is a scarce resource (typically 32-256 per system). To keep our
 * footprint small we use a single monitoring group per IntP run, named
 * after the run's PID, and assign every monitored task into it.
 */

#ifndef INTP_RESCTRL_H
#define INTP_RESCTRL_H

#include <sys/types.h>

#include <stddef.h>

#define RESCTRL_PATH_MAX    384
#define RESCTRL_MAX_DOMAINS 32

/* Mount /sys/fs/resctrl if needed and possible. Returns 0 on success or
 * if already mounted; -1 if not mountable (no perms / no kernel support). */
int resctrl_ensure_mounted(void);

/* Create a monitoring group "<name>" under /sys/fs/resctrl/mon_groups/.
 * Idempotent: returns 0 if it already exists. */
int resctrl_create_mongroup(const char *name);

/* Append PIDs to <group>/tasks. Returns 0 if at least one PID was
 * accepted (kernel rejects exited PIDs with ESRCH). */
int resctrl_assign_pids(const char *name, const pid_t *pids, size_t n_pids);

/* Sum a counter file (e.g. "llc_occupancy", "mbm_total_bytes",
 * "mbm_local_bytes") across every mon_L3_* domain in the group.
 * Returns -1 on error, otherwise the summed value (bytes). */
long resctrl_read_llc_occupancy(const char *name);
long resctrl_read_mbm_total(const char *name);
long resctrl_read_mbm_local(const char *name);

/* Remove monitoring group. Safe to call when it doesn't exist. */
int resctrl_remove_mongroup(const char *name);

/* RMID budget */
int resctrl_max_rmids(void);	/* num_rmids from info, -1 if unknown   */
int resctrl_rmids_in_use(void); /* count CTRL_MON + mon_groups, -1 err  */

/* Production hygiene (additions over the v2.1 origin):
 * - reap_stale removes mon_groups matching <prefix>*_<pid> whose owner
 *   pid is dead (crash recovery). Returns groups reaped, -1 on error.
 * - rmid_budget_ok returns 1 when allocating one more group stays
 *   within 75 % of num_rmids (or the budget is unknowable), 0 when the
 *   caller should fall through to a non-resctrl backend. */
int resctrl_reap_stale(const char *prefix);
int resctrl_rmid_budget_ok(void);

/* Lower-level helper for callers that need per-domain values. */
int resctrl_enumerate_domains(const char *group_name, const char *filename,
			      char paths[][RESCTRL_PATH_MAX], int max_domains);
long resctrl_sum_paths(char paths[][RESCTRL_PATH_MAX], int n_paths);

/* Re-read <cgroup_path>/cgroup.procs and re-assign the live member set to the
 * mon_group <name>, on a cadence keyed by <sample_idx> (tight for the first
 * few samples to catch an app's initial fork storm, then periodic). Picks up
 * tasks that joined the cgroup after init (late-launched workers, cgroup
 * migrations) that RMID fork-inheritance alone would miss. No-op when
 * cgroup_path is NULL or off-cadence; the root group is skipped by
 * resctrl_assign_pids. Returns 0 when nothing was done or assignment
 * succeeded. */
int resctrl_rescan_cgroup(const char *name, const char *cgroup_path,
			  int sample_idx);

#endif /* INTP_RESCTRL_H */
