/* SPDX-License-Identifier: MIT */
/*
 * test_detect.c — minimal sanity checks for detect.c.
 *
 * Ported from intp-comparison:
 *   variants/v2.1-c-abi-cgroup/tests/test-detect.c @ 2e87b30fc762
 *
 * These tests run on whatever system is doing the build; they assert
 * invariants (e.g. detect_num_cores() > 0) rather than specific
 * hardware.
 */

#include <string.h>

#include "../../src/core/detect.h"
#include "intp_test.h"

int main(void)
{
	INTP_TEST_BEGIN("core/detect");

	cpu_vendor_t v = detect_cpu_vendor();

	INTP_EXPECT(v == VENDOR_INTEL || v == VENDOR_AMD || v == VENDOR_ARM ||
		    v == VENDOR_UNKNOWN);

	const char *model = detect_cpu_model();

	INTP_EXPECT_NOT_NULL(model);
	INTP_EXPECT(strlen(model) > 0);

	INTP_EXPECT(detect_num_cores() >= 1);
	INTP_EXPECT(detect_num_sockets() >= 1);

	int paranoid = detect_perf_paranoid_level();

	INTP_EXPECT(paranoid >= -1 && paranoid <= 4);

	int ma = 0, mi = 0;

	INTP_EXPECT_EQ(detect_kernel_version(&ma, &mi), 0);
	INTP_EXPECT(ma > 0);

	system_capabilities_t caps;

	detect_all(&caps);
	INTP_EXPECT(caps.num_cores >= 1);
	INTP_EXPECT(strlen(caps.cpu_model) > 0);
	INTP_EXPECT(strlen(caps.default_iface) > 0);

	/* mem_bw_max_bps must be positive (default fallback if no
	 * detection). */
	INTP_EXPECT(caps.mem_bw_max_bps > 0);

	return INTP_TEST_END();
}
