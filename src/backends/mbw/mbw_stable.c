/* SPDX-License-Identifier: MIT */
/*
 * mbw_stable.c — memory bandwidth utilization, four-backend hierarchy.
 *
 * Ported from intp-comparison:
 *   variants/v2.1-c-abi-cgroup/src/mbw.c @ 2e87b30fc762
 * Formulas and backend order preserved verbatim; registration moved
 * from the per-metric accessor singleton to the ADR-0001 linker
 * section.
 *
 *   perf_uncore_imc	Intel uncore IMC CAS_COUNT.RD/WR (CAP_SYS_ADMIN)
 *   perf_amd_df	AMD Data Fabric DRAM data beats (CAP_SYS_ADMIN)
 *   perf_arm_cmn	ARM CMN HN-F memory traffic (CAP_SYS_ADMIN)
 *   resctrl_mbm	resctrl mbm_total_bytes fallback (when uncore
 *			PMU unavailable)
 *
 * Normalization: detected mem_bw_max_bps unless overridden by
 * --mem-bw-max-bps. Each backend reports its source via
 * struct intp_core_sample.backend_id.
 */

#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../../core/core.h"
#include "../../core/detect.h"
#include "../common/perfev.h"
#include "../common/resctrl.h"

static long resolve_mem_bw_bps(void)
{
	const struct intp_core_target *t = intp_core_target_get();

	if (t && t->mem_bw_max_bps_override > 0)
		return t->mem_bw_max_bps_override;
	return detect_memory_bandwidth_max_bps();
}

/* ---- backend 1: resctrl MBM -------------------------------------------- */

static struct {
	int valid;
	char group[64];
	long prev_bytes;
	long max_bps;
} mb;

static int resctrl_probe(void)
{
	const system_capabilities_t *c = detect_cached();

	if (!c->resctrl_usable)
		return -1;
	if (!c->has_rdt_mbm && !c->has_amd_qos && !c->has_arm_mpam)
		return -1;
	return 0;
}

static int resctrl_init_(void)
{
	/* Share the per-target mon_group with the llcocc backend: a
	 * cgroup/PID target's tasks can be in only one RMID, so a private
	 * group here would steal them from llcocc (or vice versa). The
	 * uncore-PMU backends own the system-wide case; for a cgroup/PID
	 * target the shared group scopes mbm_total_bytes to those tasks. */
	const struct intp_core_target *t = intp_core_target_get();

	if (resctrl_target_group_acquire(t ? t->pids : NULL,
					 t ? (size_t)t->n_pids : 0,
					 t ? t->cgroup_path : NULL, mb.group,
					 sizeof(mb.group)) != 0)
		return -1;

	long b = resctrl_read_mbm_total(mb.group);

	if (b < 0) {
		resctrl_target_group_release();
		return -1;
	}
	mb.prev_bytes = b;
	mb.max_bps = resolve_mem_bw_bps();
	mb.valid = 1;
	return 0;
}

static int resctrl_read_(struct intp_core_sample *out, double interval_sec)
{
	if (!mb.valid || interval_sec <= 0)
		return -1;
	/* Keep the shared mon_group current with the cgroup's live members
	 * (late-spawned workers, migrations) — cadence-gated, cheap,
	 * no-op when system-wide or PID-scoped. */
	resctrl_target_group_rescan(intp_core_target_get()->cgroup_path);

	long now = resctrl_read_mbm_total(mb.group);

	if (now < 0)
		return -1;

	long delta = now - mb.prev_bytes;

	mb.prev_bytes = now;

	double bps = (double)delta / interval_sec;
	double v = bps / (double)mb.max_bps * 100.0;

	if (v < 0.0)
		v = 0.0;
	if (v > 99.0)
		v = 99.0;
	out->value = v;
	out->status = INTP_CORE_OK;
	out->backend_id = "resctrl_mbm";
	out->note = NULL;
	return 0;
}

