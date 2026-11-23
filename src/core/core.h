/* SPDX-License-Identifier: MIT */
/*
 * core.h — internal core types: target binding, backend registry,
 * dispatcher state. NOT installed; the public ABI lives in
 * include/intp/.
 *
 * The backend model is ported from across-intp:
 *   variants/v2.1-c-abi-cgroup/include/backend.h @ 2e87b30fc762
 * adapted to ADR-0001 (linker-section registration, ordered selection
 * recorded for --print-schema) and ADR-0010 (semantic model and
 * degradation surfacing).
 *
 * A backend is a (probe, init, read, cleanup) tuple that collects one
 * metric through one specific kernel interface:
 *   probe()   — 0 if usable on this host (cheap, no side effects)
 *   init()    — 0 on success, allocates resources (fds, mon_groups, …)
 *   read()    — fills *out, returns 0 on success
 *   cleanup() — releases all resources, must be safe to call twice
 *
 * Concurrency: backends keep file-scope state (ported v2.1 design), so
 * at most ONE intp_ctx may be open per process. intp_open() enforces
 * this with -EBUSY. Lifting the restriction is post-1.0 work.
 */

#ifndef INTP_CORE_H
#define INTP_CORE_H

#include <intp/intp.h>
#include <intp/metrics.h>
#include <intp/plugin.h>
#include <intp/schema.h>

#include <sys/types.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#define INTP_CORE_MAX_PIDS     256
#define INTP_CORE_MAX_BACKENDS 8 /* per metric, after sort */

/* Size of the portable-metrics provider set (ADR-0009). Must equal
 * intp_portable_count(); checked with a static assert in portable.c. */
#define INTP_CORE_PORTABLE_COUNT 8

/* Max plugin-provided metric schema fragments merged into a ctx (ADR-0004). */
#define INTP_CORE_PLUGIN_METRICS_MAX 32

/* ------------------------------------------------------------------------ */
/* Target binding (singleton, ported v2.1 intp_target_t)                     */
/* ------------------------------------------------------------------------ */

/* How the binding maps to a container/VM tenant. */
enum intp_core_target_kind {
	INTP_CORE_TARGET_DEFAULT = 0, /* system-wide, or raw --pids/--cgroup */
	INTP_CORE_TARGET_CONTAINER,   /* cgroup-scoped tenant */
	INTP_CORE_TARGET_VM,	      /* VM scope cgroup + host tap netp */
};

struct intp_core_target {
	pid_t pids[INTP_CORE_MAX_PIDS];
	int n_pids;
	const char *cgroup_path; /* may be NULL */
	const char *iface;	 /* may be NULL = autodetect */
	const char *disk;	 /* may be NULL = autodetect */
	const char *tap_iface;	 /* VM netp: host tap iface, or NULL */
	int exact;		 /* leaf-only cgroup gating (--exact) */
	enum intp_core_target_kind kind;
	long nic_speed_bps_override;   /* 0 = use detection */
	long mem_bw_max_bps_override;  /* 0 = use detection */
	long llc_size_bytes_override;  /* 0 = use detection */
	long disk_bw_max_bps_override; /* 0 = use detection */
};

void intp_core_target_set(const struct intp_core_target *t);
const struct intp_core_target *intp_core_target_get(void);

/* Verbosity from opts.log_level: set at open, read by subsystems (e.g.
 * the eBPF loader's libbpf print callback). 0 quiet, 1 warn, 2 info,
 * 3 debug. */
void intp_core_set_log_level(int level);
int intp_core_log_level(void);

/* Public-facing target identity for output sinks and Prometheus labels,
 * derived from the active (process-singleton) ctx. Mirrors the
 * --print-schema "target" block so every surface describes one target.
 * `kind` is a static string; `cgroup` points into ctx-owned storage and
 * is valid for the ctx lifetime. Returns 0, or -1 (with the system
 * default filled in) when no ctx is open. */
struct intp_output_target {
	const char *kind;   /* "system" | "pid" | "cgroup" | "vm" */
	long pid;	    /* > 0 only when kind == "pid" */
	const char *cgroup; /* non-NULL only when a cgroup path is bound */
};
int intp_core_output_target(struct intp_output_target *out);

/* One-line description of a metric from the active ctx's schema, for a
 * Prometheus `# HELP` line. Returns "" if no ctx is open or the metric
 * has no description. The pointer is valid for the ctx lifetime. */
const char *intp_core_metric_help(const char *name);

/* ------------------------------------------------------------------------ */
/* Per-read sample (ported v2.1 metric_sample_t)                             */
/* ------------------------------------------------------------------------ */

enum intp_core_status {
	INTP_CORE_OK = 0,      /* primary backend, value reliable */
	INTP_CORE_UNAVAILABLE, /* no backend usable on this system */
	INTP_CORE_DEGRADED,    /* using fallback or approximation */
	INTP_CORE_PROXY,       /* value derived from a related metric */
};

