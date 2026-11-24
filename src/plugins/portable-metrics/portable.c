/* SPDX-License-Identifier: MIT */
/*
 * portable.c — VM-portable interference metrics (system.portable.*).
 *
 * Ported from across-intp:
 *   variants/v2.1-c-abi-cgroup/src/portable.c @ a214643cb1be (v0.2.1)
 * Formulas, fallback chains, and backend order preserved verbatim,
 * including the per-thread schedlat/psp accounting of v0.2.1 (C38);
 * the per-metric accessor singletons and the registry helpers
 * (metric_select_backend / metric_init / metric_read / metric_cleanup
 * from variants/v2.1-c-abi-cgroup/src/backend_registry.c) are folded
 * into the small provider API of portable.h instead of the core
 * "intp_backends" linker section — the canonical seven-metric path is
 * byte-identical whether or not the provider is enabled (ADR-0009),
 * and the whole set is gated behind --experimental (ADR-0008).
 *
 * On top of the v2.1 chains, ADR-0013 adds eBPF backends that read the
 * counters v3.3 derives these metrics from (schedlat, psp, idle_preempt,
 * membw_est), so one binary serves both variants; --engine restricts the
 * chains to one lineage.
 *
 * Why these: inside a stock KVM guest the RDT metrics (mbw/llcocc) are
 * structurally absent (resctrl is host-only) and the LL-read PMU events
 * that back llcmr are <not supported> by the vPMU. The six portable
 * metrics here are computed by the GUEST'S OWN kernel and therefore
 * survive in-guest; psp and idle_preempt are the scheduling-regime pair:
 *
 *   schedlat   run-queue / scheduling latency — eBPF
 *              sched_wakeup->sched_switch wait, or per-thread
 *              /proc/<tid>/schedstat run-delay, with PSI cpu.pressure
 *              'some' as the last fallback, %-of-interval (run-queue
 *              wait, as Volpert's PSL; normalized by interval x CPUs
 *              instead of per process).
 *   psi_mem    PSI memory.pressure 'some' — %-of-interval
 *              (memory-CAPACITY contention; bandwidth is membw_est).
 *   membw_est  DRAM-bandwidth estimate — LLC misses * 64B cacheline /
 *              interval (MB/s); architectural cache-misses event is
 *              virtualized in-guest.
 *   psi_io     PSI io.pressure 'some' — %-of-interval.
 *   schedthr   CFS throttling — cpu.stat throttled_usec,
 *              %-of-interval; cgroup-only (no system analogue). A
 *              confound guard, not a contention signal: it plays the
 *              throttling-indicator role of Volpert's PSP. throttled_usec
 *              exists whenever the cpu controller is enabled and reads 0
 *              without a cpu.max limit; it is NOT hierarchical — only
 *              this cgroup's own limit counts
 *              (Documentation/admin-guide/cgroup-v2.rst, cpu.stat).
 *   steal      hypervisor-stolen vCPU time — /proc/stat field 8,
 *              %-of-total; VM-global, host CPU contention only.
 *   psp        involuntary preemptions/s of the target's threads (NOT
 *              Volpert's PSP, which counts switches to PID 0).
 *   idle_preempt  idle-CPU takeover rate — eBPF-only; without eBPF the
 *              metric reads unavailable rather than a fake 0.
 *
 * A read reports INTP_CORE_UNAVAILABLE where the source is absent
 * (CONFIG_PSI=n, cpu controller not enabled for the cgroup, no cgroup
 * target) rather than a fake 0; schedthr without a cpu.max limit
 * reads 0.
 */

#include "portable.h"

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../../backends/common/perfev.h"
#include "../../backends/common/procutil.h"
#include "../../core/detect.h"

#if INTP_HAVE_EBPF
#include "../../backends/ebpf/ebpf_loader.h"
#endif

_Static_assert(INTP_PORTABLE_COUNT == INTP_CORE_PORTABLE_COUNT,
	       "core schema sizing must match the provider count");

/* ---- local backend/metric model (ported v2.1 backend_t/metric_t) -------- */

#define PORTABLE_MAX_BACKENDS_PER_METRIC 4

struct portable_backend {
	const char *backend_id;
	const char *description;
	unsigned int lineage; /* enum intp_core_lineage; 0 = stable */
	int (*probe)(void);
	int (*init)(void);
	int (*read)(struct intp_core_sample *out, double interval_sec);
	void (*cleanup)(void);
};

struct portable_metric {
	const char *metric_name;
	struct portable_backend *backends[PORTABLE_MAX_BACKENDS_PER_METRIC];
	int n_backends;
	struct portable_backend *active; /* selected at probe time */
	struct portable_backend *forced; /* --force-backend, or NULL */
};

/* ------------------------------------------------------------------ shared
 * file readers */

/* PSI 'some' cumulative stall time (microseconds) from the total= field. */
static int psi_some_total_us(const char *path, unsigned long long *out)
{
	FILE *f = fopen(path, "r");

	if (!f)
		return -1;
	char line[256];
	int rc = -1;

	while (fgets(line, sizeof(line), f)) {
		if (strncmp(line, "some", 4) != 0)
			continue;
		char *p = strstr(line, "total=");

		if (p && sscanf(p + 6, "%llu", out) == 1)
			rc = 0;
		break;
	}
	fclose(f);
	return rc;
}

/* CFS-bandwidth throttled time (microseconds) from a cgroup cpu.stat. */
static int cpu_stat_throttled_us(const char *path, unsigned long long *out)
{
	FILE *f = fopen(path, "r");

	if (!f)
		return -1;
	char line[128];
	int rc = -1;

	while (fgets(line, sizeof(line), f)) {
		if (sscanf(line, "throttled_usec %llu", out) == 1) {
			rc = 0;
			break;
		}
	}
	fclose(f);
	return rc;
}

/* /proc/stat aggregate cpu line: *steal = field 8 (jiffies), *total = sum. */
static int proc_stat_steal(unsigned long long *steal, unsigned long long *total)
{
	FILE *f = fopen("/proc/stat", "r");

	if (!f)
		return -1;
	char line[512];
	int rc = -1;

	if (fgets(line, sizeof(line), f) && strncmp(line, "cpu ", 4) == 0) {
		unsigned long long v[10] = {0};
		int n = sscanf(line + 4,
			       "%llu %llu %llu %llu %llu %llu %llu %llu %llu "
			       "%llu",
			       &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6],
			       &v[7], &v[8], &v[9]);

		if (n >= 8) {
			unsigned long long t = 0;

			for (int i = 0; i < n; i++)
				t += v[i];
			*steal = v[7]; /* user nice system idle iowait irq
					* softirq STEAL */
			*total = t;
			rc = 0;
		}
	}
	fclose(f);
	return rc;
}

/* ------------------------------------------------------------------ per-TID
 * accounting (ported from across-intp v0.2.1, C38) */

