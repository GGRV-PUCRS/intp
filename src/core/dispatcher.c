/* SPDX-License-Identifier: MIT */
/*
 * dispatcher.c — the sampling loop: absolute-deadline cadence, per-
 * metric reads through the registry, sample buffer management.
 *
 * Cadence ported from intp-comparison:
 *   variants/v2.1-c-abi-cgroup/intp-hybrid.c @ 2e87b30fc762
 * (clock_nanosleep TIMER_ABSTIME with timespec_add_sec) so long runs
 * do not drift.
 *
 * Value semantics (ADR-0010): backends report percent [0,100] — the
 * engines' native unit, preserved verbatim from intp-comparison. The
 * public sample carries the seven core metrics as RATIO [0,1]
 * (value / 100), matching ADR-0002's JSONL example and the
 * `intp_<metric>_ratio` Prometheus naming. tsv-legacy-v1 multiplies
 * back by 100 at the sink. An unavailable metric is appended with
 * kind INTP_VALUE_NONE — sinks must omit it, never write 0.
 */

#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../plugins/portable-metrics/portable.h"
#include "core.h"

static void timespec_add_ms(struct timespec *ts, uint32_t ms)
{
	ts->tv_sec += ms / 1000u;
	ts->tv_nsec += (long)(ms % 1000u) * 1000000L;
	if (ts->tv_nsec >= 1000000000L) {
		ts->tv_sec += ts->tv_nsec / 1000000000L;
		ts->tv_nsec %= 1000000000L;
	}
}

int intp_core_buf_append(struct intp_sample_buffer *buf, const char *name,
			 enum intp_unit unit, enum intp_value_kind kind,
			 double d, int64_t i64, uint64_t u64)
{
	if (!buf || !name)
		return -EINVAL;

	/* Replace an existing entry with the same name (one value per
	 * metric per tick). */
	struct intp_metric_value *slot = NULL;

	for (size_t i = 0; i < buf->len; i++) {
		if (buf->vals[i].name && strcmp(buf->vals[i].name, name) == 0) {
			slot = &buf->vals[i];
			break;
		}
	}
	if (!slot) {
		if (buf->len == buf->cap) {
			size_t ncap = buf->cap ? buf->cap * 2 : 16;
			struct intp_metric_value *nv =
				realloc(buf->vals, ncap * sizeof(*nv));

			if (!nv)
				return -ENOMEM;
			buf->vals = nv;
			buf->cap = ncap;
		}
		slot = &buf->vals[buf->len++];
	}

	slot->name = name;
	slot->unit = unit;
	slot->kind = kind;
	switch (kind) {
	case INTP_VALUE_DOUBLE:
		slot->d = d;
		break;
	case INTP_VALUE_INT64:
		slot->i64 = i64;
		break;
	case INTP_VALUE_UINT64:
		slot->u64 = u64;
		break;
	case INTP_VALUE_NONE:
	default:
		slot->d = 0.0;
		break;
	}
	return 0;
}

/**
 * intp_core_sample — block until the next deadline, then read every
 * core metric in the frozen order (netp nets blk mbw llcmr llcocc cpu)
 * and publish the buffer through *out.
 */
int intp_core_sample(struct intp_ctx *ctx, struct intp_sample *out)
{
	if (!ctx || !out)
		return -EINVAL;
	if (ctx->n_active == 0)
		return -ENODEV;

	if (!ctx->deadline_armed) {
		clock_gettime(CLOCK_MONOTONIC, &ctx->next_deadline);
		timespec_add_ms(&ctx->next_deadline, ctx->opts.interval_ms);
		ctx->deadline_armed = true;
	}

	int rc = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME,
				 &ctx->next_deadline, NULL);
	if (rc == EINTR)
		return -EINTR;
	if (rc != 0)
		return -rc;
	timespec_add_ms(&ctx->next_deadline, ctx->opts.interval_ms);

	double interval_sec = (double)ctx->opts.interval_ms / 1000.0;

	ctx->buf.len = 0;
	for (size_t m = 0; m < INTP_CORE_METRICS_COUNT; m++) {
		struct intp_core_sample s;

		intp_core_read_metric(&ctx->sel[m], &s, interval_sec);

		if (s.status == INTP_CORE_UNAVAILABLE || isnan(s.value)) {
			intp_core_buf_append(&ctx->buf,
					     intp_core_metric_names[m],
					     INTP_UNIT_RATIO, INTP_VALUE_NONE,
					     0.0, 0, 0);
			continue;
		}
		intp_core_buf_append(&ctx->buf, intp_core_metric_names[m],
				     INTP_UNIT_RATIO, INTP_VALUE_DOUBLE,
				     s.value / 100.0, 0, 0);
	}

	/* system.portable.* (ADR-0009), appended AFTER the seven core
	 * metrics so tsv-legacy-v1's by-name rendering of the frozen
	 * columns is unaffected. Providers report the upstream scale;
	 * convert here: percent → ratio (/100), membw_est MB/s →
	 * bytes/s (×1e6), psp/idle_preempt events/s pass through. */
	if (ctx->portable_enabled) {
		for (size_t i = 0; i < intp_portable_count(); i++) {
			struct intp_core_sample s;
			enum intp_unit unit = intp_portable_unit(i);
			const char *name = intp_portable_name(i);

			intp_portable_read(i, &s, interval_sec);

			if (s.status == INTP_CORE_UNAVAILABLE ||
			    isnan(s.value)) {
				intp_core_buf_append(&ctx->buf, name, unit,
						     INTP_VALUE_NONE, 0.0, 0,
						     0);
				continue;
			}

			double v = s.value;

			if (unit == INTP_UNIT_RATIO)
				v /= 100.0;
			else if (unit == INTP_UNIT_BYTES_PER_SECOND)
				v *= 1e6;
			intp_core_buf_append(&ctx->buf, name, unit,
					     INTP_VALUE_DOUBLE, v, 0, 0);
		}
	}

	clock_gettime(CLOCK_REALTIME, &ctx->sample.ts);
	ctx->sample.host = ctx->host;
	ctx->sample.values = ctx->buf.vals;
	ctx->sample.values_len = ctx->buf.len;

	*out = ctx->sample;
	return 0;
}
