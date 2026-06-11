/* SPDX-License-Identifier: MIT */
/*
 * prometheus.c — Prometheus exposition HTTP server, live values.
 *
 * Serves /metrics in the canonical Prometheus text format. A
 * background thread runs the normal libintp sampling loop
 * (intp_sample at the context's cadence) into a mutex-guarded
 * snapshot; each scrape renders the latest snapshot. An unavailable
 * metric is omitted from the exposition — never exported as 0
 * (ADR-0001 honest reporting). intp_scrape_staleness_seconds lets
 * operators alert on a wedged sampler.
 *
 * Metric naming follows ADR-0002: the seven core metrics are
 * intp_<short>_ratio gauges; other units map to _bytes_per_second /
 * _ops_per_second etc. intp_build_info carries build metadata.
 *
 * Text format only. The protobuf-based remote_write path is rare in
 * practice and would drag libprotobuf into libintp.so for everyone;
 * if it's ever needed, it belongs in a separate optional sink.
 */

#include <intp/intp.h>
#include <intp/metrics.h>
#include <intp/version.h>

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../core/core.h"

#if INTP_HAVE_PROMETHEUS

#include <microhttpd.h>
#include <pthread.h>
#include <time.h>

static volatile sig_atomic_t prom_stop;

static void prom_signal_handler(int sig)
{
	(void)sig;
	prom_stop = 1;
}

/* ---- latest-sample snapshot, shared sampler thread -> handlers ---- */

#define SNAP_MAX 32

struct prom_snapshot {
	pthread_mutex_t lock;
	size_t n;
	struct {
		char name[96];
		enum intp_unit unit;
		double value;
	} v[SNAP_MAX];
	struct timespec taken; /* CLOCK_REALTIME of the last sample */
	int have_sample;
};

struct prom_serve_state {
	struct intp_ctx *ctx;
	struct prom_snapshot snap;
	pthread_t sampler;
	volatile sig_atomic_t sampler_stop;
};

static void *prom_sampler_main(void *arg)
{
	struct prom_serve_state *st = arg;

	while (!st->sampler_stop) {
		struct intp_sample s;
		int rc = intp_sample(st->ctx, &s);

		if (rc == -EINTR)
			continue;
		if (rc)
			break; /* -ENODEV etc.: leave the last snapshot */

		pthread_mutex_lock(&st->snap.lock);
		st->snap.n = 0;
		for (size_t i = 0; i < s.values_len && st->snap.n < SNAP_MAX;
		     i++) {
			const struct intp_metric_value *v = &s.values[i];

			if (v->kind != INTP_VALUE_DOUBLE)
				continue; /* unavailable = omitted */
			snprintf(st->snap.v[st->snap.n].name,
				 sizeof(st->snap.v[0].name), "%s", v->name);
			st->snap.v[st->snap.n].unit = v->unit;
			st->snap.v[st->snap.n].value = v->d;
			st->snap.n++;
		}
		st->snap.taken = s.ts;
		st->snap.have_sample = 1;
		pthread_mutex_unlock(&st->snap.lock);
	}
	return NULL;
}

/* "system.interference.cpu" -> "cpu"; "system.portable.psp" -> "psp" */
static const char *prom_short_name(const char *full)
{
	const char *dot = strrchr(full, '.');

	return dot ? dot + 1 : full;
}

static const char *prom_unit_suffix(enum intp_unit u)
{
	switch (u) {
	case INTP_UNIT_RATIO:
		return "ratio";
	case INTP_UNIT_BYTES_PER_SECOND:
		return "bytes_per_second";
	case INTP_UNIT_OPS_PER_SECOND:
		return "ops_per_second";
	case INTP_UNIT_PERCENT:
		return "percent";
	default:
		return "value";
	}
}

/* Render the live /metrics body. Returns a malloc'd buffer that MHD
 * will free via MHD_RESPMEM_MUST_FREE; sets *out_len to its size.
 * Returns NULL on allocation or formatting failure. */
