/* SPDX-License-Identifier: MIT */
/*
 * test_parse_serve_uri.c — unit tests for the --serve URI parser.
 *
 * The headline regression this locks down: the host portion of the URI
 * must be captured (it was previously parsed then discarded, so --serve
 * always bound 0.0.0.0). An empty host means "loopback".
 */

#include "intp_test.h"
#include "serve_uri.h"

static struct intp_serve_target parse_ok(const char *arg)
{
	struct intp_serve_target t;
	int rc = intp_parse_serve_uri(arg, &t);

	INTP_EXPECT_EQ(rc, 0);
	return t;
}

int main(void)
{
	INTP_TEST_BEGIN("cli/parse-serve-uri");

	/* NULL and bare "prometheus" => loopback default :9100 */
	struct intp_serve_target t = parse_ok(NULL);

	INTP_EXPECT_STREQ(t.host, "");
	INTP_EXPECT_EQ(t.port, 9100);

	t = parse_ok("prometheus");
	INTP_EXPECT_STREQ(t.host, "");
	INTP_EXPECT_EQ(t.port, 9100);

	/* Explicit host + port is captured (the regression). */
	t = parse_ok("prometheus://0.0.0.0:9090");
	INTP_EXPECT_STREQ(t.host, "0.0.0.0");
	INTP_EXPECT_EQ(t.port, 9090);

	/* Empty host, explicit port => loopback on that port. */
	t = parse_ok("prometheus://:9100");
	INTP_EXPECT_STREQ(t.host, "");
	INTP_EXPECT_EQ(t.port, 9100);

	/* Host without port => default port. */
	t = parse_ok("prometheus://127.0.0.1");
	INTP_EXPECT_STREQ(t.host, "127.0.0.1");
	INTP_EXPECT_EQ(t.port, 9100);

	/* Bracketed IPv6 literal with port. */
	t = parse_ok("prometheus://[::1]:8080");
	INTP_EXPECT_STREQ(t.host, "::1");
	INTP_EXPECT_EQ(t.port, 8080);

	/* Malformed / unsupported inputs must be rejected. */
	struct intp_serve_target bad;

	INTP_EXPECT_EQ(intp_parse_serve_uri("http://x", &bad), -1);
	INTP_EXPECT_EQ(intp_parse_serve_uri("prometheusgarbage", &bad), -1);
	INTP_EXPECT_EQ(intp_parse_serve_uri("prometheus://h:0", &bad), -1);
	INTP_EXPECT_EQ(intp_parse_serve_uri("prometheus://h:99999", &bad), -1);
	INTP_EXPECT_EQ(intp_parse_serve_uri("prometheus://h:abc", &bad), -1);

	return INTP_TEST_END();
}
