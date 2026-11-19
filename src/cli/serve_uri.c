/* SPDX-License-Identifier: MIT */
/*
 * serve_uri.c — parse the intp(1) --serve URI (see serve_uri.h).
 */

#include "serve_uri.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int intp_parse_serve_uri(const char *arg, struct intp_serve_target *out)
{
	const char *proto = arg ? arg : "prometheus";

	out->host[0] = '\0';
	out->port = 9100;

	if (strncmp(proto, "prometheus", 10) != 0) {
		fprintf(stderr,
			"intp: --serve %s is not supported.\n"
			"      Supported: --serve prometheus[://[host][:port]] "
			"(default 127.0.0.1:9100)\n",
			proto);
		return -1;
	}

	const char *rest = proto + 10;

	if (rest[0] == '\0')
		return 0; /* bare "prometheus" -> loopback:9100 */

	if (!(rest[0] == ':' && rest[1] == '/' && rest[2] == '/')) {
		fprintf(stderr, "intp: malformed --serve argument: %s\n",
			proto);
		return -1;
	}
	rest += 3; /* skip "://" */

	const char *host_start = rest;
	const char *host_end;
	const char *colon;

	if (rest[0] == '[') {
		/* Bracketed IPv6 literal: [::1]:9100 */
		const char *rb = strchr(rest, ']');

		if (!rb) {
			fprintf(stderr,
				"intp: malformed --serve IPv6 host: %s\n",
				proto);
			return -1;
		}
		host_start = rest + 1;
		host_end = rb;
		colon = (rb[1] == ':') ? rb + 1 : NULL;
	} else {
		colon = strrchr(rest, ':');
		host_end = colon ? colon : rest + strlen(rest);
	}

	size_t hlen = (size_t)(host_end - host_start);

	if (hlen >= sizeof(out->host))
		hlen = sizeof(out->host) - 1;
	memcpy(out->host, host_start, hlen);
	out->host[hlen] = '\0';

	if (colon && colon[1]) {
		char *end = NULL;
		long p = strtol(colon + 1, &end, 10);

		if (end == colon + 1 || *end != '\0' || p <= 0 || p > 65535) {
			fprintf(stderr, "intp: bad --serve port: %s\n",
				colon + 1);
			return -1;
		}
		out->port = (uint16_t)p;
	}
	return 0;
}