/* /proc/<pid>/schedstat and the *_ctxt_switches lines of /proc/<pid>/status
 * describe ONE task_struct, not the thread group (fs/proc/base.c
 * proc_pid_schedstat prints task->sched_info.run_delay; fs/proc/array.c
 * task_context_switch_counts prints p->nvcsw/p->nivcsw). Reading them per
 * TGID counts only each process's leader thread, so a multi-threaded
 * workload whose leader sleeps (JVM, memcached, nginx workers) read ~0.
 * schedlat and psp therefore enumerate every TID of the target and keep a
 * per-TID baseline:
 *
 *   - known TID:  delta = cur - prev, clamped at 0 per TID (TID reuse);
 *   - new TID:    delta = cur. The thread was born inside the interval, so
 *                 its whole counter belongs to it. The map is seeded at
 *                 init(), so this only applies to threads born mid-run;
 *   - exited TID: drops out of the map; its last partial interval is lost.
 *
 * Per-TID baselines replace the old delta over a SUM of counters, where one
 * exiting task made the sum drop and the whole interval read 0. */

#define PORTABLE_MAX_TIDS 16384u /* cap per scan; DEGRADED "tid_cap" past it */
#define TIDMAP_SLOTS	  32768u /* power of two, >= 2 x PORTABLE_MAX_TIDS */

struct tid_ent {
	pid_t tid; /* 0 = empty slot */
	unsigned long long val;
};

typedef int (*tid_reader_fn)(pid_t tid, unsigned long long *out);

struct tidacc {
	struct tid_ent *prev; /* last interval's baselines */
	struct tid_ent *cur;  /* scratch, swapped with prev after each scan */
	int valid;
	int capped; /* last collection hit PORTABLE_MAX_TIDS */
};

static pid_t tid_buf[PORTABLE_MAX_TIDS];

/* Collect the target's current TIDs: re-scan cgroup.threads over the cgroup
 * subtree each call when a cgroup is targeted (recursive — catches workers
 * forked after start AND payloads that live in CHILD cgroups, as incus/lxc
 * and nested k8s pods place them under the cgroup v2 "no internal
 * processes" rule), else /proc/<pid>/task of each static --pids PID.
 * *capped is set when the PORTABLE_MAX_TIDS cap was reached. */
static int collect_target_tids(pid_t *out, int max, int *capped)
{
	const struct intp_core_target *t = intp_core_target_get();
	int n = 0;

	*capped = 0;
	if (!t)
		return 0;
	if (t->cgroup_path) {
		n = procutil_read_cgroup_threads_rec(t->cgroup_path, out,
						     (size_t)max);
	} else {
		for (int i = 0; i < t->n_pids && n < max; i++)
			n += procutil_read_proc_tasks(t->pids[i], out + n,
						      (size_t)(max - n));
	}
	if (n >= max)
		*capped = 1;
	return n;
}

static unsigned tid_hash(pid_t tid)
{
	return ((unsigned)tid * 2654435761u) & (TIDMAP_SLOTS - 1u);
}

static const struct tid_ent *tidmap_find(const struct tid_ent *map, pid_t tid)
{
	for (unsigned h = tid_hash(tid), i = 0; i < TIDMAP_SLOTS;
	     i++, h = (h + 1u) & (TIDMAP_SLOTS - 1u)) {
		if (map[h].tid == tid)
			return &map[h];
		if (map[h].tid == 0)
			return NULL;
	}
	return NULL;
}

static void tidmap_put(struct tid_ent *map, pid_t tid, unsigned long long val)
{
	for (unsigned h = tid_hash(tid), i = 0; i < TIDMAP_SLOTS;
	     i++, h = (h + 1u) & (TIDMAP_SLOTS - 1u)) {
		if (map[h].tid == 0 || map[h].tid == tid) {
			map[h].tid = tid;
			map[h].val = val;
			return;
		}
	}
}

/* Read every target TID into a->cur and, when `delta` is non-NULL, sum the
 * per-TID deltas against a->prev. Returns -1 when no TID was readable. */
static int tidacc_scan(struct tidacc *a, tid_reader_fn rd,
		       unsigned long long *delta)
{
	int n = collect_target_tids(tid_buf, (int)PORTABLE_MAX_TIDS,
				    &a->capped);

	if (n <= 0)
		return -1;
	memset(a->cur, 0, TIDMAP_SLOTS * sizeof(struct tid_ent));

	unsigned long long sum = 0;
	int any = 0;

	for (int i = 0; i < n; i++) {
		unsigned long long v;

		if (rd(tid_buf[i], &v) != 0)
			continue; /* exited mid-scan */
		any = 1;
		tidmap_put(a->cur, tid_buf[i], v);
		if (!delta)
			continue;

		const struct tid_ent *p = tidmap_find(a->prev, tid_buf[i]);

		if (!p)
			sum += v; /* born this interval */
		else if (v >= p->val)
			sum += v - p->val;
	}
	if (!any)
		return -1;

	struct tid_ent *tmp = a->prev;

	a->prev = a->cur;
	a->cur = tmp;
	if (delta)
		*delta = sum;
	return 0;
}

static void tidacc_free(struct tidacc *a)
{
	free(a->prev);
	free(a->cur);
	memset(a, 0, sizeof(*a));
}

/* Allocate the maps and seed the baselines with the TIDs present at init. */
static int tidacc_init(struct tidacc *a, tid_reader_fn rd)
{
	tidacc_free(a);
	a->prev = calloc(TIDMAP_SLOTS, sizeof(struct tid_ent));
	a->cur = calloc(TIDMAP_SLOTS, sizeof(struct tid_ent));
	if (!a->prev || !a->cur || tidacc_scan(a, rd, NULL) != 0) {
		tidacc_free(a);
		return -1;
	}
	a->valid = 1;
	return 0;
}

/* Field 2 of /proc/<tid>/task/<tid>/schedstat: this thread's cumulative
 * run-queue wait (ns). The /proc/<tid>/task/<tid> form resolves for any
 * TID, leader or not, without knowing its TGID. */
static int read_tid_runqdelay(pid_t tid, unsigned long long *out)
{
	char path[80];

	snprintf(path, sizeof(path), "/proc/%d/task/%d/schedstat", (int)tid,
		 (int)tid);

	FILE *f = fopen(path, "r");

	if (!f)
		return -1;

	unsigned long long run = 0, wait = 0;
	int rc = (fscanf(f, "%llu %llu", &run, &wait) == 2) ? 0 : -1;

	fclose(f);
	if (rc == 0)
		*out = wait;
	return rc;
}

