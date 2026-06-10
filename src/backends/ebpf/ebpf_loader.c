/* SPDX-License-Identifier: MIT */
/*
 * ebpf_loader.c — refcounted singleton around the intp_agg BPF
 * skeleton shared by the five eBPF backend shims.
 *
 * Ported from intp-comparison:
 *   variants/v3.3-ebpf-core-cgroup/src/intp_agg.c @ 2e87b30fc762
 * The pieces of v3.3's main() live here as follows:
 *   - resolve_cgroup_identity, libbpf_quiet, open/close_cache_counters,
 *     accumulate_percpu, read_global_aggregate,
 *     read_per_cgroup_aggregate, counters_diff: ported VERBATIM.
 *   - skeleton open/load/config-push/attach + cgroup_skb cgroup-fd
 *     attach + perf LLC attach: reshaped from main()'s linear setup
 *     into intp_ebpf_acquire(), with PER-PROGRAM attach bookkeeping
 *     (v3.3 used the all-or-nothing skeleton attach; ADR-0003 wants a
 *     failing program to disable only its own shim, so programs are
 *     attached individually and tracked in a bitmask).
 *   - the main loop's per-interval "read maps, counters_diff" step:
 *     intp_ebpf_read() with a consumer mask so the maps are snapshot
 *     ONCE per dispatcher tick and all shims see the same diff.
 *
 * compute_sample() formula slices live in the per-metric shims.
 */

#include "ebpf_loader.h"

#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>

#include <linux/perf_event.h>

#include <bpf/bpf.h>
#include <bpf/libbpf.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../../core/core.h"
#include "../../core/detect.h"
#include "intp_agg.bpf.h"
#include "intp_agg.skel.h"

#define CGROUP_ROOT "/sys/fs/cgroup"

/* ---- cgroup identity (ported verbatim from v3.3 intp_agg.c) ------------- */

/* Resolve a cgroup directory path to its cgroup v2 id (== directory
 * inode) and its depth under /sys/fs/cgroup (for the ancestor gate).
 * Returns 0 on success. The kernfs id bpf_get_current_cgroup_id()
 * returns equals st_ino for cgroup v2 (v3.3 DESIGN §3). */
static int resolve_cgroup_identity(const char *path, __u64 *out_cgid,
				   __u32 *out_level)
{
	struct stat st;

	if (stat(path, &st) != 0)
		return -1;
	/* target_cgid comes from stat() on the (possibly non-canonical)
	 * input path: stat() already follows symlinks, so st_ino is the
	 * real cgroup v2 inode regardless of how the path was spelled. */
	*out_cgid = (__u64)st.st_ino;

	/* Canonicalize before counting components: realpath() resolves
	 * symlinks, trailing slashes, and . / .. so the ancestor LEVEL is
	 * counted against the true path (v3.3 FIX 4). */
	char resolved[PATH_MAX];
	const char *count_path;

	if (realpath(path, resolved) != NULL) {
		count_path = resolved;
		/* Verify the resolved path lives under the cgroup v2
		 * root. On a mismatch we cannot trust the ancestor
		 * depth, so fall back to exact-mode semantics. */
		const char *root = CGROUP_ROOT;
		size_t rlen = strlen(root);

		if (strncmp(resolved, root, rlen) != 0 ||
		    (resolved[rlen] != '\0' && resolved[rlen] != '/')) {
			fprintf(stderr,
				"warning: cgroup path '%s' resolves to '%s' "
				"which is not under "
				"%s -- using exact-match (leaf) semantics\n",
				path, resolved, root);
			*out_level = 0;
			return 0;
		}
	} else {
		/* realpath failed (e.g. EACCES on an intermediate
		 * component); count against the raw input. */
		count_path = path;
	}

	/* depth = number of path components strictly under
	 * /sys/fs/cgroup. bpf_get_current_ancestor_cgroup_id(level)
	 * walks to that depth. */
	const char *root = CGROUP_ROOT;
	size_t rlen = strlen(root);
	const char *p = count_path;

	if (strncmp(count_path, root, rlen) == 0)
		p = count_path + rlen;

	__u32 level = 0;

	while (*p) {
		while (*p == '/')
			p++;
		if (!*p)
			break;
		level++;
		while (*p && *p != '/')
			p++;
	}
	*out_level = level;
	return 0;
}

