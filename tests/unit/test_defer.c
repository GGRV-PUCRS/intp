/* SPDX-License-Identifier: MIT */
/*
 * test_defer.c — the AUTO-engine defer rules of the real eBPF backends
 * (ADR-0013): for a per-PID target the eBPF cpu, llcmr and netp backends
 * step aside for the per-PID stable ones, and inside a KVM guest the eBPF
 * cpu backend steps aside for procfs. --engine=ebpf ignores both rules.
 *
 * The rules are evaluated before the probe, so their rejection reasons are
 * recorded whether or not eBPF can load here. The guest case marks the
 * cached detection result as ENV_VM, since the test host is bare metal.
 * Builds without eBPF have no eBPF backend to defer: the test SKIPs.
 */

#include <errno.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>

#include "../../src/core/core.h"
#include "../../src/core/detect.h"
#include "intp_test.h"

static struct intp_core_selection *find_sel(struct intp_ctx *ctx,
					    const char *metric)
{
	for (size_t m = 0; m < INTP_CORE_METRICS_COUNT; m++)
		if (strcmp(ctx->sel[m].metric, metric) == 0)
			return &ctx->sel[m];
	return NULL;
}

/* Rejection reason recorded for backend `id`, or NULL. */
static const char *reason_for(const struct intp_core_selection *sel,
			      const char *id)
{
	for (int r = 0; r < sel->n_rejected; r++)
		if (strcmp(sel->rejected[r].backend->id, id) == 0)
			return sel->rejected[r].reason;
	return NULL;
}

static bool mentions(const char *reason, const char *word)
{
	return reason && strstr(reason, word) != NULL;
}

int main(void)
{
	INTP_TEST_BEGIN("core/defer");

	struct intp_ctx *ctx = NULL;
	struct intp_core_extras ex = {0};

	INTP_EXPECT_EQ(intp_core_open_ex(NULL, &ex, &ctx), 0);

	bool have_ebpf = false;
	const struct intp_core_selection *cpu = find_sel(ctx, "cpu");

	for (int i = 0; i < cpu->n_candidates; i++)
		if (strcmp(cpu->candidates[i]->id, "ebpf_sched") == 0)
			have_ebpf = true;
	intp_close(ctx);
	if (!have_ebpf)
		return 77; /* -Dwith_ebpf=disabled */

	/* --- per-PID target: cpu, llcmr and netp defer; nets/blk do not --- */
	struct intp_options pid_opts = {
		.target = {.kind = INTP_TARGET_PID, .pid = getpid()},
	};

	INTP_EXPECT_EQ(intp_core_open_ex(&pid_opts, &ex, &ctx), 0);
	INTP_EXPECT(mentions(reason_for(find_sel(ctx, "cpu"), "ebpf_sched"),
			     "per-PID"));
	INTP_EXPECT(mentions(reason_for(find_sel(ctx, "llcmr"), "ebpf_perf"),
			     "per-PID"));
	INTP_EXPECT(mentions(reason_for(find_sel(ctx, "netp"), "ebpf_bytes"),
			     "per-PID"));
	INTP_EXPECT(!mentions(reason_for(find_sel(ctx, "nets"), "ebpf_softirq"),
			      "per-PID"));
	INTP_EXPECT_NOT_NULL(find_sel(ctx, "cpu")->active);
	INTP_EXPECT_STREQ(find_sel(ctx, "cpu")->active->id, "procfs_pid");
	intp_close(ctx);

	/* --- the same target under --engine=ebpf: no defer --- */
	ex.engine = INTP_ENGINE_EBPF;
	INTP_EXPECT_EQ(intp_core_open_ex(&pid_opts, &ex, &ctx), 0);
	INTP_EXPECT(!mentions(reason_for(find_sel(ctx, "cpu"), "ebpf_sched"),
			      "per-PID"));
	intp_close(ctx);
	ex.engine = INTP_ENGINE_AUTO;

	/* --- inside a KVM guest: only the eBPF cpu backend defers --- */
	/* detect_cached() hands out the cached result read-only; the test
	 * edits it in place to stand in for a guest. */
	const system_capabilities_t *cached = detect_cached();
	system_capabilities_t *caps;

	memcpy(&caps, &cached, sizeof(caps));
	exec_env_t real_env = caps->env;

	caps->env = ENV_VM;
	INTP_EXPECT_EQ(intp_core_open_ex(NULL, &ex, &ctx), 0);
	INTP_EXPECT(mentions(reason_for(find_sel(ctx, "cpu"), "ebpf_sched"),
			     "KVM guest"));
	INTP_EXPECT(!mentions(reason_for(find_sel(ctx, "llcmr"), "ebpf_perf"),
			      "KVM guest"));
	INTP_EXPECT_NOT_NULL(find_sel(ctx, "cpu")->active);
	INTP_EXPECT_STREQ(find_sel(ctx, "cpu")->active->id, "procfs_system");
	intp_close(ctx);

	/* --engine=ebpf keeps the eBPF cpu backend in a guest. */
	ex.engine = INTP_ENGINE_EBPF;
	INTP_EXPECT_EQ(intp_core_open_ex(NULL, &ex, &ctx), 0);
	INTP_EXPECT(!mentions(reason_for(find_sel(ctx, "cpu"), "ebpf_sched"),
			      "KVM guest"));
	intp_close(ctx);

	/* --force-backend overrides the guest rule under AUTO. */
	static const struct intp_core_force force_cpu = {
		.metric = "cpu",
		.backend_id = "ebpf_sched",
	};

	ex.engine = INTP_ENGINE_AUTO;
	ex.forces = &force_cpu;
	ex.n_forces = 1;
	INTP_EXPECT_EQ(intp_core_open_ex(NULL, &ex, &ctx), 0);
	if (intp_probe_ebpf() == INTP_PROBE_OK) {
		INTP_EXPECT_NOT_NULL(find_sel(ctx, "cpu")->active);
		INTP_EXPECT_STREQ(find_sel(ctx, "cpu")->active->id,
				  "ebpf_sched");
	}
	intp_close(ctx);

	caps->env = real_env;
	return INTP_TEST_END();
}
