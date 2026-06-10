/* SPDX-License-Identifier: MIT */
/*
 * registry.c — backend registry: linker-section collection, ordered
 * per-metric candidate lists, probe-driven selection, init fall-through.
 *
 * Selection semantics ported from intp-comparison:
 *   variants/v2.1-c-abi-cgroup/src/backend_registry.c @ 2e87b30fc762
 * (metric_select_backend / metric_init / metric_read / metric_cleanup /
 * metric_force_backend), reshaped onto ADR-0001: candidates come from
 * the "intp_backends" linker section instead of per-metric accessor
 * singletons, every probe/init rejection is recorded with a reason,
 * and a metric whose whole list fails is *marked unavailable*, never
 * silently zeroed.
 */

#include <dirent.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core.h"

/* Linker-generated bounds of the registration section. */
extern const struct intp_core_backend *const __start_intp_backends[];
extern const struct intp_core_backend *const __stop_intp_backends[];

static struct intp_core_target g_target;

void intp_core_target_set(const struct intp_core_target *t)
{
	if (t)
		g_target = *t;
}

const struct intp_core_target *intp_core_target_get(void)
{
	return &g_target;
}

/* Target-spec helpers ported VERBATIM from intp-comparison:
 *   variants/v2.1-c-abi-cgroup/src/backend_registry.c @ 2e87b30fc762
 * (intp_parse_pid_list / intp_find_pids_by_comm). */

int intp_parse_pid_list(const char *spec, pid_t *out, int max)
{
	if (!spec || !out || max <= 0)
		return 0;
	int n = 0;
	const char *p = spec;

	while (*p && n < max) {
		char *end;
		long v = strtol(p, &end, 10);

		if (end == p || v <= 0)
			break;
		out[n++] = (pid_t)v;
		if (*end == ',')
			p = end + 1;
		else
			p = end;
	}
	return n;
}

int intp_find_pids_by_comm(const char *comm, pid_t *out, int max)
{
	if (!comm || !out || max <= 0)
		return 0;
	DIR *d = opendir("/proc");

	if (!d)
		return 0;
	struct dirent *e;
	int n = 0;

	while ((e = readdir(d)) != NULL && n < max) {
		if (e->d_type != DT_DIR && e->d_type != DT_UNKNOWN)
			continue;
		if (e->d_name[0] < '0' || e->d_name[0] > '9')
			continue;
		char path[320], buf[256];

		snprintf(path, sizeof(path), "/proc/%.32s/comm", e->d_name);
		FILE *f = fopen(path, "r");

		if (!f)
			continue;
		if (fgets(buf, sizeof(buf), f)) {
			size_t len = strlen(buf);

			while (len &&
			       (buf[len - 1] == '\n' || buf[len - 1] == '\r'))
				buf[--len] = '\0';
			if (strcmp(buf, comm) == 0)
				out[n++] = (pid_t)atoi(e->d_name);
		}
		fclose(f);
	}
	closedir(d);
	return n;
}

const char *const intp_core_metric_short_names[INTP_CORE_METRICS_COUNT] = {
	"netp", "nets", "blk", "mbw", "llcmr", "llcocc", "cpu",
};

const char *intp_core_status_str(enum intp_core_status s)
{
	switch (s) {
	case INTP_CORE_OK:
		return "ok";
	case INTP_CORE_DEGRADED:
		return "degraded";
	case INTP_CORE_PROXY:
		return "proxy";
	case INTP_CORE_UNAVAILABLE:
	default:
		return "unavailable";
	}
}

static bool backend_disabled(const struct intp_ctx *ctx,
			     const struct intp_core_backend *b)
{
	const char *const *p;
	char spec[80];

	if (!ctx->opts.disabled_backends)
		return false;

	/* Match either the bare backend id or "metric:id". */
	snprintf(spec, sizeof(spec), "%s:%s", b->metric, b->id);
	for (p = ctx->opts.disabled_backends; *p; p++) {
		if (strcmp(*p, b->id) == 0 || strcmp(*p, spec) == 0)
			return true;
	}
	return false;
}

static void record_rejection(struct intp_core_selection *sel,
			     const struct intp_core_backend *b,
			     const char *reason)
{
	if (sel->n_rejected >= INTP_CORE_MAX_BACKENDS)
		return;
	sel->rejected[sel->n_rejected].backend = b;
	sel->rejected[sel->n_rejected].reason = reason;
	sel->n_rejected++;
}

/* Order: priority descending, then chain_order ascending, then id
 * (last-resort determinism). Section order is deliberately NOT used as
 * a tie-break — compilers may emit file-scope objects in reverse
 * definition order. */
static int backend_before(const struct intp_core_backend *a,
			  const struct intp_core_backend *b)
{
	if (a->priority != b->priority)
		return a->priority > b->priority;
	if (a->chain_order != b->chain_order)
		return a->chain_order < b->chain_order;
	return strcmp(a->id, b->id) < 0;
}

/**
 * intp_core_registry_build — collect section entries into per-metric
 * candidate lists, deterministically ordered (see backend_before).
 */
int intp_core_registry_build(struct intp_ctx *ctx)
{
	for (size_t m = 0; m < INTP_CORE_METRICS_COUNT; m++) {
		struct intp_core_selection *sel = &ctx->sel[m];

		sel->metric = intp_core_metric_short_names[m];
		sel->full_name = intp_core_metric_names[m];
		sel->n_candidates = 0;
		sel->n_rejected = 0;
		sel->active = NULL;
		sel->disabled = false;

		const struct intp_core_backend *const *it;

		for (it = __start_intp_backends; it < __stop_intp_backends;
		     it++) {
			const struct intp_core_backend *b = *it;

			if (!b || !b->metric || !b->id)
				continue;
			if (strcmp(b->metric, sel->metric) != 0)
				continue;
			if (sel->n_candidates >= INTP_CORE_MAX_BACKENDS)
				return -ENOSPC;

			/* Insertion sort by backend_before. */
			int pos = sel->n_candidates;

			while (pos > 0 &&
			       backend_before(b, sel->candidates[pos - 1])) {
				sel->candidates[pos] = sel->candidates[pos - 1];
				pos--;
			}
			sel->candidates[pos] = b;
			sel->n_candidates++;
		}
	}
	return 0;
}

