/* SPDX-License-Identifier: MIT */
/*
 * ebpf_loader.h — refcounted singleton around the intp_agg BPF
 * skeleton shared by the five eBPF backend shims.
 *
 * Ported from across-intp:
 *   variants/v3.3-ebpf-core-cgroup/src/intp_agg.c @ 2e87b30fc762
 * (skeleton lifecycle, cgroup-identity resolution, perf-event LLC
 * counter attach, per-interval map snapshot + counters_diff), reshaped
 * from the standalone main() into an acquire/release singleton so the
 * five per-metric shims (ADR-0003) can share ONE loaded BPF object and
 * ONE map snapshot per dispatcher tick.
 *
 * Concurrency: file-scope singleton, same single-ctx-per-process rule
 * as every other backend (see core.h).
 */

#ifndef INTP_BACKEND_EBPF_LOADER_H
#define INTP_BACKEND_EBPF_LOADER_H

#include "intp_agg.bpf.h"

/* Per-program-group attach state, queryable via intp_ebpf_program_ok().
 * A bit is set only when every program the group needs attached. */
enum intp_ebpf_prog_bit {
	INTP_EBPF_NETP_CG = 1 << 0,  /* cgroup_skb egress+ingress	   */
	INTP_EBPF_NETP_DEV = 1 << 1, /* net_dev_xmit + netif_receive_skb */
	INTP_EBPF_NETS = 1 << 2,     /* softirq entry+exit		   */
	INTP_EBPF_BLK = 1 << 3,	     /* block_rq_issue + block_rq_complete */
	INTP_EBPF_CPU = 1 << 4,	     /* sched_switch			   */
	INTP_EBPF_LLC = 1 << 5,	     /* perf_event LLC refs+misses	   */
	INTP_EBPF_SCHEDLAT = 1 << 6, /* sched_wakeup{,_new} + tp_schedlat */
	INTP_EBPF_REGIME = 1 << 7,   /* tp_schedlat (psp + idle_preempt)  */
};

/* Shim identities for the once-per-tick snapshot consumer mask. */
enum intp_ebpf_shim {
	INTP_EBPF_SHIM_NETP = 0,
	INTP_EBPF_SHIM_NETS,
	INTP_EBPF_SHIM_BLK,
	INTP_EBPF_SHIM_LLCMR,
	INTP_EBPF_SHIM_CPU,
	/* system.portable.* consumers (ADR-0013) */
	INTP_EBPF_SHIM_SCHEDLAT,
	INTP_EBPF_SHIM_PSP,
	INTP_EBPF_SHIM_IDLE_PREEMPT,
	INTP_EBPF_SHIM_MEMBW_EST,
	INTP_EBPF_SHIM_COUNT
};

/* First acquire: open/load/attach the skeleton, resolve the cgroup
 * target, push struct intp_config, prime the snapshot baseline.
 * Subsequent acquires bump the refcount. Returns 0 on success. A
 * failed first acquire is cached: later acquires fail fast. */
int intp_ebpf_acquire(void);

/* Refcount down; on zero detach/destroy everything. Safe to call only
 * after a successful acquire (shims guard with their own flag). */
void intp_ebpf_release(void);

/* 1 if every program in the group attached (bit = INTP_EBPF_*). */
int intp_ebpf_program_ok(int bit);

/* 1 when the loader resolved a cgroup target (shims pick the d_cg /
 * d_global operand exactly as v3.3 compute_sample does). */
int intp_ebpf_have_cgroup(void);

/* 1 when the target is a PID set (-p / --pids / --comm) with no cgroup.
 * The BPF programs attribute per cgroup or system-wide only (v3.3 has no
 * per-PID gate), so in this mode an eBPF backend would silently widen the
 * scope to the whole host. Shims whose metric has a per-PID stable-ABI
 * backend step aside (defer) so the PID attribution is kept. */
int intp_ebpf_pid_target(void);

/* Snapshot accessor: the dispatcher calls the shims sequentially each
 * tick; the loader snapshots the maps ONCE per tick (re-snapshot when a
 * shim_id repeats — consumer mask) and serves every shim the same
 * counters_diff. For system-wide runs *d_cg is all-zero. Returns 0 on
 * success. */
int intp_ebpf_read(int shim_id, double interval_sec, struct intp_counters *d_cg,
		   struct intp_counters *d_global);

/* safe_pct — ported VERBATIM from v3.3 intp_agg.c (compute_sample's
 * clamp helper): percent of num/den, 0 when den <= 0, clamped 0..100. */
static inline double intp_ebpf_safe_pct(double num, double den)
{
	if (den <= 0.0)
		return 0.0;
	double p = num / den * 100.0;

	if (p < 0.0)
		p = 0.0;
	if (p > 100.0)
		p = 100.0;
	return p;
}

#endif /* INTP_BACKEND_EBPF_LOADER_H */
