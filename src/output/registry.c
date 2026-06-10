/* SPDX-License-Identifier: MIT */
/*
 * registry.c (output) — name → vtable lookup for the in-tree sinks.
 *
 * tsv-legacy-v1 and prometheus land in Phase 3; otlp is deferred (see
 * CHANGELOG). Unknown names fall through to the plugin loader once it
 * supports output-sink plugins.
 */

#include "output.h"

#include <stddef.h>
#include <string.h>

extern const struct intp_output_vtable intp_output_jsonl;

static const struct intp_output_vtable *const sinks[] = {
	&intp_output_jsonl,
	NULL,
};

const struct intp_output_vtable *intp_output_find(const char *format_name)
{
	if (!format_name)
		return NULL;
	for (size_t i = 0; sinks[i]; i++) {
		if (strcmp(sinks[i]->format_name, format_name) == 0)
			return sinks[i];
	}
	return NULL;
}

const struct intp_output_vtable *const *intp_output_list(void)
{
	return sinks;
}
