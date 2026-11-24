/* SPDX-License-Identifier: MIT */
/*
 * portable.h — internal provider API for the system.portable.* metric
 * set (ADR-0009). NOT installed; not part of the public ABI.
 *
 * The provider wraps the eight VM-portable metrics ported from
 * across-intp:
 *   variants/v2.1-c-abi-cgroup/src/portable.c @ 2e87b30fc762
 * plus eBPF backends that read the v3.3 counters the shared loader
 * already collects (ADR-0013).
 * It deliberately does NOT register into the core "intp_backends"
 * linker section: the canonical seven-metric fingerprint stays
 * byte-identical whether or not the provider is enabled, and the
 * provider is gated behind --experimental --portable-metrics
 * (ADR-0008).
 *
 * Canonical order (index i):
 *   0 schedlat  1 psi_mem  2 membw_est  3 psi_io
 *   4 schedthr  5 steal    6 psp        7 idle_preempt
 *
 * Values are reported on the upstream scale: percent [0,100] for
 * schedlat/psi_mem/psi_io/schedthr/steal, MB/s for membw_est,
 * events/s for psp/idle_preempt. The dispatcher converts to the
 * public units (ratio [0,1], bytes/s, ops/s) when appending samples.
 */

#ifndef INTP_PORTABLE_H
#define INTP_PORTABLE_H

#include <stddef.h>

#include "../../core/core.h"

#define INTP_PORTABLE_COUNT 8

/* Number of portable metrics (always INTP_PORTABLE_COUNT). */
size_t intp_portable_count(void);

/* Full metric name for index i: "system.portable.schedlat" … */
const char *intp_portable_name(size_t i);

/* Public unit for index i: RATIO for the percent metrics,
 * BYTES_PER_SECOND for membw_est, OPS_PER_SECOND for psp and
 * idle_preempt. */
enum intp_unit intp_portable_unit(size_t i);

/* Probe + init the per-metric fallback chains (call once per open,
 * after intp_core_target_set() and the detection pass). A metric whose
 * whole chain rejects reads as unavailable, never zero. Returns 0 if
 * at least one metric bound a backend, -ENODEV otherwise. */
int intp_portable_select_and_init(void);

/* Read metric i. Fills *out; on any failure (no active backend, read
 * error, out-of-range i) reports INTP_CORE_UNAVAILABLE with value NAN.
 * Safe to call without a prior intp_portable_select_and_init(). */
void intp_portable_read(size_t i, struct intp_core_sample *out,
			double interval_sec);

/* Operator override (--force-backend metric:backend_id), recorded before
 * intp_portable_select_and_init(): the named backend is bound if its
 * probe and init pass, regardless of --engine; otherwise normal selection
 * runs and a warning is printed. `metric` is the short name ("schedlat").
 * Returns 0 when the pair exists, -ENOENT otherwise. */
int intp_portable_force(const char *metric, const char *backend_id);

/* Drop every recorded override (also done by intp_portable_cleanup). */
void intp_portable_force_clear(void);

/* Backend id bound for metric i after intp_portable_select_and_init(),
 * or NULL when the metric is unavailable. */
const char *intp_portable_backend(size_t i);

/* Candidate j of metric i in chain order: fills *id, *desc and *lineage
 * (enum intp_core_lineage bits, 0 = stable) and returns 0, or -1 past the
 * end. For --list-backends. */
int intp_portable_candidate(size_t i, size_t j, const char **id,
			    const char **desc, unsigned int *lineage);

/* Release every backend resource. Idempotent. */
void intp_portable_cleanup(void);

#endif /* INTP_PORTABLE_H */