/**
 * intp_core_select — pick the first candidate whose probe() passes,
 * per metric. Ported metric_select_backend(); operator disables and
 * probe failures are recorded instead of dropped.
 */
int intp_core_select(struct intp_ctx *ctx)
{
	int any = 0;

	for (size_t m = 0; m < INTP_CORE_METRICS_COUNT; m++) {
		struct intp_core_selection *sel = &ctx->sel[m];

		if (sel->disabled) {
			sel->active = NULL;
			continue;
		}
		sel->active = NULL;
		for (int i = 0; i < sel->n_candidates; i++) {
			const struct intp_core_backend *b = sel->candidates[i];

			if (backend_disabled(ctx, b)) {
				record_rejection(sel, b,
						 "disabled by operator");
				continue;
			}
			if (sel->active) {
				record_rejection(sel, b,
						 "lower priority than the "
						 "selected backend");
				continue;
			}
			if (!b->probe || b->probe() == 0) {
				sel->active = b;
				any++;
				continue;
			}
			record_rejection(sel, b,
					 b->requires
						 ? b->requires
						 : "capability probe failed");
		}
	}
	return any > 0 ? 0 : -ENODEV;
}

/**
 * intp_core_init_backends — init each selected backend; on failure
 * fall through to the next probing candidate (ported metric_init()).
 */
int intp_core_init_backends(struct intp_ctx *ctx)
{
	ctx->n_active = 0;

	for (size_t m = 0; m < INTP_CORE_METRICS_COUNT; m++) {
		struct intp_core_selection *sel = &ctx->sel[m];
		int start = -1;

		if (!sel->active)
			continue;
		for (int i = 0; i < sel->n_candidates; i++) {
			if (sel->candidates[i] == sel->active) {
				start = i;
				break;
			}
		}
		if (start < 0) {
			sel->active = NULL;
			continue;
		}

		const struct intp_core_backend *bound = NULL;

		for (int i = start; i < sel->n_candidates; i++) {
			const struct intp_core_backend *b = sel->candidates[i];

			if (backend_disabled(ctx, b))
				continue;
			/* Re-check probe when falling through. */
			if (i > start && b->probe && b->probe() != 0) {
				record_rejection(
					sel, b,
					b->requires
						? b->requires
						: "capability probe failed");
				continue;
			}
			if (!b->init || b->init() == 0) {
				bound = b;
				break;
			}
			record_rejection(sel, b, "init failed");
			if (b->cleanup)
				b->cleanup();
		}

		sel->active = bound;
		if (bound)
			ctx->n_active++;
	}
	return 0;
}

/**
 * intp_core_read_metric — read via the active backend, or report
 * UNAVAILABLE. Ported metric_read(): a failing read degrades the
 * sample to unavailable for that tick rather than emitting stale or
 * zero values.
 */
void intp_core_read_metric(struct intp_core_selection *sel,
			   struct intp_core_sample *out, double interval_sec)
{
	if (!out)
		return;
	out->value = NAN;
	out->status = INTP_CORE_UNAVAILABLE;
	out->backend_id = "none";
	out->note = NULL;

	if (!sel || !sel->active || !sel->active->read)
		return;
	if (sel->active->read(out, interval_sec) != 0) {
		out->status = INTP_CORE_UNAVAILABLE;
		out->backend_id = sel->active->id;
	}
}

void intp_core_cleanup_backends(struct intp_ctx *ctx)
{
	for (size_t m = 0; m < INTP_CORE_METRICS_COUNT; m++) {
		struct intp_core_selection *sel = &ctx->sel[m];

		for (int i = 0; i < sel->n_candidates; i++) {
			const struct intp_core_backend *b = sel->candidates[i];

			if (b && b->cleanup)
				b->cleanup();
		}
		sel->active = NULL;
	}
	ctx->n_active = 0;
}

/**
 * intp_core_force_backend — operator override (ported
 * metric_force_backend): bind a specific backend if its probe passes.
 */
int intp_core_force_backend(struct intp_ctx *ctx, const char *metric,
			    const char *backend_id)
{
	if (!metric || !backend_id)
		return -EINVAL;

	for (size_t m = 0; m < INTP_CORE_METRICS_COUNT; m++) {
		struct intp_core_selection *sel = &ctx->sel[m];

		if (strcmp(sel->metric, metric) != 0)
			continue;
		for (int i = 0; i < sel->n_candidates; i++) {
			const struct intp_core_backend *b = sel->candidates[i];

			if (strcmp(b->id, backend_id) != 0)
				continue;
			if (!b->probe || b->probe() == 0) {
				sel->active = b;
				return 0;
			}
			return -ENODEV;
		}
		return -ENOENT;
	}
	return -ENOENT;
}

void intp_core_disable_metric(struct intp_ctx *ctx, const char *metric)
{
	for (size_t m = 0; m < INTP_CORE_METRICS_COUNT; m++) {
		struct intp_core_selection *sel = &ctx->sel[m];

		if (strcmp(sel->metric, metric) == 0) {
			sel->disabled = true;
			sel->active = NULL;
			return;
		}
	}
}