/* nonvoluntary_ctxt_switches of /proc/<tid>/task/<tid>/status. */
static int read_tid_nonvol(pid_t tid, unsigned long long *out)
{
	char path[80];

	snprintf(path, sizeof(path), "/proc/%d/task/%d/status", (int)tid,
		 (int)tid);

	FILE *f = fopen(path, "r");

	if (!f)
		return -1;

	char line[256];
	int rc = -1;

	while (fgets(line, sizeof(line), f)) {
		if (sscanf(line, "nonvoluntary_ctxt_switches: %llu", out) ==
		    1) {
			rc = 0;
			break;
		}
	}
	fclose(f);
	return rc;
}

static double clamp_pct(double v)
{
	if (v < 0.0)
		v = 0.0;
	if (v > 100.0)
		v = 100.0;
	return v;
}

static long online_cpus(void)
{
	long n = sysconf(_SC_NPROCESSORS_ONLN);

	return n < 1 ? 1 : n;
}

/* ==================================================================
 * eBPF-sourced backends (ADR-0013)
 *
 * The loader already attaches v3.3's tp_sched_wakeup{,_new} and
 * tp_schedlat programs and sums their per-cgroup / host-wide counters
 * (schedlat_wait_ns_sum, psp_count, idle_preempt_count) together with the
 * perf LLC miss counter (llc_misses). These backends read them exactly as
 * v3.3's intp_agg.c main loop does under --portable-metrics:
 *
 *   schedlat     = wait_ns / (interval_ns x num_cores) x 100
 *   psp          = psp_count / interval           (events/s)
 *   idle_preempt = idle_preempt_count / interval  (events/s)
 *   membw_est    = llc_misses x 64 B / interval   (MB/s)
 *
 * with the cgroup slot when a cgroup is targeted and the global slot
 * otherwise. Under the AUTO engine they decline a per-PID target (the
 * BPF gate has no per-PID scope), so the per-thread stable-ABI backends
 * keep it.
 * ================================================================== */

#if INTP_HAVE_EBPF

struct pebpf_state {
	int valid;
	int cgroup_mode;
	int num_cores;
};

static int pebpf_probe(void)
{
	if (intp_probe_ebpf() != INTP_PROBE_OK)
		return -1;
	if (intp_core_engine_get() == INTP_ENGINE_AUTO &&
	    intp_ebpf_pid_target())
		return -1;
	return 0;
}

static int pebpf_init(struct pebpf_state *ps, int prog_bit)
{
	if (intp_ebpf_acquire() != 0)
		return -1;
	if (!intp_ebpf_program_ok(prog_bit)) {
		intp_ebpf_release();
		return -1;
	}
	ps->cgroup_mode = intp_ebpf_have_cgroup();
	ps->num_cores = detect_cached()->num_cores;
	ps->valid = 1;
	return 0;
}

static void pebpf_cleanup(struct pebpf_state *ps)
{
	if (ps->valid)
		intp_ebpf_release();
	memset(ps, 0, sizeof(*ps));
}

/* This tick's counter delta for the target scope, or NULL. */
static const struct intp_counters *pebpf_delta(struct pebpf_state *ps, int shim,
					       double iv,
					       struct intp_counters *d_cg,
					       struct intp_counters *d_g)
{
	if (!ps->valid || iv <= 0.0)
		return NULL;
	if (intp_ebpf_read(shim, iv, d_cg, d_g) != 0)
		return NULL;
	return ps->cgroup_mode ? d_cg : d_g;
}

static struct pebpf_state pe_schedlat, pe_psp, pe_idle, pe_membw;

static int ebpf_schedlat_init(void)
{
	return pebpf_init(&pe_schedlat, INTP_EBPF_SCHEDLAT);
}
static int ebpf_schedlat_read(struct intp_core_sample *out, double iv)
{
	struct intp_counters d_cg, d_g;
	const struct intp_counters *d =
		pebpf_delta(&pe_schedlat, INTP_EBPF_SHIM_SCHEDLAT, iv, &d_cg,
			    &d_g);

	if (!d)
		return -1;

	double avail_ns =
		iv * 1.0e9 *
		(pe_schedlat.num_cores > 0 ? pe_schedlat.num_cores : 1);

	out->value =
		intp_ebpf_safe_pct((double)d->schedlat_wait_ns_sum, avail_ns);
	out->status = INTP_CORE_OK;
	out->backend_id = "ebpf_schedlat";
	out->note = NULL;
	return 0;
}
static void ebpf_schedlat_cleanup(void)
{
	pebpf_cleanup(&pe_schedlat);
}

static int ebpf_psp_init(void)
{
	return pebpf_init(&pe_psp, INTP_EBPF_REGIME);
}
static int ebpf_psp_read(struct intp_core_sample *out, double iv)
{
	struct intp_counters d_cg, d_g;
	const struct intp_counters *d =
		pebpf_delta(&pe_psp, INTP_EBPF_SHIM_PSP, iv, &d_cg, &d_g);

	if (!d)
		return -1;
	out->value = (double)d->psp_count / iv;
	out->status = INTP_CORE_OK;
	out->backend_id = "ebpf_psp";
	out->note = NULL;
	return 0;
}
static void ebpf_psp_cleanup(void)
{
	pebpf_cleanup(&pe_psp);
}

static int ebpf_idle_init(void)
{
	return pebpf_init(&pe_idle, INTP_EBPF_REGIME);
}
static int ebpf_idle_read(struct intp_core_sample *out, double iv)
{
	struct intp_counters d_cg, d_g;
	const struct intp_counters *d =
		pebpf_delta(&pe_idle, INTP_EBPF_SHIM_IDLE_PREEMPT, iv, &d_cg,
			    &d_g);

	if (!d)
		return -1;
	out->value = (double)d->idle_preempt_count / iv;
	out->status = INTP_CORE_OK;
	out->backend_id = "ebpf_idle_preempt";
	out->note = NULL;
	return 0;
}
static void ebpf_idle_cleanup(void)
{
	pebpf_cleanup(&pe_idle);
}

/* membw_est from the loader's LLC miss counter (v3.3). In the AUTO chain
 * it comes AFTER the stable-ABI perf backends: on network-heavy targets
 * the per-packet eBPF network hooks run in the target cgroup and their own
 * cache misses are charged to this counter (across-intp C31; JSA section
 * 6.2.3 measured a 6-7x rise over the C reading). */
static int ebpf_membw_init(void)
{
	return pebpf_init(&pe_membw, INTP_EBPF_LLC);
}
static int ebpf_membw_read(struct intp_core_sample *out, double iv)
{
	struct intp_counters d_cg, d_g;
	const struct intp_counters *d =
		pebpf_delta(&pe_membw, INTP_EBPF_SHIM_MEMBW_EST, iv, &d_cg,
			    &d_g);

	if (!d)
		return -1;
	out->value = ((double)d->llc_misses * 64.0 / iv) / 1.0e6; /* MB/s */
	out->status = INTP_CORE_OK;
	out->backend_id = "ebpf_llc_misses";
	out->note = NULL;
	return 0;
}
static void ebpf_membw_cleanup(void)
{
	pebpf_cleanup(&pe_membw);
}

