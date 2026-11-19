/* SPDX-License-Identifier: MIT */
/*
 * output.h — internal registry of in-tree output sinks.
 *
 * Every in-tree sink implements struct intp_output_vtable from the
 * public plugin ABI (ADR-0004): in-tree formats and out-of-tree
 * format plugins share one contract, so the dispatcher cannot grow a
 * privileged path that plugins can't use.
 */

#ifndef INTP_OUTPUT_H
#define INTP_OUTPUT_H

#include <intp/plugin.h>

/* Look up an in-tree sink by --format name; NULL if unknown. */
const struct intp_output_vtable *intp_output_find(const char *format_name);

/* NULL-terminated list of the in-tree sinks, for help/usage text. */
const struct intp_output_vtable *const *intp_output_list(void);

#endif /* INTP_OUTPUT_H */
