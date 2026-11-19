/* SPDX-License-Identifier: MIT */
/*
 * plugin_loader.c — dlopen-based plugin loader (ADR-0004).
 *
 * Flow per plugin: trust-gate the dir + .so -> dlopen(RTLD_NOW|RTLD_LOCAL)
 * -> dlsym(intp_plugin_descriptor) -> validate ABI range + kind +
 * requirements (BEFORE running plugin code) -> reject frozen-core metric
 * names -> optional intp_plugin_self_test() -> init() + register.
 *
 * Trust model (the dlopen of a .so is arbitrary code execution): a plugin
 * directory and the .so itself must be owned by root (or, for user dirs
 * like --plugin-dir / $XDG_DATA_HOME, by the invoking user) and must not
 * be group- or other-writable. Under a setuid binary, user-controlled
 * dirs are skipped entirely. The self-test is a smoke gate, not a sandbox.
 */

#include "plugin_loader.h"

#include <sys/stat.h>

#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "core.h"

#define SO_PREFIX "intp-plugin-"
#define SCAN_MAX  64

/* Log at -v (level 1) and up; default runs stay quiet. */
static void plog(int min_level, const char *fmt, ...)
{
	if (intp_core_log_level() < min_level)
		return;

	va_list ap;

	va_start(ap, fmt);
	fputs("intp: plugin: ", stderr);
	vfprintf(stderr, fmt, ap);
	fputc('\n', stderr);
	va_end(ap);
}

static bool name_in_list(const char *const *list, const char *name)
{
	if (!list)
		return false;
	for (; *list; list++)
		if (strcmp(*list, name) == 0)
			return true;
	return false;
}

static struct intp_plugin_set *set_of(struct intp_ctx *ctx)
{
	if (!ctx->plugins)
		ctx->plugins = calloc(1, sizeof(struct intp_plugin_set));
	return ctx->plugins;
}

static bool provider_loaded(struct intp_ctx *ctx, const char *name)
{
	struct intp_plugin_set *set = ctx->plugins;

	if (!set)
		return false;
	for (size_t i = 0; i < set->n_providers; i++)
		if (strcmp(set->providers[i].desc->name, name) == 0)
			return true;
	return false;
}

/* Writable bits the loader refuses. System dirs (root-owned) must not be
 * group- or other-writable. A user-supplied dir (--plugin-dir / $XDG) is
 * the user's responsibility, so only world-writable is refused there
 * (umask 002 makes a user's own private-group dirs group-writable). */
static mode_t forbidden_write(bool allow_euid)
{
	return allow_euid ? (mode_t)S_IWOTH : (mode_t)(S_IWGRP | S_IWOTH);
}

/* Owned by root, or (user dirs only) by us; not writable by others. */
static bool owner_ok(const struct stat *st, bool allow_euid)
{
	if (st->st_mode & forbidden_write(allow_euid))
		return false;
	if (st->st_uid == 0)
		return true;
	return allow_euid && st->st_uid == geteuid();
}

static bool requirements_ok(const struct intp_plugin_requirements *r)
{
	if (r->needs_btf && access("/sys/kernel/btf/vmlinux", R_OK) != 0)
		return false;
	if (r->needs_paths) {
		for (const char *const *p = r->needs_paths; *p; p++)
			if (access(*p, R_OK) != 0)
				return false;
	}
	/* min_kernel / vendor_filter checks are deferred (M4 follow-up);
	 * the conservative default is to load and let the plugin's own
	 * self_test / init fail closed on an unsupported host. */
	return true;
}

static void register_provider(struct intp_ctx *ctx, void *handle,
			      const struct intp_plugin_descriptor *desc,
			      const char *path)
{
	struct intp_plugin_set *set = set_of(ctx);

	if (!set || set->n_providers >= INTP_MAX_PLUGINS) {
		plog(1, "%s: too many plugins; skipping", path);
		dlclose(handle);
		return;
	}

	struct intp_plugin_ctx *pc = &set->providers[set->n_providers];

	memset(pc, 0, sizeof(*pc));
	pc->handle = handle;
	pc->desc = desc;
	snprintf(pc->origin, sizeof(pc->origin), "%s", path);

	if (desc->vt.metric_provider.init &&
	    desc->vt.metric_provider.init(pc) != 0) {
		plog(1, "%s: init() failed; skipping", path);
		dlclose(handle);
		return;
	}
	pc->initialized = true;
	set->n_providers++;
	plog(2, "loaded metric-provider '%s' (%s)", desc->name, path);
}

