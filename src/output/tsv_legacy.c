/* SPDX-License-Identifier: MIT */
/*
 * tsv_legacy.c — the tsv-legacy-v1 output sink (ADR-0002): the exact
 * seven-column TSV every intp-comparison consumer (IADA, the
 * dissertation plotting code, the Meyer-CSV converter) already parses.
 *
 * Format contract (arbiter: the v2.1 binary with --no-header; see
 * ADR-0010):
 *   - seven tab-separated values per line, frozen order
 *     netp nets blk mbw llcmr llcocc cpu;
 *   - each value rendered "%.0f" of the percent reading (the public
 *     sample carries ratios; this sink multiplies by 100);
 *   - "--" for an unavailable metric — never 0;
 *   - newline-terminated, NO header line, NO leading timestamp (the
 *     bench harness adds its own ts column; that is harness-owned).
 *
 * Rendering ported from intp-comparison:
 *   variants/v2.1-c-abi-cgroup/intp-hybrid.c @ 2e87b30fc762
 * (format_value / emit_tsv).
 */

#include <intp/metrics.h>

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "output.h"

static FILE *g_out;

static int tsv_open(struct intp_plugin_ctx *ctx, int fd)
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

static void format_value(char *buf, size_t bufsz,
			 const struct intp_metric_value *v)
{
	if (!v || v->kind != INTP_VALUE_DOUBLE || isnan(v->d)) {
		snprintf(buf, bufsz, "--");
		return;
	}
	snprintf(buf, bufsz, "%.0f", v->d * 100.0);
}

static int tsv_emit(struct intp_plugin_ctx *ctx,
		    const struct intp_sample *sample)
{
	(void)ctx;
	if (!g_out || !sample)
		return -1;

	char b[16];

	for (size_t m = 0; m < INTP_CORE_METRICS_COUNT; m++) {
		const struct intp_metric_value *v = NULL;

		/* Frozen column order = intp_core_metric_names order;
		 * look the value up by name so the sink stays correct
		 * even if the sample array ever reorders. */
		for (size_t i = 0; i < sample->values_len; i++) {
			if (sample->values[i].name &&
			    strcmp(sample->values[i].name,
				   intp_core_metric_names[m]) == 0) {
				v = &sample->values[i];
				break;
			}
		}
		format_value(b, sizeof(b), v);
		fprintf(g_out, m ? "\t%s" : "%s", b);
	}
	fprintf(g_out, "\n");
	return 0;
}

static void tsv_close(struct intp_plugin_ctx *ctx)
{
	(void)ctx;
	if (g_out) {
		fclose(g_out);
		g_out = NULL;
	}
}

const struct intp_output_vtable intp_output_tsv_legacy = {
	.format_name = "tsv-legacy-v1",
	.open = tsv_open,
	.emit = tsv_emit,
	.close = tsv_close,
};
