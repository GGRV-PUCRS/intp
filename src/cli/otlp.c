/* SPDX-License-Identifier: MIT */
/*
 * otlp.c — OTLP/HTTP+JSON metrics exporter (see otlp.h, ADR-0011).
 *
 * One POST per sample (default 1 Hz) of an OTLP ExportMetricsServiceRequest
 * to the configured collector, over a reused CURL handle with short
 * timeouts. A dead/slow collector degrades to a single logged error and
 * dropped exports — it never stalls or fails the sampling loop. Config
 * follows the OTel env conventions, so it works with existing setups.
 *
 * File-static state: like the other in-CLI sinks this is single-instance
 * per process (the CLI opens exactly one sink).
 */

#include "otlp.h"

#include <intp/version.h>

#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../core/core.h"
#include "otlp_json.h"

#define OTLP_MAX_ATTRS 48

static struct {
	CURL *curl;
	struct curl_slist *headers;
	char endpoint[1024];
	char *attr_storage; /* OTEL_RESOURCE_ATTRIBUTES copy, tokenized in place
			     */
	struct otlp_kv env_attrs[OTLP_MAX_ATTRS];
	size_t n_env_attrs;
	int fail_logged;
} g;

static size_t discard_cb(char *ptr, size_t size, size_t nmemb, void *userdata)
{
	(void)ptr;
	(void)userdata;
	return size * nmemb; /* ignore the collector's response body */
}

/* Endpoint precedence per the OTel spec: signal-specific env (used as-is)
 * -> base env (+ /v1/metrics) -> default. */
static void resolve_endpoint(void)
{
	const char *e = getenv("OTEL_EXPORTER_OTLP_METRICS_ENDPOINT");

	if (e && e[0]) {
		snprintf(g.endpoint, sizeof(g.endpoint), "%s", e);
		return;
	}
	e = getenv("OTEL_EXPORTER_OTLP_ENDPOINT");
	if (e && e[0]) {
		snprintf(g.endpoint, sizeof(g.endpoint), "%s/v1/metrics", e);
		return;
	}
	snprintf(g.endpoint, sizeof(g.endpoint),
		 "http://localhost:4318/v1/metrics");
}

static void parse_headers(void)
{
	g.headers = curl_slist_append(NULL, "Content-Type: application/json");

	const char *h = getenv("OTEL_EXPORTER_OTLP_HEADERS");

	if (!h || !h[0])
		return;

	char *copy = strdup(h);

	if (!copy)
		return;

	char *save = NULL;

	for (char *tok = strtok_r(copy, ",", &save); tok;
	     tok = strtok_r(NULL, ",", &save)) {
		char *eq = strchr(tok, '=');

		if (!eq)
			continue;
		*eq = '\0';
		char line[1024];

		snprintf(line, sizeof(line), "%s: %s", tok, eq + 1);
		g.headers = curl_slist_append(g.headers, line);
	}
	free(copy);
}

static void parse_env_attrs(void)
{
	const char *a = getenv("OTEL_RESOURCE_ATTRIBUTES");

	if (!a || !a[0])
		return;
	g.attr_storage = strdup(a);
	if (!g.attr_storage)
		return;

	char *save = NULL;

	for (char *tok = strtok_r(g.attr_storage, ",", &save);
	     tok && g.n_env_attrs < OTLP_MAX_ATTRS;
	     tok = strtok_r(NULL, ",", &save)) {
		char *eq = strchr(tok, '=');

		if (!eq)
			continue;
		*eq = '\0';
		g.env_attrs[g.n_env_attrs].key = tok;
		g.env_attrs[g.n_env_attrs].value = eq + 1;
		g.n_env_attrs++;
	}
}

static int otlp_open(struct intp_plugin_ctx *ctx, int fd)
{
	(void)ctx;
	(void)fd; /* network sink: the output fd is unused */

	memset(&g, 0, sizeof(g));
	curl_global_init(CURL_GLOBAL_DEFAULT);
	resolve_endpoint();
	parse_headers();
	parse_env_attrs();

	g.curl = curl_easy_init();
	if (!g.curl) {
		fprintf(stderr, "intp: otlp: failed to initialise libcurl\n");
		return -1;
	}
	curl_easy_setopt(g.curl, CURLOPT_URL, g.endpoint);
	curl_easy_setopt(g.curl, CURLOPT_HTTPHEADER, g.headers);
	curl_easy_setopt(g.curl, CURLOPT_POST, 1L);
	curl_easy_setopt(g.curl, CURLOPT_CONNECTTIMEOUT, 2L);
	curl_easy_setopt(g.curl, CURLOPT_TIMEOUT, 5L);
	curl_easy_setopt(g.curl, CURLOPT_NOSIGNAL, 1L);
	curl_easy_setopt(g.curl, CURLOPT_WRITEFUNCTION, discard_cb);

	fprintf(stderr, "intp: exporting OTLP/JSON metrics to %s\n",
		g.endpoint);
	return 0;
}

static int otlp_emit(struct intp_plugin_ctx *ctx,
		     const struct intp_sample *sample)
{
	(void)ctx;
	if (!g.curl || !sample)
		return -1;

	struct otlp_kv attrs[OTLP_MAX_ATTRS];
	size_t n = 0;
	char pidbuf[24];
	struct intp_output_target ot;

	intp_core_output_target(&ot);
	attrs[n++] =
		(struct otlp_kv){"host.name", sample->host ? sample->host : ""};
	attrs[n++] = (struct otlp_kv){"service.name", "intp"};
	attrs[n++] = (struct otlp_kv){"service.version", INTP_VERSION};
	attrs[n++] = (struct otlp_kv){"intp.target.kind", ot.kind};
	if (ot.cgroup && n < OTLP_MAX_ATTRS)
		attrs[n++] = (struct otlp_kv){"intp.target.cgroup", ot.cgroup};
	if (ot.pid > 0 && n < OTLP_MAX_ATTRS) {
		snprintf(pidbuf, sizeof(pidbuf), "%ld", ot.pid);
		attrs[n++] = (struct otlp_kv){"process.pid", pidbuf};
	}
	for (size_t i = 0; i < g.n_env_attrs && n < OTLP_MAX_ATTRS; i++)
		attrs[n++] = g.env_attrs[i];

	char *body = otlp_json_build(sample, attrs, n);

	if (!body)
		return -1;

	curl_easy_setopt(g.curl, CURLOPT_POSTFIELDS, body);
	curl_easy_setopt(g.curl, CURLOPT_POSTFIELDSIZE, (long)strlen(body));

	CURLcode rc = curl_easy_perform(g.curl);

	if (rc != CURLE_OK) {
		if (!g.fail_logged) {
			fprintf(stderr,
				"intp: otlp export to %s failed: %s "
				"(further errors suppressed)\n",
				g.endpoint, curl_easy_strerror(rc));
			g.fail_logged = 1;
		}
	} else {
		g.fail_logged = 0;
	}
	free(body);
	/* A dead collector must never abort profiling. */
	return 0;
}

static void otlp_close(struct intp_plugin_ctx *ctx)
{
	(void)ctx;
	if (g.curl)
		curl_easy_cleanup(g.curl);
	if (g.headers)
		curl_slist_free_all(g.headers);
	free(g.attr_storage);
	curl_global_cleanup();
	memset(&g, 0, sizeof(g));
}

const struct intp_output_vtable intp_output_otlp = {
	.format_name = "otlp",
	.open = otlp_open,
	.emit = otlp_emit,
	.close = otlp_close,
};
