/* SPDX-License-Identifier: MIT */
/*
 * otlp_json.h — serialize an intp sample as an OTLP/JSON
 * ExportMetricsServiceRequest (the protobuf-JSON mapping every OTLP
 * collector accepts over HTTP). Serialize-only; no parser, no protobuf.
 *
 * CLI-local (ADR-0011): the OTLP exporter is compiled into the `intp`
 * binary, never into libintp.so.
 */

#ifndef INTP_CLI_OTLP_JSON_H
#define INTP_CLI_OTLP_JSON_H

#include <intp/metrics.h>

#include <stddef.h>

/* One resource attribute (string-valued). */
struct otlp_kv {
	const char *key;
	const char *value;
};

/*
 * Build the OTLP/JSON body for one sample. `res`/`n_res` are the
 * Resource attributes (host.name, service.*, intp.target.*, operator
 * attrs). The seven core gauges and any namespaced metrics map to OTLP
 * Gauge data points; INTP_UNIT_COUNT metrics map to a monotonic
 * cumulative Sum. Unavailable (NONE) and NaN values are omitted.
 *
 * Returns a malloc'd NUL-terminated JSON string (caller frees), or NULL
 * on allocation failure.
 */
char *otlp_json_build(const struct intp_sample *sample,
		      const struct otlp_kv *res, size_t n_res);

#endif /* INTP_CLI_OTLP_JSON_H */
