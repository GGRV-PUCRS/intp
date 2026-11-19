/* SPDX-License-Identifier: MIT */
/*
 * test_otlp_json.c — golden test for the OTLP/JSON metrics serializer.
 * Pins: resource attributes, gauge vs monotonic-Sum mapping, 64-bit ints
 * as JSON strings, timeUnixNano, and NONE/NaN omission. The serializer is
 * pure (no libcurl), so this runs in every build.
 */

#include <intp/metrics.h>

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "intp_test.h"
#include "otlp_json.h"

#define HAS(hay, needle)   INTP_EXPECT(strstr((hay), (needle)) != NULL)
#define HASNT(hay, needle) INTP_EXPECT(strstr((hay), (needle)) == NULL)

int main(void)
{
	INTP_TEST_BEGIN("cli/otlp-json");

	struct intp_metric_value vals[4] = {0};

	vals[0].name = INTP_METRIC_CPU; /* gauge, asDouble */
	vals[0].unit = INTP_UNIT_RATIO;
	vals[0].kind = INTP_VALUE_DOUBLE;
	vals[0].d = 0.66;

	vals[1].name = "system.portable.psp"; /* counter -> Sum, asInt string */
	vals[1].unit = INTP_UNIT_COUNT;
	vals[1].kind = INTP_VALUE_UINT64;
	vals[1].u64 = 42;

	vals[2].name = INTP_METRIC_MBW; /* NONE -> omitted */
	vals[2].unit = INTP_UNIT_RATIO;
	vals[2].kind = INTP_VALUE_NONE;

	vals[3].name = INTP_METRIC_BLK; /* NaN -> omitted */
	vals[3].unit = INTP_UNIT_RATIO;
	vals[3].kind = INTP_VALUE_DOUBLE;
	vals[3].d = NAN;

	struct intp_sample s = {0};

	s.ts.tv_sec = 1;
	s.ts.tv_nsec = 500000000;
	s.host = "h1";
	s.values = vals;
	s.values_len = 4;

	struct otlp_kv res[] = {
		{"host.name", "h1"},
		{"service.name", "intp"},
	};
	char *j = otlp_json_build(&s, res, 2);

	INTP_EXPECT_NOT_NULL(j);
	if (j) {
		HAS(j, "\"resourceMetrics\"");
		HAS(j, "\"key\":\"host.name\"");
		HAS(j, "\"stringValue\":\"h1\"");
		HAS(j, "\"name\":\"system.interference.cpu\"");
		HAS(j, "\"gauge\"");
		HAS(j, "\"asDouble\":0.66");
		HAS(j, "\"name\":\"system.portable.psp\"");
		HAS(j, "\"sum\"");
		HAS(j, "\"isMonotonic\":true");
		HAS(j, "\"aggregationTemporality\":2");
		HAS(j, "\"asInt\":\"42\"");
		HAS(j, "\"timeUnixNano\":\"1500000000\"");
		HASNT(j, "system.interference.mbw"); /* NONE omitted */
		HASNT(j, "system.interference.blk"); /* NaN omitted */
		free(j);
	}

	return INTP_TEST_END();
}