#define PEBPF_BACKEND(name, bid, desc, initf, readf, cleanupf) \
	static struct portable_backend name = {                \
		.backend_id = bid,                             \
		.description = desc,                           \
		.lineage = INTP_LINEAGE_EBPF,                  \
		.probe = pebpf_probe,                          \
		.init = initf,                                 \
		.read = readf,                                 \
		.cleanup = cleanupf,                           \
	}

PEBPF_BACKEND(schedlat_ebpf, "ebpf_schedlat",
	      "eBPF sched_wakeup->sched_switch run-queue wait (v3.3)",
	      ebpf_schedlat_init, ebpf_schedlat_read, ebpf_schedlat_cleanup);
PEBPF_BACKEND(psp_ebpf, "ebpf_psp",
	      "eBPF sched_switch, prev still RUNNABLE (v3.3), ev/s",
	      ebpf_psp_init, ebpf_psp_read, ebpf_psp_cleanup);
PEBPF_BACKEND(idle_preempt_ebpf, "ebpf_idle_preempt",
	      "eBPF sched_switch, prev = idle task (v3.3), ev/s",
	      ebpf_idle_init, ebpf_idle_read, ebpf_idle_cleanup);
PEBPF_BACKEND(membw_est_ebpf, "ebpf_llc_misses",
	      "eBPF perf LLC misses * 64B / interval (v3.3)", ebpf_membw_init,
	      ebpf_membw_read, ebpf_membw_cleanup);

#endif /* INTP_HAVE_EBPF */

/* ==================================================================
 * Generic PSI metric (schedlat / psi_mem / psi_io share this core)
 * ================================================================== */

typedef struct {
	int valid;
	unsigned long long prev_us;
	char path[300];
} psi_state_t;

static int psi_seed(psi_state_t *s, const char *path)
{
	snprintf(s->path, sizeof(s->path), "%s", path);
	if (psi_some_total_us(s->path, &s->prev_us) != 0)
		return -1;
	s->valid = 1;
	return 0;
}

static int psi_sample(psi_state_t *s, struct intp_core_sample *out,
		      double interval_sec, const char *bid)
{
	if (!s->valid)
		return -1;
	unsigned long long cur = 0;

	if (psi_some_total_us(s->path, &cur) != 0)
		return -1;
	double d = (cur >= s->prev_us) ? (double)(cur - s->prev_us) : 0.0;

	s->prev_us = cur;
	double iv_us = interval_sec * 1.0e6;

	out->value = (iv_us > 0.0) ? clamp_pct(d / iv_us * 100.0) : 0.0;
	out->status = INTP_CORE_OK;
	out->backend_id = bid;
	out->note = NULL;
	return 0;
}

/* Build a cgroup-relative PSI path, or fail if no cgroup target. */
static int cg_psi_path(const char *basename, char *out, size_t n)
{
	const struct intp_core_target *t = intp_core_target_get();

	if (!t || !t->cgroup_path)
		return -1;
	snprintf(out, n, "%s/%s", t->cgroup_path, basename);
	return access(out, R_OK) == 0 ? 0 : -1;
}

/* ---------------- schedlat (cpu.pressure / schedstat / /proc/pressure/cpu)
 * ---- */

static psi_state_t schedlat_st;

static int schedlat_cg_probe(void)
{
	char p[300];

	return cg_psi_path("cpu.pressure", p, sizeof(p));
}
static int schedlat_cg_init(void)
{
	char p[300];

	if (cg_psi_path("cpu.pressure", p, sizeof(p)) != 0)
		return -1;
	return psi_seed(&schedlat_st, p);
}
static int schedlat_cg_read(struct intp_core_sample *out, double iv)
{
	return psi_sample(&schedlat_st, out, iv, "psi_cpu_cgroup");
}
static void schedlat_cg_cleanup(void)
{
	schedlat_st.valid = 0;
}

/* schedstat run-delay over the target's threads (no PSI dependency).
 * Its scale matches the v3.3 eBPF schedlat: Σ per-thread run-queue wait ÷
 * (interval × ncpus), so it is the stable-ABI fallback for that backend.
 * The PSI cpu.pressure backends below are fallbacks only — PSI 'some' is a
 * core-count-independent wall-clock stall FRACTION, a different (non-cored)
 * quantity that must not be mixed with this series in a
 * cross-variant/cross-env comparison. Per-TID, not per-TGID (C38). */
static struct tidacc schedstat_acc;
static long schedstat_ncpus;

static int schedlat_pid_probe(void)
{
	int capped;
	int n = collect_target_tids(tid_buf, (int)PORTABLE_MAX_TIDS, &capped);

	if (n <= 0)
		return -1;

	unsigned long long v;

	return read_tid_runqdelay(tid_buf[0], &v); /* CONFIG_SCHEDSTATS? */
}
static int schedlat_pid_init(void)
{
	if (tidacc_init(&schedstat_acc, read_tid_runqdelay) != 0)
		return -1;
	schedstat_ncpus = online_cpus();
	return 0;
}
static int schedlat_pid_read(struct intp_core_sample *out, double iv)
{
	if (!schedstat_acc.valid)
		return -1;

	unsigned long long d = 0;

	if (tidacc_scan(&schedstat_acc, read_tid_runqdelay, &d) != 0)
		return -1;

	double avail_ns = iv * 1.0e9 * (double)schedstat_ncpus;

	out->value = (avail_ns > 0.0) ? clamp_pct((double)d / avail_ns * 100.0)
				      : 0.0;
	out->status = schedstat_acc.capped ? INTP_CORE_DEGRADED : INTP_CORE_OK;
	out->backend_id = "schedstat_pid";
	out->note = schedstat_acc.capped ? "tid_cap" : NULL;
	return 0;
}
static void schedlat_pid_cleanup(void)
{
	tidacc_free(&schedstat_acc);
}

static int schedlat_sys_probe(void)
{
	return access("/proc/pressure/cpu", R_OK) == 0 ? 0 : -1;
}
static int schedlat_sys_init(void)
{
	return psi_seed(&schedlat_st, "/proc/pressure/cpu");
}
static int schedlat_sys_read(struct intp_core_sample *out, double iv)
{
	return psi_sample(&schedlat_st, out, iv, "psi_cpu_system");
}

