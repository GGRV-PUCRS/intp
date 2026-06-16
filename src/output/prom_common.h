/* SPDX-License-Identifier: MIT */
/*
 * prom_common.h — shared Prometheus exposition helpers.
 *
 * Used by BOTH the one-shot `--format prometheus` sink (prometheus_fmt.c)
 * and the long-running `--serve` endpoint (src/lib/prometheus.c) so the
 * two renderers cannot drift on naming, units, type classification, or
 * label-value escaping. Internal; not part of the public ABI.
 */

#ifndef INTP_OUTPUT_PROM_COMMON_H
#define INTP_OUTPUT_PROM_COMMON_H

#include <intp/metrics.h>

#include <stddef.h>

/* "system.interference.cpu" -> "cpu"; "system.portable.psp" -> "psp" */
const char *intp_prom_short_name(const char *full);

/* Metric-name unit suffix: ratio / percent / bytes / bytes_per_second /
 * ops_per_second / nanoseconds / .../ seconds / total (counters). Never
 * returns "value" for a real unit. */
const char *intp_prom_unit_suffix(enum intp_unit u);

/* "counter" for monotonic COUNT metrics, "gauge" otherwise. */
const char *intp_prom_type_for(enum intp_unit u, enum intp_value_kind k);

/* Escape a Prometheus label VALUE: '\\' -> "\\\\", '"' -> "\\\"",
 * newline -> "\\n". '/' is legal unescaped and passes through (cgroup
 * paths). Writes at most cap-1 bytes plus NUL into dst; always
 * NUL-terminates. Returns dst. */
char *intp_prom_escape_label(char *dst, size_t cap, const char *src);

/* Build the shared identity label block into dst, e.g.
 *   {instance="h",target_kind="container",cgroup="/sys/.../pg",pid="42"}
 * All values are escaped. host and target_kind are required; cgroup
 * (NULL) and pid (<= 0) are omitted. Returns bytes written (excl. NUL).
 * A 2048-byte dst comfortably holds host + an escaped cgroup path. */
size_t intp_prom_labels(char *dst, size_t cap, const char *host,
			const char *target_kind, const char *cgroup, long pid);

#endif /* INTP_OUTPUT_PROM_COMMON_H */
