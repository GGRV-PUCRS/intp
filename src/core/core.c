/* SPDX-License-Identifier: MIT */
/*
 * core.c — context lifecycle (open/close), target resolution, schema
 * construction, and the JSON schema dump behind intp_print_schema().
 *
 * Target resolution ported from across-intp:
 *   variants/v2.1-c-abi-cgroup/intp-hybrid.c @ 2e87b30fc762
 * (cgroup targets resolve their member PIDs from cgroup.procs at open
 * time; PID targets bind directly).
 */

#include "core.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../backends/common/procutil.h"
#include "../plugins/portable-metrics/portable.h"
#include "detect.h"
#include "plugin_loader.h"

/* Backends keep file-scope state (ported design); enforce one ctx per
 * process until the backends are made reentrant (post-1.0). */
static struct intp_ctx *g_open_ctx;

/* Verbosity (opts.log_level), set at open and read by subsystems such as
 * the eBPF loader's libbpf print callback. 0 quiet, 1 warn, 2 info, 3 debug. */
static int g_log_level;

void intp_core_set_log_level(int level)
{
	g_log_level = level;
}

int intp_core_log_level(void)
{
	return g_log_level;
}

static const char *const metric_descriptions[INTP_CORE_METRICS_COUNT] = {
	"NIC-level network utilization (share of physical link bandwidth)",
	"network stack utilization (CPU time in NET_TX/NET_RX softirq)",
	"block I/O utilization (see per-backend semantic model)",
	"memory bandwidth utilization (share of sustained DRAM bandwidth)",
	"last-level cache miss ratio (misses / references)",
	"last-level cache occupancy (share of LLC capacity)",
	"CPU utilization (busy share of total CPU time)",
};

/* One-liners for the system.portable.* set, canonical provider order
 * (schedlat psi_mem membw_est psi_io schedthr steal psp idle_preempt;
 * ADR-0009). */
static const char *const portable_descriptions[INTP_CORE_PORTABLE_COUNT] = {
	"run-queue/scheduling latency (schedstat run-delay or PSI cpu)",
	"memory-capacity pressure (PSI memory.pressure 'some')",
	"DRAM bandwidth estimate (LLC misses x 64B cacheline / interval)",
	"block I/O pressure (PSI io.pressure 'some')",
	"CFS bandwidth throttling (cpu.stat throttled_usec, cgroup-only)",
	"hypervisor-stolen vCPU time (/proc/stat field 8, VM-global)",
	"involuntary preemption rate (nonvoluntary_ctxt_switches)",
	"idle-CPU takeover rate (eBPF-only; unavailable on this provider)",
};

static int resolve_target(struct intp_ctx *ctx, const struct intp_options *opts)
{
	struct intp_core_target *t = &ctx->target;

	memset(t, 0, sizeof(*t));

	switch (opts->target.kind) {
	case INTP_TARGET_SYSTEM:
		break;
	case INTP_TARGET_PID:
		if (opts->target.pid <= 0)
			return -EINVAL;
		t->pids[0] = opts->target.pid;
		t->n_pids = 1;
		break;
	case INTP_TARGET_CGROUP:
		if (!opts->target.cgroup_path)
			return -EINVAL;
		snprintf(ctx->cgroup_path_buf, sizeof(ctx->cgroup_path_buf),
			 "%s", opts->target.cgroup_path);
		t->cgroup_path = ctx->cgroup_path_buf;
		t->kind = INTP_CORE_TARGET_CONTAINER;
		/* Read the whole subtree: under the cgroup v2
		 * no-internal-processes rule a container cgroup (incus
		 * ".lxc", systemd init.scope) lists no PIDs at its top,
		 * and an empty set sends per-task backends such as
		 * llcmr perf_hwcache to system-wide counting. Same walk
		 * as v3.3 read_cgroup_pids_rec (cutting-edge-intp D15). */
		t->n_pids = procutil_read_cgroup_procs_rec(
			t->cgroup_path, t->pids, INTP_CORE_MAX_PIDS);
		break;
	case INTP_TARGET_VM:
		/* VM netp comes from the host-side tap iface; the other
		 * six metrics are scoped by the VM's qemu-scope cgroup
		 * when cgroup_path is set in the union alongside (ported
		 * v2.1 --target-vm semantics). */
		if (!opts->target.tap_iface || !opts->target.tap_iface[0])
			return -EINVAL;
		t->tap_iface = opts->target.tap_iface;
		t->kind = INTP_CORE_TARGET_VM;
		if (opts->target.cgroup_path) {
			snprintf(ctx->cgroup_path_buf,
				 sizeof(ctx->cgroup_path_buf), "%s",
				 opts->target.cgroup_path);
			t->cgroup_path = ctx->cgroup_path_buf;
			t->n_pids = procutil_read_cgroup_procs_rec(
				t->cgroup_path, t->pids, INTP_CORE_MAX_PIDS);
		}
		break;
	default:
		return -EINVAL;
	}
	return 0;
}

