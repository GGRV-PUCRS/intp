/* SPDX-License-Identifier: MIT */
/*
 * serve_uri.h — parse the intp(1) --serve URI into a bind target.
 *
 * Factored out of main.c so it can be unit-tested (the parser is pure;
 * it writes no global state and performs no I/O beyond an error message
 * to stderr on malformed input).
 */

#ifndef INTP_CLI_SERVE_URI_H
#define INTP_CLI_SERVE_URI_H

#include <stdint.h>

struct intp_serve_target {
	/* Empty string means "bind loopback" (127.0.0.1). A literal host
	 * (e.g. "0.0.0.0" or "192.0.2.10") is bound as given. */
	char host[256];
	uint16_t port;
};

/*
 * Parse a --serve argument into *out (out->host defaults to "" and
 * out->port to 9100 before parsing). Accepts:
 *   NULL or "prometheus"                 -> host="",   port=9100
 *   "prometheus://[host][:port]"         -> host,      port (default 9100)
 *   "prometheus://[v6]" with bracketed IPv6 host
 * Only the "prometheus" scheme is supported.
 *
 * Returns 0 on success, -1 on a malformed/unsupported argument (a
 * diagnostic is written to stderr in that case).
 */
int intp_parse_serve_uri(const char *arg, struct intp_serve_target *out);

#endif /* INTP_CLI_SERVE_URI_H */
