/* SPDX-License-Identifier: MIT */
/*
 * plugin_loader.h — internal plugin loader (ADR-0004).
 *
 * Concrete definition of the opaque struct intp_plugin_ctx, plus the
 * load/sample/unload entry points the core lifecycle drives. Not
 * installed; the public plugin ABI is include/intp/plugin.h.
 *
 * Scope (1.0): metric_provider and output_sink plugins. Backend plugins
 * (which would merge into the frozen-core selection path) are deferred.
 */

#ifndef INTP_CORE_PLUGIN_LOADER_H
#define INTP_CORE_PLUGIN_LOADER_H

#include <intp/plugin.h>

#include <limits.h>
#include <stdbool.h>
#include <stddef.h>

#define INTP_MAX_PLUGINS 32

struct intp_ctx;
struct intp_sample_buffer;

/* Concrete definition of plugin.h's forward-declared opaque ctx. One per
 * loaded plugin; passed to every vtable callback. */
struct intp_plugin_ctx {
	void *handle;				   /* dlopen handle */
	const struct intp_plugin_descriptor *desc; /* points into the .so */
	void *priv;				   /* set/get_private storage */
	char origin[PATH_MAX];			   /* .so path (diagnostics) */
	bool initialized;			   /* init() ran -> shutdown() */
	int faults;				   /* consecutive sample() faults */
};

struct intp_plugin_set {
	struct intp_plugin_ctx providers[INTP_MAX_PLUGINS]; /* metric_provider */
	size_t n_providers;
};

/* Discover, dlopen, validate, self-test and register every plugin on the
 * search path into ctx->plugins. Never fails the open: a bad/untrusted
 * plugin is skipped (and logged at -v). extra_dirs (--plugin-dir / opts.
 * plugin_dirs), enabled (--enable-plugin allowlist) and disabled
 * (--disable-plugin / opts.disabled_plugins) are NULL-terminated; any may
 * be NULL. */
void intp_plugin_load_all(struct intp_ctx *ctx, const char *const *extra_dirs,
			  const char *const *enabled,
			  const char *const *disabled);

/* Append loaded metric-provider schema fragments into ctx->schema,
 * bounded by the schema array (excess is logged and dropped). */
void intp_plugin_merge_schema(struct intp_ctx *ctx);

/* Run each loaded provider's sample() into buf (per tick, after core +
 * portable). A provider that faults repeatedly is quarantined. */
void intp_plugin_sample_providers(struct intp_ctx *ctx,
				  struct intp_sample_buffer *buf);

/* shutdown() + dlclose every plugin, then free ctx->plugins. */
void intp_plugin_unload_all(struct intp_ctx *ctx);

#endif /* INTP_CORE_PLUGIN_LOADER_H */
