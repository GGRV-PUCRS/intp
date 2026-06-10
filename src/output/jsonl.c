/* SPDX-License-Identifier: MIT */
/*
 * jsonl.c — JSON-Lines output sink (ADR-0002, primary machine format).
 *
 * One JSON object per sample:
 *
 *   {"schema_version":"0.1.0","ts":"2026-05-08T12:34:56.789Z",
 *    "host":"…","target":{…},
 *    "metrics":{"system.interference.cpu":0.66, …}}
 *
 * A metric that is unavailable on this host is OMITTED from the
 * metrics object — missing key means unavailable, never zero
 * (ADR-0001 honest reporting; ADR-0002 consumer contract).
 */

#include <intp/metrics.h>
#include <intp/schema.h>

#include <math.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

#include "output.h"

static FILE *g_out;

static int jsonl_open(struct intp_plugin_ctx *ctx, int fd)
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

static void jsonl_escape(FILE *f, const char *s)
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
		default:
			fputc(*s, f);
			break;
		}
	}
}

static int jsonl_emit(struct intp_plugin_ctx *ctx,
		      const struct intp_sample *sample)
{
	(void)ctx;
	if (!g_out || !sample)
		return -1;

	FILE *f = g_out;
	struct tm tm;
	time_t sec = sample->ts.tv_sec;

	gmtime_r(&sec, &tm);

	char tsbuf[40];

	strftime(tsbuf, sizeof(tsbuf), "%Y-%m-%dT%H:%M:%S", &tm);

	fprintf(f,
		"{\"schema_version\":\"%s\",\"ts\":\"%s.%03ldZ\",\"host\":\"",
		INTP_SCHEMA_VERSION_STR, tsbuf, sample->ts.tv_nsec / 1000000L);
	jsonl_escape(f, sample->host ? sample->host : "");
	fprintf(f, "\",\"metrics\":{");

	bool first = true;

	for (size_t i = 0; i < sample->values_len; i++) {
		const struct intp_metric_value *v = &sample->values[i];

		if (v->kind == INTP_VALUE_NONE)
			continue; /* unavailable = omitted */
		fprintf(f, "%s\"", first ? "" : ",");
		jsonl_escape(f, v->name);
		fprintf(f, "\":");
		switch (v->kind) {
		case INTP_VALUE_DOUBLE:
			if (isnan(v->d))
				fputs("null", f);
			else
				fprintf(f, "%.6g", v->d);
			break;
		case INTP_VALUE_INT64:
			fprintf(f, "%lld", (long long)v->i64);
			break;
		case INTP_VALUE_UINT64:
			fprintf(f, "%llu", (unsigned long long)v->u64);
			break;
		default:
			fputs("null", f);
			break;
		}
		first = false;
	}
	fprintf(f, "}}\n");
	return 0;
}

static void jsonl_close(struct intp_plugin_ctx *ctx)
{
	(void)ctx;
	if (g_out) {
		fclose(g_out);
		g_out = NULL;
	}
}

const struct intp_output_vtable intp_output_jsonl = {
	.format_name = "jsonl",
	.open = jsonl_open,
	.emit = jsonl_emit,
	.close = jsonl_close,
};
