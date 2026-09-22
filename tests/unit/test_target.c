/* SPDX-License-Identifier: MIT */
/*
 * test_target.c — target parsing and override propagation.
 *
 * Ported from across-intp:
 *   variants/v2.1-c-abi-cgroup/tests/test-target.c @ 2e87b30fc762
 * onto the intp_test.h macros. Covers:
 *   - intp_parse_pid_list("1,2,3") -> 3 PIDs, correct values
 *   - intp_find_pids_by_comm(<own comm>) includes our own pid
 *   - nic_speed_bps_override flows through netp_resolve_speed
 */

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "../../src/backends/netp/netp.h"
#include "../../src/core/core.h"
#include "intp_test.h"

static void test_parse_pid_list(void)
{
	pid_t pids[INTP_CORE_MAX_PIDS];
	int n = intp_parse_pid_list("1,2,3", pids, INTP_CORE_MAX_PIDS);

	INTP_EXPECT_EQ(n, 3);
	INTP_EXPECT_EQ(pids[0], 1);
	INTP_EXPECT_EQ(pids[1], 2);
	INTP_EXPECT_EQ(pids[2], 3);

	/* Trailing garbage stops parsing without changing already-parsed
	 * PIDs. */
	n = intp_parse_pid_list("42,abc", pids, INTP_CORE_MAX_PIDS);
	INTP_EXPECT_EQ(n, 1);
	INTP_EXPECT_EQ(pids[0], 42);

	/* Zero/negative PIDs are rejected. */
	n = intp_parse_pid_list("0,1", pids, INTP_CORE_MAX_PIDS);
	INTP_EXPECT_EQ(n, 0);
}

/* Read /proc/self/comm so the test does not depend on a specific
 * binary name being present. */
static int read_self_comm(char *buf, size_t buf_size)
{
	FILE *f = fopen("/proc/self/comm", "r");

	if (!f)
		return -1;
	if (!fgets(buf, (int)buf_size, f)) {
		fclose(f);
		return -1;
	}
	fclose(f);
	size_t n = strlen(buf);

	while (n && (buf[n - 1] == '\n' || buf[n - 1] == '\r'))
		buf[--n] = '\0';
	return 0;
}

static void test_find_pids_by_comm(void)
{
	char comm[64];

	if (read_self_comm(comm, sizeof(comm)) != 0 || !comm[0]) {
		fprintf(stderr, "# SKIP find_pids_by_comm: /proc/self/comm "
				"unreadable\n");
		return;
	}
	pid_t pids[INTP_CORE_MAX_PIDS];
	int n = intp_find_pids_by_comm(comm, pids, INTP_CORE_MAX_PIDS);

	INTP_EXPECT(n >= 1);
	int found = 0;
	pid_t me = getpid();

	for (int i = 0; i < n; i++) {
		if (pids[i] == me) {
			found = 1;
			break;
		}
	}
	INTP_EXPECT(found);
}

static void test_nic_speed_override(void)
{
	struct intp_core_target tgt;

	memset(&tgt, 0, sizeof(tgt));
	tgt.nic_speed_bps_override = 10000000000L; /* 10 Gb/s */
	intp_core_target_set(&tgt);

	int assumed = -1;
	long bps = netp_resolve_speed("eth0", &assumed);

	INTP_EXPECT(bps == 10000000000L);
	INTP_EXPECT_EQ(assumed, 0);

	/* Clearing the override should fall through to detection (and
	 * when the named iface does not exist, to the DEFAULT_BPS
	 * assumed path). */
	memset(&tgt, 0, sizeof(tgt));
	intp_core_target_set(&tgt);
	assumed = -1;
	bps = netp_resolve_speed("__no_such_iface__", &assumed);
	INTP_EXPECT(bps > 0);
	INTP_EXPECT_EQ(assumed, 1);
}

int main(void)
{
	INTP_TEST_BEGIN("target-parsing");
	test_parse_pid_list();
	test_find_pids_by_comm();
	test_nic_speed_override();
	return INTP_TEST_END();
}
