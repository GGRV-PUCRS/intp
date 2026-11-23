/* SPDX-License-Identifier: MIT */
/*
 * llcocc_stable.c — LLC occupancy, two backends.
 *
 * Ported from across-intp:
 *   variants/v2.1-c-abi-cgroup/src/llcocc.c @ 2e87b30fc762
 * Formulas and backend order preserved verbatim; registration moved
 * from the per-metric accessor singleton to the ADR-0001 linker
 * section.
 *
 *   resctrl		resctrl llc_occupancy summed across L3 domains
 *   proxy_from_miss_ratio
 *			reuse llcmr value as a coarse occupancy
 *			indicator (status PROXY)
 *
 * The proxy is only a directional signal: a process with many LLC
 * misses is contending for cache, but the value is not directly
 * comparable to occupancy in bytes. It exists so consumers always
 * receive a non-NaN value when at least perf is available.
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../../core/core.h"
#include "../../core/detect.h"
#include "../common/resctrl.h"

extern double intp_llcmr_last_value(void); /* defined in the llcmr backend */
extern const char *intp_llcmr_event_family(void); /* ditto */

static struct {
	int valid;
	char group[64];
	long llc_size_bytes;
} st;

static long resolve_llc_bytes(void)
{
	const struct intp_core_target *t = intp_core_target_get();

	if (t && t->llc_size_bytes_override > 0)
		return t->llc_size_bytes_override;

	long b = detect_cached()->llc_size_bytes;

	return b > 0 ? b * detect_cached()->num_sockets : 0;
}

/* ---- backend 1: resctrl llc_occupancy ---------------------------------- */

static int resctrl_probe(void)
{
	const system_capabilities_t *c = detect_cached();

	if (!c->resctrl_usable)
		return -1;
	if (!c->has_rdt_cmt && !c->has_amd_qos && !c->has_arm_mpam)
		return -1;
	if (resolve_llc_bytes() <= 0)
		return -1;
	return 0;
}

static int resctrl_init_(void)
{
	/* Share the per-target mon_group with the mbw backend: both read
	 * the same RMID's mon_data (llc_occupancy here, mbm_total_bytes
	 * there), and a cgroup/PID target's tasks can be in only one RMID,
	 * so a private group would steal them from mbw (or vice versa). */
	const struct intp_core_target *t = intp_core_target_get();

	if (resctrl_target_group_acquire(t ? t->pids : NULL,
					 t ? (size_t)t->n_pids : 0,
					 t ? t->cgroup_path : NULL, st.group,
					 sizeof(st.group)) != 0)
		return -1;

	long b = resctrl_read_llc_occupancy(st.group);

	if (b < 0) {
		resctrl_target_group_release();
		return -1;
	}
	st.llc_size_bytes = resolve_llc_bytes();
	st.valid = 1;
	return 0;
}

static int resctrl_read_(struct intp_core_sample *out, double interval_sec)
{
	(void)interval_sec;
	if (!st.valid || st.llc_size_bytes <= 0)
		return -1;

	/* Re-sync the shared mon_group with the cgroup's live members
	 * (cadence-gated; no-op for pid/system targets). */
	resctrl_target_group_rescan(intp_core_target_get()->cgroup_path);

	long b = resctrl_read_llc_occupancy(st.group);

	if (b < 0)
		return -1;

	double v = (double)b / (double)st.llc_size_bytes * 100.0;

	if (v < 0.0)
		v = 0.0;
	if (v > 99.0)
		v = 99.0;
	out->value = v;
	out->status = INTP_CORE_OK;
	out->backend_id = "resctrl";
	out->note = NULL;
	return 0;
}

static void resctrl_cleanup_(void)
{
	if (st.valid) {
		resctrl_target_group_release();
		st.valid = 0;
	}
}

/* ---- backend 2: proxy from llcmr --------------------------------------- */

static int proxy_probe(void)
{
	/* Proxy is usable when llcmr's primary backend is. We avoid
	 * reaching into llcmr's probe directly; instead we require perf
	 * availability, which is necessary for any miss-ratio backend. */
	const system_capabilities_t *c = detect_cached();

	return c->perf_usable ? 0 : -1;
}

static int proxy_init(void)
{
	/* The proxy has nothing to read unless llcmr opened perf counters.
	 * llcmr initialises first (metric order), so refuse here rather
	 * than let --print-schema claim a proxy with no source; the metric
	 * then reads unavailable, which is what the samples show anyway. */
	return intp_llcmr_event_family() ? 0 : -1;
}

static int proxy_read(struct intp_core_sample *out, double interval_sec)
{
	(void)interval_sec;

	double miss = intp_llcmr_last_value();

	if (isnan(miss))
		return -1;
	if (miss < 0.0)
		miss = 0.0;
	if (miss > 99.0)
		miss = 99.0;
	out->value = miss;
	out->status = INTP_CORE_PROXY;
	out->backend_id = "proxy_from_miss_ratio";
	out->note = "directional_only";
	return 0;
}

static void proxy_cleanup(void)
{
}

static const struct intp_core_backend b_resctrl = {
	.metric = "llcocc",
	.id = "resctrl",
	.description = "resctrl llc_occupancy summed across L3 domains",
	.model = "occupancy_share",
	.scope = "pid",
	.requires = "requires resctrl occupancy monitoring (CMT/AMD QoS/MPAM) "
		    "and a known LLC size",
	.priority = INTP_PRIORITY_MEDIUM,
	.lineage = INTP_LINEAGE_STABLE | INTP_LINEAGE_EBPF,
	.probe = resctrl_probe,
	.init = resctrl_init_,
	.read = resctrl_read_,
	.cleanup = resctrl_cleanup_,
};

static const struct intp_core_backend b_proxy = {
	.metric = "llcocc",
	.id = "proxy_from_miss_ratio",
	.description = "directional-only proxy: llcmr percentage",
	.model = "proxy_from_miss_ratio",
	.scope = "system",
	.requires = "requires usable perf events (llcmr miss-ratio source)",
	.priority = INTP_PRIORITY_LOWEST,
	.probe = proxy_probe,
	.init = proxy_init,
	.read = proxy_read,
	.cleanup = proxy_cleanup,
};

INTP_BACKEND_REGISTER(b_resctrl);
INTP_BACKEND_REGISTER(b_proxy);
