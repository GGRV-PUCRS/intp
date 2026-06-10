/* SPDX-License-Identifier: MIT */
/*
 * cpu_ebpf.c — CPU utilization, eBPF backend shim.
 *
 * Ported from intp-comparison:
 *   variants/v3.3-ebpf-core-cgroup/src/intp_agg.c @ 2e87b30fc762
 * compute_sample()'s canonical cpu slice, verbatim:
 *
 *   cpu = cpu_on_ns_sum / (interval_ns * num_cores) * 100, clamped
 *
 * cpu_on_ns_sum is the on-CPU time closed at sched_switch, gated on
 * the outgoing task's cgroup identity (ancestor gate). Per-cgroup the
 * sum comes from the target's agg_per_cgroup slot, system-wide from
 * agg_global (where the gate passes everything). num_cores resolves
 * through detect_cached(), v3.3's caps.num_cores.
 */

#include <string.h>

#include "../../core/core.h"
#include "../../core/detect.h"
#include "ebpf_loader.h"

static struct {
	int valid;
	int cgroup_mode;
	int num_cores;
} cs;

static struct intp_core_backend cpu_ebpf_backend;

static int cpu_ebpf_probe(void)
{
	if (intp_probe_ebpf() != INTP_PROBE_OK)
		return -1;

	const struct intp_core_target *t = intp_core_target_get();

	cpu_ebpf_backend.scope = (t && t->cgroup_path) ? "cgroup" : "system";
	return 0;
}

static int cpu_ebpf_init(void)
{
	if (intp_ebpf_acquire() != 0)
		return -1;
	if (!intp_ebpf_program_ok(INTP_EBPF_CPU)) {
		intp_ebpf_release();
		return -1;
	}
	cs.cgroup_mode = intp_ebpf_have_cgroup();
	cs.num_cores = detect_cached()->num_cores;
	cs.valid = 1;
	return 0;
}

static int cpu_ebpf_read(struct intp_core_sample *out, double interval_sec)
{
	if (!cs.valid || interval_sec <= 0)
		return -1;

	struct intp_counters d_cg, d_g;

	if (intp_ebpf_read(INTP_EBPF_SHIM_CPU, interval_sec, &d_cg, &d_g) != 0)
		return -1;

	const struct intp_counters *d = cs.cgroup_mode ? &d_cg : &d_g;
	double interval_ns = interval_sec * 1e9;
	double cpu_ns_available =
		interval_ns * (cs.num_cores > 0 ? cs.num_cores : 1);

	out->value =
		intp_ebpf_safe_pct((double)d->cpu_on_ns_sum, cpu_ns_available);
	out->status = INTP_CORE_OK;
	out->backend_id = "ebpf_sched";
	out->note = NULL;
	return 0;
}

static void cpu_ebpf_cleanup(void)
{
	if (cs.valid) {
		cs.valid = 0;
		intp_ebpf_release();
	}
	memset(&cs, 0, sizeof(cs));
}

static struct intp_core_backend cpu_ebpf_backend = {
	.metric = "cpu",
	.id = "ebpf_sched",
	.description = "eBPF sched_switch on-CPU time, cgroup-identity gated",
	.model = "sched_oncpu_share",
	.scope = "system",
	.requires = "requires eBPF (kernel 5.8+, BTF, CAP_BPF or root)",
	.priority = INTP_PRIORITY_HIGH,
	.probe = cpu_ebpf_probe,
	.init = cpu_ebpf_init,
	.read = cpu_ebpf_read,
	.cleanup = cpu_ebpf_cleanup,
};

INTP_BACKEND_REGISTER(cpu_ebpf_backend);