static struct portable_backend schedlat_cg = {
	.backend_id = "psi_cpu_cgroup",
	.description = "PSI cpu.pressure 'some' over the target cgroup",
	.probe = schedlat_cg_probe,
	.init = schedlat_cg_init,
	.read = schedlat_cg_read,
	.cleanup = schedlat_cg_cleanup,
};
static struct portable_backend schedlat_pid = {
	.backend_id = "schedstat_pid",
	.description = "per-thread /proc/<tid>/schedstat run-queue wait",
	.probe = schedlat_pid_probe,
	.init = schedlat_pid_init,
	.read = schedlat_pid_read,
	.cleanup = schedlat_pid_cleanup,
};
static struct portable_backend schedlat_sys = {
	.backend_id = "psi_cpu_system",
	.description = "system PSI /proc/pressure/cpu 'some'",
	.probe = schedlat_sys_probe,
	.init = schedlat_sys_init,
	.read = schedlat_sys_read,
	.cleanup = schedlat_cg_cleanup,
};
/* Order matters: schedstat (cored, eBPF-scale-consistent) is PREFERRED; the
 * PSI cpu.pressure backends are fallbacks whose 'some' stall fraction is on
 * a different (non-cored) scale and carry a distinct backend_id so the
 * divergence is visible in the output provenance (DESIGN §5.2). */
static struct portable_metric schedlat_m = {
	.metric_name = "schedlat",
#if INTP_HAVE_EBPF
	.backends = {&schedlat_ebpf, &schedlat_pid, &schedlat_cg,
		     &schedlat_sys},
	.n_backends = 4,
#else
	.backends = {&schedlat_pid, &schedlat_cg, &schedlat_sys},
	.n_backends = 3,
#endif
};

/* ---------------- psi_mem (memory.pressure / /proc/pressure/memory) ----------
 */

static psi_state_t psi_mem_st;

static int psi_mem_cg_probe(void)
{
	char p[300];

	return cg_psi_path("memory.pressure", p, sizeof(p));
}
static int psi_mem_cg_init(void)
{
	char p[300];

	if (cg_psi_path("memory.pressure", p, sizeof(p)) != 0)
		return -1;
	return psi_seed(&psi_mem_st, p);
}
static int psi_mem_cg_read(struct intp_core_sample *out, double iv)
{
	return psi_sample(&psi_mem_st, out, iv, "psi_mem_cgroup");
}
static void psi_mem_cleanup(void)
{
	psi_mem_st.valid = 0;
}

static int psi_mem_sys_probe(void)
{
	return access("/proc/pressure/memory", R_OK) == 0 ? 0 : -1;
}
static int psi_mem_sys_init(void)
{
	return psi_seed(&psi_mem_st, "/proc/pressure/memory");
}
static int psi_mem_sys_read(struct intp_core_sample *out, double iv)
{
	return psi_sample(&psi_mem_st, out, iv, "psi_mem_system");
}

static struct portable_backend psi_mem_cg = {
	.backend_id = "psi_mem_cgroup",
	.lineage = INTP_LINEAGE_STABLE | INTP_LINEAGE_EBPF,
	.description = "PSI memory.pressure 'some' over the target cgroup",
	.probe = psi_mem_cg_probe,
	.init = psi_mem_cg_init,
	.read = psi_mem_cg_read,
	.cleanup = psi_mem_cleanup,
};
static struct portable_backend psi_mem_sys = {
	.backend_id = "psi_mem_system",
	.lineage = INTP_LINEAGE_STABLE | INTP_LINEAGE_EBPF,
	.description = "system PSI /proc/pressure/memory 'some'",
	.probe = psi_mem_sys_probe,
	.init = psi_mem_sys_init,
	.read = psi_mem_sys_read,
	.cleanup = psi_mem_cleanup,
};
static struct portable_metric psi_mem_m = {
	.metric_name = "psi_mem",
	.backends = {&psi_mem_cg, &psi_mem_sys},
	.n_backends = 2,
};

/* ---------------- psi_io (io.pressure / /proc/pressure/io) -------------------
 */

static psi_state_t psi_io_st;

static int psi_io_cg_probe(void)
{
	char p[300];

	return cg_psi_path("io.pressure", p, sizeof(p));
}
static int psi_io_cg_init(void)
{
	char p[300];

	if (cg_psi_path("io.pressure", p, sizeof(p)) != 0)
		return -1;
	return psi_seed(&psi_io_st, p);
}
static int psi_io_cg_read(struct intp_core_sample *out, double iv)
{
	return psi_sample(&psi_io_st, out, iv, "psi_io_cgroup");
}
static void psi_io_cleanup(void)
{
	psi_io_st.valid = 0;
}

static int psi_io_sys_probe(void)
{
	return access("/proc/pressure/io", R_OK) == 0 ? 0 : -1;
}
static int psi_io_sys_init(void)
{
	return psi_seed(&psi_io_st, "/proc/pressure/io");
}
static int psi_io_sys_read(struct intp_core_sample *out, double iv)
{
	return psi_sample(&psi_io_st, out, iv, "psi_io_system");
}

static struct portable_backend psi_io_cg = {
	.backend_id = "psi_io_cgroup",
	.lineage = INTP_LINEAGE_STABLE | INTP_LINEAGE_EBPF,
	.description = "PSI io.pressure 'some' over the target cgroup",
	.probe = psi_io_cg_probe,
	.init = psi_io_cg_init,
	.read = psi_io_cg_read,
	.cleanup = psi_io_cleanup,
};
static struct portable_backend psi_io_sys = {
	.backend_id = "psi_io_system",
	.lineage = INTP_LINEAGE_STABLE | INTP_LINEAGE_EBPF,
	.description = "system PSI /proc/pressure/io 'some'",
	.probe = psi_io_sys_probe,
	.init = psi_io_sys_init,
	.read = psi_io_sys_read,
	.cleanup = psi_io_cleanup,
};
static struct portable_metric psi_io_m = {
	.metric_name = "psi_io",
	.backends = {&psi_io_cg, &psi_io_sys},
	.n_backends = 2,
};

/* ---------------- schedthr (cpu.stat throttled_usec, cgroup-only) ------------
 */

static struct {
	int valid;
	unsigned long long prev_us;
	char path[300];
} schedthr_st;

static int schedthr_probe(void)
{
	const struct intp_core_target *t = intp_core_target_get();

	if (!t || !t->cgroup_path)
		return -1; /* no system analogue */
	char p[300];

	snprintf(p, sizeof(p), "%s/cpu.stat", t->cgroup_path);
	unsigned long long v;

	return cpu_stat_throttled_us(p, &v) == 0 ? 0 : -1;
}
static int schedthr_init(void)
{
	const struct intp_core_target *t = intp_core_target_get();

	snprintf(schedthr_st.path, sizeof(schedthr_st.path), "%s/cpu.stat",
		 t->cgroup_path);
	if (cpu_stat_throttled_us(schedthr_st.path, &schedthr_st.prev_us) != 0)
		return -1;
	schedthr_st.valid = 1;
	return 0;
}
static int schedthr_read(struct intp_core_sample *out, double iv)
{
	if (!schedthr_st.valid)
		return -1;
	unsigned long long cur = 0;

	if (cpu_stat_throttled_us(schedthr_st.path, &cur) != 0)
		return -1;
	double d = (cur >= schedthr_st.prev_us)
			   ? (double)(cur - schedthr_st.prev_us)
			   : 0.0;

	schedthr_st.prev_us = cur;
	double iv_us = iv * 1.0e6;

	out->value = (iv_us > 0.0) ? clamp_pct(d / iv_us * 100.0) : 0.0;
	out->status = INTP_CORE_OK;
	out->backend_id = "cpu.stat";
	out->note = NULL;
	return 0;
}
static void schedthr_cleanup(void)
{
	schedthr_st.valid = 0;
}

