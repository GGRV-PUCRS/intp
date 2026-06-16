/* SPDX-License-Identifier: MIT */
/*
 * prom_common.c — shared Prometheus exposition helpers (see prom_common.h).
 */

#include "prom_common.h"

#include <stdio.h>
#include <string.h>

const char *intp_prom_short_name(const char *full)
{
	const char *dot = strrchr(full, '.');

	return dot ? dot + 1 : full;
}

const char *intp_prom_unit_suffix(enum intp_unit u)
{
	switch (u) {
	case INTP_UNIT_RATIO:
		return "ratio";
	case INTP_UNIT_PERCENT:
		return "percent";
	case INTP_UNIT_BYTES:
		return "bytes";
	case INTP_UNIT_BYTES_PER_SECOND:
		return "bytes_per_second";
	case INTP_UNIT_OPS_PER_SECOND:
		return "ops_per_second";
	case INTP_UNIT_NANOSECONDS:
		return "nanoseconds";
	case INTP_UNIT_MICROSECONDS:
		return "microseconds";
	case INTP_UNIT_MILLISECONDS:
		return "milliseconds";
	case INTP_UNIT_SECONDS:
		return "seconds";
	case INTP_UNIT_COUNT:
		return "total"; /* OpenMetrics counter convention */
	case INTP_UNIT_UNSPECIFIED:
	default:
		return "info";
	}
}

const char *intp_prom_type_for(enum intp_unit u, enum intp_value_kind k)
{
	(void)k;
	/* All seven core metrics are RATIO gauges; only COUNT-unit
	 * portable/plugin metrics are monotonic counters. */
	return u == INTP_UNIT_COUNT ? "counter" : "gauge";
}

char *intp_prom_escape_label(char *dst, size_t cap, const char *src)
{
	size_t o = 0;

	if (cap == 0)
		return dst;
	for (; src && *src; src++) {
		const char *rep = NULL;

		switch (*src) {
		case '\\':
			rep = "\\\\";
			break;
		case '"':
			rep = "\\\"";
			break;
		case '\n':
			rep = "\\n";
			break;
		default:
			break;
		}
		if (rep) {
			if (o + 2 >= cap)
				break;
			dst[o++] = rep[0];
			dst[o++] = rep[1];
		} else {
			if (o + 1 >= cap)
				break;
			dst[o++] = *src;
		}
	}
	dst[o] = '\0';
	return dst;
}

static size_t append(char *dst, size_t cap, size_t n, const char *fmt,
		     const char *arg)
{
	int w = snprintf(dst + n, n < cap ? cap - n : 0, fmt, arg);

	return w > 0 ? n + (size_t)w : n;
}

size_t intp_prom_labels(char *dst, size_t cap, const char *host,
			const char *target_kind, const char *cgroup, long pid)
{
	char esc[1024];
	size_t n = 0;

	n = append(dst, cap, n, "{instance=\"%s\"",
		   intp_prom_escape_label(esc, sizeof(esc), host ? host : ""));
	if (target_kind)
		n = append(dst, cap, n, ",target_kind=\"%s\"",
			   intp_prom_escape_label(esc, sizeof(esc),
						  target_kind));
	if (cgroup)
		n = append(dst, cap, n, ",cgroup=\"%s\"",
			   intp_prom_escape_label(esc, sizeof(esc), cgroup));
	if (pid > 0) {
		char pbuf[24];

		snprintf(pbuf, sizeof(pbuf), "%ld", pid);
		n = append(dst, cap, n, ",pid=\"%s\"", pbuf);
	}
	n = append(dst, cap, n, "%s", "}");
	return n;
}