static void load_one(struct intp_ctx *ctx, const char *dir, const char *fname,
		     uid_t dir_owner, bool allow_euid,
		     const char *const *enabled, const char *const *disabled)
{
	char path[PATH_MAX];

	if (snprintf(path, sizeof(path), "%s/%s", dir, fname) >=
	    (int)sizeof(path))
		return;

	struct stat st;

	if (stat(path, &st) != 0)
		return;
	if (st.st_mode & forbidden_write(allow_euid)) {
		plog(1, "skip %s: writable by others", path);
		return;
	}
	if (st.st_uid != 0 && st.st_uid != dir_owner) {
		plog(1, "skip %s: owner does not match the plugin dir", path);
		return;
	}

	void *h = dlopen(path, RTLD_NOW | RTLD_LOCAL);

	if (!h) {
		plog(1, "dlopen %s: %s", path, dlerror());
		return;
	}

	const struct intp_plugin_descriptor *desc =
		dlsym(h, "intp_plugin_descriptor");

	if (!desc) {
		plog(1, "%s: no intp_plugin_descriptor symbol", path);
		dlclose(h);
		return;
	}

	/* Validate BEFORE running any plugin code beyond constructors. */
	if (!(desc->abi_min <= INTP_PLUGIN_ABI_VERSION &&
	      desc->abi_min <= desc->abi_max)) {
		plog(1, "%s: ABI v%u-%u incompatible (host v%u)", path,
		     desc->abi_min, desc->abi_max, INTP_PLUGIN_ABI_VERSION);
		dlclose(h);
		return;
	}
	if (desc->kind != INTP_PLUGIN_KIND_BACKEND &&
	    desc->kind != INTP_PLUGIN_KIND_METRIC_PROVIDER &&
	    desc->kind != INTP_PLUGIN_KIND_OUTPUT_SINK) {
		plog(1, "%s: unknown plugin kind %d", path, (int)desc->kind);
		dlclose(h);
		return;
	}
	if (!desc->name) {
		plog(1, "%s: descriptor has no name", path);
		dlclose(h);
		return;
	}
	if (name_in_list(disabled, desc->name)) {
		plog(2, "%s: disabled by request", desc->name);
		dlclose(h);
		return;
	}
	if (enabled && enabled[0] && !name_in_list(enabled, desc->name)) {
		dlclose(h);
		return; /* allowlist active and this isn't on it */
	}
	if (provider_loaded(ctx, desc->name)) {
		dlclose(h);
		return; /* first (highest-precedence dir) wins */
	}
	if (!requirements_ok(&desc->requires)) {
		plog(1, "%s: host requirements not met; skipping", desc->name);
		dlclose(h);
		return;
	}

	/* Protect the frozen seven: a plugin may never publish a
	 * system.interference.* metric. */
	if (desc->metrics) {
		for (const struct intp_metric_schema *m = desc->metrics;
		     m->name; m++) {
			if (strncmp(m->name, "system.interference.", 20) == 0) {
				plog(1,
				     "%s: refuses to publish frozen core "
				     "metric '%s'",
				     path, m->name);
				dlclose(h);
				return;
			}
		}
	}

	int (*self_test)(void) = dlsym(h, "intp_plugin_self_test");

	if (self_test && self_test() != 0) {
		plog(1, "%s: self-test failed; skipping", path);
		dlclose(h);
		return;
	}

	switch (desc->kind) {
	case INTP_PLUGIN_KIND_METRIC_PROVIDER:
		register_provider(ctx, h, desc, path);
		break;
	case INTP_PLUGIN_KIND_OUTPUT_SINK:
	case INTP_PLUGIN_KIND_BACKEND:
	default:
		/* Output-sink and backend plugins need, respectively, sink-ctx
		 * dispatch plumbing and frozen-core selection surgery; both are
		 * deferred past the metric_provider keystone (ADR-0004). */
		plog(1, "%s: only metric-provider plugins load today; skipping",
		     path);
		dlclose(h);
		break;
	}
}

static int cmp_name(const void *a, const void *b)
{
	return strcmp((const char *)a, (const char *)b);
}

