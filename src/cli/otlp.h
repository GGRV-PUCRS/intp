/* SPDX-License-Identifier: MIT */
/*
 * otlp.h — OTLP/HTTP+JSON metrics output sink (`--format otlp`).
 *
 * Compiled into the `intp` CLI only; libcurl lives in the CLI's
 * dependency closure, never in libintp.so (ADR-0011). Available only when
 * built with -Dwith_otel=true (guarded by INTP_HAVE_OTEL).
 */

#ifndef INTP_CLI_OTLP_H
#define INTP_CLI_OTLP_H

#include <intp/plugin.h>

extern const struct intp_output_vtable intp_output_otlp;

#endif /* INTP_CLI_OTLP_H */
