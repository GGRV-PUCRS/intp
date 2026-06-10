/* SPDX-License-Identifier: MIT */
/*
 * netp_ebpf.c — network physical utilization, eBPF backend shim.
 *
 * Ported from intp-comparison:
 *   variants/v3.3-ebpf-core-cgroup/src/intp_agg.c @ 2e87b30fc762
 * compute_sample()'s canonical-netp slice (cgroup_skb bytes / VM tap
 * override) plus the device-level rate v3.3 computes as netp_dev,
 * which IS the system-wide canonical source here (v3.2 behaviour);
 * tap_total_bytes() ported verbatim. Formula and clamps preserved.
 *
 * Scope follows the target, like v3.3 itself:
 *   cgroup target : canonical bytes from the cgroup_skb counters.
 *	v3.3 reads them from agg_global — cgroup_skb is attached ONLY
 *	to the target cgroup, so agg_global.netp_* equals the target's
 *	bytes (the membw_est fidelity fix dropped the per-cgroup HASH
 *	update). Preserved as-is.
 *   VM target	   : host tap iface /sys/class/net/<tap>/statistics
 *	(cgroup_skb cannot see guest traffic).
 *   system-wide   : device-level bytes from net_dev_xmit /
 *	netif_receive_skb ('lo' excluded), v3.2 parity.
 *
 * Normalization: bytes/s over the host NIC line rate, resolved like
 * the stable netp backends (override > detection > assumed 1 Gbps,
 * DEGRADED when assumed).
 */

#include <stdio.h>
#include <string.h>

#include "../../core/core.h"
#include "../../core/detect.h"
#include "../netp/netp.h"
#include "ebpf_loader.h"

enum netp_mode { NETP_EBPF_SYS = 0, NETP_EBPF_CG, NETP_EBPF_VM };

static struct {
	int valid;
	enum netp_mode mode;
	long nic_speed_bps;
	int assumed_speed;
	char tap[64];
	unsigned long long tap_prev;
	int tap_ok;
} ne;

/* VM mode netp source — ported verbatim from v3.3 intp_agg.c (itself a
 * mirror of v2.1's b_tap path): read the host tap/vnet iface byte
 * counters from /sys/class/net/<tap>/statistics. */
static int tap_total_bytes(const char *tap, unsigned long long *out_total)
{
	char p[160];
	unsigned long long tx = 0, rx = 0;
	FILE *f;

	snprintf(p, sizeof(p), "/sys/class/net/%.63s/statistics/tx_bytes", tap);
	f = fopen(p, "r");
	if (!f)
		return -1;
	if (fscanf(f, "%llu", &tx) != 1) {
		fclose(f);
		return -1;
	}
	fclose(f);

	snprintf(p, sizeof(p), "/sys/class/net/%.63s/statistics/rx_bytes", tap);
	f = fopen(p, "r");
	if (!f)
		return -1;
	if (fscanf(f, "%llu", &rx) != 1) {
		fclose(f);
		return -1;
	}
	fclose(f);

	*out_total = tx + rx;
	return 0;
}

static struct intp_core_backend netp_ebpf_backend;

static int netp_ebpf_probe(void)
{
	if (intp_probe_ebpf() != INTP_PROBE_OK)
		return -1;

	/* The backend serves both scopes like v3.3; surface the one the
	 * current target selects. */
	const struct intp_core_target *t = intp_core_target_get();

	netp_ebpf_backend.scope = (t && t->cgroup_path) ? "cgroup" : "system";
	return 0;
}