struct intp_core_sample {
	double value; /* percent [0,100] as documented */
	enum intp_core_status status;
	const char *backend_id;
	const char *note; /* human-readable diagnostic, or NULL */
};

const char *intp_core_status_str(enum intp_core_status s);

/* ------------------------------------------------------------------------ */
/* Backend descriptor and linker-section registration (ADR-0001)             */
/* ------------------------------------------------------------------------ */

/* Which across-intp lineage a backend reproduces (ADR-0013). A backend
 * may belong to both: resctrl (mbw/llcocc) and the VM tap netp source are
 * shared by v2.1 and v3.3. Zero means stable-ABI, so pure-C backends need
 * no annotation. */
enum intp_core_lineage {
	INTP_LINEAGE_STABLE = 1 << 0, /* stable-ABI C, across-intp v2.1 */
	INTP_LINEAGE_EBPF = 1 << 1,   /* eBPF/CO-RE, across-intp v3.3 */
};

/* Backend engine selection (--engine). AUTO is the hybrid default: per
 * metric, the highest-priority backend whose probe passes, eBPF first.
 * STABLE and EBPF restrict every chain to one lineage, so a run
 * reproduces one across-intp variant end to end: a metric that lineage
 * cannot serve reads unavailable instead of borrowing from the other. */
enum intp_core_engine {
	INTP_ENGINE_AUTO = 0,
	INTP_ENGINE_STABLE,
	INTP_ENGINE_EBPF,
};

void intp_core_engine_set(enum intp_core_engine e);
enum intp_core_engine intp_core_engine_get(void);
const char *intp_core_engine_name(enum intp_core_engine e);

/* 1 if backend lineage bits `lineage` (0 = stable) may run under the
 * current engine. Shared with the portable-metrics provider. */
int intp_core_lineage_allowed(unsigned int lineage);

struct intp_core_backend {
	const char *metric;	 /* short name: "cpu", "blk", … */
	const char *id;		 /* backend id: "procfs_system", "resctrl", … */
	const char *description; /* one line, for --list-backends */
	const char *model;	 /* semantic model id (ADR-0010) */
	const char *scope;	 /* "system" | "pid" | "cgroup" | "vm" */
	const char *
		requires; /* human-readable probe requirement */
	enum intp_priority priority;
	/* Tie-break within (metric, priority): lower comes first. The
	 * linker-section order is NOT a reliable tie-break — compilers
	 * may emit file-scope objects in reverse definition order — so
	 * same-priority chains must set this explicitly. */
	int chain_order;
	unsigned int lineage; /* enum intp_core_lineage bits; 0 = stable */
	/* Optional, AUTO engine only: return a reason (static string) when
	 * this backend should step aside for a lower-priority one even
	 * though its probe passes — e.g. an eBPF backend that cannot keep
	 * a per-PID scope. --force-backend and the STABLE/EBPF engines
	 * ignore it. NULL = never defers. */
	const char *(*defer)(void);
	int (*probe)(void);
	int (*init)(void);
	int (*read)(struct intp_core_sample *out, double interval_sec);
	void (*cleanup)(void);
};

#if defined(__has_attribute)
#if __has_attribute(retain)
#define INTP_SECTION_KEEP __attribute__((used, retain))
#else
#define INTP_SECTION_KEEP __attribute__((used))
#endif
#else
#define INTP_SECTION_KEEP __attribute__((used))
#endif

/**
 * INTP_BACKEND_REGISTER — place a pointer to a backend descriptor in
 * the "intp_backends" linker section. The registry walks the section
 * at intp_open() time; no central list to edit.
 */
#define INTP_BACKEND_REGISTER(sym)                                              \
	INTP_SECTION_KEEP                                                       \
	__attribute__((                                                         \
		section("intp_backends"),                                       \
		aligned(sizeof(                                                 \
			void *)))) static const struct intp_core_backend *const \
		sym##_reg = &(sym)

/* ------------------------------------------------------------------------ */
/* Selection state (per metric, recorded for --print-schema)                 */
/* ------------------------------------------------------------------------ */

struct intp_core_rejection {
	const struct intp_core_backend *backend;
	const char *reason; /* static string */
};

struct intp_core_selection {
	const char *metric;    /* short name */
	const char *full_name; /* system.interference.* */
	const struct intp_core_backend *candidates[INTP_CORE_MAX_BACKENDS];
	int n_candidates;
	const struct intp_core_backend *active; /* NULL = unavailable */
	struct intp_core_rejection rejected[INTP_CORE_MAX_BACKENDS];
	int n_rejected;
	bool disabled; /* by operator */
};

/* ------------------------------------------------------------------------ */
/* Context                                                                   */
/* ------------------------------------------------------------------------ */

struct intp_sample_buffer {
	struct intp_metric_value *vals;
	size_t len;
	size_t cap;
};

struct intp_ctx {
	struct intp_options opts; /* shallow copy; lists owned by caller */
	struct intp_core_target target;
	char cgroup_path_buf[512];
	char host[256];

	struct intp_core_selection sel[INTP_CORE_METRICS_COUNT];
	int n_active; /* metrics with a bound backend */

