/* SPDX-License-Identifier: MIT */
/*
 * portable.c — VM-portable interference metrics (system.portable.*).
 *
 * Ported from intp-comparison:
 *   variants/v2.1-c-abi-cgroup/src/portable.c @ 2e87b30fc762
 * Formulas, fallback chains, and backend order preserved verbatim;
 * the per-metric accessor singletons and the registry helpers
 * (metric_select_backend / metric_init / metric_read / metric_cleanup
 * from variants/v2.1-c-abi-cgroup/src/backend_registry.c) are folded
 * into the small provider API of portable.h instead of the core
 * "intp_backends" linker section — the canonical seven-metric path is
 * byte-identical whether or not the provider is enabled (ADR-0009),
 * and the whole set is gated behind --experimental (ADR-0008).
 *
 * Why these eight: inside a stock KVM guest the RDT metrics
 * (mbw/llcocc) are structurally absent (resctrl is host-only) and the
 * LL-read PMU events that back llcmr are <not supported> by the vPMU.
 * The metrics here are computed by the GUEST'S OWN kernel and
 * therefore survive in-guest:
 *
 *   schedlat   run-queue / scheduling latency — per-PID
 *              /proc/<pid>/schedstat run-delay (preferred) or PSI
 *              cpu.pressure 'some', %-of-interval.
 *   psi_mem    PSI memory.pressure 'some' — %-of-interval
 *              (memory-CAPACITY contention; bandwidth is membw_est).
 *   membw_est  DRAM-bandwidth estimate — LLC misses * 64B cacheline /
 *              interval (MB/s); architectural cache-misses event is
 *              virtualized in-guest.
 *   psi_io     PSI io.pressure 'some' — %-of-interval.
 *   schedthr   CFS throttling — cpu.stat throttled_usec,
 *              %-of-interval; cgroup-only (no system analogue).
 *   steal      hypervisor-stolen vCPU time — /proc/stat field 8,
 *              %-of-total; VM-global.
 *   psp        involuntary preemption rate — Σ
 *              nonvoluntary_ctxt_switches over the target, events/s.
 *   idle_preempt  idle-CPU takeover rate — eBPF-only; the probe
 *              always fails so this provider reports unavailable
 *              rather than a fake 0.
 *
 * A read reports INTP_CORE_UNAVAILABLE where the source is absent
 * (CONFIG_PSI=n, no cpu.max quota, no cgroup target) rather than a
 * fake 0.
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

_Static_assert(INTP_PORTABLE_COUNT == INTP_CORE_PORTABLE_COUNT,
	       "core schema sizing must match the provider count");

/* ---- local backend/metric model (ported v2.1 backend_t/metric_t) -------- */

#define PORTABLE_MAX_BACKENDS_PER_METRIC 4