static int netp_ebpf_init(void)
{
	if (intp_ebpf_acquire() != 0)
		return -1;

	const struct intp_core_target *t = intp_core_target_get();

	if (t && t->tap_iface && t->tap_iface[0]) {
		ne.mode = NETP_EBPF_VM;
		snprintf(ne.tap, sizeof(ne.tap), "%s", t->tap_iface);
	} else if (intp_ebpf_have_cgroup()) {
		ne.mode = NETP_EBPF_CG;
	} else {
		ne.mode = NETP_EBPF_SYS;
	}

	/* Each mode needs its program group attached (VM sources netp
	 * from sysfs, no BPF program of its own). */
	int bit = (ne.mode == NETP_EBPF_CG)    ? INTP_EBPF_NETP_CG
		  : (ne.mode == NETP_EBPF_SYS) ? INTP_EBPF_NETP_DEV
					       : 0;

	if (bit && !intp_ebpf_program_ok(bit)) {
		intp_ebpf_release();
		return -1;
	}

	/* Speed denominator: the explicit interface if pinned, else the
	 * host default iface — exactly like the stable netp backends
	 * (override > detection > assumed 1 Gbps). */
	const char *speed_iface = (t && t->iface && t->iface[0])
					  ? t->iface
					  : detect_default_iface();

	ne.nic_speed_bps = netp_resolve_speed(speed_iface, &ne.assumed_speed);

	if (ne.mode == NETP_EBPF_VM) {
		ne.tap_ok = (tap_total_bytes(ne.tap, &ne.tap_prev) == 0);
		if (!ne.tap_ok)
			fprintf(stderr,
				"warn: cannot read tap iface '%s' statistics "
				"-- VM netp will read 0\n",
				ne.tap);
	}

	ne.valid = 1;
	return 0;
}

static int netp_ebpf_read(struct intp_core_sample *out, double interval_sec)
{
	if (!ne.valid || interval_sec <= 0)
		return -1;

	struct intp_counters d_cg, d_g;

	if (intp_ebpf_read(INTP_EBPF_SHIM_NETP, interval_sec, &d_cg, &d_g) != 0)
		return -1;

	double v;
	const char *note = NULL;

	switch (ne.mode) {
	case NETP_EBPF_VM: {
		/* v3.3 main loop's tap path: delta over the interval,
		 * host-NIC normalized; an unreadable tap reads 0. */
		v = 0.0;
		unsigned long long tap_cur = ne.tap_prev;

		if (ne.tap_ok && tap_total_bytes(ne.tap, &tap_cur) == 0) {
			unsigned long long d = (tap_cur >= ne.tap_prev)
						       ? (tap_cur - ne.tap_prev)
						       : 0;
			v = intp_ebpf_safe_pct((double)d / interval_sec,
					       (double)ne.nic_speed_bps);
			ne.tap_prev = tap_cur;
		}
		note = "vm_tap";
		break;
	}
	case NETP_EBPF_CG: {
		/* Canonical per-cgroup bytes: cgroup_skb is attached
		 * ONLY to the target cgroup, so agg_global.netp_* is
		 * the target's bytes (v3.3 compute_sample). */
		double cg_bps =
			(double)(d_g.netp_tx_bytes + d_g.netp_rx_bytes) /
			interval_sec;
		v = intp_ebpf_safe_pct(cg_bps, (double)ne.nic_speed_bps);
		break;
	}
	case NETP_EBPF_SYS:
	default: {
		/* Device-level host-wide rate ('lo' excluded), what
		 * v3.3 computes as netp_dev and v3.2 reported as the
		 * canonical system-wide netp. */
		double dev_bps = (double)(d_g.netp_dev_tx_bytes +
					  d_g.netp_dev_rx_bytes) /
				 interval_sec;
		v = intp_ebpf_safe_pct(dev_bps, (double)ne.nic_speed_bps);
		break;
	}
	}

	out->value = v;
	out->backend_id = "ebpf_bytes";
	if (ne.assumed_speed) {
		out->status = INTP_CORE_DEGRADED;
		out->note = "assumed_1gbps";
	} else {
		out->status = INTP_CORE_OK;
		out->note = note;
	}
	return 0;
}

static void netp_ebpf_cleanup(void)
{
	if (ne.valid) {
		ne.valid = 0;
		intp_ebpf_release();
	}
	memset(&ne, 0, sizeof(ne));
}

static struct intp_core_backend netp_ebpf_backend = {
	.metric = "netp",
	.id = "ebpf_bytes",
	.description = "eBPF cgroup_skb / device tracepoint byte counters "
		       "(host NIC-speed normalized)",
	.model = "link_share_bytes",
	.scope = "system",
	.requires = "requires eBPF (kernel 5.8+, BTF, CAP_BPF or root)",
	.priority = INTP_PRIORITY_HIGH,
	.probe = netp_ebpf_probe,
	.init = netp_ebpf_init,
	.read = netp_ebpf_read,
	.cleanup = netp_ebpf_cleanup,
};

INTP_BACKEND_REGISTER(netp_ebpf_backend);