int intp_core_open_ex(const struct intp_options *opts,
		      const struct intp_core_extras *extras,
		      struct intp_ctx **out)
{
	static const struct intp_options defaults;

	if (!out)
		return -EINVAL;
	if (g_open_ctx)
		return -EBUSY;
	if (!opts)
		opts = &defaults;

	/* Experimental fence (ADR-0008): the library enforces the gate
	 * even when the CLI's friendly message was bypassed. */
	if (opts->target.kind == INTP_TARGET_VM && !opts->experimental)
		return -EPERM;
	if (extras && extras->portable_metrics && !opts->experimental)
		return -EPERM;

	struct intp_ctx *ctx = calloc(1, sizeof(*ctx));

	if (!ctx)
		return -ENOMEM;

	ctx->opts = *opts;
	if (ctx->opts.interval_ms == 0)
		ctx->opts.interval_ms = 1000;
	intp_core_set_log_level(ctx->opts.log_level);

	int rc = resolve_target(ctx, opts);

	if (rc) {
		free(ctx);
		return rc;
	}

	if (gethostname(ctx->host, sizeof(ctx->host) - 1) != 0)
		snprintf(ctx->host, sizeof(ctx->host), "unknown");

	if (extras) {
		ctx->target.nic_speed_bps_override =
			extras->nic_speed_bps_override;
		ctx->target.mem_bw_max_bps_override =
			extras->mem_bw_max_bps_override;
		ctx->target.llc_size_bytes_override =
			extras->llc_size_bytes_override;
		ctx->target.disk_bw_max_bps_override =
			extras->disk_bw_max_bps_override;
		if (extras->iface)
			ctx->target.iface = extras->iface;
		if (extras->disk)
			ctx->target.disk = extras->disk;
		ctx->target.exact = extras->exact;
		/* Multi-PID targets (--pids/--comm) OVERRIDE the single
		 * -p pid from the public options. */
		if (extras->pids && extras->n_pids > 0) {
			int n = extras->n_pids;

			if (n > INTP_CORE_MAX_PIDS)
				n = INTP_CORE_MAX_PIDS;
			for (int i = 0; i < n; i++)
				ctx->target.pids[i] = extras->pids[i];
			ctx->target.n_pids = n;
		}
		ctx->portable_enabled =
			ctx->opts.experimental && extras->portable_metrics;
	}

	intp_core_target_set(&ctx->target);

	/* One detection pass before any backend probes; backends read
	 * the cached result via detect_cached(). */
	(void)detect_cached();

	rc = intp_core_registry_build(ctx);
	if (rc) {
		free(ctx);
		return rc;
	}

	if (extras && extras->disabled_metrics) {
		const char *const *p;

		for (p = extras->disabled_metrics; *p; p++)
			intp_core_disable_metric(ctx, *p);
	}

	/* Selection or init binding nothing is not an open() failure:
	 * the context stays introspectable (--print-schema explains why
	 * every metric is unavailable); intp_sample() returns -ENODEV. */
	(void)intp_core_select(ctx);

	/* Operator forcing overrides the selection before init (the
	 * v2.1 flow: --force-backend falls back to normal selection
	 * when the forced backend's probe rejects). */
	if (extras) {
		for (size_t i = 0; i < extras->n_forces; i++) {
			const struct intp_core_force *fc = &extras->forces[i];

			if (intp_core_force_backend(ctx, fc->metric,
						    fc->backend_id) != 0)
				fprintf(stderr,
					"intp: --force-backend %s:%s failed "
					"(probe rejected); keeping the "
					"selected backend\n",
					fc->metric, fc->backend_id);
		}
	}

	(void)intp_core_init_backends(ctx);

	/* Portable-metrics provider (ADR-0009): probe + init its own
	 * fallback chains after the core backends, only when enabled.
	 * Metrics that bind nothing read as unavailable, never zero. */
	if (ctx->portable_enabled)
		(void)intp_portable_select_and_init();

	for (size_t m = 0; m < INTP_CORE_METRICS_COUNT; m++) {
		ctx->schema[m].name = intp_core_metric_names[m];
		ctx->schema[m].unit = INTP_UNIT_RATIO;
		ctx->schema[m].description = metric_descriptions[m];
		ctx->schema[m].always_present = ctx->sel[m].active != NULL;
	}
	ctx->schema_len = INTP_CORE_METRICS_COUNT;