/* ---- perf_event for llcmr (ported verbatim from v3.3 intp_agg.c) -------- */

typedef struct {
	int *fds;
	int n_fds;
	struct bpf_link **links;
} perf_attach_t;

static long sys_perf_event_open(struct perf_event_attr *a, pid_t pid, int cpu,
				int group_fd, unsigned long flags)
{
	return syscall(__NR_perf_event_open, a, pid, cpu, group_fd, flags);
}

/* Open one HW_CACHE_LL counter per CPU and attach prog to each. When
 * cgroup_fd >= 0 the counter is opened in cgroup mode
 * (PERF_FLAG_PID_CGROUP, pid==cgroup_fd) so it only accrues while a
 * task of that cgroup is on-CPU — the per-cgroup llcmr attribution.
 * Otherwise it is opened system-wide (pid=-1). */
static int open_cache_counters(struct bpf_program *prog,
			       unsigned long cache_result, perf_attach_t *out,
			       int n_cpus, int cgroup_fd, int verbose)
{
	out->fds = calloc((size_t)n_cpus, sizeof(int));
	out->links = calloc((size_t)n_cpus, sizeof(struct bpf_link *));
	out->n_fds = 0;
	if (!out->fds || !out->links)
		return -1;

	struct perf_event_attr attr;

	memset(&attr, 0, sizeof(attr));
	attr.type = PERF_TYPE_HW_CACHE;
	attr.size = sizeof(attr);
	attr.config = (PERF_COUNT_HW_CACHE_LL) |
		      (PERF_COUNT_HW_CACHE_OP_READ << 8) | (cache_result << 16);
	/* Match V3: sample_period 1000 so even ~1k LLC events/sec
	 * trigger at least one BPF invocation per interval. The
	 * kernel-side increment scales by sample_period so the absolute
	 * count stays correct. */
	attr.sample_period = 1000;
	attr.wakeup_events = 1;
	attr.disabled = 0;

	pid_t pid_arg = cgroup_fd >= 0 ? (pid_t)cgroup_fd : -1;
	unsigned long flags = cgroup_fd >= 0 ? PERF_FLAG_PID_CGROUP : 0UL;

	/* vm-guest fallback (v3.3 C25/P5): the model-specific LL-read
	 * events are <not supported> under a KVM vPMU even with pmu=on,
	 * while the ARCHITECTURAL PERF_COUNT_HW_CACHE_REFERENCES/_MISSES
	 * are virtualized. Probe the LL event on cpu 0; if it cannot
	 * open, switch all CPUs to the architectural event (raise
	 * sample_period: it is much higher rate, and the BPF side scales
	 * by the period so counts stay correct). */
	{
		int probe =
			(int)sys_perf_event_open(&attr, pid_arg, 0, -1, flags);
		if (probe < 0) {
			attr.type = PERF_TYPE_HARDWARE;
			attr.config = (cache_result ==
				       PERF_COUNT_HW_CACHE_RESULT_MISS)
					      ? PERF_COUNT_HW_CACHE_MISSES
					      : PERF_COUNT_HW_CACHE_REFERENCES;
			attr.sample_period = 100000;
			if (verbose)
				fprintf(stderr,
					"warn: HW_CACHE LL perf unsupported "
					"(vPMU?); "
					"using architectural cache %s\n",
					cache_result == PERF_COUNT_HW_CACHE_RESULT_MISS
						? "misses"
						: "references");
		} else {
			close(probe);
		}
	}

	int opened = 0;

	for (int cpu = 0; cpu < n_cpus; cpu++) {
		int fd = (int)sys_perf_event_open(&attr, pid_arg, cpu, -1,
						  flags);
		if (fd < 0) {
			if (verbose)
				fprintf(stderr,
					"warn: perf_event_open on cpu %d "
					"failed: %s\n",
					cpu, strerror(errno));
			continue;
		}

		struct bpf_link *link =
			bpf_program__attach_perf_event(prog, fd);

		if (!link) {
			if (verbose)
				fprintf(stderr,
					"warn: attach perf_event on cpu %d "
					"failed: %s\n",
					cpu, strerror(errno));
			close(fd);
			continue;
		}
		out->fds[opened] = fd;
		out->links[opened] = link;
		opened++;
	}
	out->n_fds = opened;
	return opened > 0 ? 0 : -1;
}

