/* SPDX-License-Identifier: MIT */
/*
 * nets_ebpf.c — network stack (softirq) utilization, eBPF backend shim.
 *
 * Ported from across-intp:
 *   variants/v3.3-ebpf-core-cgroup/src/intp_agg.c @ 2e87b30fc762
 * compute_sample()'s nets slice, verbatim:
 *
 *   system-wide : nets_sys = (NET_TX + NET_RX softirq ns) / interval_ns
 *	as a clamped percent (the v3.2/v2.1-parity value).
 *   cgroup	 : softirq is NOT cgroup-keyable (v3.3 C2), so the
 *	per-cgroup nets is the explicit byte-share PROXY
 *	    nets_sys * bytes(cg) / bytes_total
 *	mixing byte LEVELS on purpose: bytes(cg) is the target's SOCKET
 *	bytes (cgroup_skb, agg_global.netp_*), bytes_total the host-wide
 *	DEVICE bytes (net_dev_xmit/netif_receive_skb). Falls back to 0
 *	when no device bytes were observed host-wide this interval.
 *	Surfaced as status PROXY, note "byteshare_proxy"; parity checks
 *	must exempt it (across-intp C2).
 */

#include <string.h>

#include "../../core/core.h"
#include "ebpf_loader.h"

static struct {
	int valid;
	int cgroup_mode;
} ns;

static struct intp_core_backend nets_ebpf_backend;

static int nets_ebpf_probe(void)
{
	if (intp_probe_ebpf() != INTP_PROBE_OK)
		return -1;

	const struct intp_core_target *t = intp_core_target_get();

	if (t && t->cgroup_path) {
		nets_ebpf_backend.scope = "cgroup";
		nets_ebpf_backend.model = "softirq_byteshare_proxy";
	} else {
		nets_ebpf_backend.scope = "system";
		nets_ebpf_backend.model = "softirq_time_share";
	}
	return 0;
}

static int nets_ebpf_init(void)
{
	if (intp_ebpf_acquire() != 0)
		return -1;

	ns.cgroup_mode = intp_ebpf_have_cgroup();

	/* The proxy numerator/denominator come from the cgroup_skb and
	 * device-tracepoint byte counters; without them the per-cgroup
	 * value would be a permanent fake 0, so require them too. */
	int bits = INTP_EBPF_NETS;

	if (ns.cgroup_mode)
		bits |= INTP_EBPF_NETP_CG | INTP_EBPF_NETP_DEV;

	if (!intp_ebpf_program_ok(bits)) {
		intp_ebpf_release();
		return -1;
	}
	ns.valid = 1;
	return 0;
}

static int nets_ebpf_read(struct intp_core_sample *out, double interval_sec)
{
	if (!ns.valid || interval_sec <= 0)
		return -1;

	struct intp_counters d_cg, d_g;

	if (intp_ebpf_read(INTP_EBPF_SHIM_NETS, interval_sec, &d_cg, &d_g) != 0)
		return -1;

	double interval_ns = interval_sec * 1e9;
	double net_lat_total =
		(double)(d_g.nets_tx_lat_ns_sum + d_g.nets_rx_lat_ns_sum);
	double nets_sys_pct = intp_ebpf_safe_pct(net_lat_total, interval_ns);

	out->backend_id = "ebpf_softirq";
	if (!ns.cgroup_mode) {
		out->value = nets_sys_pct;
		out->status = INTP_CORE_OK;
		out->note = NULL;
		return 0;
	}

	double bytes_cg = (double)(d_g.netp_tx_bytes + d_g.netp_rx_bytes);
	double bytes_total =
		(double)(d_g.netp_dev_tx_bytes + d_g.netp_dev_rx_bytes);
	double share = (bytes_total > 0.0) ? (bytes_cg / bytes_total) : 0.0;

	if (share < 0.0)
		share = 0.0;
	if (share > 1.0)
		share = 1.0;

	out->value = nets_sys_pct * share;
	out->status = INTP_CORE_PROXY;
	out->note = "byteshare_proxy";
	return 0;
}

static void nets_ebpf_cleanup(void)
{
	if (ns.valid) {
		ns.valid = 0;
		intp_ebpf_release();
	}
	memset(&ns, 0, sizeof(ns));
}

static struct intp_core_backend nets_ebpf_backend = {
	.metric = "nets",
	.id = "ebpf_softirq",
	.description = "eBPF softirq entry/exit NET_TX+NET_RX time "
		       "(per-cgroup: byte-share proxy)",
	.model = "softirq_time_share",
	.scope = "system",
	.requires = "requires eBPF (kernel 5.8+, BTF, CAP_BPF or root)",
	.priority = INTP_PRIORITY_HIGH,
	.lineage = INTP_LINEAGE_EBPF,
	.probe = nets_ebpf_probe,
	.init = nets_ebpf_init,
	.read = nets_ebpf_read,
	.cleanup = nets_ebpf_cleanup,
};

INTP_BACKEND_REGISTER(nets_ebpf_backend);