static void resctrl_cleanup_(void)
{
	if (mb.valid) {
		resctrl_target_group_release();
		mb.valid = 0;
	}
}

/* ---- shared perf-uncore helper ------------------------------------------ */

typedef int (*open_pmu_fn)(int **fds_out);

static struct {
	int valid;
	int *fds;
	int n_fds;
	uint64_t prev_total;
	long max_bps;
	const char *backend_id;
	int beat_bytes;
} pu;

static int pu_init_with(open_pmu_fn fn, const char *id, int beat_bytes)
{
	int *fds = NULL;
	int n = fn(&fds);

	if (n <= 0)
		return -1;
	pu.fds = fds;
	pu.n_fds = n;
	pu.beat_bytes = beat_bytes;
	pu.backend_id = id;
	pu.max_bps = resolve_mem_bw_bps();
	pu.prev_total = 0;
	for (int i = 0; i < n; i++) {
		uint64_t v = 0;

		perfev_read(fds[i], &v);
		pu.prev_total += v;
	}
	pu.valid = 1;
	return 0;
}

static int pu_read(struct intp_core_sample *out, double interval_sec)
{
	if (!pu.valid || interval_sec <= 0)
		return -1;

	uint64_t total = 0;

	for (int i = 0; i < pu.n_fds; i++) {
		uint64_t v = 0;

		perfev_read(pu.fds[i], &v);
		total += v;
	}

	uint64_t delta = total - pu.prev_total;

	pu.prev_total = total;

	double bytes_per_sec =
		(double)delta * (double)pu.beat_bytes / interval_sec;
	double v = bytes_per_sec / (double)pu.max_bps * 100.0;

	if (v < 0.0)
		v = 0.0;
	if (v > 99.0)
		v = 99.0;
	out->value = v;
	out->status = INTP_CORE_OK;
	out->backend_id = pu.backend_id;
	out->note = NULL;
	return 0;
}

static void pu_cleanup(void)
{
	if (!pu.valid)
		return;
	for (int i = 0; i < pu.n_fds; i++)
		perfev_close(pu.fds[i]);
	free(pu.fds);
	pu.fds = NULL;
	pu.n_fds = 0;
	pu.valid = 0;
}

/* The uncore memory-controller PMUs (Intel IMC, AMD Data Fabric, ARM
 * CMN) count TOTAL socket DRAM traffic -- they physically cannot
 * attribute bandwidth to a cgroup or a PID set. Reject them for any
 * sub-system target so the resctrl mbm backend (per-RMID, isolatable)
 * is selected instead. DELIBERATE DEVIATION from the ported v2.1 chain,
 * which preferred IMC unconditionally and therefore reported the
 * SYSTEM-WIDE figure as a cgroup's mbw (verified on the RDT testbed: an
 * idle cgroup read ~58% while the kernel's own per-RMID mon_group read
 * 0%). v3.3 already used resctrl for per-cgroup mbw; intp now matches
 * that for cgroup/PID targets while keeping IMC for system-wide (its
 * most accurate source). On a non-RDT host this correctly leaves
 * per-cgroup mbw UNAVAILABLE rather than silently system-wide. */
static int uncore_target_is_system_wide(void)
{
	const struct intp_core_target *t = intp_core_target_get();

	return !t || (!t->cgroup_path && t->n_pids == 0);
}

/* ---- backend 2: Intel uncore IMC --------------------------------------- */

static int imc_probe(void)
{
	const system_capabilities_t *c = detect_cached();

	if (!uncore_target_is_system_wide())
		return -1;
	if (c->vendor != VENDOR_INTEL)
		return -1;
	if (!c->perf_uncore_imc)
		return -1;
	if (c->perf_paranoid > -1 && geteuid() != 0)
		return -1;
	return 0;
}

static int imc_init(void)
{
	return pu_init_with(perfev_open_uncore_imc_intel, "perf_uncore_imc",
			    64);
}