static struct portable_backend schedthr_b = {
	.backend_id = "cpu.stat",
	.lineage = INTP_LINEAGE_STABLE | INTP_LINEAGE_EBPF,
	.description = "CFS throttled_usec over the target cgroup",
	.probe = schedthr_probe,
	.init = schedthr_init,
	.read = schedthr_read,
	.cleanup = schedthr_cleanup,
};
static struct portable_metric schedthr_m = {
	.metric_name = "schedthr",
	.backends = {&schedthr_b},
	.n_backends = 1,
};

/* ---------------- steal (/proc/stat field 8, VM-global) ----------------------
 */

static struct {
	int valid;
	unsigned long long prev_steal;
	unsigned long long prev_total;
} steal_st;

static int steal_probe(void)
{
	unsigned long long s, t;

	return proc_stat_steal(&s, &t) == 0 ? 0 : -1;
}
static int steal_init(void)
{
	if (proc_stat_steal(&steal_st.prev_steal, &steal_st.prev_total) != 0)
		return -1;
	steal_st.valid = 1;
	return 0;
}
static int steal_read(struct intp_core_sample *out, double iv)
{
	(void)iv;
	if (!steal_st.valid)
		return -1;
	unsigned long long cs = 0, ct = 0;

	if (proc_stat_steal(&cs, &ct) != 0)
		return -1;
	unsigned long long ds =
		(cs >= steal_st.prev_steal) ? cs - steal_st.prev_steal : 0;
	unsigned long long dt =
		(ct >= steal_st.prev_total) ? ct - steal_st.prev_total : 0;

	steal_st.prev_steal = cs;
	steal_st.prev_total = ct;
	out->value =
		(dt > 0) ? clamp_pct((double)ds / (double)dt * 100.0) : 0.0;
	out->status = INTP_CORE_OK;
	out->backend_id = "proc_stat";
	out->note = NULL;
	return 0;
}
static void steal_cleanup(void)
{
	steal_st.valid = 0;
}

static struct portable_backend steal_b = {
	.backend_id = "proc_stat",
	.lineage = INTP_LINEAGE_STABLE | INTP_LINEAGE_EBPF,
	.description = "/proc/stat field 8 (hypervisor-stolen) %",
	.probe = steal_probe,
	.init = steal_init,
	.read = steal_read,
	.cleanup = steal_cleanup,
};
static struct portable_metric steal_m = {
	.metric_name = "steal",
	.backends = {&steal_b},
	.n_backends = 1,
};

/* ---------------- membw_est (LLC misses * 64B / interval -> MB/s) ------------
 */

#define CACHELINE_BYTES 64.0

typedef struct {
	int *fd_miss;
	uint64_t *prev_miss;
	int n;
	int valid;
	const char *bid;
} mbest_state_t;

static mbest_state_t mb;

static int mb_alloc(int n)
{
	mb.fd_miss = calloc((size_t)n, sizeof(int));
	mb.prev_miss = calloc((size_t)n, sizeof(uint64_t));
	if (!mb.fd_miss || !mb.prev_miss) {
		free(mb.fd_miss);
		free(mb.prev_miss);
		mb.fd_miss = NULL;
		mb.prev_miss = NULL;
		return -1;
	}
	for (int i = 0; i < n; i++)
		mb.fd_miss[i] = -1;
	mb.n = n;
	return 0;
}
static void mb_free(void)
{
	if (mb.fd_miss)
		for (int i = 0; i < mb.n; i++)
			perfev_close(mb.fd_miss[i]);
	free(mb.fd_miss);
	free(mb.prev_miss);
	memset(&mb, 0, sizeof(mb));
}

/* membw_est does NOT reject ENV_VM: perfev_open_llc_cache* fall back to the
 * ARCHITECTURAL cache-misses event, which IS virtualized in a KVM guest
 * (C26) — so this is exactly the metric that recovers the memory-bandwidth
 * dimension in-guest where the LL-read llcmr cannot. */
static int mb_perf_ok(int need_sys)
{
	const system_capabilities_t *c = detect_cached();

	if (!c->perf_usable)
		return -1;
	if (need_sys && c->perf_paranoid > 0 && geteuid() != 0)
		return -1;
	if (!need_sys && c->perf_paranoid > 1 && geteuid() != 0)
		return -1;
	return 0;
}

/* cgroup-mode: one architectural/LL miss counter per online CPU. */
static int mb_cg_probe(void)
{
	const struct intp_core_target *t = intp_core_target_get();

	if (!t || !t->cgroup_path)
		return -1;
	if (mb_perf_ok(1) != 0)
		return -1;
	int fd = open(t->cgroup_path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);

	if (fd < 0)
		return -1;
	close(fd);
	return 0;
}
static int mb_cg_init(void)
{
	const struct intp_core_target *t = intp_core_target_get();
	int cgfd = open(t->cgroup_path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);

	if (cgfd < 0)
		return -1;
	long ncpu = online_cpus();

	if (mb_alloc((int)ncpu) != 0) {
		close(cgfd);
		return -1;
	}
	mb.bid = "membw_cgroup";
	int opened = 0;

	for (int cpu = 0; cpu < (int)ncpu; cpu++) {
		int loads = -1, miss = -1;

		int rc = perfev_open_llc_cache_cgroup(cgfd, cpu, &loads, &miss);

		if (rc < 0) {
			mb.fd_miss[cpu] = -1;
			continue;
		}
		perfev_close(loads); /* membw_est needs misses only */
		mb.fd_miss[cpu] = miss;
		perfev_read(miss, &mb.prev_miss[cpu]);
		opened++;
	}
	close(cgfd);
	if (opened == 0) {
		mb_free();
		return -1;
	}
	mb.valid = 1;
	return 0;
}

