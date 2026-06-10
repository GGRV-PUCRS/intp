/* SPDX-License-Identifier: MIT */
/*
 * cpu_procfs.c — CPU utilization, three backends.
 *
 * Ported from intp-comparison:
 *   variants/v2.1-c-abi-cgroup/src/cpu.c @ 2e87b30fc762
 * Formulas and backend order preserved verbatim; registration moved
 * from the per-metric accessor singleton to the ADR-0001 linker
 * section.
 *
 *   cgroup		native per-cgroup CPU via cgroup v2 cpu.stat
 *			usage_usec. Selected when the target names a
 *			cgroup v2 path. Counts all current+future tasks
 *			in the cgroup (no PID snapshot).
 *   procfs_pid		per-PID via /proc/<pid>/stat utime+stime,
 *			normalized by /proc/stat total jiffies delta.
 *			Selected when the target has PIDs but no usable
 *			cgroup.
 *   procfs_system	system-wide /proc/stat (1 - idle/total). Always
 *			probeable; the fallback when no target given.
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "../../core/core.h"
#include "../common/procutil.h"

static struct {
	int valid;
	unsigned long sum_proc_jiffies; /* utime+stime summed across PIDs */
	unsigned long total_jiffies;
} pid_state;

static struct {
	int valid;
	unsigned long total_jiffies;
	unsigned long idle_jiffies;
} sys_state;

/* ---- cgroup backend: native per-cgroup CPU via cgroup v2 cpu.stat ----
 *
 * v2.1's distinguishing feature: attribute by cgroup, not by a PID
 * snapshot. cpu.stat's usage_usec is the cumulative CPU time of every
 * task currently in the cgroup (and its descendants), so children
 * spawned after startup are counted automatically — no cgroup.procs
 * re-scan, no churn. Selected ahead of the PID/system backends
 * whenever the target names a cgroup v2 path.
 */
static struct {
	int valid;
	unsigned long long usage_usec; /* cumulative cgroup CPU time */
	long ncpus;
} cg_state;

static int cpu_cgroup_usage(const char *cgpath, unsigned long long *out)
{
	char path[512];

	snprintf(path, sizeof(path), "%s/cpu.stat", cgpath);

	FILE *f = fopen(path, "r");

	if (!f)
		return -1;

	char line[128];
	int rc = -1;

	while (fgets(line, sizeof(line), f)) {
		if (sscanf(line, "usage_usec %llu", out) == 1) {
			rc = 0;
			break;
		}
	}
	fclose(f);
	return rc;
}

static int cpu_cgroup_probe(void)
{
	const struct intp_core_target *t = intp_core_target_get();

	if (!t || !t->cgroup_path)
		return -1;

	char path[512];

	snprintf(path, sizeof(path), "%s/cpu.stat", t->cgroup_path);
	return (access(path, R_OK) == 0) ? 0 : -1;
}

static int cpu_cgroup_init(void)
{
	const struct intp_core_target *t = intp_core_target_get();

	if (cpu_cgroup_usage(t->cgroup_path, &cg_state.usage_usec) != 0)
		return -1;
	cg_state.ncpus = sysconf(_SC_NPROCESSORS_ONLN);
	if (cg_state.ncpus < 1)
		cg_state.ncpus = 1;
	cg_state.valid = 1;
	return 0;
}

static int cpu_cgroup_read(struct intp_core_sample *out, double interval_sec)
{
	if (!cg_state.valid)
		return -1;

	const struct intp_core_target *t = intp_core_target_get();
	unsigned long long now = 0;

	if (cpu_cgroup_usage(t->cgroup_path, &now) != 0)
		return -1;

	double d_usec = (double)(now - cg_state.usage_usec);

	cg_state.usage_usec = now;

	/* % of total machine CPU over the window — same normalization
	 * basis as procfs_pid (target CPU time / total available CPU
	 * time across cores). */
	double avail_usec = (double)cg_state.ncpus * interval_sec * 1e6;
	double v = (avail_usec > 0.0) ? (d_usec / avail_usec) * 100.0 : 0.0;

	if (v < 0.0)
		v = 0.0;
	if (v > 100.0)
		v = 100.0;
	out->value = v;
	out->status = INTP_CORE_OK;
	out->backend_id = "cgroup";
	out->note = NULL;
	return 0;
}

static void cpu_cgroup_cleanup(void)
{
	cg_state.valid = 0;
}

static const struct intp_core_backend cpu_cgroup_backend = {
	.metric = "cpu",
	.id = "cgroup",
	.description = "native per-cgroup CPU time via cgroup v2 cpu.stat",
	.model = "cgroup_cpustat_share",
	.scope = "cgroup",
	.requires = "requires a cgroup target with readable cpu.stat",
	.priority = INTP_PRIORITY_MEDIUM,
	.probe = cpu_cgroup_probe,
	.init = cpu_cgroup_init,
	.read = cpu_cgroup_read,
	.cleanup = cpu_cgroup_cleanup,
};

