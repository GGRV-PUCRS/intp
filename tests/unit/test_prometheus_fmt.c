/* SPDX-License-Identifier: MIT */
/*
 * test_prometheus_fmt.c — golden test for the one-shot --format
 * prometheus sink. Pins: per-metric HELP/TYPE, identity labels with
 * escaping, INT64/UINT64 rendering, counter (_total) vs gauge naming,
 * unit-suffix completeness (no "_value" leak), NONE/NaN omission.
 */

#include <intp/metrics.h>
#include <intp/plugin.h>

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "../../src/output/output.h"
#include "../../src/output/prom_common.h"
#include "intp_test.h"

#define HAS(hay, needle) INTP_EXPECT(strstr((hay), (needle)) != NULL)
#define HASNT(hay, needle) INTP_EXPECT(strstr((hay), (needle)) == NULL)

static int emit_to_string(const struct intp_sample *s, char *out, size_t outsz)
{
	const struct intp_output_vtable *sink = intp_output_find("prometheus");

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
	INTP_TEST_BEGIN("output/prometheus-fmt");

	/* Unit-suffix completeness: no real unit may map to "value". */
	enum intp_unit units[] = {
		INTP_UNIT_RATIO,	  INTP_UNIT_PERCENT,
		INTP_UNIT_BYTES,	  INTP_UNIT_BYTES_PER_SECOND,
		INTP_UNIT_OPS_PER_SECOND, INTP_UNIT_NANOSECONDS,
		INTP_UNIT_MICROSECONDS,	  INTP_UNIT_MILLISECONDS,
		INTP_UNIT_SECONDS,	  INTP_UNIT_COUNT,
	};
	for (size_t i = 0; i < sizeof(units) / sizeof(units[0]); i++)
		INTP_EXPECT(strcmp(intp_prom_unit_suffix(units[i]), "value") !=
			    0);

	/* Type classification: COUNT is a counter, RATIO a gauge. */
	INTP_EXPECT_STREQ(intp_prom_type_for(INTP_UNIT_COUNT, INTP_VALUE_UINT64),
			  "counter");
	INTP_EXPECT_STREQ(intp_prom_type_for(INTP_UNIT_RATIO, INTP_VALUE_DOUBLE),
			  "gauge");

	struct intp_metric_value vals[5] = {0};

	vals[0].name = INTP_METRIC_CPU;
	vals[0].unit = INTP_UNIT_RATIO;
	vals[0].kind = INTP_VALUE_DOUBLE;
	vals[0].d = 0.66;

	vals[1].name = INTP_METRIC_BLK; /* NONE -> omitted */
	vals[1].unit = INTP_UNIT_RATIO;
	vals[1].kind = INTP_VALUE_NONE;

	vals[2].name = "system.portable.psp"; /* COUNT -> counter, _total */
	vals[2].unit = INTP_UNIT_COUNT;
	vals[2].kind = INTP_VALUE_UINT64;
	vals[2].u64 = 42;

	vals[3].name = INTP_METRIC_MBW; /* NaN -> omitted */
	vals[3].unit = INTP_UNIT_RATIO;
	vals[3].kind = INTP_VALUE_DOUBLE;
	vals[3].d = NAN;

	vals[4].name = "system.portable.steal"; /* PERCENT, int64 */
	vals[4].unit = INTP_UNIT_PERCENT;
	vals[4].kind = INTP_VALUE_INT64;
	vals[4].i64 = -7;

	struct intp_sample s = {
		.host = "ho\"st",
		.values = vals,
		.values_len = 5,
	};
	char out[4096];

	INTP_EXPECT_EQ(emit_to_string(&s, out, sizeof(out)), 0);

	HAS(out, "# TYPE intp_cpu_ratio gauge");
	HAS(out, "intp_cpu_ratio{instance=\"ho\\\"st\",target_kind=\"system\"} "
		 "0.660000");
	HAS(out, "# TYPE intp_psp_total counter"); /* COUNT -> counter + _total */
	HAS(out, "intp_psp_total{instance=\"ho\\\"st\",target_kind=\"system\"} "
		 "42");
	HAS(out, "intp_steal_percent{"); /* PERCENT suffix, int64 below */
	HAS(out, "} -7");
	HASNT(out, "intp_blk"); /* NONE omitted */
	HASNT(out, "intp_mbw"); /* NaN omitted */
	HASNT(out, "_value");	/* no "value" suffix leaked into a name */

	return INTP_TEST_END();
}
