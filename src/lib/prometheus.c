/* SPDX-License-Identifier: MIT */
/*
 * prometheus.c — Prometheus exposition HTTP server, live values.
 *
 * Serves /metrics in the canonical Prometheus text format. A background
 * thread runs the normal libintp sampling loop (intp_sample at the
 * context's cadence) into a mutex-guarded snapshot; each scrape renders
 * the latest snapshot. An unavailable metric is omitted from the
 * exposition — never exported as 0 (ADR-0001 honest reporting).
 * intp_scrape_staleness_seconds lets operators alert on a wedged sampler.
 *
 * Every series carries identity labels ({instance,target_kind,cgroup,
 * pid}) so a multi-target scraper (e.g. expdb monitoring per container/
 * VM) can attribute it. Naming, unit suffixes, type classification, and
 * label-value escaping are shared with the one-shot --format prometheus
 * sink through prom_common, so the two renderers cannot drift.
 *
 * The server binds the loopback interface by default; pass an explicit
 * host (including 0.0.0.0) through the CLI --serve URI to expose it. The
 * render buffer grows on demand so a large metric set never fails a
 * scrape.
 *
 * Text format only. The protobuf-based remote_write path is rare in
 * practice and would drag libprotobuf into libintp.so for everyone; if
 * it's ever needed, it belongs in a separate optional sink.
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
#include "../output/prom_common.h"

#if INTP_HAVE_PROMETHEUS

#include <microhttpd.h>
#include <pthread.h>
#include <time.h>

#include <inttypes.h>
#include <stdarg.h>
#include <stdint.h>

#include <arpa/inet.h>
#include <netinet/in.h>

static volatile sig_atomic_t prom_stop;

static void prom_signal_handler(int sig)
{
	(void)sig;
	prom_stop = 1;
}

/* ---- latest-sample snapshot, shared sampler thread -> handlers ---- */

#define SNAP_MAX 64

struct prom_snapshot {
	pthread_mutex_t lock;
	size_t n;
	struct {
		char name[96];
		const char *help; /* static schema string, ctx-lifetime */
		enum intp_unit unit;
		enum intp_value_kind kind;
		union {
			double d;
			int64_t i64;
			uint64_t u64;
		} val;
	} v[SNAP_MAX];
	struct timespec taken; /* CLOCK_REALTIME of the last sample */
	int have_sample;
};

struct prom_serve_state {
	struct intp_ctx *ctx;
	struct prom_snapshot snap;
	char host_esc[300];	/* escaped host, for intp_build_info */
	char inst_label[320];	/* {instance="…"} for server-health metrics */
	char full_label[2048];	/* {instance,target_kind,cgroup,pid} per metric */
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

			if (v->kind == INTP_VALUE_NONE)
				continue; /* unavailable = omitted */
			size_t k = st->snap.n;

			snprintf(st->snap.v[k].name, sizeof(st->snap.v[0].name),
				 "%s", v->name);
			st->snap.v[k].help = intp_core_metric_help(v->name);
			st->snap.v[k].unit = v->unit;
			st->snap.v[k].kind = v->kind;
			switch (v->kind) {
			case INTP_VALUE_DOUBLE:
				st->snap.v[k].val.d = v->d;
				break;
			case INTP_VALUE_INT64:
				st->snap.v[k].val.i64 = v->i64;
				break;
			case INTP_VALUE_UINT64:
				st->snap.v[k].val.u64 = v->u64;
				break;
			default:
				continue;
			}
			st->snap.n++;
		}
		st->snap.taken = s.ts;
		st->snap.have_sample = 1;
		pthread_mutex_unlock(&st->snap.lock);
	}
	return NULL;
}

/* ---- growable render buffer (a failed scrape on overflow is worse than
 * a realloc; ADR-0001 honest reporting must survive large metric sets) ---- */

struct prom_buf {
	char *p;
	size_t len;
	size_t cap;
};

/* Append printf-style; grow on overflow. Returns 0, or -ENOMEM / -EINVAL. */
static int prom_buf_appendf(struct prom_buf *b, const char *fmt, ...)
{
	for (;;) {
		va_list ap;

		va_start(ap, fmt);
		int w = vsnprintf(b->p + b->len, b->cap - b->len, fmt, ap);
		va_end(ap);

		if (w < 0)
			return -EINVAL;
		if ((size_t)w < b->cap - b->len) {
			b->len += (size_t)w;
			return 0;
		}

		size_t need = b->len + (size_t)w + 1;
		size_t ncap = b->cap ? b->cap * 2 : 16384;

		if (ncap < need)
			ncap = need;

		char *np = realloc(b->p, ncap);

		if (!np)
			return -ENOMEM;
		b->p = np;
		b->cap = ncap;
		/* retry with the larger buffer */
	}
}

