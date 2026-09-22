/* SPDX-License-Identifier: MIT */
/*
 * blk_ebpf.c — block I/O utilization, eBPF backend shim.
 *
 * Ported from across-intp:
 *   variants/v3.3-ebpf-core-cgroup/src/intp_agg.c @ 2e87b30fc762
 * compute_sample()'s canonical blk slice, verbatim:
 *
 *   blk = svctm_ns_sum / interval_ns * 100, clamped 0..100
 *
 * where svctm = block_rq_complete - block_rq_issue per request (v3.3
 * C7: the canonical unit stays %-of-interval service time — the
 * project's declared canonical blk model; io_ticks is the stable
 * backends' declared degraded model). Per-cgroup the sum comes from
 * the target's agg_per_cgroup slot (current-task gate at completion),
 * system-wide from agg_global. The diagnostic blk_MBps volume column
 * is NOT ported (diagnostics land in a later phase).
 */

#include <string.h>

#include "../../core/core.h"
#include "ebpf_loader.h"

static struct {
	int valid;
	int cgroup_mode;
} bs;

static struct intp_core_backend blk_ebpf_backend;

static int blk_ebpf_probe(void)
{
	if (intp_probe_ebpf() != INTP_PROBE_OK)
		return -1;

	const struct intp_core_target *t = intp_core_target_get();

	blk_ebpf_backend.scope = (t && t->cgroup_path) ? "cgroup" : "system";
	return 0;
}

static int blk_ebpf_init(void)
{
	if (intp_ebpf_acquire() != 0)
		return -1;
	if (!intp_ebpf_program_ok(INTP_EBPF_BLK)) {
		intp_ebpf_release();
		return -1;
	}
	bs.cgroup_mode = intp_ebpf_have_cgroup();
	bs.valid = 1;
	return 0;
}

static int blk_ebpf_read(struct intp_core_sample *out, double interval_sec)
{
	if (!bs.valid || interval_sec <= 0)
		return -1;

	struct intp_counters d_cg, d_g;

	if (intp_ebpf_read(INTP_EBPF_SHIM_BLK, interval_sec, &d_cg, &d_g) != 0)
		return -1;

	/* v3.3: per-cgroup delta with a target, host-wide otherwise
	 * (agg_global accumulates svctm unconditionally). */
	const struct intp_counters *d = bs.cgroup_mode ? &d_cg : &d_g;
	double interval_ns = interval_sec * 1e9;

	out->value =
		intp_ebpf_safe_pct((double)d->blk_svctm_ns_sum, interval_ns);
	out->status = INTP_CORE_OK;
	out->backend_id = "ebpf_svctm";
	out->note = NULL;
	return 0;
}

static void blk_ebpf_cleanup(void)
{
	if (bs.valid) {
		bs.valid = 0;
		intp_ebpf_release();
	}
	memset(&bs, 0, sizeof(bs));
}

static struct intp_core_backend blk_ebpf_backend = {
	.metric = "blk",
	.id = "ebpf_svctm",
	.description = "eBPF block_rq_issue/complete service time "
		       "(%-of-interval svctm)",
	.model = "svctm_busy_fraction",
	.scope = "system",
	.requires = "requires eBPF (kernel 5.8+, BTF, CAP_BPF or root)",
	.priority = INTP_PRIORITY_HIGH,
	.probe = blk_ebpf_probe,
	.init = blk_ebpf_init,
	.read = blk_ebpf_read,
	.cleanup = blk_ebpf_cleanup,
};

INTP_BACKEND_REGISTER(blk_ebpf_backend);
