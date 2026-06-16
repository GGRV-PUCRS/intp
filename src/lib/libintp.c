/* SPDX-License-Identifier: MIT */
/*
 * libintp.c — public ABI surface.
 *
 * Every exported symbol is defined here with explicit default
 * visibility; the implementations live in src/core/ (registry,
 * dispatcher, context lifecycle). Keeping the ABI surface in one file
 * keeps abidiff reviews focused.
 */

#include <intp/intp.h>
#include <intp/metrics.h>
#include <intp/plugin.h>
#include <intp/schema.h>

#include <errno.h>
#include <stddef.h>
#include <string.h>

#include "../core/core.h"
#include "../core/plugin_loader.h"

/* INTP_VERSION is already a quoted string literal (Meson injects it
 * via configuration_data.set_quoted), so we return it as-is. */
__attribute__((visibility("default"))) const char *intp_version_string(void)
{
	return INTP_VERSION;
}

__attribute__((visibility("default")))
const char *const intp_core_metric_names[INTP_CORE_METRICS_COUNT] = {
	INTP_METRIC_NETP,  INTP_METRIC_NETS,   INTP_METRIC_BLK, INTP_METRIC_MBW,
	INTP_METRIC_LLCMR, INTP_METRIC_LLCOCC, INTP_METRIC_CPU,
};

__attribute__((visibility("default"))) const char *
intp_unit_name(enum intp_unit u)
{
	switch (u) {
	case INTP_UNIT_UNSPECIFIED:
		return "unspecified";
	case INTP_UNIT_RATIO:
		return "ratio";
	case INTP_UNIT_PERCENT:
		return "percent";
	case INTP_UNIT_BYTES:
		return "bytes";
	case INTP_UNIT_BYTES_PER_SECOND:
		return "bytes_per_second";
	case INTP_UNIT_OPS_PER_SECOND:
		return "ops_per_second";
	case INTP_UNIT_NANOSECONDS:
		return "ns";
	case INTP_UNIT_MICROSECONDS:
		return "us";
	case INTP_UNIT_MILLISECONDS:
		return "ms";
	case INTP_UNIT_SECONDS:
		return "s";
	case INTP_UNIT_COUNT:
		return "count";
	}
	return "unspecified";
}

__attribute__((visibility("default"))) const struct intp_metric_value *
intp_sample_find(const struct intp_sample *sample, const char *name)
{
	if (!sample || !name)
		return NULL;
	for (size_t i = 0; i < sample->values_len; i++) {
		if (sample->values[i].name &&
		    strcmp(sample->values[i].name, name) == 0)
			return &sample->values[i];
	}
	return NULL;
}

__attribute__((visibility("default"))) const char *intp_strerror(int err)
{
	if (err >= 0)
		return "no error";
	return strerror(-err);
}

/* ------------------------------------------------------------------------ */
/* Lifecycle / sampling / schema — backed by src/core/.                      */
/* ------------------------------------------------------------------------ */

__attribute__((visibility("default"))) int
intp_open(const struct intp_options *opts, struct intp_ctx **out)
{
	return intp_core_open(opts, out);
}

__attribute__((visibility("default"))) void intp_close(struct intp_ctx *ctx)
{
	intp_core_close(ctx);
}

__attribute__((visibility("default"))) int intp_sample(struct intp_ctx *ctx,
						       struct intp_sample *out)
{
	return intp_core_sample(ctx, out);
}

__attribute__((visibility("default"))) int
intp_subscribe(struct intp_ctx *ctx, intp_sample_callback cb, void *user)
{
	if (!ctx || !cb)
		return -EINVAL;

	for (;;) {
		struct intp_sample s;
		int rc = intp_core_sample(ctx, &s);

		if (rc)
			return rc;
		if (cb(&s, user) != 0)
			return 0;
	}
}

__attribute__((visibility("default"))) int
intp_print_schema(struct intp_ctx *ctx, int fd)
{
	return intp_core_print_schema(ctx, fd);
}

/* Plugin sample-buffer helpers. Plugins do not link against libintp
 * directly, but they call back into these through the dispatcher. */
__attribute__((visibility("default"))) int
intp_sample_append_double(struct intp_sample_buffer *buf, const char *name,
			  enum intp_unit unit, double value)
{
	return intp_core_buf_append(buf, name, unit, INTP_VALUE_DOUBLE, value,
				    0, 0);
}

__attribute__((visibility("default"))) int
intp_sample_append_i64(struct intp_sample_buffer *buf, const char *name,
		       enum intp_unit unit, int64_t value)
{
	return intp_core_buf_append(buf, name, unit, INTP_VALUE_INT64, 0.0,
				    value, 0);
}

__attribute__((visibility("default"))) int
intp_sample_append_u64(struct intp_sample_buffer *buf, const char *name,
		       enum intp_unit unit, uint64_t value)
{
	return intp_core_buf_append(buf, name, unit, INTP_VALUE_UINT64, 0.0, 0,
				    value);
}

__attribute__((visibility("default"))) int
intp_sample_append_unavailable(struct intp_sample_buffer *buf, const char *name)
{
	return intp_core_buf_append(buf, name, INTP_UNIT_UNSPECIFIED,
				    INTP_VALUE_NONE, 0.0, 0, 0);
}

/* Plugin private-state accessors — backed by the loader's per-plugin
 * struct intp_plugin_ctx (src/core/plugin_loader.h). */
__attribute__((visibility("default"))) void *
intp_plugin_ctx_get_private(struct intp_plugin_ctx *ctx)
{
	return ctx ? ctx->priv : NULL;
}

__attribute__((visibility("default"))) void
intp_plugin_ctx_set_private(struct intp_plugin_ctx *ctx, void *priv)
{
	if (ctx)
		ctx->priv = priv;
}

/* Schema introspection. */
__attribute__((visibility("default"))) size_t
intp_schema_size(const struct intp_ctx *ctx)
{
	return ctx ? ctx->schema_len : 0;
}

__attribute__((visibility("default"))) const struct intp_metric_schema *
intp_schema_get(const struct intp_ctx *ctx, size_t index)
{
	if (!ctx || index >= ctx->schema_len)
		return NULL;
	return &ctx->schema[index];
}

__attribute__((visibility("default"))) const struct intp_metric_schema *
intp_schema_find(const struct intp_ctx *ctx, const char *name)
{
	if (!ctx || !name)
		return NULL;
	for (size_t i = 0; i < ctx->schema_len; i++) {
		if (strcmp(ctx->schema[i].name, name) == 0)
			return &ctx->schema[i];
	}
	return NULL;
}