struct portable_backend {
	const char *backend_id;
	const char *description;
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

/* Collect the target's current PIDs: re-scan the cgroup subtree each call
 * when a cgroup is targeted (recursive — catches workers forked after start
 * AND payloads that live in CHILD cgroups, as incus/lxc and nested k8s pods
 * place them under the cgroup v2 "no internal processes" rule), else the
 * static --pids set. */
static int collect_target_pids(pid_t *out, int max)
{
	const struct intp_core_target *t = intp_core_target_get();

	if (!t)
		return 0;
	if (t->cgroup_path)
		return procutil_read_cgroup_procs_rec(t->cgroup_path, out,
						      (size_t)max);
	int n = t->n_pids < max ? t->n_pids : max;

	for (int i = 0; i < n; i++)
		out[i] = t->pids[i];
	return n;
}

/* Sum field-2 (run-queue wait ns) of /proc/<pid>/schedstat across the
 * target's current PIDs. This is Σ per-task run-queue wait over the
 * interval; the caller normalizes by (interval_ns × ncpus), matching the
 * v3.3 eBPF schedlat scale. */
static int sum_runqdelay_ns(unsigned long long *out)
{
	pid_t pids[INTP_CORE_MAX_PIDS];
	int n = collect_target_pids(pids, INTP_CORE_MAX_PIDS);

	if (n <= 0)
		return -1;
	unsigned long long sum = 0;
	int any = 0;

	for (int i = 0; i < n; i++) {
		char path[64];

		snprintf(path, sizeof(path), "/proc/%d/schedstat",
			 (int)pids[i]);
		FILE *f = fopen(path, "r");

		if (!f)
			continue;
		unsigned long long run = 0, wait = 0;

		if (fscanf(f, "%llu %llu", &run, &wait) == 2) {
			sum += wait;
			any = 1;
		}
		fclose(f);
	}
	if (!any)
		return -1;
	*out = sum;
	return 0;
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

/* schedstat run-delay over the target's cgroup/PIDs (no PSI dependency).
 * This is the PREFERRED schedlat backend because its scale matches the v3.3
 * eBPF schedlat: Σ per-task run-queue wait ÷ (interval × ncpus). The PSI
 * cpu.pressure backend below is a fallback only — PSI 'some' is a
 * core-count-independent wall-clock stall FRACTION, a different (non-cored)
 * quantity that must not be mixed with this series in a
 * cross-variant/cross-env comparison. */
static struct {
	int valid;
	unsigned long long prev_wait_ns;
	long ncpus;
} schedstat_st;

static int schedlat_pid_probe(void)
{
	pid_t pids[INTP_CORE_MAX_PIDS];
	int n = collect_target_pids(pids, INTP_CORE_MAX_PIDS);

	if (n <= 0)
		return -1;
	char p[64];

	snprintf(p, sizeof(p), "/proc/%d/schedstat", (int)pids[0]);
	return access(p, R_OK) == 0 ? 0 : -1; /* CONFIG_SCHEDSTATS present? */
}
static int schedlat_pid_init(void)
{
	if (sum_runqdelay_ns(&schedstat_st.prev_wait_ns) != 0)
		return -1;
	schedstat_st.ncpus = online_cpus();
	schedstat_st.valid = 1;
	return 0;
}
static int schedlat_pid_read(struct intp_core_sample *out, double iv)
{
	if (!schedstat_st.valid)
		return -1;
	unsigned long long cur = 0;

	if (sum_runqdelay_ns(&cur) != 0)
		return -1;
	double d = (cur >= schedstat_st.prev_wait_ns)
			   ? (double)(cur - schedstat_st.prev_wait_ns)
			   : 0.0;

	schedstat_st.prev_wait_ns = cur;
	double avail_ns = iv * 1.0e9 * (double)schedstat_st.ncpus;

	out->value = (avail_ns > 0.0) ? clamp_pct(d / avail_ns * 100.0) : 0.0;
	out->status = INTP_CORE_OK;
	out->backend_id = "schedstat_pid";
	out->note = NULL;
	return 0;
}
static void schedlat_pid_cleanup(void)
{
	schedstat_st.valid = 0;
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
	.description = "per-PID /proc/<pid>/schedstat run-queue wait",
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
	.backends = {&schedlat_pid, &schedlat_cg, &schedlat_sys},
	.n_backends = 3,
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
	.description = "PSI memory.pressure 'some' over the target cgroup",
	.probe = psi_mem_cg_probe,
	.init = psi_mem_cg_init,
	.read = psi_mem_cg_read,
	.cleanup = psi_mem_cleanup,
};
static struct portable_backend psi_mem_sys = {
	.backend_id = "psi_mem_system",
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
	.description = "PSI io.pressure 'some' over the target cgroup",
	.probe = psi_io_cg_probe,
	.init = psi_io_cg_init,
	.read = psi_io_cg_read,
	.cleanup = psi_io_cleanup,
};
static struct portable_backend psi_io_sys = {
	.backend_id = "psi_io_system",
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

		if (perfev_open_llc_cache_cgroup(cgfd, cpu, &loads, &miss) < 0) {
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
	.backends = {&membw_est_cg, &membw_est_hw},
	.n_backends = 2,
};

/* ---------------- psp (involuntary preemption rate, Volpert PSP) -------------
 */
/* Sum nonvoluntary_ctxt_switches (/proc/<pid>/status) over the target's
 * tasks; the per-interval delta / interval = involuntary preemptions/s.
 * C-ABI analogue of v3.3's sched_switch(prev-still-RUNNABLE) BPF counter.
 * Scheduling-regime. */
static int sum_nonvol_ctxt(unsigned long long *out)
{
	pid_t pids[INTP_CORE_MAX_PIDS];
	int n = collect_target_pids(pids, INTP_CORE_MAX_PIDS);

	if (n <= 0)
		return -1;
	unsigned long long sum = 0;
	int any = 0;

	for (int i = 0; i < n; i++) {
		char path[64];

		snprintf(path, sizeof(path), "/proc/%d/status", (int)pids[i]);
		FILE *f = fopen(path, "r");

		if (!f)
			continue;
		char line[256];

		while (fgets(line, sizeof(line), f)) {
			unsigned long long v;

			if (sscanf(line, "nonvoluntary_ctxt_switches: %llu",
				   &v) == 1) {
				sum += v;
				any = 1;
				break;
			}
		}
		fclose(f);
	}
	if (!any)
		return -1;
	*out = sum;
	return 0;
}

static struct {
	unsigned long long prev;
	int valid;
} psp_st;
static int psp_probe(void)
{
	unsigned long long tmp;

	return sum_nonvol_ctxt(&tmp) == 0 ? 0 : -1;
}
static int psp_init(void)
{
	if (sum_nonvol_ctxt(&psp_st.prev) != 0)
		return -1;
	psp_st.valid = 1;
	return 0;
}
static int psp_read(struct intp_core_sample *out, double iv)
{
	if (!psp_st.valid)
		return -1;
	unsigned long long cur = 0;

	if (sum_nonvol_ctxt(&cur) != 0)
		return -1;
	double d = (cur >= psp_st.prev) ? (double)(cur - psp_st.prev) : 0.0;

	psp_st.prev = cur;
	out->value = (iv > 0.0) ? d / iv : 0.0; /* preemptions / s */
	out->status = INTP_CORE_OK;
	out->backend_id = "nonvol_ctxt_pid";
	out->note = NULL;
	return 0;
}
static void psp_cleanup(void)
{
	psp_st.valid = 0;
}
static struct portable_backend psp_pid = {
	.backend_id = "nonvol_ctxt_pid",
	.description = "sum /proc/<pid>/status nonvoluntary_ctxt_switches "
		       "over target, ev/s",
	.probe = psp_probe,
	.init = psp_init,
	.read = psp_read,
	.cleanup = psp_cleanup,
};
static struct portable_metric psp_m = {
	.metric_name = "psp",
	.backends = {&psp_pid},
	.n_backends = 1,
};

/* ---------------- idle_preempt (idle-CPU takeover rate) --------------------- */
/* Not derivable per-process from /proc (idle is a global CPU concept); v3.3
 * measures it via the sched_switch BPF program. The probe always fails so
 * the metric reads unavailable rather than a fake 0 — honest unavailability
 * on this provider. */
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
	.backends = {&idle_preempt_none},
	.n_backends = 1,
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

int intp_portable_select_and_init(void)
{
	int any = 0;

	for (size_t i = 0; i < INTP_PORTABLE_COUNT; i++) {
		struct portable_metric *m = portable_all[i];

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
	}
}