static int cpu_pid_probe(void)
{
	const struct intp_core_target *t = intp_core_target_get();

	return (t && t->n_pids > 0) ? 0 : -1;
}

static unsigned long sum_pid_jiffies(void)
{
	const struct intp_core_target *t = intp_core_target_get();
	unsigned long sum = 0;

	for (int i = 0; i < t->n_pids; i++) {
		unsigned long u = 0, s = 0;

		if (procutil_read_proc_stat(t->pids[i], &u, &s) == 0)
			sum += (u + s);
	}
	return sum;
}

static int cpu_pid_init(void)
{
	pid_state.sum_proc_jiffies = sum_pid_jiffies();
	if (procutil_read_stat_total(&pid_state.total_jiffies, NULL) != 0)
		return -1;
	pid_state.valid = 1;
	return 0;
}

static int cpu_pid_read(struct intp_core_sample *out, double interval_sec)
{
	(void)interval_sec;
	if (!pid_state.valid)
		return -1;

	unsigned long pj = sum_pid_jiffies();
	unsigned long tj = 0;

	if (procutil_read_stat_total(&tj, NULL) != 0)
		return -1;

	long dpj = (long)(pj - pid_state.sum_proc_jiffies);
	long dtj = (long)(tj - pid_state.total_jiffies);

	pid_state.sum_proc_jiffies = pj;
	pid_state.total_jiffies = tj;

	/* Target CPU time as a fraction of total CPU time across all
	 * cores (system-relative; see v2.1 DESIGN.md). */
	double v = 0.0;

	if (dtj > 0 && dpj >= 0)
		v = ((double)dpj / (double)dtj) * 100.0;

	if (v < 0.0)
		v = 0.0;
	if (v > 100.0)
		v = 100.0;
	out->value = v;
	out->status = INTP_CORE_OK;
	out->backend_id = "procfs_pid";
	out->note = NULL;
	return 0;
}

static void cpu_pid_cleanup(void)
{
	pid_state.valid = 0;
}

static int cpu_sys_probe(void)
{
	unsigned long t = 0, i = 0;

	return procutil_read_stat_total(&t, &i) == 0 ? 0 : -1;
}

static int cpu_sys_init(void)
{
	if (procutil_read_stat_total(&sys_state.total_jiffies,
				     &sys_state.idle_jiffies) != 0)
		return -1;
	sys_state.valid = 1;
	return 0;
}

static int cpu_sys_read(struct intp_core_sample *out, double interval_sec)
{
	(void)interval_sec;
	if (!sys_state.valid)
		return -1;

	unsigned long total = 0, idle = 0;

	if (procutil_read_stat_total(&total, &idle) != 0)
		return -1;

	long dt = (long)(total - sys_state.total_jiffies);
	long di = (long)(idle - sys_state.idle_jiffies);

	sys_state.total_jiffies = total;
	sys_state.idle_jiffies = idle;

	double v = (dt > 0) ? (1.0 - (double)di / (double)dt) * 100.0 : 0.0;

	if (v < 0.0)
		v = 0.0;
	if (v > 100.0)
		v = 100.0;
	out->value = v;
	out->status = INTP_CORE_OK;
	out->backend_id = "procfs_system";
	out->note = NULL;
	return 0;
}

static void cpu_sys_cleanup(void)
{
	sys_state.valid = 0;
}

static const struct intp_core_backend cpu_pid_backend = {
	.metric = "cpu",
	.id = "procfs_pid",
	.description = "per-PID utime+stime via /proc/<pid>/stat",
	.model = "proc_pid_stat_share",
	.scope = "pid",
	.requires = "requires a PID target",
	.priority = INTP_PRIORITY_LOW,
	.probe = cpu_pid_probe,
	.init = cpu_pid_init,
	.read = cpu_pid_read,
	.cleanup = cpu_pid_cleanup,
};

static const struct intp_core_backend cpu_sys_backend = {
	.metric = "cpu",
	.id = "procfs_system",
	.description = "system-wide /proc/stat (1 - idle/total)",
	.model = "proc_stat_busy_fraction",
	.scope = "system",
	.requires = "requires readable /proc/stat",
	.priority = INTP_PRIORITY_LOWEST,
	.probe = cpu_sys_probe,
	.init = cpu_sys_init,
	.read = cpu_sys_read,
	.cleanup = cpu_sys_cleanup,
};

INTP_BACKEND_REGISTER(cpu_cgroup_backend);
INTP_BACKEND_REGISTER(cpu_pid_backend);
INTP_BACKEND_REGISTER(cpu_sys_backend);
