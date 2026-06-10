/* SPDX-License-Identifier: MIT */
/*
 * test_dispatcher.c — sampling pipeline: ratio conversion, frozen
 * metric order, unavailable metrics carried as INTP_VALUE_NONE, and
 * the sample-buffer append helpers.
 */

#include "intp_test.h"

#include "../../src/core/core.h"

#include <errno.h>
#include <math.h>
#include <string.h>

/* A deterministic backend for "mbw" so the dispatcher has a known
 * value to publish (50% → 0.5 ratio). */
static int fifty_probe(void)
{
	return 0;
}

static int fifty_init(void)
{
	return 0;
}

static int fifty_read(struct intp_core_sample *out, double interval_sec)
{
	(void)interval_sec;
	out->value = 50.0;
	out->status = INTP_CORE_OK;
	out->backend_id = "fake_fifty";
	out->note = NULL;
	return 0;
}

static void fifty_cleanup(void)
{
}

static const struct intp_core_backend fake_fifty = {
	.metric = "mbw",
	.id = "fake_fifty",
	.description = "test backend returning 50%",
	.model = "test",
	.scope = "system",
	.requires = "always available",
	.priority = INTP_PRIORITY_HIGHEST,
	.probe = fifty_probe,
	.init = fifty_init,
	.read = fifty_read,
	.cleanup = fifty_cleanup,
};

INTP_BACKEND_REGISTER(fake_fifty);

int main(void)
{
	INTP_TEST_BEGIN("core/dispatcher");

	/* --- sample buffer helpers --- */
	struct intp_sample_buffer buf = { 0 };

	INTP_EXPECT_EQ(intp_sample_append_double(&buf, "a.b", INTP_UNIT_RATIO,
						 0.25), 0);
	INTP_EXPECT_EQ(intp_sample_append_unavailable(&buf, "a.c"), 0);
	INTP_EXPECT_EQ((int)buf.len, 2);
	INTP_EXPECT_EQ((int)buf.vals[0].kind, (int)INTP_VALUE_DOUBLE);
	INTP_EXPECT_EQ((int)buf.vals[1].kind, (int)INTP_VALUE_NONE);

	/* Re-appending an existing name replaces, not duplicates. */
	INTP_EXPECT_EQ(intp_sample_append_double(&buf, "a.b", INTP_UNIT_RATIO,
						 0.75), 0);
	INTP_EXPECT_EQ((int)buf.len, 2);
	INTP_EXPECT(buf.vals[0].d > 0.74 && buf.vals[0].d < 0.76);
	free(buf.vals);

	/* --- end-to-end sample through the dispatcher --- */
	struct intp_options opts = {
		.interval_ms = 10,	/* keep the test fast */
	};
	struct intp_ctx *ctx = NULL;

	INTP_EXPECT_EQ(intp_open(&opts, &ctx), 0);

	struct intp_sample s;

	INTP_EXPECT_EQ(intp_sample(ctx, &s), 0);

	/* All seven core metrics appear, in the frozen order. */
	INTP_EXPECT_EQ((int)s.values_len, INTP_CORE_METRICS_COUNT);
	for (size_t i = 0; i < INTP_CORE_METRICS_COUNT; i++)
		INTP_EXPECT_STREQ(s.values[i].name,
				  intp_core_metric_names[i]);

	/* mbw backed by the fake: 50% → ratio 0.5. */
	const struct intp_metric_value *mbw =
		intp_sample_find(&s, INTP_METRIC_MBW);

	INTP_EXPECT_NOT_NULL(mbw);
	INTP_EXPECT_EQ((int)mbw->kind, (int)INTP_VALUE_DOUBLE);
	INTP_EXPECT_EQ((int)mbw->unit, (int)INTP_UNIT_RATIO);
	INTP_EXPECT(mbw->d > 0.49 && mbw->d < 0.51);

	/* cpu backed by the real procfs backend: a valid ratio. */
	const struct intp_metric_value *cpu =
		intp_sample_find(&s, INTP_METRIC_CPU);

	INTP_EXPECT_NOT_NULL(cpu);
	INTP_EXPECT_EQ((int)cpu->kind, (int)INTP_VALUE_DOUBLE);
	INTP_EXPECT(cpu->d >= 0.0 && cpu->d <= 1.0);

	/* blk has no Phase-1 backend: present in the array but NONE. */
	const struct intp_metric_value *blk = NULL;

	for (size_t i = 0; i < s.values_len; i++) {
		if (strcmp(s.values[i].name, INTP_METRIC_BLK) == 0)
			blk = &s.values[i];
	}
	INTP_EXPECT_NOT_NULL(blk);
	INTP_EXPECT_EQ((int)blk->kind, (int)INTP_VALUE_NONE);

	/* host string is populated. */
	INTP_EXPECT_NOT_NULL(s.host);
	INTP_EXPECT(strlen(s.host) > 0);

	intp_close(ctx);

	return INTP_TEST_END();
}