static void scan_dir(struct intp_ctx *ctx, const char *dir, bool allow_euid,
		     const char *const *enabled, const char *const *disabled)
{
	struct stat st;

	if (!dir || !dir[0] || stat(dir, &st) != 0)
		return; /* missing dir: silent */
	if (!S_ISDIR(st.st_mode))
		return;
	if (!owner_ok(&st, allow_euid)) {
		plog(1, "skip plugin dir %s: untrusted owner/permissions", dir);
		return;
	}
	uid_t dir_owner = st.st_uid;

	DIR *dp = opendir(dir);

	if (!dp)
		return;

	static char names[SCAN_MAX][256];
	size_t n = 0;
	struct dirent *e;

	while ((e = readdir(dp)) != NULL && n < SCAN_MAX) {
		if (strncmp(e->d_name, SO_PREFIX, strlen(SO_PREFIX)) != 0)
			continue;
		const char *dot = strrchr(e->d_name, '.');

		if (!dot || strcmp(dot, ".so") != 0)
			continue;
		snprintf(names[n++], sizeof(names[0]), "%s", e->d_name);
	}
	closedir(dp);

	qsort(names, n, sizeof(names[0]), cmp_name); /* deterministic order */
	for (size_t i = 0; i < n; i++)
		load_one(ctx, dir, names[i], dir_owner, allow_euid, enabled,
			 disabled);
}

void intp_plugin_load_all(struct intp_ctx *ctx, const char *const *extra_dirs,
			  const char *const *enabled,
			  const char *const *disabled)
{
	/* A setuid intp must not load user-controlled plugins (classic
	 * privesc): skip --plugin-dir / $INTP_PLUGIN_DIR / $XDG dirs. */
	bool setuid = (getuid() != geteuid());

	if (!setuid && extra_dirs) {
		for (const char *const *d = extra_dirs; *d; d++)
			scan_dir(ctx, *d, true, enabled, disabled);
	}
	if (!setuid) {
		const char *env = getenv("INTP_PLUGIN_DIR");

		if (env && env[0]) {
			char *copy = strdup(env);

			if (copy) {
				char *save = NULL;

				for (char *tok = strtok_r(copy, ":", &save);
				     tok; tok = strtok_r(NULL, ":", &save))
					scan_dir(ctx, tok, true, enabled,
						 disabled);
				free(copy);
			}
		}
	}

	scan_dir(ctx, "/usr/local/lib/intp/plugins", false, enabled, disabled);
	scan_dir(ctx, "/usr/lib/intp/plugins", false, enabled, disabled);

	if (!setuid) {
		char xdg[PATH_MAX];
		const char *base = getenv("XDG_DATA_HOME");
		const char *home = getenv("HOME");

		xdg[0] = '\0';
		if (base && base[0])
			snprintf(xdg, sizeof(xdg), "%s/intp/plugins", base);
		else if (home && home[0])
			snprintf(xdg, sizeof(xdg),
				 "%s/.local/share/intp/plugins", home);
		if (xdg[0])
			scan_dir(ctx, xdg, true, enabled, disabled);
	}
}

void intp_plugin_merge_schema(struct intp_ctx *ctx)
{
	struct intp_plugin_set *set = ctx->plugins;

	if (!set)
		return;

	const size_t cap = INTP_CORE_METRICS_COUNT + INTP_CORE_PORTABLE_COUNT +
			   INTP_CORE_PLUGIN_METRICS_MAX;

	for (size_t i = 0; i < set->n_providers; i++) {
		const struct intp_metric_schema *m =
			set->providers[i].desc->metrics;

		for (; m && m->name; m++) {
			if (ctx->schema_len >= cap) {
				plog(1,
				     "schema full; dropping plugin metric %s",
				     m->name);
				return;
			}
			ctx->schema[ctx->schema_len++] = *m;
		}
	}
}

void intp_plugin_sample_providers(struct intp_ctx *ctx,
				  struct intp_sample_buffer *buf)
{
	struct intp_plugin_set *set = ctx->plugins;

	if (!set)
		return;
	for (size_t i = 0; i < set->n_providers; i++) {
		struct intp_plugin_ctx *pc = &set->providers[i];

		if (pc->faults >= 8)
			continue; /* quarantined */
		if (!pc->desc->vt.metric_provider.sample)
			continue;
		if (pc->desc->vt.metric_provider.sample(pc, buf) != 0) {
			if (++pc->faults == 8)
				plog(1,
				     "plugin '%s' quarantined after 8 faults",
				     pc->desc->name);
		} else {
			pc->faults = 0;
		}
	}
}

void intp_plugin_unload_all(struct intp_ctx *ctx)
{
	struct intp_plugin_set *set = ctx->plugins;

	if (!set)
		return;
	for (size_t i = 0; i < set->n_providers; i++) {
		struct intp_plugin_ctx *pc = &set->providers[i];

		if (pc->initialized && pc->desc->vt.metric_provider.shutdown)
			pc->desc->vt.metric_provider.shutdown(pc);
		if (pc->handle)
			dlclose(pc->handle);
	}
	free(set);
	ctx->plugins = NULL;
}
