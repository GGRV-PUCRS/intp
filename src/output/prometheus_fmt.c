/* SPDX-License-Identifier: MIT */
/*
 * prometheus_fmt.c — one-shot Prometheus text exposition sink
 * (`--format prometheus`), per ADR-0002 naming: the seven core
 * metrics are `intp_<metric>_ratio` gauges; `intp_info` carries build
 * metadata. This sink writes the exposition to the output fd once per
 * sample tick (textfile-collector / debugging use). The long-running
 * scrape endpoint is `--serve` (libmicrohttpd; separate path).
 *
 * The v2.1 origin used an `intp_v2{metric=…}` name family; Prometheus
 * naming was never part of the frozen IADA contract, so this sink
 * adopts the ADR-0002 names.
 */

#include <intp/metrics.h>
#include <intp/schema.h>
#include <intp/version.h>

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "output.h"

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

/* "system.interference.cpu" → "cpu" */
static const char *short_name(const char *full)
{
	const char *dot = strrchr(full, '.');

	return dot ? dot + 1 : full;
}

static int prom_emit(struct intp_plugin_ctx *ctx,
		     const struct intp_sample *sample)
{
	(void)ctx;
	if (!g_out || !sample)
		return -1;

	fprintf(g_out,
		"# HELP intp_info intp build information\n"
		"# TYPE intp_info gauge\n"
		"intp_info{version=\"%s\",schema=\"%s\"} 1\n",
		INTP_VERSION, INTP_SCHEMA_VERSION_STR);

	for (size_t i = 0; i < sample->values_len; i++) {
		const struct intp_metric_value *v = &sample->values[i];

		if (v->kind != INTP_VALUE_DOUBLE || isnan(v->d))
			continue; /* unavailable = omitted, never 0 */
		fprintf(g_out, "intp_%s_ratio{host=\"%s\"} %.6f\n",
			short_name(v->name), sample->host ? sample->host : "",
			v->d);
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
