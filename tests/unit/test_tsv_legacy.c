/* SPDX-License-Identifier: MIT */
/*
 * test_tsv_legacy.c — golden-format test for the tsv-legacy-v1 sink.
 *
 * Drives canned samples through the sink and byte-compares the
 * output. The format contract (ADR-0002/ADR-0010): seven tab-separated
 * "%.0f" percent values in the frozen order netp nets blk mbw llcmr
 * llcocc cpu, "--" for unavailable, newline-terminated, no header.
 * Also pins the ratio→percent round-trip: rendering ratio×100 must
 * agree with rendering the original engine percent for representative
 * values.
 */

#include <intp/metrics.h>
#include <intp/plugin.h>

#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "../../src/output/output.h"
#include "intp_test.h"

static struct intp_metric_value mk(const char *name, double ratio)
{
	struct intp_metric_value v = {
		.name = name,
		.unit = INTP_UNIT_RATIO,
		.kind = INTP_VALUE_DOUBLE,
	};

	v.d = ratio;
	return v;
}

static struct intp_metric_value mk_none(const char *name)
{
	struct intp_metric_value v = {
		.name = name,
		.unit = INTP_UNIT_RATIO,
		.kind = INTP_VALUE_NONE,
	};

	return v;
}

static int emit_to_string(const struct intp_sample *s, char *out, size_t outsz)
{
	const struct intp_output_vtable *sink =
		intp_output_find("tsv-legacy-v1");

	if (!sink)
		return -1;

	int fds[2];

	if (pipe(fds) != 0)
		return -1;
	if (sink->open(NULL, fds[1]) != 0) {
		close(fds[0]);
		close(fds[1]);
		return -1;
	}
	sink->emit(NULL, s);
	sink->close(NULL);
	close(fds[1]);

	ssize_t n = read(fds[0], out, outsz - 1);

	close(fds[0]);
	if (n < 0)
		return -1;
	out[n] = '\0';
	return 0;
}

int main(void)
{
	INTP_TEST_BEGIN("output/tsv-legacy-v1");

	/* Mixed availability, values chosen to exercise rounding. */
	struct intp_metric_value vals[7] = {
		mk(INTP_METRIC_NETP, 0.0),    /* "0"   */
		mk(INTP_METRIC_NETS, 0.0042), /* "0"   */
		mk(INTP_METRIC_BLK, 0.13),    /* "13"  */
		mk_none(INTP_METRIC_MBW),     /* "--"  */
		mk(INTP_METRIC_LLCMR, 0.426), /* "43"  */
		mk_none(INTP_METRIC_LLCOCC),  /* "--"  */
		mk(INTP_METRIC_CPU, 0.99),    /* "99"  */
	};
	struct intp_sample s = {
		.host = "test",
		.values = vals,
		.values_len = 7,
	};
	char out[256];

	INTP_EXPECT_EQ(emit_to_string(&s, out, sizeof(out)), 0);
	INTP_EXPECT_STREQ(out, "0\t0\t13\t--\t43\t--\t99\n");

	/* Order independence: the sink looks values up by name, so a
	 * permuted array renders identically. */
	struct intp_metric_value perm[7] = {
		vals[6], vals[4], vals[0], vals[5], vals[2], vals[1], vals[3],
	};
	struct intp_sample s2 = {
		.host = "test",
		.values = perm,
		.values_len = 7,
	};

	INTP_EXPECT_EQ(emit_to_string(&s2, out, sizeof(out)), 0);
	INTP_EXPECT_STREQ(out, "0\t0\t13\t--\t43\t--\t99\n");

	/* A missing entry (not in the array at all) renders "--". */
	struct intp_sample s3 = {
		.host = "test",
		.values = vals,
		.values_len = 3, /* only netp/nets/blk present */
	};

	INTP_EXPECT_EQ(emit_to_string(&s3, out, sizeof(out)), 0);
	INTP_EXPECT_STREQ(out, "0\t0\t13\t--\t--\t--\t--\n");

	/* Ratio→percent rendering agrees with rendering the original
	 * engine percent across a representative sweep. Exact .5
	 * percent values are excluded: there the "%.0f" half-even
	 * rounding can differ by 1 in the last digit because pct/100
	 * is not exactly representable (ADR-0010 rounding note) —
	 * engine values are counter quotients that never land on an
	 * exactly-representable half, so both paths agree on real
	 * data. */
	for (int p = 0; p <= 10000; p++) {
		if (p % 100 == 50)
			continue; /* exact .5 boundary, see above */

		double pct = (double)p / 100.0; /* 0.00 .. 100.00 */
		double ratio = pct / 100.0;
		char a[16], b[16];

		snprintf(a, sizeof(a), "%.0f", pct);
		snprintf(b, sizeof(b), "%.0f", ratio * 100.0);
		if (strcmp(a, b) != 0) {
			INTP_FAIL_("round-trip mismatch at pct=%g: %s vs %s",
				   pct, a, b);
			break;
		}
	}

	return INTP_TEST_END();
}