static void close_cache_counters(perf_attach_t *p)
{
	if (!p)
		return;
	for (int i = 0; i < p->n_fds; i++) {
		if (p->links[i])
			bpf_link__destroy(p->links[i]);
		if (p->fds[i] >= 0)
			close(p->fds[i]);
	}
	free(p->fds);
	free(p->links);
	p->fds = NULL;
	p->links = NULL;
	p->n_fds = 0;
}

/* ---- aggregate snapshot (ported verbatim from v3.3 intp_agg.c) ---------- */

/* Sum a per-cgroup HASH value (one struct per CPU) into out. */
static void accumulate_percpu(struct intp_counters *out,
			      const struct intp_counters *per_cpu, int n_cpus)
{
	for (int i = 0; i < n_cpus; i++) {
		out->netp_tx_bytes += per_cpu[i].netp_tx_bytes;
		out->netp_rx_bytes += per_cpu[i].netp_rx_bytes;
		out->netp_dev_tx_bytes += per_cpu[i].netp_dev_tx_bytes;
		out->netp_dev_rx_bytes += per_cpu[i].netp_dev_rx_bytes;
		out->nets_tx_lat_ns_sum += per_cpu[i].nets_tx_lat_ns_sum;
		out->nets_tx_lat_n += per_cpu[i].nets_tx_lat_n;
		out->nets_rx_lat_ns_sum += per_cpu[i].nets_rx_lat_ns_sum;
		out->nets_rx_lat_n += per_cpu[i].nets_rx_lat_n;
		out->blk_svctm_ns_sum += per_cpu[i].blk_svctm_ns_sum;
		out->blk_ops += per_cpu[i].blk_ops;
		out->blk_bytes += per_cpu[i].blk_bytes;
		out->cpu_on_ns_sum += per_cpu[i].cpu_on_ns_sum;
		out->llc_refs += per_cpu[i].llc_refs;
		out->llc_misses += per_cpu[i].llc_misses;
		out->schedlat_wait_ns_sum += per_cpu[i].schedlat_wait_ns_sum;
		out->psp_count += per_cpu[i].psp_count;
		out->idle_preempt_count += per_cpu[i].idle_preempt_count;
	}
}

/* delta = cur - prev, field-by-field, saturating on underflow (can
 * happen if cur was read while a probe was mid-add on another CPU —
 * extremely rare in practice; saturate to 0 instead of wrapping). */
static void counters_diff(const struct intp_counters *cur,
			  const struct intp_counters *prev,
			  struct intp_counters *delta)
{
#define SUB(field)     \
	delta->field = \
		(cur->field >= prev->field) ? (cur->field - prev->field) : 0
	SUB(netp_tx_bytes);
	SUB(netp_rx_bytes);
	SUB(netp_dev_tx_bytes);
	SUB(netp_dev_rx_bytes);
	SUB(nets_tx_lat_ns_sum);
	SUB(nets_tx_lat_n);
	SUB(nets_rx_lat_ns_sum);
	SUB(nets_rx_lat_n);
	SUB(blk_svctm_ns_sum);
	SUB(blk_ops);
	SUB(blk_bytes);
	SUB(cpu_on_ns_sum);
	SUB(llc_refs);
	SUB(llc_misses);
	SUB(schedlat_wait_ns_sum);
	SUB(psp_count);
	SUB(idle_preempt_count);
#undef SUB
}

/* ---- singleton state ----------------------------------------------------- */

#define INTP_EBPF_MAX_LINKS 16

static struct {
	int refcnt;
	int failed; /* first acquire failed; fail fast afterwards */

	struct intp_agg_bpf *skel;
	struct intp_config cfg;
	int have_cgroup;
	int vm_mode;
	unsigned int attached; /* INTP_EBPF_* bitmask */