/* ---- backend 3: AMD Data Fabric ---------------------------------------- */

static int amd_df_probe(void)
{
	const system_capabilities_t *c = detect_cached();

	if (!uncore_target_is_system_wide())
		return -1;
	if (c->vendor != VENDOR_AMD)
		return -1;
	if (!c->perf_amd_df)
		return -1;
	if (c->perf_paranoid > -1 && geteuid() != 0)
		return -1;
	return 0;
}

static int amd_df_init(void)
{
	return pu_init_with(perfev_open_amd_df, "perf_amd_df", 64);
}

/* ---- backend 4: ARM CMN ------------------------------------------------ */

static int arm_cmn_probe(void)
{
	const system_capabilities_t *c = detect_cached();

	if (!uncore_target_is_system_wide())
		return -1;
	if (c->vendor != VENDOR_ARM)
		return -1;
	if (!c->perf_arm_cmn)
		return -1;
	if (c->perf_paranoid > -1 && geteuid() != 0)
		return -1;
	return 0;
}

static int arm_cmn_init(void)
{
	return pu_init_with(perfev_open_arm_cmn, "perf_arm_cmn", 64);
}

/* ---- registration ------------------------------------------------------ */

static const struct intp_core_backend b_resctrl = {
	.metric = "mbw",
	.id = "resctrl_mbm",
	.description = "resctrl mbm_total_bytes summed across L3 domains",
	.model = "mbm_bandwidth_share",
	.scope = "cgroup",
	.requires = "requires mounted resctrl with MBM (Intel RDT, AMD QoS, or "
		    "ARM MPAM)",
	.priority = INTP_PRIORITY_LOWEST,
	.probe = resctrl_probe,
	.init = resctrl_init_,
	.read = resctrl_read_,
	.cleanup = resctrl_cleanup_,
};
static const struct intp_core_backend b_imc = {
	.metric = "mbw",
	.id = "perf_uncore_imc",
	.description = "Intel uncore IMC CAS_COUNT.RD+WR (CAP_SYS_ADMIN)",
	.model = "imc_cas_bandwidth_share",
	.scope = "system",
	.requires = "requires Intel uncore_imc PMU and root (or "
		    "perf_event_paranoid = -1)",
	.priority = INTP_PRIORITY_MEDIUM,
	.probe = imc_probe,
	.init = imc_init,
	.read = pu_read,
	.cleanup = pu_cleanup,
};
static const struct intp_core_backend b_amd_df = {
	.metric = "mbw",
	.id = "perf_amd_df",
	.description = "AMD Data Fabric DRAM data beats (CAP_SYS_ADMIN)",
	.model = "df_bandwidth_share",
	.scope = "system",
	.requires = "requires AMD amd_df PMU and root (or perf_event_paranoid "
		    "= -1)",
	.priority = INTP_PRIORITY_LOW,
	.chain_order = 0,
	.probe = amd_df_probe,
	.init = amd_df_init,
	.read = pu_read,
	.cleanup = pu_cleanup,
};
static const struct intp_core_backend b_arm_cmn = {
	.metric = "mbw",
	.id = "perf_arm_cmn",
	.description = "ARM CMN HN-F memory traffic (CAP_SYS_ADMIN)",
	.model = "cmn_bandwidth_share",
	.scope = "system",
	.requires =
		"requires ARM CMN PMU and root (or perf_event_paranoid = -1)",
	.priority = INTP_PRIORITY_LOW,
	.chain_order = 1,
	.probe = arm_cmn_probe,
	.init = arm_cmn_init,
	.read = pu_read,
	.cleanup = pu_cleanup,
};

INTP_BACKEND_REGISTER(b_imc);
INTP_BACKEND_REGISTER(b_amd_df);
INTP_BACKEND_REGISTER(b_arm_cmn);
INTP_BACKEND_REGISTER(b_resctrl);
