/* SPDX-License-Identifier: MIT */
/*
 * test_procutil.c — exercise procutil parsers against the live /proc.
 *
 * Ported from across-intp:
 *   variants/v2.1-c-abi-cgroup/tests/test-procutil.c @ 2e87b30fc762
 * adapted to the in-tree assertion macros and struct-tag type names.
 */

#include <sys/stat.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../../src/backends/common/procutil.h"
#include "intp_test.h"

static void write_procs(const char *dir, const char *body)
{
	char p[512];

	snprintf(p, sizeof(p), "%s/cgroup.procs", dir);

	FILE *f = fopen(p, "w");

	if (f) {
		fputs(body, f);
		fclose(f);
	}
}

/* Build a fake nested cgroup tree (top has NO direct procs, the payload
 * lives in child + grandchild cgroups — the incus/lxc layout) and check
 * that the recursive reader captures the descendants the non-recursive
 * one misses. */
static void test_cgroup_procs_recursion(void)
{
	char top[] = "/tmp/intp-cgtest-XXXXXX";

	if (!mkdtemp(top)) {
		INTP_EXPECT(0 && "mkdtemp failed");
		return;
	}

	char leaf[64], sub[96], dotlxc[96];

	snprintf(leaf, sizeof(leaf), "%s/leaf", top);
	snprintf(sub, sizeof(sub), "%s/sub", leaf);
	snprintf(dotlxc, sizeof(dotlxc), "%s/.lxc", top);

	INTP_EXPECT(mkdir(leaf, 0755) == 0);
	INTP_EXPECT(mkdir(sub, 0755) == 0);
	INTP_EXPECT(mkdir(dotlxc, 0755) == 0);

	write_procs(top, "");		   /* no internal processes */
	write_procs(leaf, "1001\n1002\n"); /* payload in the child */
	write_procs(sub, "1003\n");	   /* and a grandchild */
	write_procs(dotlxc, "1004\n");	   /* ".lxc" payload (incus) */

	pid_t pids[16];
	int flat = procutil_read_cgroup_procs(top, pids, 16);

	INTP_EXPECT_EQ(flat, 0); /* non-recursive misses payload */

	int rec = procutil_read_cgroup_procs_rec(top, pids, 16);

	INTP_EXPECT_EQ(rec, 4); /* subtree incl. .lxc captured */

	int got1001 = 0, got1003 = 0, got1004 = 0;

	for (int i = 0; i < rec; i++) {
		if (pids[i] == 1001)
			got1001 = 1;
		if (pids[i] == 1003)
			got1003 = 1;
		if (pids[i] == 1004)
			got1004 = 1;
	}
	INTP_EXPECT(got1001 && got1003 && got1004);

	char rmpath[160];

	snprintf(rmpath, sizeof(rmpath), "%s/cgroup.procs", sub);
	unlink(rmpath);
	snprintf(rmpath, sizeof(rmpath), "%s/cgroup.procs", leaf);
	unlink(rmpath);
	snprintf(rmpath, sizeof(rmpath), "%s/cgroup.procs", dotlxc);
	unlink(rmpath);
	snprintf(rmpath, sizeof(rmpath), "%s/cgroup.procs", top);
	unlink(rmpath);
	rmdir(sub);
	rmdir(leaf);
	rmdir(dotlxc);
	rmdir(top);
}

/* cgroup.threads walks the same subtree as cgroup.procs (C38): a fake
 * tree whose leaf lists three TIDs and whose .lxc child lists one. */
static void test_cgroup_threads_recursion(void)
{
	char top[] = "/tmp/intp-cgthr-XXXXXX";

	if (!mkdtemp(top)) {
		INTP_EXPECT(0 && "mkdtemp failed");
		return;
	}

	char leaf[64], dotlxc[64], p[128];

	snprintf(leaf, sizeof(leaf), "%s/leaf", top);
	snprintf(dotlxc, sizeof(dotlxc), "%s/.lxc", top);
	INTP_EXPECT(mkdir(leaf, 0755) == 0);
	INTP_EXPECT(mkdir(dotlxc, 0755) == 0);

	const char *dirs[] = {top, leaf, dotlxc};
	const char *bodies[] = {"", "2001\n2002\n2003\n", "2004\n"};

	for (int i = 0; i < 3; i++) {
		snprintf(p, sizeof(p), "%s/cgroup.threads", dirs[i]);

		FILE *f = fopen(p, "w");

		if (f) {
			fputs(bodies[i], f);
			fclose(f);
		}
	}

	pid_t tids[16];

	INTP_EXPECT_EQ(procutil_read_cgroup_threads_rec(top, tids, 16), 4);
	/* The cap is honoured. */
	INTP_EXPECT_EQ(procutil_read_cgroup_threads_rec(top, tids, 2), 2);

	for (int i = 2; i >= 0; i--) {
		snprintf(p, sizeof(p), "%s/cgroup.threads", dirs[i]);
		unlink(p);
	}
	rmdir(dotlxc);
	rmdir(leaf);
	rmdir(top);
}

/* /proc/<pid>/task lists this process (its own TID == PID for the
 * leader); a PID that does not exist yields 0. */
static void test_proc_tasks(void)
{
	pid_t tids[64];
	int n = procutil_read_proc_tasks(getpid(), tids, 64);
	int self = 0;

	INTP_EXPECT(n >= 1);
	for (int i = 0; i < n; i++)
		if (tids[i] == getpid())
			self = 1;
	INTP_EXPECT(self);
	INTP_EXPECT_EQ(procutil_read_proc_tasks((pid_t)0x7ffffff0, tids, 64),
		       0);
}

int main(void)
{
	INTP_TEST_BEGIN("backends/procutil");

	char buf[128];
	int n = procutil_read_file("/proc/uptime", buf, sizeof(buf));

	INTP_EXPECT(n > 0);

	long v = procutil_read_long("/proc/sys/kernel/pid_max");

	INTP_EXPECT(v > 0);

	struct diskstats_entry ds[32];
	int nd = procutil_read_diskstats(ds, 32);

	INTP_EXPECT(nd >= 0); /* zero entries valid on diskless runners */

	struct netdev_entry nets[16];
	int nn = procutil_read_netdev(nets, 16);

	INTP_EXPECT(nn >= 1); /* loopback at minimum */

	unsigned long net_tx = 0, net_rx = 0;

	INTP_EXPECT_EQ(procutil_read_net_softirqs(&net_tx, &net_rx), 0);

	unsigned long total = 0, idle = 0;

	INTP_EXPECT_EQ(procutil_read_stat_total(&total, &idle), 0);
	INTP_EXPECT(total > idle);

	unsigned long ut = 0, st = 0;

	INTP_EXPECT_EQ(procutil_read_proc_stat(getpid(), &ut, &st), 0);
	/* utime+stime can legitimately be 0 for very fresh processes. */

	test_cgroup_procs_recursion();
	test_cgroup_threads_recursion();
	test_proc_tasks();

	return INTP_TEST_END();
}