/* hwcache: per-PID (or system-wide on cpu 0) single miss counter. */
static int mb_hw_probe(void)
{
	const struct intp_core_target *t = intp_core_target_get();
	int need_sys = (t->n_pids == 0);

	return mb_perf_ok(need_sys);
}
static int mb_hw_init(void)
{
	const struct intp_core_target *t = intp_core_target_get();
	int n = t->n_pids > 0 ? t->n_pids : 1;

	if (mb_alloc(n) != 0)
		return -1;
	mb.bid = "perf_hwcache";
	for (int i = 0; i < n; i++) {
		pid_t pid = t->n_pids > 0 ? t->pids[i] : -1;
		int loads = -1, miss = -1;

		if (perfev_open_llc_cache(pid, &loads, &miss) < 0) {
			mb_free();
			return -1;
		}
		perfev_close(loads);
		mb.fd_miss[i] = miss;
		perfev_read(miss, &mb.prev_miss[i]);
	}
	mb.valid = 1;
	return 0;
}

static int mb_read(struct intp_core_sample *out, double iv)
{
	if (!mb.valid)
		return -1;
	uint64_t miss_d = 0;

	for (int i = 0; i < mb.n; i++) {
		if (mb.fd_miss[i] < 0)
			continue;
		uint64_t m = 0;

		/* Only accumulate on a successful read, and clamp cur<prev
		 * (a failed read or counter reset on an offlined CPU would
		 * otherwise underflow the uint64 subtraction into a spurious
		 * ~2^64 spike). Mirrors v3.3's counters_diff cur>=prev
		 * clamp. */
		if (perfev_read(mb.fd_miss[i], &m) == 0) {
			if (m >= mb.prev_miss[i])
				miss_d += m - mb.prev_miss[i];
			mb.prev_miss[i] = m;
		}
	}
	double mbps =
		(iv > 0.0) ? (((double)miss_d * CACHELINE_BYTES) / iv) / 1.0e6
			   : 0.0;

	if (mbps < 0.0)
		mbps = 0.0;
	out->value = mbps; /* MB/s, NOT a percentage */
	out->status = INTP_CORE_OK;
	out->backend_id = mb.bid;
	out->note = NULL;
	return 0;
}
static void mb_cleanup(void)
{
	mb_free();
}

static struct portable_backend membw_est_cg = {
	.backend_id = "membw_cgroup",
	.description = "per-cgroup LLC misses * 64B / interval (cgroup-mode "
		       "perf)",
	.probe = mb_cg_probe,
	.init = mb_cg_init,
	.read = mb_read,
	.cleanup = mb_cleanup,
};
static struct portable_backend membw_est_hw = {
	.backend_id = "perf_hwcache",
	.description = "LLC misses * 64B / interval (per-PID / system perf)",
	.probe = mb_hw_probe,
	.init = mb_hw_init,
	.read = mb_read,
	.cleanup = mb_cleanup,
};
static struct portable_metric membw_est_m = {
	.metric_name = "membw_est",
#if INTP_HAVE_EBPF
	.backends = {&membw_est_cg, &membw_est_hw, &membw_est_ebpf},
	.n_backends = 3,
#else
	.backends = {&membw_est_cg, &membw_est_hw},
	.n_backends = 2,
#endif
};

/* ---------------- psp (involuntary preemption rate of the target's threads)
 */
/* Not Volpert's PSP, which counts switches from an observed task to PID 0 as
 * a throttling indicator; the throttling guard here is schedthr. psp sums
 * nonvoluntary_ctxt_switches (/proc/<tid>/task/<tid>/status) over every
 * thread of the target; the per-interval per-TID delta / interval =
 * involuntary preemptions/s. Per-thread stable-ABI analogue of v3.3's
 * sched_switch (prev-still-RUNNABLE) BPF counter. Scheduling-regime. */
static struct tidacc psp_acc;

static int psp_probe(void)
{
	int capped;
	int n = collect_target_tids(tid_buf, (int)PORTABLE_MAX_TIDS, &capped);

	if (n <= 0)
		return -1;

	unsigned long long v;

	return read_tid_nonvol(tid_buf[0], &v);
}
static int psp_init(void)
{
	return tidacc_init(&psp_acc, read_tid_nonvol);
}
static int psp_read(struct intp_core_sample *out, double iv)
{
	if (!psp_acc.valid)
		return -1;

	unsigned long long d = 0;

	if (tidacc_scan(&psp_acc, read_tid_nonvol, &d) != 0)
		return -1;
	out->value = (iv > 0.0) ? (double)d / iv : 0.0; /* preemptions / s */
	out->status = psp_acc.capped ? INTP_CORE_DEGRADED : INTP_CORE_OK;
	out->backend_id = "nonvol_ctxt_pid";
	out->note = psp_acc.capped ? "tid_cap" : NULL;
	return 0;
}
static void psp_cleanup(void)
{
	tidacc_free(&psp_acc);
}
static struct portable_backend psp_pid = {
	.backend_id = "nonvol_ctxt_pid",
	.description = "per-thread /proc/<tid>/status "
		       "nonvoluntary_ctxt_switches over target, ev/s",
	.probe = psp_probe,
	.init = psp_init,
	.read = psp_read,
	.cleanup = psp_cleanup,
};
static struct portable_metric psp_m = {
	.metric_name = "psp",
#if INTP_HAVE_EBPF
	.backends = {&psp_ebpf, &psp_pid},
	.n_backends = 2,
#else
	.backends = {&psp_pid},
	.n_backends = 1,
#endif
};

/* ---------------- idle_preempt (idle-CPU takeover rate) --------------------- */
/* Not derivable per-process from /proc (idle is a global CPU concept); v3.3
 * measures it via the sched_switch BPF program, served by idle_preempt_ebpf
 * above when eBPF is available. This stable-ABI placeholder's probe always
 * fails, so without eBPF (or under --engine=stable, which matches v2.1) the
 * metric reads unavailable rather than a fake 0. */
static int idle_preempt_probe(void)
{
	return -1;
}
static struct portable_backend idle_preempt_none = {
	.backend_id = "unavailable",
	.description =
		"idle-CPU takeover rate — eBPF-only (not derivable from /proc)",
	.probe = idle_preempt_probe,
	.init = NULL,
	.read = NULL,
	.cleanup = NULL,
};
static struct portable_metric idle_preempt_m = {
	.metric_name = "idle_preempt",
#if INTP_HAVE_EBPF
	.backends = {&idle_preempt_ebpf, &idle_preempt_none},
	.n_backends = 2,
#else
	.backends = {&idle_preempt_none},
	.n_backends = 1,
#endif
};

/* ==================================================================
 * Provider API (portable.h) — selection/init/read/cleanup semantics
 * ported verbatim from v2.1 backend_registry.c metric_select_backend /
 * metric_init / metric_read / metric_cleanup.
 * ================================================================== */

/* The 8 VM-portable metrics in canonical order (schedlat psi_mem membw_est
 * psi_io schedthr steal psp idle_preempt) — matches v2.1
 * intp_portable_metrics() and v3.3. */