	int cgroup_fd;
	struct bpf_link *links[INTP_EBPF_MAX_LINKS];
	int n_links;
	struct bpf_link *skb_egress_link;
	struct bpf_link *skb_ingress_link;
	perf_attach_t perf_refs;
	perf_attach_t perf_miss;

	/* once-per-tick snapshot (consumer mask) */
	unsigned int consumed_mask;
	struct intp_counters prev_g, prev_c;
	struct intp_counters d_g, d_c;
} st = {.cgroup_fd = -1};

/* Read agg_global (PERCPU_ARRAY[key=0]) across all CPUs and sum into
 * out (ported verbatim, skel taken from the singleton). */
static int read_global_aggregate(struct intp_counters *out)
{
	int n_cpus = libbpf_num_possible_cpus();

	if (n_cpus <= 0)
		return -1;

	struct intp_counters *per_cpu =
		calloc((size_t)n_cpus, sizeof(*per_cpu));

	if (!per_cpu)
		return -1;

	__u32 key = 0;
	int fd = bpf_map__fd(st.skel->maps.agg_global);

	if (bpf_map_lookup_elem(fd, &key, per_cpu) != 0) {
		free(per_cpu);
		return -1;
	}

	memset(out, 0, sizeof(*out));
	accumulate_percpu(out, per_cpu, n_cpus);
	free(per_cpu);
	return 0;
}

/* Read agg_per_cgroup[cgid] (a plain HASH, single struct per key) into
 * out. Returns 0 on success, -1 if the slot does not yet exist (no
 * events charged to the target cgroup this run -> caller treats as
 * all-zero). */
static int read_per_cgroup_aggregate(__u64 cgid, struct intp_counters *out)
{
	memset(out, 0, sizeof(*out));

	int fd = bpf_map__fd(st.skel->maps.agg_per_cgroup);

	if (bpf_map_lookup_elem(fd, &cgid, out) != 0)
		return -1; /* not created yet; out already zeroed */
	return 0;
}

/* libbpf log filter (ported verbatim): only warnings reach stderr. */
static int libbpf_quiet(enum libbpf_print_level lvl, const char *fmt,
			va_list ap)
{
	if (lvl == LIBBPF_WARN)
		return vfprintf(stderr, fmt, ap);
	return 0;
}

/* ---- attach helpers ------------------------------------------------------ */

/* Attach one program, remembering the link for release. Returns 0 on
 * success. v3.3 used the all-or-nothing skeleton attach; attaching
 * individually lets a failing program disable only its shim
 * (ADR-0003) while a load/verifier failure still fails everything. */
static int attach_prog(struct bpf_program *prog)
{
	if (st.n_links >= INTP_EBPF_MAX_LINKS)
		return -1;

	struct bpf_link *link = bpf_program__attach(prog);

	if (!link)
		return -1;
	st.links[st.n_links++] = link;
	return 0;
}

static void teardown(void)
{
	close_cache_counters(&st.perf_refs);
	close_cache_counters(&st.perf_miss);
	if (st.skb_egress_link)
		bpf_link__destroy(st.skb_egress_link);
	if (st.skb_ingress_link)
		bpf_link__destroy(st.skb_ingress_link);
	st.skb_egress_link = NULL;
	st.skb_ingress_link = NULL;
	for (int i = 0; i < st.n_links; i++)
		bpf_link__destroy(st.links[i]);
	st.n_links = 0;
	if (st.cgroup_fd >= 0)
		close(st.cgroup_fd);
	st.cgroup_fd = -1;
	if (st.skel)
		intp_agg_bpf__destroy(st.skel);
	st.skel = NULL;
	st.attached = 0;
	st.have_cgroup = 0;
	st.vm_mode = 0;
	st.consumed_mask = 0;
	memset(&st.cfg, 0, sizeof(st.cfg));
	memset(&st.prev_g, 0, sizeof(st.prev_g));
	memset(&st.prev_c, 0, sizeof(st.prev_c));
	memset(&st.d_g, 0, sizeof(st.d_g));
	memset(&st.d_c, 0, sizeof(st.d_c));
}

/* ---- public API ----------------------------------------------------------- */

