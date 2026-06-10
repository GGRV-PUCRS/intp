/* SPDX-License-Identifier: MIT */
/*
 * llcmr_ebpf.c — LLC miss ratio, eBPF backend shim.
 *
 * Ported from intp-comparison:
 *   variants/v3.3-ebpf-core-cgroup/src/intp_agg.c @ 2e87b30fc762
 * compute_sample()'s canonical llcmr slice, verbatim:
 *
 *   llcmr = llc_misses / llc_refs * 100, 0 when refs == 0, clamped
 *
 * The counters come from perf_event-attached BPF programs (one
 * HW_CACHE_LL READ counter pair per CPU, period-scaled in the kernel;
 * architectural-events fallback under a vPMU — see ebpf_loader.c's
 * open_cache_counters). With a cgroup target the counters are opened
 * in cgroup mode (PERF_FLAG_PID_CGROUP) and the per-cgroup slot is
 * read; system-wide reads agg_global.
 */

#include <string.h>

#include "../../core/core.h"
#include "ebpf_loader.h"

static struct {
	int valid;
	int cgroup_mode;
} ls;

static struct intp_core_backend llcmr_ebpf_backend;

static int llcmr_ebpf_probe(void)
{
	if (intp_probe_ebpf() != INTP_PROBE_OK)
		return -1;

	const struct intp_core_target *t = intp_core_target_get();

	llcmr_ebpf_backend.scope = (t && t->cgroup_path) ? "cgroup" : "system";
	return 0;
}

static int llcmr_ebpf_init(void)
{
	if (intp_ebpf_acquire() != 0)
		return -1;
	if (!intp_ebpf_program_ok(INTP_EBPF_LLC)) {
		intp_ebpf_release();
		return -1;
	}
	ls.cgroup_mode = intp_ebpf_have_cgroup();
	ls.valid = 1;
	return 0;
}

static int llcmr_ebpf_read(struct intp_core_sample *out, double interval_sec)
{
	if (!ls.valid || interval_sec <= 0)
		return -1;

	struct intp_counters d_cg, d_g;

	if (intp_ebpf_read(INTP_EBPF_SHIM_LLCMR, interval_sec, &d_cg, &d_g) !=
	    0)
		return -1;

	const struct intp_counters *d = ls.cgroup_mode ? &d_cg : &d_g;

	/* v3.3 guard: safe_pct returns 0 when refs <= 0 and clamps to
	 * 0..100 — no extra conditions. */
	out->value =
		intp_ebpf_safe_pct((double)d->llc_misses, (double)d->llc_refs);
	out->status = INTP_CORE_OK;
	out->backend_id = "ebpf_perf";
	out->note = NULL;
	return 0;
}

static void llcmr_ebpf_cleanup(void)
{
	if (ls.valid) {
		ls.valid = 0;
		intp_ebpf_release();
	}
	memset(&ls, 0, sizeof(ls));
}

static struct intp_core_backend llcmr_ebpf_backend = {
	.metric = "llcmr",
	.id = "ebpf_perf",
	.description = "eBPF perf_event LLC references/misses "
		       "(cgroup-mode counters when targeted)",
	.model = "miss_per_reference",
	.scope = "system",
	.requires = "requires eBPF and PMU-backed perf "
		    "(kernel 5.8+, BTF, CAP_BPF+CAP_PERFMON or root)",
	.priority = INTP_PRIORITY_HIGH,
	.probe = llcmr_ebpf_probe,
	.init = llcmr_ebpf_init,
	.read = llcmr_ebpf_read,
	.cleanup = llcmr_ebpf_cleanup,
};

INTP_BACKEND_REGISTER(llcmr_ebpf_backend);