static int prom_append_value(struct prom_buf *b, enum intp_value_kind kind,
			     const void *val)
{
	const union {
		double d;
		int64_t i64;
		uint64_t u64;
	} *v = val;

	switch (kind) {
	case INTP_VALUE_DOUBLE:
		return prom_buf_appendf(b, "%.6f\n", v->d);
	case INTP_VALUE_INT64:
		return prom_buf_appendf(b, "%" PRId64 "\n", v->i64);
	case INTP_VALUE_UINT64:
		return prom_buf_appendf(b, "%" PRIu64 "\n", v->u64);
	default:
		return prom_buf_appendf(b, "0\n");
	}
}

/* Render the live /metrics body. Returns a malloc'd buffer that MHD will
 * free via MHD_RESPMEM_MUST_FREE; sets *out_len. NULL only on OOM. */
static char *prom_render_metrics(struct prom_serve_state *st, size_t *out_len)
{
	struct prom_buf b = {.p = malloc(16384), .len = 0, .cap = 16384};

	if (!b.p)
		return NULL;

	int rc = prom_buf_appendf(
		&b,
		"# HELP intp_build_info Build-time configuration of the running "
		"intp binary.\n"
		"# TYPE intp_build_info gauge\n"
		"intp_build_info{instance=\"%s\",version=\"%s\",schema=\"%s\","
		"ebpf=\"%s\"} 1\n",
		st->host_esc, intp_version_string(), INTP_SCHEMA_VERSION_STR,
		INTP_HAVE_EBPF ? "enabled" : "disabled");

	pthread_mutex_lock(&st->snap.lock);

	/* Count the core metrics carrying real values. */
	int implemented = 0;

	for (size_t i = 0; i < st->snap.n; i++) {
		if (strncmp(st->snap.v[i].name, "system.interference.", 20) ==
		    0)
			implemented++;
	}
	if (!rc)
		rc = prom_buf_appendf(
			&b,
			"# HELP intp_metrics_implemented Core metrics reporting "
			"real values this scrape (of %d).\n"
			"# TYPE intp_metrics_implemented gauge\n"
			"intp_metrics_implemented%s %d\n",
			INTP_CORE_METRICS_COUNT, st->inst_label, implemented);

	if (!rc && st->snap.have_sample) {
		struct timespec now;

		clock_gettime(CLOCK_REALTIME, &now);

		double age =
			(double)(now.tv_sec - st->snap.taken.tv_sec) +
			(double)(now.tv_nsec - st->snap.taken.tv_nsec) / 1e9;
		rc = prom_buf_appendf(
			&b,
			"# HELP intp_scrape_staleness_seconds Age of the "
			"rendered sample at scrape time.\n"
			"# TYPE intp_scrape_staleness_seconds gauge\n"
			"intp_scrape_staleness_seconds%s %.3f\n",
			st->inst_label, age < 0 ? 0.0 : age);
	}

	for (size_t i = 0; i < st->snap.n && !rc; i++) {
		const char *sname = intp_prom_short_name(st->snap.v[i].name);
		const char *suffix = intp_prom_unit_suffix(st->snap.v[i].unit);
		const char *type = intp_prom_type_for(st->snap.v[i].unit,
						      st->snap.v[i].kind);
		const char *help = st->snap.v[i].help && st->snap.v[i].help[0]
					   ? st->snap.v[i].help
					   : st->snap.v[i].name;

		rc = prom_buf_appendf(&b,
				      "# HELP intp_%s_%s %s\n"
				      "# TYPE intp_%s_%s %s\n"
				      "intp_%s_%s%s ",
				      sname, suffix, help, sname, suffix, type,
				      sname, suffix, st->full_label);
		if (!rc)
			rc = prom_append_value(&b, st->snap.v[i].kind,
					       &st->snap.v[i].val);
	}
	pthread_mutex_unlock(&st->snap.lock);

	if (rc) {
		free(b.p);
		return NULL;
	}
	*out_len = b.len;
	return b.p;
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

/* Build the per-process identity label blocks once at serve start (host
 * and target are immutable for the ctx lifetime). */
static void prom_build_labels(struct prom_serve_state *st)
{
	const char *host = st->ctx->host;
	struct intp_output_target ot;

	intp_core_output_target(&ot);
	intp_prom_escape_label(st->host_esc, sizeof(st->host_esc), host);
	intp_prom_labels(st->inst_label, sizeof(st->inst_label), host, NULL,
			 NULL, 0);
	intp_prom_labels(st->full_label, sizeof(st->full_label), host, ot.kind,
			 ot.cgroup, ot.pid);
}

/* Serve an already-opened context (the CLI path: --serve honors the full
 * targeting/backend-control surface parsed before it). Binds
 * opts->bind_host (NULL/"" => loopback 127.0.0.1; "0.0.0.0" = all
 * interfaces). Blocks until SIGINT/SIGTERM. Does NOT close the context. */
int intp_core_serve_prometheus_ex(struct intp_ctx *ctx,
				  const struct intp_serve_opts *opts)
{
	if (!ctx || !opts)
		return -EINVAL;

	struct prom_serve_state st;

	memset(&st, 0, sizeof(st));
	st.ctx = ctx;
	pthread_mutex_init(&st.snap.lock, NULL);
	prom_build_labels(&st);

	/* Default to loopback; only bind a routable/wildcard address when
	 * the operator asked for it explicitly. */
	const char *host = (opts->bind_host && opts->bind_host[0])
				   ? opts->bind_host
				   : "127.0.0.1";
	struct sockaddr_in sa;

	memset(&sa, 0, sizeof(sa));
	sa.sin_family = AF_INET;
	sa.sin_port = htons(opts->port);
	if (inet_pton(AF_INET, host, &sa.sin_addr) != 1) {
		fprintf(stderr,
			"intp: --serve host '%s' is not an IPv4 literal; "
			"binding 127.0.0.1 instead.\n",
			host);
		inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr);
	}

	struct MHD_Daemon *d = MHD_start_daemon(
		MHD_USE_INTERNAL_POLLING_THREAD, opts->port, NULL, NULL,
		&prom_handler, &st, MHD_OPTION_SOCK_ADDR,
		(const struct sockaddr *)&sa, MHD_OPTION_END);

	if (!d) {
		pthread_mutex_destroy(&st.snap.lock);
		return -EADDRINUSE;
	}

	if (pthread_create(&st.sampler, NULL, prom_sampler_main, &st) != 0) {
		MHD_stop_daemon(d);
		pthread_mutex_destroy(&st.snap.lock);
		return -EAGAIN;
	}

	/* Restore the caller's signal disposition on exit so libintp doesn't
	 * permanently override their handlers. */
	struct sigaction old_int, old_term;
	struct sigaction sa_sig = {.sa_handler = prom_signal_handler};

	sigemptyset(&sa_sig.sa_mask);
	sigaction(SIGINT, &sa_sig, &old_int);
	sigaction(SIGTERM, &sa_sig, &old_term);

	while (!prom_stop)
		pause();

	st.sampler_stop = 1;
	/* The sampler blocks in clock_nanosleep inside intp_sample; the
	 * signal already interrupted it (EINTR) or the next tick ends within
	 * one interval. */
	pthread_join(st.sampler, NULL);

	sigaction(SIGINT, &old_int, NULL);
	sigaction(SIGTERM, &old_term, NULL);
	MHD_stop_daemon(d);
	pthread_mutex_destroy(&st.snap.lock);
	return 0;
}

int intp_core_serve_prometheus(struct intp_ctx *ctx, uint16_t port)
{
	struct intp_serve_opts opts = {.bind_host = NULL, .port = port};

	return intp_core_serve_prometheus_ex(ctx, &opts);
}

__attribute__((visibility("default"))) int intp_serve_prometheus(uint16_t port)
{
	/* Public convenience wrapper: default system-wide context at the 1 s
	 * cadence, bound to loopback. Linked consumers needing targeting or a
	 * routable bind address open their own context and use the richer
	 * internal entry through the CLI. */
	struct intp_ctx *ctx = NULL;
	int rc = intp_open(NULL, &ctx);

	if (rc)
		return rc;

	rc = intp_core_serve_prometheus(ctx, port);
	intp_close(ctx);
	return rc;
}

#else /* !INTP_HAVE_PROMETHEUS */

int intp_core_serve_prometheus_ex(struct intp_ctx *ctx,
				  const struct intp_serve_opts *opts)
{
	(void)ctx;
	(void)opts;
	return -ENOSYS;
}

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
