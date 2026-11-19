/* SPDX-License-Identifier: MIT */
/*
 * test_jsonl_golden.c — structural golden test for the JSONL sink (the
 * default --format). Pins: the "target" object, host escaping, NONE
 * omission, NaN→null, and INT64/UINT64 rendering. The timestamp is
 * non-deterministic, so we assert on substrings rather than byte-equal.
 */

#include <intp/metrics.h>
#include <intp/plugin.h>

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "../../src/output/output.h"
#include "intp_test.h"

#define HAS(hay, needle)   INTP_EXPECT(strstr((hay), (needle)) != NULL)
#define HASNT(hay, needle) INTP_EXPECT(strstr((hay), (needle)) == NULL)

static int emit_to_string(const char *fmt, const struct intp_sample *s,
			  char *out, size_t outsz)
{
	const struct intp_output_vtable *sink = intp_output_find(fmt);

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
	INTP_TEST_BEGIN("output/jsonl-golden");

	struct intp_metric_value vals[5] = {0};

	vals[0].name = INTP_METRIC_CPU;
	vals[0].unit = INTP_UNIT_RATIO;
	vals[0].kind = INTP_VALUE_DOUBLE;
	vals[0].d = 0.66;

	vals[1].name = INTP_METRIC_BLK; /* unavailable -> omitted */
	vals[1].unit = INTP_UNIT_RATIO;
	vals[1].kind = INTP_VALUE_NONE;

	vals[2].name = "system.portable.psp"; /* uint64 counter */
	vals[2].unit = INTP_UNIT_COUNT;
	vals[2].kind = INTP_VALUE_UINT64;
	vals[2].u64 = 42;

	vals[3].name = INTP_METRIC_MBW; /* NaN -> null */
	vals[3].unit = INTP_UNIT_RATIO;
	vals[3].kind = INTP_VALUE_DOUBLE;
	vals[3].d = NAN;

	vals[4].name = "system.portable.steal"; /* int64 */
	vals[4].unit = INTP_UNIT_PERCENT;
	vals[4].kind = INTP_VALUE_INT64;
	vals[4].i64 = -7;

	struct intp_sample s = {
		.host = "ho\"st", /* the literal host is  ho"st  */
		.values = vals,
		.values_len = 5,
	};
	char out[1024];

	INTP_EXPECT_EQ(emit_to_string("jsonl", &s, out, sizeof(out)), 0);

	/* No ctx open in the unit test -> target is the system default. */
	HAS(out, "\"target\":{\"kind\":\"system\"}");
	HAS(out, "\"host\":\"ho\\\"st\""); /* host escaped */
	HAS(out, "\"system.interference.cpu\":0.66");
	HAS(out, "\"system.portable.psp\":42");
	HAS(out, "\"system.portable.steal\":-7");
	HAS(out, "\"system.interference.mbw\":null"); /* NaN -> null */
	HASNT(out, "system.interference.blk");	      /* NONE omitted */

	return INTP_TEST_END();
}