	if (ctx->portable_enabled) {
		for (size_t i = 0; i < intp_portable_count(); i++) {
			struct intp_metric_schema *s =
				&ctx->schema[ctx->schema_len++];

			s->name = intp_portable_name(i);
			s->unit = intp_portable_unit(i);
			s->description = portable_descriptions[i];
			s->always_present = false;
		}
	}

	/* Load plugins last, so their metrics append after the frozen seven
	 * and the portable set (preserving tsv-legacy column order). A bad
	 * plugin is skipped, never failing the open. */
	intp_plugin_load_all(ctx, ctx->opts.plugin_dirs,
			     extras ? extras->enabled_plugins : NULL,
			     ctx->opts.disabled_plugins);
	intp_plugin_merge_schema(ctx);

	g_open_ctx = ctx;
	*out = ctx;
	return 0;
}

int intp_core_open(const struct intp_options *opts, struct intp_ctx **out)
{
	return intp_core_open_ex(opts, NULL, out);
}

void intp_core_close(struct intp_ctx *ctx)
{
	if (!ctx)
		return;
	intp_plugin_unload_all(ctx);
	if (ctx->portable_enabled)
		intp_portable_cleanup();
	intp_core_cleanup_backends(ctx);
	free(ctx->buf.vals);
	if (g_open_ctx == ctx)
		g_open_ctx = NULL;
	free(ctx);
}

/* ------------------------------------------------------------------------ */
/* JSON schema dump                                                          */
/* ------------------------------------------------------------------------ */

static void json_escape(FILE *f, const char *s)
{
	for (; s && *s; s++) {
		switch (*s) {
		case '"':
			fputs("\\\"", f);
			break;
		case '\\':
			fputs("\\\\", f);
			break;
		case '\n':
			fputs("\\n", f);
			break;
		case '\t':
			fputs("\\t", f);
			break;
		default:
			fputc(*s, f);
			break;
		}
	}
}

/* Defined in the llcmr backend: "ll_read", "arch_cache_refs",
 * "vendor_raw", or NULL when no perf counter is open. */
extern const char *intp_llcmr_event_family(void);

/* --print-schema: the mbw normalization ceiling and where it came from,
 * so a fallback ceiling (cutting-edge-intp C34) is visible up front. */
static void print_mbw_ceiling(FILE *f, const struct intp_ctx *ctx)
{
	long over = ctx->target.mem_bw_max_bps_override;

	if (over > 0) {
		fprintf(f, "      \"ceiling_bps\": %ld,\n", over);
		fprintf(f, "      \"ceiling_source\": \"--mem-bw-max-bps\",\n");
		return;
	}
	fprintf(f, "      \"ceiling_bps\": %ld,\n",
		detect_memory_bandwidth_max_bps());
	fprintf(f, "      \"ceiling_source\": \"");
	json_escape(f, detect_memory_bandwidth_source());
	fprintf(f, "\",\n");
	fprintf(f, "      \"ceiling_fallback\": %s,\n",
		detect_memory_bandwidth_is_fallback() ? "true" : "false");
}

static const char *target_kind_str(const struct intp_ctx *ctx)
{
	switch (ctx->opts.target.kind) {
	case INTP_TARGET_PID:
		return "pid";
	case INTP_TARGET_CGROUP:
		return "cgroup";
	case INTP_TARGET_VM:
		return "vm";
	case INTP_TARGET_SYSTEM:
	default:
		return "system";
	}
}

int intp_core_output_target(struct intp_output_target *out)
{
	out->kind = "system";
	out->pid = 0;
	out->cgroup = NULL;
	if (!g_open_ctx)
		return -1;

	const struct intp_ctx *ctx = g_open_ctx;

	out->kind = target_kind_str(ctx);
	if (ctx->opts.target.kind == INTP_TARGET_PID)
		out->pid = ctx->opts.target.pid;
	if (ctx->opts.target.kind == INTP_TARGET_CGROUP &&
	    ctx->cgroup_path_buf[0])
		out->cgroup = ctx->cgroup_path_buf;
	return 0;
}

const char *intp_core_metric_help(const char *name)
{
	if (!g_open_ctx || !name)
		return "";

	const struct intp_metric_schema *s =
		intp_schema_find(g_open_ctx, name);

	return (s && s->description) ? s->description : "";
}

