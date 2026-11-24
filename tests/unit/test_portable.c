/* SPDX-License-Identifier: MIT */
/*
 * test_portable.c — portable-metrics provider contract (ADR-0009):
 * the eight system.portable.* names in canonical order, the unit
 * mapping, and honest unavailability when reading without init.
 */

#include <errno.h>
#include <math.h>
#include <string.h>

#include "../../src/plugins/portable-metrics/portable.h"
#include "intp_test.h"

static void test_names_canonical_order(void)
{
	INTP_EXPECT_EQ(intp_portable_count(), 8);

	INTP_EXPECT_STREQ(intp_portable_name(0), "system.portable.schedlat");
	INTP_EXPECT_STREQ(intp_portable_name(1), "system.portable.psi_mem");
	INTP_EXPECT_STREQ(intp_portable_name(2), "system.portable.membw_est");
	INTP_EXPECT_STREQ(intp_portable_name(3), "system.portable.psi_io");
	INTP_EXPECT_STREQ(intp_portable_name(4), "system.portable.schedthr");
	INTP_EXPECT_STREQ(intp_portable_name(5), "system.portable.steal");
	INTP_EXPECT_STREQ(intp_portable_name(6), "system.portable.psp");
	INTP_EXPECT_STREQ(intp_portable_name(7),
			  "system.portable.idle_preempt");

	/* Every name carries the experimental namespace prefix. */
	for (size_t i = 0; i < intp_portable_count(); i++) {
		const char *name = intp_portable_name(i);

		INTP_EXPECT_NOT_NULL(name);
		INTP_EXPECT(strncmp(name, "system.portable.", 16) == 0);
	}

	/* Out-of-range index is NULL, not a crash. */
	INTP_EXPECT(intp_portable_name(8) == NULL);
}

static void test_units_mapping(void)
{
	/* Percent-of-interval metrics (schedlat psi_mem psi_io schedthr
	 * steal) publish as RATIO; membw_est is a bandwidth rate; the
	 * scheduling-regime rates (psp idle_preempt) are events/s. An
	 * out-of-range index maps to UNSPECIFIED. */
	static const enum intp_unit expected[9] = {
		INTP_UNIT_RATIO,	    /* schedlat */
		INTP_UNIT_RATIO,	    /* psi_mem */
		INTP_UNIT_BYTES_PER_SECOND, /* membw_est */
		INTP_UNIT_RATIO,	    /* psi_io */
		INTP_UNIT_RATIO,	    /* schedthr */
		INTP_UNIT_RATIO,	    /* steal */
		INTP_UNIT_OPS_PER_SECOND,   /* psp */
		INTP_UNIT_OPS_PER_SECOND,   /* idle_preempt */
		INTP_UNIT_UNSPECIFIED,	    /* out of range */
	};

	for (size_t i = 0; i < 9; i++) {
		enum intp_unit u = intp_portable_unit(i);

		INTP_EXPECT_EQ(u, expected[i]);
	}
}

static void test_read_without_init_reports_unavailable(void)
{
	/* No intp_portable_select_and_init() has run in this process:
	 * every read must report UNAVAILABLE (value NAN) and not crash —
	 * including the out-of-range index. */
	for (size_t i = 0; i <= intp_portable_count(); i++) {
		struct intp_core_sample s;

		intp_portable_read(i, &s, 1.0);
		INTP_EXPECT_EQ(s.status, INTP_CORE_UNAVAILABLE);
		INTP_EXPECT(isnan(s.value));
		INTP_EXPECT_NOT_NULL(s.backend_id);
	}

	/* NULL out must be tolerated. */
	intp_portable_read(0, NULL, 1.0);

	/* Cleanup without init is idempotent and safe. */
	intp_portable_cleanup();
	intp_portable_cleanup();
}

/* --force-backend for system.portable.* (ADR-0013): known pairs are
 * recorded, unknown ones are -ENOENT. */
static void test_force_lookup(void)
{
	INTP_EXPECT_EQ(intp_portable_force("psp", "nonvol_ctxt_pid"), 0);
	INTP_EXPECT_EQ(intp_portable_force("psp", "no_such"), -ENOENT);
	INTP_EXPECT_EQ(intp_portable_force("no_such", "proc_stat"), -ENOENT);
	INTP_EXPECT_EQ(intp_portable_force(NULL, "proc_stat"), -ENOENT);
	intp_portable_force_clear();
}

/* Chain introspection: every metric lists at least one candidate, and
 * the eBPF-only idle_preempt keeps its stable-ABI placeholder last. */
static void test_candidates(void)
{
	const char *id, *desc;
	unsigned int lin;

	for (size_t i = 0; i < intp_portable_count(); i++)
		INTP_EXPECT_EQ(intp_portable_candidate(i, 0, &id, &desc, &lin),
			       0);

	size_t last = 0;

	while (intp_portable_candidate(7, last + 1, &id, &desc, &lin) == 0)
		last++;
	INTP_EXPECT_EQ(intp_portable_candidate(7, last, &id, &desc, &lin), 0);
	INTP_EXPECT_STREQ(id, "unavailable");
	INTP_EXPECT_EQ(intp_portable_candidate(8, 0, &id, &desc, &lin), -1);
	INTP_EXPECT(intp_portable_backend(0) == NULL); /* nothing bound */
}

int main(void)
{
	INTP_TEST_BEGIN("portable-metrics-provider");
	test_names_canonical_order();
	test_units_mapping();
	test_read_without_init_reports_unavailable();
	test_force_lookup();
	test_candidates();
	return INTP_TEST_END();
}
