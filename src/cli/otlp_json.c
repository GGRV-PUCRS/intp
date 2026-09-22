/* SPDX-License-Identifier: MIT */
/*
 * otlp_json.c — OTLP/JSON metrics serializer (see otlp_json.h).
 */

#include "otlp_json.h"

#include <inttypes.h>
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- growable string buffer ---- */

struct buf {
	char *p;
	size_t len;
	size_t cap;
	int err;
};

static void buf_appendf(struct buf *b, const char *fmt, ...)
{
	if (b->err)
		return;
	for (;;) {
		va_list ap;

		va_start(ap, fmt);
		int w = vsnprintf(b->p + b->len, b->cap - b->len, fmt, ap);

		va_end(ap);
		if (w < 0) {
			b->err = 1;
			return;
		}
		if ((size_t)w < b->cap - b->len) {
			b->len += (size_t)w;
			return;
		}
		size_t need = b->len + (size_t)w + 1;
		size_t ncap = b->cap ? b->cap * 2 : 4096;

		if (ncap < need)
			ncap = need;
		char *np = realloc(b->p, ncap);

		if (!np) {
			b->err = 1;
			return;
		}
		b->p = np;
		b->cap = ncap;
	}
}

/* Append a JSON string literal (quoted + escaped). */
static void buf_json_str(struct buf *b, const char *s)
{
	if (b->err)
		return;
	buf_appendf(b, "\"");
	for (const char *c = s ? s : ""; *c; c++) {
		switch (*c) {
		case '"':
			buf_appendf(b, "\\\"");
			break;
		case '\\':
			buf_appendf(b, "\\\\");
			break;
		case '\n':
			buf_appendf(b, "\\n");
			break;
		case '\t':
			buf_appendf(b, "\\t");
			break;
		case '\r':
			buf_appendf(b, "\\r");
			break;
		default:
			if ((unsigned char)*c < 0x20)
				buf_appendf(b, "\\u%04x", (unsigned)*c);
			else
				buf_appendf(b, "%c", *c);
			break;
		}
	}
	buf_appendf(b, "\"");
}

/* intp unit -> UCUM unit string (OTLP `unit`). */
static const char *ucum(enum intp_unit u)
{
	switch (u) {
	case INTP_UNIT_RATIO:
		return "1";
	case INTP_UNIT_PERCENT:
		return "%";
	case INTP_UNIT_BYTES:
		return "By";
	case INTP_UNIT_BYTES_PER_SECOND:
		return "By/s";
	case INTP_UNIT_OPS_PER_SECOND:
		return "{operation}/s";
	case INTP_UNIT_NANOSECONDS:
		return "ns";
	case INTP_UNIT_MICROSECONDS:
		return "us";
	case INTP_UNIT_MILLISECONDS:
		return "ms";
	case INTP_UNIT_SECONDS:
		return "s";
	case INTP_UNIT_COUNT:
		return "{count}";
	case INTP_UNIT_UNSPECIFIED:
	default:
		return "";
	}
}

static void emit_attr(struct buf *b, const char *key, const char *value,
		      int first)
{
	buf_appendf(b, "%s{\"key\":", first ? "" : ",");
	buf_json_str(b, key);
	buf_appendf(b, ",\"value\":{\"stringValue\":");
	buf_json_str(b, value);
	buf_appendf(b, "}}");
}

/* One data point (timeUnixNano string + the typed value). */
static void emit_datapoint(struct buf *b, uint64_t t_ns,
			   const struct intp_metric_value *v)
{
	buf_appendf(b, "{\"timeUnixNano\":\"%" PRIu64 "\",", t_ns);
	switch (v->kind) {
	case INTP_VALUE_DOUBLE:
		/* %.9g round-trips a double; OTLP asDouble is a JSON number. */
		buf_appendf(b, "\"asDouble\":%.9g", v->d);
		break;
	case INTP_VALUE_INT64:
		buf_appendf(b, "\"asInt\":\"%" PRId64 "\"", v->i64);
		break;
	case INTP_VALUE_UINT64: {
		/* OTLP asInt is sint64; saturate values above INT64_MAX. */
		uint64_t u = v->u64;
		int64_t s = u > (uint64_t)INT64_MAX ? INT64_MAX : (int64_t)u;

		buf_appendf(b, "\"asInt\":\"%" PRId64 "\"", s);
		break;
	}
	default:
		buf_appendf(b, "\"asInt\":\"0\"");
		break;
	}
	buf_appendf(b, "}");
}

char *otlp_json_build(const struct intp_sample *sample,
		      const struct otlp_kv *res, size_t n_res)
{
	struct buf b = {0};
	uint64_t t_ns = (uint64_t)sample->ts.tv_sec * 1000000000ull +
			(uint64_t)sample->ts.tv_nsec;

	buf_appendf(&b,
		    "{\"resourceMetrics\":[{\"resource\":{\"attributes\":[");
	for (size_t i = 0; i < n_res; i++)
		emit_attr(&b, res[i].key, res[i].value, i == 0);
	buf_appendf(&b, "]},\"scopeMetrics\":[{\"scope\":{\"name\":\"intp\"},"
			"\"metrics\":[");

	int first = 1;

	for (size_t i = 0; i < sample->values_len; i++) {
		const struct intp_metric_value *v = &sample->values[i];

		if (v->kind == INTP_VALUE_NONE)
			continue; /* unavailable -> omitted */
		if (v->kind == INTP_VALUE_DOUBLE && isnan(v->d))
			continue;

		buf_appendf(&b, "%s{\"name\":", first ? "" : ",");
		buf_json_str(&b, v->name);
		buf_appendf(&b, ",\"unit\":");
		buf_json_str(&b, ucum(v->unit));

		if (v->unit == INTP_UNIT_COUNT) {
			/* Monotonic cumulative Sum (aggregationTemporality=2). */
			buf_appendf(&b,
				    ",\"sum\":{\"aggregationTemporality\":2,"
				    "\"isMonotonic\":true,\"dataPoints\":[");
			emit_datapoint(&b, t_ns, v);
			buf_appendf(&b, "]}}");
		} else {
			buf_appendf(&b, ",\"gauge\":{\"dataPoints\":[");
			emit_datapoint(&b, t_ns, v);
			buf_appendf(&b, "]}}");
		}
		first = 0;
	}

	buf_appendf(&b, "]}]}]}");

	if (b.err) {
		free(b.p);
		return NULL;
	}
	return b.p ? b.p : strdup("{}");
}