int intp_core_print_schema(struct intp_ctx *ctx, int fd)
{
	if (!ctx || fd < 0)
		return -EINVAL;

	int dupfd = dup(fd);

	if (dupfd < 0)
		return -errno;

	FILE *f = fdopen(dupfd, "w");

	if (!f) {
		int err = -errno;

		close(dupfd);
		return err;
	}

	fprintf(f, "{\n");
	fprintf(f, "  \"$schema\": "
		   "\"https://json-schema.org/draft/2020-12/schema\",\n");
	fprintf(f, "  \"intp_version\": \"%s\",\n", intp_version_string());
	fprintf(f, "  \"schema_version\": \"%s\",\n", INTP_SCHEMA_VERSION_STR);
	fprintf(f, "  \"host\": \"");
	json_escape(f, ctx->host);
	fprintf(f, "\",\n");

	fprintf(f, "  \"target\": { \"kind\": \"%s\"", target_kind_str(ctx));
	if (ctx->opts.target.kind == INTP_TARGET_PID)
		fprintf(f, ", \"pid\": %d", (int)ctx->opts.target.pid);
	if (ctx->opts.target.kind == INTP_TARGET_CGROUP) {
		fprintf(f, ", \"cgroup\": \"");
		json_escape(f, ctx->cgroup_path_buf);
		fprintf(f, "\"");
	}
	fprintf(f, " },\n");

	fprintf(f, "  \"interval_ms\": %u,\n", ctx->opts.interval_ms);
	fprintf(f, "  \"metrics\": {\n");

	for (size_t m = 0; m < INTP_CORE_METRICS_COUNT; m++) {
		const struct intp_core_selection *sel = &ctx->sel[m];
		const struct intp_core_backend *b = sel->active;

		fprintf(f, "    \"%s\": {\n", sel->full_name);
		fprintf(f, "      \"unit\": \"%s\",\n",
			intp_unit_name(INTP_UNIT_RATIO));
		/* Stability per ADR-0008, sourced internally: the seven
		 * core metrics are stable; system.portable.* below is
		 * experimental until its validation campaign passes. */
		fprintf(f, "      \"stability\": \"stable\",\n");
		fprintf(f, "      \"description\": \"");
		json_escape(f, metric_descriptions[m]);
		fprintf(f, "\",\n");
		fprintf(f, "      \"available\": %s,\n", b ? "true" : "false");
		if (sel->disabled)
			fprintf(f, "      \"disabled\": true,\n");
		if (b) {
			fprintf(f, "      \"backend\": \"%s\",\n", b->id);
			fprintf(f, "      \"model\": \"%s\",\n",
				b->model ? b->model : "unspecified");
			fprintf(f, "      \"scope\": \"%s\",\n",
				b->scope ? b->scope : "system");
			if (strcmp(intp_core_metric_short_names[m], "llcmr") == 0 &&
			    intp_llcmr_event_family())
				fprintf(f, "      \"event_family\": \"%s\",\n",
					intp_llcmr_event_family());
			if (strcmp(intp_core_metric_short_names[m], "mbw") == 0)
				print_mbw_ceiling(f, ctx);
		}
		fprintf(f, "      \"rejected\": [");
		for (int r = 0; r < sel->n_rejected; r++) {
			fprintf(f,
				"%s\n        { \"backend\": \"%s\", "
				"\"reason\": \"",
				r ? "," : "", sel->rejected[r].backend->id);
			json_escape(f, sel->rejected[r].reason);
			fprintf(f, "\" }");
		}
		fprintf(f, "%s]\n", sel->n_rejected ? "\n      " : "");
		fprintf(f, "    }%s\n",
			(m + 1 < INTP_CORE_METRICS_COUNT ||
			 ctx->portable_enabled)
				? ","
				: "");
	}

	/* system.portable.* (ADR-0009): present only when the provider
	 * is enabled; every entry is marked experimental (ADR-0008). */
	if (ctx->portable_enabled) {
		size_t n = intp_portable_count();

		for (size_t i = 0; i < n; i++) {
			const struct intp_metric_schema *s =
				&ctx->schema[INTP_CORE_METRICS_COUNT + i];

			fprintf(f, "    \"%s\": {\n", s->name);
			fprintf(f, "      \"unit\": \"%s\",\n",
				intp_unit_name(s->unit));
			fprintf(f, "      \"stability\": \"experimental\",\n");
			fprintf(f, "      \"description\": \"");
			json_escape(f, s->description);
			fprintf(f, "\"\n");
			fprintf(f, "    }%s\n", (i + 1 < n) ? "," : "");
		}
	}

	fprintf(f, "  }\n");
	fprintf(f, "}\n");

	fclose(f);
	return 0;
}