int intp_ebpf_acquire(void)
{
	if (st.refcnt > 0) {
		st.refcnt++;
		return 0;
	}
	if (st.failed)
		return -1; /* cached first failure: fail fast */

	libbpf_set_print(libbpf_quiet);

	const struct intp_core_target *t = intp_core_target_get();

	st.vm_mode = (t && t->tap_iface && t->tap_iface[0]) ? 1 : 0;

	/* ------- resolve cgroup identity (v3.3 DESIGN §3) ------- */
	memset(&st.cfg, 0, sizeof(st.cfg));
	st.have_cgroup = 0;
	if (t && t->cgroup_path) {
		if (resolve_cgroup_identity(t->cgroup_path, &st.cfg.target_cgid,
					    &st.cfg.target_level) != 0) {
			fprintf(stderr,
				"warning: cannot stat cgroup '%s' (%s) -- "
				"falling back to "
				"system-wide\n",
				t->cgroup_path, strerror(errno));
		} else {
			st.have_cgroup = 1;
			/* The CLI --exact flag arrives in a later
			 * phase; ancestor gate (v3.3 default). */
			st.cfg.exact_flag = 0;
		}
	}
	st.cfg.system_wide = st.have_cgroup ? 0 : 1;

	/* ------- open + load BPF skeleton ------- */
	st.skel = intp_agg_bpf__open();
	if (!st.skel)
		goto fail;

	if (intp_agg_bpf__load(st.skel))
		goto fail; /* verifier rejection lands here: all bits off */

	/* ------- push cgroup-identity config into intp_cfg_map ------- */
	unsigned int cfg_key = 0;

	if (bpf_map__update_elem(st.skel->maps.intp_cfg_map, &cfg_key,
				 sizeof(cfg_key), &st.cfg, sizeof(st.cfg),
				 BPF_ANY) != 0)
		goto fail;

	/* ------- attach tracepoints, per program group -------
	 * cgroup_skb ingress/egress (attached manually to the cgroup fd
	 * below) and the perf_event LLC programs (attached manually
	 * per-CPU via perf_event_open below) are never auto-attached.
	 * The schedlat/PSP programs (tp_sched_wakeup{,_new},
	 * tp_schedlat) and the writeback byte-volume top-up
	 * (tp_block_bio_complete) only feed counters no shim consumes
	 * yet (portable metrics / diag columns, later phases), so their
	 * attach is best-effort with no bit. */
	if (attach_prog(st.skel->progs.tp_net_dev_xmit) == 0 &&
	    attach_prog(st.skel->progs.tp_netif_receive_skb) == 0)
		st.attached |= INTP_EBPF_NETP_DEV;
	if (attach_prog(st.skel->progs.tp_softirq_entry) == 0 &&
	    attach_prog(st.skel->progs.tp_softirq_exit) == 0)
		st.attached |= INTP_EBPF_NETS;
	if (attach_prog(st.skel->progs.tp_block_rq_issue) == 0 &&
	    attach_prog(st.skel->progs.tp_block_rq_complete) == 0)
		st.attached |= INTP_EBPF_BLK;
	if (attach_prog(st.skel->progs.tp_sched_switch) == 0)
		st.attached |= INTP_EBPF_CPU;
	(void)attach_prog(st.skel->progs.tp_block_bio_complete);
	(void)attach_prog(st.skel->progs.tp_sched_wakeup);
	(void)attach_prog(st.skel->progs.tp_sched_wakeup_new);
	(void)attach_prog(st.skel->progs.tp_schedlat);

	/* ------- cgroup_skb ingress+egress on the target cgroup fd -------
	 * Canonical per-cgroup netp (v3.3 C1). Only in container mode
	 * (cgroup target, not VM mode): cgroup_skb cannot see guest
	 * traffic, so VM mode sources netp from the tap iface instead.
	 * System-wide: skipped — unattached, not fatal. */
	if (st.have_cgroup) {
		st.cgroup_fd = open(t->cgroup_path, O_RDONLY | O_DIRECTORY);
		if (st.cgroup_fd >= 0 && !st.vm_mode) {
			st.skb_egress_link = bpf_program__attach_cgroup(
				st.skel->progs.cg_skb_egress, st.cgroup_fd);
			st.skb_ingress_link = bpf_program__attach_cgroup(
				st.skel->progs.cg_skb_ingress, st.cgroup_fd);
			if (st.skb_egress_link && st.skb_ingress_link)
				st.attached |= INTP_EBPF_NETP_CG;
		}
	}

	/* ------- perf_event programs for llcmr (cgroup mode when
	 * targeted). Failure marks only the LLC bit. ------- */
	{
		int n_cpus = detect_num_cores();
		int perf_cgroup_fd = (st.have_cgroup && st.cgroup_fd >= 0)
					     ? st.cgroup_fd
					     : -1;
		int refs_ok =
			open_cache_counters(st.skel->progs.perf_llc_refs,
					    PERF_COUNT_HW_CACHE_RESULT_ACCESS,
					    &st.perf_refs, n_cpus,
					    perf_cgroup_fd, 0) == 0;
		int miss_ok =
			open_cache_counters(st.skel->progs.perf_llc_misses,
					    PERF_COUNT_HW_CACHE_RESULT_MISS,
					    &st.perf_miss, n_cpus,
					    perf_cgroup_fd, 0) == 0;
		if (refs_ok && miss_ok) {
			st.attached |= INTP_EBPF_LLC;
		} else {
			/* miss ratio needs BOTH counters; a half-open
			 * pair would read a fake 0 or 100. */
			close_cache_counters(&st.perf_refs);
			close_cache_counters(&st.perf_miss);
		}
	}

	/* ------- prime the snapshot baseline (v3.3 pre-loop reads) ------- */
	if (read_global_aggregate(&st.prev_g) != 0)
		goto fail;
	memset(&st.prev_c, 0, sizeof(st.prev_c));
	if (st.have_cgroup)
		read_per_cgroup_aggregate(st.cfg.target_cgid, &st.prev_c);

	st.consumed_mask = 0;
	st.refcnt = 1;
	return 0;

fail:
	teardown();
	st.failed = 1;
	return -1;
}

