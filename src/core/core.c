/* SPDX-License-Identifier: MIT */
/*
 * core.c — context lifecycle (open/close), target resolution, schema
 * construction, and the JSON schema dump behind intp_print_schema().
 *
 * Target resolution ported from intp-comparison:
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
#include "detect.h"

/* Backends keep file-scope state (ported design); enforce one ctx per
 * process until the backends are made reentrant (post-1.0). */
static struct intp_ctx *g_open_ctx;

static const char *const metric_descriptions[INTP_CORE_METRICS_COUNT] = {
	"NIC-level network utilization (share of physical link bandwidth)",
	"network stack utilization (CPU time in NET_TX/NET_RX softirq)",
	"block I/O utilization (see per-backend semantic model)",
	"memory bandwidth utilization (share of sustained DRAM bandwidth)",
	"last-level cache miss ratio (misses / references)",
	"last-level cache occupancy (share of LLC capacity)",
	"CPU utilization (busy share of total CPU time)",
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
		t->n_pids = procutil_read_cgroup_procs(t->cgroup_path, t->pids,
						       INTP_CORE_MAX_PIDS);
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

	struct intp_ctx *ctx = calloc(1, sizeof(*ctx));

	if (!ctx)
		return -ENOMEM;

	ctx->opts = *opts;
	if (ctx->opts.interval_ms == 0)
		ctx->opts.interval_ms = 1000;

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

	for (size_t m = 0; m < INTP_CORE_METRICS_COUNT; m++) {
		ctx->schema[m].name = intp_core_metric_names[m];
		ctx->schema[m].unit = INTP_UNIT_RATIO;
		ctx->schema[m].description = metric_descriptions[m];
		ctx->schema[m].always_present = ctx->sel[m].active != NULL;
	}
	ctx->schema_len = INTP_CORE_METRICS_COUNT;

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

static const char *target_kind_str(const struct intp_ctx *ctx)
{
	switch (ctx->opts.target.kind) {
	case INTP_TARGET_PID:
		return "pid";
	case INTP_TARGET_CGROUP:
		return "cgroup";
	case INTP_TARGET_SYSTEM:
	default:
		return "system";
	}
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
			(m + 1 < INTP_CORE_METRICS_COUNT) ? "," : "");
	}

	fprintf(f, "  }\n");
	fprintf(f, "}\n");

	fclose(f);
	return 0;
}
