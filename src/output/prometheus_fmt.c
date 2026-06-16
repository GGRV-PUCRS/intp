/* SPDX-License-Identifier: MIT */
/*
 * prometheus_fmt.c — one-shot Prometheus text exposition sink
 * (`--format prometheus`), per ADR-0002 naming: the seven core metrics
 * are `intp_<metric>_ratio` gauges; `intp_build_info` carries build
 * metadata. This sink writes the exposition to the output fd once per
 * sample tick (textfile-collector / debugging use). The long-running
 * scrape endpoint is `--serve` (libmicrohttpd; separate path) — both
 * render through prom_common so naming/units/labels cannot diverge.
 *
 * Every series carries identity labels ({instance,target_kind,cgroup,
 * pid}) so a multi-target scraper can attribute it. INT64/UINT64 metrics
 * (portable/plugin counters) are rendered, not dropped; an unavailable
 * metric is omitted (ADR-0001 honest reporting).
 */

#include <intp/metrics.h>
#include <intp/schema.h>
#include <intp/version.h>

#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <unistd.h>

#include "../core/core.h"
#include "output.h"
#include "prom_common.h"

static FILE *g_out;

static int prom_open(struct intp_plugin_ctx *ctx, int fd)
{
	(void)ctx;

	int dupfd = dup(fd);

	if (dupfd < 0)
		return -1;
	g_out = fdopen(dupfd, "w");
	if (!g_out) {
		close(dupfd);
		return -1;
	}
	setvbuf(g_out, NULL, _IOLBF, 0);
	return 0;
}

static int prom_emit(struct intp_plugin_ctx *ctx,
		     const struct intp_sample *sample)
{
	(void)ctx;
	if (!g_out || !sample)
		return -1;

	struct intp_output_target ot;

	intp_core_output_target(&ot);

	char labels[2048];

	intp_prom_labels(labels, sizeof(labels), sample->host, ot.kind,
			 ot.cgroup, ot.pid);

	fprintf(g_out,
		"# HELP intp_build_info intp build information\n"
		"# TYPE intp_build_info gauge\n"
		"intp_build_info{version=\"%s\",schema=\"%s\"} 1\n",
		INTP_VERSION, INTP_SCHEMA_VERSION_STR);

	for (size_t i = 0; i < sample->values_len; i++) {
		const struct intp_metric_value *v = &sample->values[i];

		if (v->kind == INTP_VALUE_NONE)
			continue; /* unavailable = omitted, never 0 */
		if (v->kind == INTP_VALUE_DOUBLE && isnan(v->d))
			continue;

		const char *sname = intp_prom_short_name(v->name);
		const char *suffix = intp_prom_unit_suffix(v->unit);
		const char *type = intp_prom_type_for(v->unit, v->kind);
		const char *help = intp_core_metric_help(v->name);

		fprintf(g_out, "# HELP intp_%s_%s %s\n# TYPE intp_%s_%s %s\n",
			sname, suffix, help[0] ? help : v->name, sname, suffix,
			type);
		fprintf(g_out, "intp_%s_%s%s ", sname, suffix, labels);
		switch (v->kind) {
		case INTP_VALUE_DOUBLE:
			fprintf(g_out, "%.6f\n", v->d);
			break;
		case INTP_VALUE_INT64:
			fprintf(g_out, "%" PRId64 "\n", v->i64);
			break;
		case INTP_VALUE_UINT64:
			fprintf(g_out, "%" PRIu64 "\n", v->u64);
			break;
		default:
			fprintf(g_out, "0\n");
			break;
		}
	}
	return 0;
}

static void prom_close(struct intp_plugin_ctx *ctx)
{
	(void)ctx;
	if (g_out) {
		fclose(g_out);
		g_out = NULL;
	}
}

const struct intp_output_vtable intp_output_prometheus_fmt = {
	.format_name = "prometheus",
	.open = prom_open,
	.emit = prom_emit,
	.close = prom_close,
};