static struct portable_metric *const portable_all[INTP_PORTABLE_COUNT] = {
	&schedlat_m, &psi_mem_m, &membw_est_m, &psi_io_m,
	&schedthr_m, &steal_m,	 &psp_m,       &idle_preempt_m,
};

static const char *const portable_full_names[INTP_PORTABLE_COUNT] = {
	"system.portable.schedlat",  "system.portable.psi_mem",
	"system.portable.membw_est", "system.portable.psi_io",
	"system.portable.schedthr",  "system.portable.steal",
	"system.portable.psp",	     "system.portable.idle_preempt",
};

/* Units per ADR-0009: percent metrics are published as RATIO [0,1],
 * membw_est as bytes/s, psp/idle_preempt as events/s. */
static const enum intp_unit portable_units[INTP_PORTABLE_COUNT] = {
	INTP_UNIT_RATIO,	    /* schedlat */
	INTP_UNIT_RATIO,	    /* psi_mem */
	INTP_UNIT_BYTES_PER_SECOND, /* membw_est */
	INTP_UNIT_RATIO,	    /* psi_io */
	INTP_UNIT_RATIO,	    /* schedthr */
	INTP_UNIT_RATIO,	    /* steal */
	INTP_UNIT_OPS_PER_SECOND,   /* psp */
	INTP_UNIT_OPS_PER_SECOND,   /* idle_preempt */
};

const char *intp_portable_backend(size_t i)
{
	if (i >= INTP_PORTABLE_COUNT || !portable_all[i]->active)
		return NULL;
	return portable_all[i]->active->backend_id;
}

int intp_portable_candidate(size_t i, size_t j, const char **id,
			    const char **desc, unsigned int *lineage)
{
	if (i >= INTP_PORTABLE_COUNT ||
	    j >= (size_t)portable_all[i]->n_backends)
		return -1;

	const struct portable_backend *b = portable_all[i]->backends[j];

	*id = b->backend_id;
	*desc = b->description;
	*lineage = b->lineage;
	return 0;
}

size_t intp_portable_count(void)
{
	return INTP_PORTABLE_COUNT;
}

const char *intp_portable_name(size_t i)
{
	return (i < INTP_PORTABLE_COUNT) ? portable_full_names[i] : NULL;
}

enum intp_unit intp_portable_unit(size_t i)
{
	return (i < INTP_PORTABLE_COUNT) ? portable_units[i]
					 : INTP_UNIT_UNSPECIFIED;
}

/* Ported metric_select_backend(): first backend whose probe passes. */
static int portable_select_backend(struct portable_metric *m)
{
	if (!m)
		return -1;
	if (m->n_backends == 0)
		return -1;
	for (int i = 0; i < m->n_backends; i++) {
		struct portable_backend *b = m->backends[i];

		if (!b || !b->probe)
			continue;
		if (!intp_core_lineage_allowed(b->lineage))
			continue;
		if (b->probe() == 0) {
			m->active = b;
			return 0;
		}
	}
	m->active = NULL;
	return -1;
}

/* Ported metric_init(): init the selected backend; on failure fall
 * through to the next probing candidate. */
static int portable_init_metric(struct portable_metric *m)
{
	if (!m)
		return -1;

	int start = 0;

	for (int i = 0; i < m->n_backends; i++) {
		if (m->backends[i] == m->active) {
			start = i;
			break;
		}
	}

	for (int i = start; i < m->n_backends; i++) {
		struct portable_backend *b = m->backends[i];

		if (!b || !b->init)
			continue;
		if (!intp_core_lineage_allowed(b->lineage))
			continue;
		/* Re-check the probe when falling through. */
		if (i > start && b->probe && b->probe() != 0)
			continue;
		if (b->init() == 0) {
			m->active = b;
			return 0;
		}
		if (b->cleanup)
			b->cleanup();
	}

	m->active = NULL;
	return -1;
}

int intp_portable_force(const char *metric, const char *backend_id)
{
	if (!metric || !backend_id)
		return -ENOENT;
	for (size_t i = 0; i < INTP_PORTABLE_COUNT; i++) {
		struct portable_metric *m = portable_all[i];

		if (strcmp(m->metric_name, metric) != 0)
			continue;
		for (int j = 0; j < m->n_backends; j++) {
			if (strcmp(m->backends[j]->backend_id, backend_id) ==
			    0) {
				m->forced = m->backends[j];
				return 0;
			}
		}
		return -ENOENT;
	}
	return -ENOENT;
}

void intp_portable_force_clear(void)
{
	for (size_t i = 0; i < INTP_PORTABLE_COUNT; i++)
		portable_all[i]->forced = NULL;
}

/* Bind the operator-forced backend (engine ignored). 0 on success. */
static int portable_bind_forced(struct portable_metric *m)
{
	struct portable_backend *b = m->forced;

	if (!b->probe || b->probe() != 0 || !b->init) {
		fprintf(stderr,
			"intp: --force-backend %s:%s failed (probe rejected); "
			"keeping the selected backend\n",
			m->metric_name, b->backend_id);
		return -1;
	}
	if (b->init() != 0) {
		if (b->cleanup)
			b->cleanup();
		fprintf(stderr,
			"intp: --force-backend %s:%s failed (init failed); "
			"keeping the selected backend\n",
			m->metric_name, b->backend_id);
		return -1;
	}
	m->active = b;
	return 0;
}

int intp_portable_select_and_init(void)
{
	int any = 0;

	for (size_t i = 0; i < INTP_PORTABLE_COUNT; i++) {
		struct portable_metric *m = portable_all[i];

		if (m->forced && portable_bind_forced(m) == 0) {
			any++;
			continue;
		}
		portable_select_backend(m);
		if (m->active && portable_init_metric(m) == 0)
			any++;
	}
	return any > 0 ? 0 : -ENODEV;
}

/* Ported metric_read(): a failing read degrades to unavailable for the
 * tick rather than emitting stale or zero values. */
void intp_portable_read(size_t i, struct intp_core_sample *out,
			double interval_sec)
{
	if (!out)
		return;
	out->value = NAN;
	out->status = INTP_CORE_UNAVAILABLE;
	out->backend_id = "none";
	out->note = NULL;

	if (i >= INTP_PORTABLE_COUNT)
		return;

	struct portable_metric *m = portable_all[i];

	if (!m->active || !m->active->read)
		return;
	if (m->active->read(out, interval_sec) != 0) {
		out->status = INTP_CORE_UNAVAILABLE;
		out->backend_id = m->active->backend_id;
	}
}

void intp_portable_cleanup(void)
{
	for (size_t i = 0; i < INTP_PORTABLE_COUNT; i++) {
		struct portable_metric *m = portable_all[i];

		for (int j = 0; j < m->n_backends; j++) {
			struct portable_backend *b = m->backends[j];

			if (b && b->cleanup)
				b->cleanup();
		}
		m->active = NULL;
		m->forced = NULL;
	}
}
