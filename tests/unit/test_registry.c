/* SPDX-License-Identifier: MIT */
/*
 * test_registry.c — backend registry mechanics: linker-section
 * collection, priority ordering, operator disables, probe-driven
 * selection, init fall-through, unavailable marking.
 *
 * The fake backends below register into the same "intp_backends"
 * section as the real in-tree ones (this binary links the internal
 * static archive with link_whole, so both sets are visible). The
 * fakes back the "netp" metric, which has no real backend in the
 * Phase-1 tree; cpu has the real procfs chain.
 */

#include <errno.h>
#include <string.h>

#include "../../src/core/core.h"
#include "intp_test.h"

static int fake_hi_probe_rc;
static int fake_hi_init_rc;

static int fake_hi_probe(void)
{
	return fake_hi_probe_rc;
}

static int fake_hi_init(void)
{
	return fake_hi_init_rc;
}

static int fake_hi_read(struct intp_core_sample *out, double interval_sec)
{
	(void)interval_sec;
	out->value = 42.0;
	out->status = INTP_CORE_OK;
	out->backend_id = "fake_hi";
	out->note = NULL;
	return 0;
}

static void cleanup_noop(void)
{
}

static const struct intp_core_backend fake_hi = {
	.metric = "netp",
	.id = "fake_hi",
	.description = "test backend, high priority",
	.model = "test",
	.scope = "system",
	.requires = "fake_hi probe must pass",
	.priority = INTP_PRIORITY_HIGHEST,
	.probe = fake_hi_probe,
	.init = fake_hi_init,
	.read = fake_hi_read,
	.cleanup = cleanup_noop,
};

static int probe_ok(void)
{
	return 0;
}

static int init_ok(void)
{
	return 0;
}

static int fake_lo_read(struct intp_core_sample *out, double interval_sec)
{
	(void)interval_sec;
	out->value = 7.0;
	out->status = INTP_CORE_DEGRADED;
	out->backend_id = "fake_lo";
	out->note = NULL;
	return 0;
}

/* HIGH outranks every real in-tree pure-C backend (MEDIUM and below),
 * so fall-through from fake_hi lands here deterministically regardless
 * of which real backends probe successfully on the host. */
static const struct intp_core_backend fake_lo = {
	.metric = "netp",
	.id = "fake_lo",
	.description = "test backend, below fake_hi but above the real ones",
	.model = "test",
	.scope = "system",
	.requires = "always available",
	.priority = INTP_PRIORITY_HIGH,
	.probe = probe_ok,
	.init = init_ok,
	.read = fake_lo_read,
	.cleanup = cleanup_noop,
};

INTP_BACKEND_REGISTER(fake_hi);
INTP_BACKEND_REGISTER(fake_lo);

static struct intp_core_selection *find_sel(struct intp_ctx *ctx,
					    const char *metric)
{
	for (size_t m = 0; m < INTP_CORE_METRICS_COUNT; m++) {
		if (strcmp(ctx->sel[m].metric, metric) == 0)
			return &ctx->sel[m];
	}
	return NULL;
}

int main(void)
{
	INTP_TEST_BEGIN("core/registry");

	/* --- selection prefers the higher-priority passing backend --- */
	fake_hi_probe_rc = 0;
	fake_hi_init_rc = 0;

	struct intp_ctx *ctx = NULL;

	INTP_EXPECT_EQ(intp_open(NULL, &ctx), 0);
	INTP_EXPECT_NOT_NULL(ctx);

	struct intp_core_selection *netp = find_sel(ctx, "netp");

	INTP_EXPECT_NOT_NULL(netp);
	INTP_EXPECT_NOT_NULL(netp->active);
	INTP_EXPECT_STREQ(netp->active->id, "fake_hi");
	INTP_EXPECT(netp->n_candidates >= 2);

	struct intp_core_selection *cpu = find_sel(ctx, "cpu");

	INTP_EXPECT_NOT_NULL(cpu->active);
	INTP_EXPECT_STREQ(cpu->active->id, "procfs_system");

	/* Disabling every backend of a metric marks it unavailable. */
	{
		const char *all_nets_off[] = {
			"nets:procfs_softirq",
			"nets:procfs_throughput",
			NULL,
		};
		struct intp_options opts_off = {
			.disabled_backends = all_nets_off,
		};
		struct intp_ctx *ctx_off = NULL;

		intp_close(ctx);
		INTP_EXPECT_EQ(intp_open(&opts_off, &ctx_off), 0);

		struct intp_core_selection *nets = find_sel(ctx_off, "nets");

		INTP_EXPECT(nets->active == NULL);
		INTP_EXPECT(nets->n_rejected >= 2);
		intp_close(ctx_off);
		INTP_EXPECT_EQ(intp_open(NULL, &ctx), 0);
	}

	/* Second concurrent open is refused (singleton target state). */
	struct intp_ctx *ctx2 = NULL;

	INTP_EXPECT_EQ(intp_open(NULL, &ctx2), -EBUSY);

	intp_close(ctx);

	/* --- probe failure falls through to the next candidate --- */
	fake_hi_probe_rc = -1;
	INTP_EXPECT_EQ(intp_open(NULL, &ctx), 0);
	netp = find_sel(ctx, "netp");
	INTP_EXPECT_NOT_NULL(netp->active);
	INTP_EXPECT_STREQ(netp->active->id, "fake_lo");
	INTP_EXPECT(netp->n_rejected >= 1);
	INTP_EXPECT_STREQ(netp->rejected[0].reason, "fake_hi probe must pass");
	intp_close(ctx);

	/* --- init failure falls through (probe ok, init fails) --- */
	fake_hi_probe_rc = 0;
	fake_hi_init_rc = -1;
	INTP_EXPECT_EQ(intp_open(NULL, &ctx), 0);
	netp = find_sel(ctx, "netp");
	INTP_EXPECT_NOT_NULL(netp->active);
	INTP_EXPECT_STREQ(netp->active->id, "fake_lo");
	intp_close(ctx);
	fake_hi_init_rc = 0;

	/* --- operator disable skips a backend (bare id form) --- */
	const char *disabled[] = {"fake_hi", NULL};
	struct intp_options opts = {
		.disabled_backends = disabled,
	};

	INTP_EXPECT_EQ(intp_open(&opts, &ctx), 0);
	netp = find_sel(ctx, "netp");
	INTP_EXPECT_STREQ(netp->active->id, "fake_lo");
	intp_close(ctx);

	/* --- metric:id disable form --- */
	const char *disabled2[] = {"netp:fake_hi", NULL};
	struct intp_options opts2 = {
		.disabled_backends = disabled2,
	};

	INTP_EXPECT_EQ(intp_open(&opts2, &ctx), 0);
	netp = find_sel(ctx, "netp");
	INTP_EXPECT_STREQ(netp->active->id, "fake_lo");
	intp_close(ctx);

	return INTP_TEST_END();
}