	struct intp_sample_buffer buf;
	struct intp_sample sample;

	struct timespec next_deadline; /* CLOCK_MONOTONIC */
	bool deadline_armed;

	/* The portable-metrics provider is active for this ctx (requires
	 * opts.experimental AND extras->portable_metrics; ADR-0008/0009). */
	bool portable_enabled;

	struct intp_metric_schema schema[INTP_CORE_METRICS_COUNT +
					 INTP_CORE_PORTABLE_COUNT +
					 INTP_CORE_PLUGIN_METRICS_MAX];
	size_t schema_len;

	/* Loaded plugins (struct intp_plugin_set *, plugin_loader.h), or
	 * NULL when none are loaded. */
	void *plugins;
};

/* ------------------------------------------------------------------------ */
/* Core entry points (called from the public wrappers in libintp.c)          */
/* ------------------------------------------------------------------------ */

int intp_core_open(const struct intp_options *opts, struct intp_ctx **out);
void intp_core_close(struct intp_ctx *ctx);
int intp_core_sample(struct intp_ctx *ctx, struct intp_sample *out);
int intp_core_print_schema(struct intp_ctx *ctx, int fd);

/* Extended open used by the CLI (static link): carries the operator
 * knobs that are not yet part of the public ABI — backend forcing,
 * metric disables, and the hardware-normalizer overrides. The public
 * intp_options grows these fields in the planned pre-1.0 ABI
 * extension; until then this internal struct is the only carrier. */
struct intp_core_force {
	const char *metric;
	const char *backend_id;
};

struct intp_core_extras {
	const struct intp_core_force *forces;
	size_t n_forces;
	const char *const *disabled_metrics; /* NULL-terminated */
	const char *const
		*enabled_plugins;    /* --enable-plugin allowlist, NULL-term */
	long nic_speed_bps_override; /* 0 = use detection */
	long mem_bw_max_bps_override;
	long llc_size_bytes_override;
	long disk_bw_max_bps_override;
	const char *iface; /* NULL = autodetect */
	const char *disk;
	int exact;	      /* leaf-only cgroup gating (--exact) */
	int portable_metrics; /* enable the system.portable.* provider
			       * (requires opts->experimental; ADR-0009) */
	int engine;	      /* enum intp_core_engine (--engine); 0 = AUTO */
	/* Multi-PID targets (--pids/--comm): routed around the single-pid
	 * public option. When given (n_pids > 0) these OVERRIDE the
	 * single -p pid. */
	const pid_t *pids;
	int n_pids;
};

int intp_core_open_ex(const struct intp_options *opts,
		      const struct intp_core_extras *extras,
		      struct intp_ctx **out);

/* Prometheus exposition over an already-opened context (src/lib/
 * prometheus.c): background sampling thread + /metrics endpoint.
 * Blocks until SIGINT/SIGTERM; does not close the context. Returns
 * -ENOSYS when built with -Dwith_prometheus=false. */
int intp_core_serve_prometheus(struct intp_ctx *ctx, uint16_t port);

/* Richer serve entry used by the CLI: binds bind_host instead of the
 * wildcard address. bind_host NULL or "" => loopback 127.0.0.1;
 * "0.0.0.0" exposes on all interfaces. intp_core_serve_prometheus()
 * above is a thin wrapper with bind_host = NULL (loopback default). */
struct intp_serve_opts {
	const char *bind_host;
	uint16_t port;
};
int intp_core_serve_prometheus_ex(struct intp_ctx *ctx,
				  const struct intp_serve_opts *opts);

/* Registry helpers (also used by the unit tests). */
int intp_core_registry_build(struct intp_ctx *ctx);
int intp_core_select(struct intp_ctx *ctx);
int intp_core_init_backends(struct intp_ctx *ctx);
void intp_core_read_metric(struct intp_core_selection *sel,
			   struct intp_core_sample *out, double interval_sec);
void intp_core_cleanup_backends(struct intp_ctx *ctx);
int intp_core_force_backend(struct intp_ctx *ctx, const char *metric,
			    const char *backend_id);
void intp_core_disable_metric(struct intp_ctx *ctx, const char *metric);

/* Frozen short-name order: netp nets blk mbw llcmr llcocc cpu. */
extern const char *const intp_core_metric_short_names[INTP_CORE_METRICS_COUNT];

/* Target-spec helpers ported from across-intp:
 *   variants/v2.1-c-abi-cgroup/src/backend_registry.c @ 2e87b30fc762
 * (--pids CSV parsing and /proc comm matching; shared with the unit
 * tests). */
int intp_parse_pid_list(const char *spec, pid_t *out, int max);
int intp_find_pids_by_comm(const char *comm, pid_t *out, int max);

/* Sample buffer (real implementations behind the public append API). */
int intp_core_buf_append(struct intp_sample_buffer *buf, const char *name,
			 enum intp_unit unit, enum intp_value_kind kind,
			 double d, int64_t i64, uint64_t u64);

#endif /* INTP_CORE_H */