static char *prom_render_metrics(struct prom_serve_state *st, size_t *out_len)
{
	size_t cap = 8192;
	char *buf = malloc(cap);

	if (!buf)
		return NULL;

	size_t n = 0;

#define APPEND(fmt, ...)                                                  \
	do {                                                              \
		int w = snprintf(buf + n, cap - n, (fmt), ##__VA_ARGS__); \
		if (w < 0 || (size_t)w >= cap - n) {                      \
			free(buf);                                        \
			return NULL;                                      \
		}                                                         \
		n += (size_t)w;                                           \
	} while (0)

	APPEND("# HELP intp_build_info Build-time configuration of the running "
	       "intp binary.\n"
	       "# TYPE intp_build_info gauge\n"
	       "intp_build_info{version=\"%s\",schema=\"%s\",ebpf=\"%s\"} 1\n",
	       intp_version_string(), INTP_SCHEMA_VERSION_STR,
	       INTP_HAVE_EBPF ? "enabled" : "disabled");

	pthread_mutex_lock(&st->snap.lock);

	/* Count the core metrics carrying real values. */
	int implemented = 0;

	for (size_t i = 0; i < st->snap.n; i++) {
		if (strncmp(st->snap.v[i].name, "system.interference.", 20) ==
		    0)
			implemented++;
	}
	APPEND("# HELP intp_metrics_implemented Core metrics reporting real "
	       "values this scrape (of %d).\n"
	       "# TYPE intp_metrics_implemented gauge\n"
	       "intp_metrics_implemented %d\n",
	       INTP_CORE_METRICS_COUNT, implemented);

	if (st->snap.have_sample) {
		struct timespec now;

		clock_gettime(CLOCK_REALTIME, &now);

		double age =
			(double)(now.tv_sec - st->snap.taken.tv_sec) +
			(double)(now.tv_nsec - st->snap.taken.tv_nsec) / 1e9;
		APPEND("# HELP intp_scrape_staleness_seconds Age of the "
		       "rendered sample at scrape time.\n"
		       "# TYPE intp_scrape_staleness_seconds gauge\n"
		       "intp_scrape_staleness_seconds %.3f\n",
		       age < 0 ? 0.0 : age);
	}

	for (size_t i = 0; i < st->snap.n; i++) {
		APPEND("intp_%s_%s %.6f\n", prom_short_name(st->snap.v[i].name),
		       prom_unit_suffix(st->snap.v[i].unit),
		       st->snap.v[i].value);
	}
	pthread_mutex_unlock(&st->snap.lock);

#undef APPEND
	*out_len = n;
	return buf;
}

static enum MHD_Result prom_handler(void *cls, struct MHD_Connection *conn,
				    const char *url, const char *method,
				    const char *version,
				    const char *upload_data,
				    size_t *upload_data_size, void **req_cls)
{
	struct prom_serve_state *st = cls;

	(void)version;
	(void)upload_data;
	(void)upload_data_size;
	(void)req_cls;

	if (strcmp(method, "GET") != 0) {
		const char *body = "Method Not Allowed\n";
		struct MHD_Response *r =
			MHD_create_response_from_buffer(strlen(body),
							(void *)(uintptr_t)body,
							MHD_RESPMEM_PERSISTENT);
		enum MHD_Result rc = MHD_queue_response(conn, 405, r);

		MHD_destroy_response(r);
		return rc;
	}

	if (strcmp(url, "/metrics") != 0) {
		const char *body = "intp Prometheus exposition server\n"
				   "Endpoints:\n"
				   "  /metrics — Prometheus text format\n";
		struct MHD_Response *r =
			MHD_create_response_from_buffer(strlen(body),
							(void *)(uintptr_t)body,
							MHD_RESPMEM_PERSISTENT);
		enum MHD_Result rc = MHD_queue_response(conn, MHD_HTTP_OK, r);

		MHD_destroy_response(r);
		return rc;
	}

	size_t body_len = 0;
	char *body = prom_render_metrics(st, &body_len);

	if (!body)
		return MHD_NO;

	struct MHD_Response *r =
		MHD_create_response_from_buffer(body_len, body,
						MHD_RESPMEM_MUST_FREE);

	MHD_add_response_header(r, "Content-Type",
				"text/plain; version=0.0.4; charset=utf-8");

	enum MHD_Result rc = MHD_queue_response(conn, MHD_HTTP_OK, r);

	MHD_destroy_response(r);
	return rc;
}

/* Serve an already-opened context (the CLI path: --serve honors the
 * full targeting/backend-control surface parsed before it). Blocks
 * until SIGINT/SIGTERM. Does NOT close the context. */
int intp_core_serve_prometheus(struct intp_ctx *ctx, uint16_t port)
{
	if (!ctx)
		return -EINVAL;

	struct prom_serve_state st;

	memset(&st, 0, sizeof(st));
	st.ctx = ctx;
	pthread_mutex_init(&st.snap.lock, NULL);

	struct MHD_Daemon *d =
		MHD_start_daemon(MHD_USE_INTERNAL_POLLING_THREAD, port, NULL,
				 NULL, &prom_handler, &st, MHD_OPTION_END);

	if (!d) {
		pthread_mutex_destroy(&st.snap.lock);
		return -EADDRINUSE;
	}

	if (pthread_create(&st.sampler, NULL, prom_sampler_main, &st) != 0) {
		MHD_stop_daemon(d);
		pthread_mutex_destroy(&st.snap.lock);
		return -EAGAIN;
	}

	/* Restore the caller's signal disposition on exit so libintp
	 * doesn't permanently override their handlers. */
	struct sigaction old_int, old_term;
	struct sigaction sa = {.sa_handler = prom_signal_handler};

	sigemptyset(&sa.sa_mask);
	sigaction(SIGINT, &sa, &old_int);
	sigaction(SIGTERM, &sa, &old_term);

	while (!prom_stop)
		pause();

	st.sampler_stop = 1;
	/* The sampler blocks in clock_nanosleep inside intp_sample; the
	 * signal already interrupted it (EINTR) or the next tick ends
	 * within one interval. */
	pthread_join(st.sampler, NULL);

	sigaction(SIGINT, &old_int, NULL);
	sigaction(SIGTERM, &old_term, NULL);
	MHD_stop_daemon(d);
	pthread_mutex_destroy(&st.snap.lock);
	return 0;
}

__attribute__((visibility("default"))) int intp_serve_prometheus(uint16_t port)
{
	/* Public convenience wrapper: default system-wide context at
	 * the 1 s cadence. Linked consumers needing targeting open
	 * their own context and use the richer internal entry through
	 * the CLI. */
	struct intp_ctx *ctx = NULL;
	int rc = intp_open(NULL, &ctx);

	if (rc)
		return rc;

	rc = intp_core_serve_prometheus(ctx, port);
	intp_close(ctx);
	return rc;
}

#else /* !INTP_HAVE_PROMETHEUS */

int intp_core_serve_prometheus(struct intp_ctx *ctx, uint16_t port)
{
	(void)ctx;
	(void)port;
	return -ENOSYS;
}

__attribute__((visibility("default"))) int intp_serve_prometheus(uint16_t port)
{
	(void)port;
	return -ENOSYS;
}

#endif