void intp_ebpf_release(void)
{
	if (st.refcnt <= 0)
		return;
	if (--st.refcnt > 0)
		return;
	teardown();
}

int intp_ebpf_program_ok(int bit)
{
	return st.refcnt > 0 &&
	       (st.attached & (unsigned int)bit) == (unsigned int)bit;
}

int intp_ebpf_have_cgroup(void)
{
	return st.refcnt > 0 && st.have_cgroup;
}

/* Snapshot the maps and diff against the previous tick (the per-tick
 * body of v3.3's main loop). */
static int snapshot(void)
{
	struct intp_counters cur_g, cur_c;

	if (read_global_aggregate(&cur_g) != 0)
		return -1;
	memset(&cur_c, 0, sizeof(cur_c));
	if (st.have_cgroup)
		read_per_cgroup_aggregate(st.cfg.target_cgid, &cur_c);

	counters_diff(&cur_g, &st.prev_g, &st.d_g);
	counters_diff(&cur_c, &st.prev_c, &st.d_c);
	st.prev_g = cur_g;
	st.prev_c = cur_c;
	return 0;
}

int intp_ebpf_read(int shim_id, double interval_sec, struct intp_counters *d_cg,
		   struct intp_counters *d_global)
{
	(void)interval_sec; /* shims apply it in their formula slices */

	if (st.refcnt <= 0 || !st.skel)
		return -1;
	if (shim_id < 0 || shim_id >= INTP_EBPF_SHIM_COUNT)
		return -1;

	/* Consumer mask: the dispatcher calls the shims sequentially
	 * once per tick, so a repeated shim_id means a new tick has
	 * started — re-snapshot; every shim within one tick gets the
	 * SAME diff. */
	unsigned int bit = 1u << shim_id;

	if (st.consumed_mask == 0 || (st.consumed_mask & bit)) {
		if (snapshot() != 0)
			return -1;
		st.consumed_mask = 0;
	}
	st.consumed_mask |= bit;

	if (d_cg)
		*d_cg = st.d_c;
	if (d_global)
		*d_global = st.d_g;
	return 0;
}
