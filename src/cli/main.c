/* SPDX-License-Identifier: MIT */
/*
 * intp(1) — Linux interference profiler CLI.
 *
 * A thin wrapper over libintp; everything the binary can do, a linked
 * program can also do via <intp/intp.h>. Sampling flow:
 *
 *   parse args → intp_open (backend probe + selection) →
 *   sink->open → intp_sample loop → sink->close → intp_close
 *
 * On SIGINT/SIGTERM the sampling loop exits cleanly and backend
 * cleanup releases any held resources (perf fds, resctrl mon_groups).
 */

#include <intp/intp.h>

#include <errno.h>
#include <getopt.h>
#include <limits.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "../core/core.h"
#include "../core/detect.h"
#include "../output/output.h"
#include "serve_uri.h"
#if INTP_HAVE_OTEL
#include "otlp.h"
#endif

static const char usage_text[] =
	"Usage: intp [OPTIONS]\n"
	"\n"
	"Linux interference profiler — measures cross-workload interference\n"
	"across CPU, last-level cache, memory bandwidth, block I/O, and\n"
	"network resources. Produces structured samples on stdout.\n"
	"\n"
	"Targeting:\n"
	"  -p, --pid PID              profile a specific process\n"
	"      --pids LIST            profile a comma-separated PID list\n"
	"      --comm NAME            profile every process with this comm "
	"name\n"
	"  -c, --cgroup PATH          profile a cgroup-v2 path (post-1.0)\n"
	"      --target-container P   alias of --cgroup (container tenant)\n"
	"      --target-vm IFACE      (experimental) profile a KVM/QEMU VM "
	"via its\n"
	"                             host tap iface; combine with -c "
	"QEMU_SCOPE\n"
	"      --tap-iface NAME       host tap/vnet iface for VM netp\n"
	"      --exact                leaf-only cgroup matching (skip the "
	"subtree)\n"
	"      (default: profile the whole system)\n"
	"\n"
	"Experimental (require --experimental; ADR-0008):\n"
	"      --experimental         opt in to experimental features\n"
	"      --portable-metrics     (experimental) append the "
	"system.portable.*\n"
	"                             metric set (schedlat psi_mem membw_est "
	"psi_io\n"
	"                             schedthr steal psp idle_preempt)\n"
	"\n"
	"Sampling:\n"
	"  -i, --interval DURATION    sampling interval (default: 1s; e.g. "
	"100ms)\n"
	"  -d, --duration DURATION    stop after DURATION (default: run "
	"forever)\n"
	"\n"
	"Output:\n"
	"  -f, --format FORMAT        jsonl (default), tsv-legacy-v1, "
	"prometheus,\n"
	"                             otlp (OTLP/HTTP+JSON; built with "
	"-Dwith_otel)\n"
	"      --print-schema         dump active schema as JSON Schema and "
	"exit\n"
	"      --serve[=URI]          long-running exposition mode (Prometheus "
	"on :9100)\n"
	"                             URI examples: prometheus  "
	"prometheus://:9090\n"
	"                             (note: must use --serve=URI, not --serve "
	"URI)\n"
	"\n"
	"Diagnostics:\n"
	"      --probe                check whether eBPF backends will work "
	"here\n"
	"      --list-backends        print capabilities and per-metric "
	"backend\n"
	"                             candidates, then exit\n"
	"\n"
	"Backend control:\n"
	"      --disable-backend NAME force a core backend off (NAME or "
	"metric:NAME)\n"
	"      --force-backend M:ID   force a backend, e.g. mbw:resctrl_mbm\n"
	"      --disable-metric M     skip metric M entirely\n"
	"\n"
	"Hardware overrides (0/absent = autodetect):\n"
	"      --nic-speed-bps N      link capacity for netp normalization\n"
	"      --mem-bw-max-bps N     max DRAM bandwidth for mbw\n"
	"      --llc-size-bytes N     LLC capacity for llcocc\n"
	"      --disk-bw-max-bps N    device bandwidth ceiling (cgroup blk)\n"
	"      --iface NAME           network interface\n"
	"      --disk NAME            block device\n"
	"\n"
	"Plugins:\n"
	"      --plugin-dir DIR       additional plugin search directory\n"
	"      --enable-plugin NAME   force a plugin to load "
	"(comma-separated)\n"
	"      --disable-plugin NAME  force a plugin off (comma-separated)\n"
	"\n"
	"Misc:\n"
	"  -v, --verbose              increase logging level\n"
	"  -V, --version              print version and build options, then "
	"exit\n"
	"  -h, --help                 print this help and exit\n"
	"\n"
	"For documentation see intp(1) or https://github.com/GGRV-PUCRS/intp.\n";

static void print_version(void)
{
	printf("intp %s\n", intp_version_string());
	printf("Build options:\n");
#if INTP_HAVE_EBPF
	printf("  eBPF backends:        enabled\n");
#else
	printf("  eBPF backends:        disabled\n");
#endif
#if INTP_HAVE_BPFTRACE
	printf("  bpftrace plugin:      enabled\n");
#endif
#if INTP_HAVE_GPU_NVML
	printf("  GPU NVML plugin:      enabled\n");
#endif
#if INTP_HAVE_GPU_ROCM
	printf("  GPU ROCm plugin:      enabled\n");
#endif
#if INTP_HAVE_GPU_ZES
	printf("  GPU Level Zero plug.: enabled\n");
#endif
#if INTP_HAVE_PROMETHEUS
	printf("  Prometheus sink:      enabled\n");
#else
	printf("  Prometheus sink:      disabled\n");
#endif
#if INTP_HAVE_OTEL
	printf("  OpenTelemetry sink:   enabled\n");
#endif
	printf("\n");
	printf("intp is the production successor to across-intp "
	       "(https://github.com/GGRV-PUCRS/across-intp).\n");
	printf("License: MIT. See LICENSE in the source distribution.\n");
}

static void print_help(void)
{
	fputs(usage_text, stdout);
}

static volatile sig_atomic_t g_running = 1;

static void on_signal(int sig)
{
	(void)sig;
	g_running = 0;
}

/* Parse "1", "1.5", "2s", "100ms" into milliseconds; <0 on error. */
static long parse_duration_ms(const char *s)
{
	char *end;
	double v = strtod(s, &end);

	if (end == s || v < 0)
		return -1;
	if (*end == '\0' || strcmp(end, "s") == 0)
		return (long)(v * 1000.0);
	if (strcmp(end, "ms") == 0)
		return (long)v;
	return -1;
}

enum {
	OPT_PRINT_SCHEMA = 1000,
	OPT_PLUGIN_DIR,
	OPT_ENABLE_PLUGIN,
	OPT_DISABLE_PLUGIN,
	OPT_DISABLE_BACKEND,
	OPT_SERVE,
	OPT_PROBE,
	OPT_LIST_BACKENDS,
	OPT_FORCE_BACKEND,
	OPT_DISABLE_METRIC,
	OPT_NIC_SPEED,
	OPT_MEM_BW,
	OPT_LLC_SIZE,
	OPT_DISK_BW,
	OPT_IFACE,
	OPT_DISK,
	OPT_PIDS,
	OPT_COMM,
	OPT_TARGET_CONTAINER,
	OPT_TARGET_VM,
	OPT_TAP_IFACE,
	OPT_EXACT,
	OPT_EXPERIMENTAL,
	OPT_PORTABLE,
};

static const struct option long_opts[] = {
	{"pid", required_argument, NULL, 'p'},
	{"cgroup", required_argument, NULL, 'c'},
	{"interval", required_argument, NULL, 'i'},
	{"duration", required_argument, NULL, 'd'},
	{"format", required_argument, NULL, 'f'},
	{"verbose", no_argument, NULL, 'v'},
	{"version", no_argument, NULL, 'V'},
	{"help", no_argument, NULL, 'h'},
	{"print-schema", no_argument, NULL, OPT_PRINT_SCHEMA},
	{"plugin-dir", required_argument, NULL, OPT_PLUGIN_DIR},
	{"enable-plugin", required_argument, NULL, OPT_ENABLE_PLUGIN},
	{"disable-plugin", required_argument, NULL, OPT_DISABLE_PLUGIN},
	{"disable-backend", required_argument, NULL, OPT_DISABLE_BACKEND},
	{"serve", optional_argument, NULL, OPT_SERVE},
	{"probe", no_argument, NULL, OPT_PROBE},
	{"list-backends", no_argument, NULL, OPT_LIST_BACKENDS},
	{"force-backend", required_argument, NULL, OPT_FORCE_BACKEND},
	{"disable-metric", required_argument, NULL, OPT_DISABLE_METRIC},
	{"nic-speed-bps", required_argument, NULL, OPT_NIC_SPEED},
	{"mem-bw-max-bps", required_argument, NULL, OPT_MEM_BW},
	{"llc-size-bytes", required_argument, NULL, OPT_LLC_SIZE},
	{"disk-bw-max-bps", required_argument, NULL, OPT_DISK_BW},
	{"iface", required_argument, NULL, OPT_IFACE},
	{"disk", required_argument, NULL, OPT_DISK},
	{"pids", required_argument, NULL, OPT_PIDS},
	{"comm", required_argument, NULL, OPT_COMM},
	{"target-container", required_argument, NULL, OPT_TARGET_CONTAINER},
	{"target-vm", required_argument, NULL, OPT_TARGET_VM},
	{"tap-iface", required_argument, NULL, OPT_TAP_IFACE},
	{"exact", no_argument, NULL, OPT_EXACT},
	{"experimental", no_argument, NULL, OPT_EXPERIMENTAL},
	{"portable-metrics", no_argument, NULL, OPT_PORTABLE},
	{NULL, 0, NULL, 0},
};

/* Parse "metric:backend_id" for --force-backend (ported parse_force
 * from across-intp intp-hybrid.c @ 2e87b30fc762). */
static int parse_force(const char *spec, struct intp_core_force *out)
{
	const char *colon = strchr(spec, ':');

	if (!colon || colon == spec || !colon[1])
		return -1;
	static char metric_bufs[16][16];
	static int n_used;

	if (n_used >= 16)
		return -1;

	char *mbuf = metric_bufs[n_used];
	size_t mlen = (size_t)(colon - spec);

	if (mlen >= 16)
		return -1;
	memcpy(mbuf, spec, mlen);
	mbuf[mlen] = '\0';
	n_used++;
	out->metric = mbuf;
	out->backend_id = colon + 1;
	return 0;
}

/* Port of the v2.1 --list-backends report: capability dump, selected
 * backend per metric, then the full ordered candidate list. */
static int run_list_backends(struct intp_ctx *ctx)
{
	print_capabilities(detect_cached(), stdout);
	printf("\n# selected backends:\n");
	for (size_t m = 0; m < INTP_CORE_METRICS_COUNT; m++) {
		const struct intp_core_selection *sel = &ctx->sel[m];
		const char *bid = sel->active ? sel->active->id : "none";
		const char *desc = sel->active
					   ? sel->active->description
					   : "no backend probed successfully";

		printf("  %-7s %-22s %s\n", sel->metric, bid, desc);
	}
	printf("\n# backend candidates (ordered by priority):\n");
	for (size_t m = 0; m < INTP_CORE_METRICS_COUNT; m++) {
		const struct intp_core_selection *sel = &ctx->sel[m];

		printf("%s:\n", sel->metric);
		for (int j = 0; j < sel->n_candidates; j++) {
			const struct intp_core_backend *b = sel->candidates[j];
			const char *mark = (sel->active == b) ? "*" : " ";

			printf("  %s %-22s %s\n", mark, b->id,
			       b->description ? b->description : "");
		}
	}
	return 0;
}

/* Run intp_probe_ebpf() and write a one-line human-readable status.
 * Exit code semantics let scripts branch on the failure mode:
 *   0  ok                      eBPF will work
 *   2  kernel too old          host needs an upgrade
 *   3  not permitted           process needs CAP_BPF / CAP_SYS_ADMIN
 *   4  unsupported             libintp built without eBPF
 *   5  error                   probe itself failed (see errno) */
static int run_probe_ebpf(void)
{
	enum intp_probe_result r = intp_probe_ebpf();
	switch (r) {
	case INTP_PROBE_OK:
		printf("intp probe: eBPF backends OK\n");
		return 0;
	case INTP_PROBE_KERNEL_TOO_OLD:
		printf("intp probe: eBPF UNAVAILABLE — kernel lacks BPF "
		       "program support (need 5.8+)\n");
		return 2;
	case INTP_PROBE_NOT_PERMITTED:
		printf("intp probe: eBPF UNAVAILABLE — caller lacks CAP_BPF or "
		       "CAP_SYS_ADMIN\n");
		return 3;
	case INTP_PROBE_UNSUPPORTED:
		printf("intp probe: eBPF UNAVAILABLE — this build was compiled "
		       "without libbpf\n");
		return 4;
	case INTP_PROBE_ERROR:
	default:
		printf("intp probe: eBPF UNKNOWN — probe failed (errno %d: "
		       "%s)\n",
		       errno, strerror(errno));
		return 5;
	}
}

#define MAX_DISABLED 32

/* Split a comma-separated list (e.g. --enable-plugin a,b) into arr from
 * index n. Tokens are strdup'd (process-lifetime; never freed). */
static size_t add_csv(const char *spec, const char **arr, size_t n, size_t max)
{
	char *copy = strdup(spec);

	if (!copy)
		return n;

	char *save = NULL;

	for (char *t = strtok_r(copy, ",", &save); t && n < max;
	     t = strtok_r(NULL, ",", &save))
		arr[n++] = strdup(t);
	free(copy);
	return n;
}

static int run_sampling(const struct intp_options *opts,
			const struct intp_core_extras *extras,
			const char *format, long duration_ms,
			bool print_schema_only, bool list_backends,
			bool do_serve,
			const struct intp_serve_target *serve_tgt)
{
	struct intp_ctx *ctx = NULL;
	int rc = intp_core_open_ex(opts, extras, &ctx);

	if (rc) {
		fprintf(stderr, "intp: open failed: %s\n", intp_strerror(rc));
		return 1;
	}

	if (list_backends) {
		rc = run_list_backends(ctx);
		intp_close(ctx);
		return rc ? 1 : 0;
	}

	if (print_schema_only) {
		rc = intp_print_schema(ctx, STDOUT_FILENO);
		intp_close(ctx);
		return rc ? 1 : 0;
	}

	if (do_serve) {
		const char *bind =
			serve_tgt->host[0] ? serve_tgt->host : "127.0.0.1";
		struct intp_serve_opts so = {
			.bind_host =
				serve_tgt->host[0] ? serve_tgt->host : NULL,
			.port = serve_tgt->port,
		};

		fprintf(stderr,
			"intp: serving Prometheus exposition on %s:%u (live "
			"values, %ums cadence).\n"
			"intp: scrape http://%s:%u/metrics ; SIGINT to stop.\n",
			bind, serve_tgt->port,
			opts->interval_ms ? opts->interval_ms : 1000, bind,
			serve_tgt->port);
		rc = intp_core_serve_prometheus_ex(ctx, &so);
		intp_close(ctx);
		if (rc == -ENOSYS) {
			fprintf(stderr,
				"intp: --serve prometheus unavailable: built "
				"with -Dwith_prometheus=false.\n");
			return 4;
		}
		if (rc < 0) {
			fprintf(stderr, "intp: prometheus server failed: %s\n",
				strerror(-rc));
			return 1;
		}
		return 0;
	}

	const struct intp_output_vtable *sink = NULL;
#if INTP_HAVE_OTEL
	if (strcmp(format, "otlp") == 0)
		sink = &intp_output_otlp;
#endif
	if (!sink)
		sink = intp_output_find(format);

	if (!sink) {
		fprintf(stderr,
			"intp: unknown or not-yet-built format '%s'."
			" Available:",
			format);
		const struct intp_output_vtable *const *it = intp_output_list();
		for (; *it; it++)
			fprintf(stderr, " %s", (*it)->format_name);
		fprintf(stderr, "\n");
		intp_close(ctx);
		return 2;
	}

	if (sink->open && sink->open(NULL, STDOUT_FILENO) != 0) {
		fprintf(stderr, "intp: failed to open output sink\n");
		intp_close(ctx);
		return 1;
	}

	struct sigaction sigact = {.sa_handler = on_signal};

	sigemptyset(&sigact.sa_mask);
	sigaction(SIGINT, &sigact, NULL);
	sigaction(SIGTERM, &sigact, NULL);

	struct timespec start;

	clock_gettime(CLOCK_MONOTONIC, &start);

	int exit_code = 0;

	while (g_running) {
		struct intp_sample s;

		rc = intp_sample(ctx, &s);
		if (rc == -EINTR)
			break;
		if (rc) {
			fprintf(stderr, "intp: sampling failed: %s\n",
				intp_strerror(rc));
			exit_code = 1;
			break;
		}
		if (sink->emit)
			sink->emit(NULL, &s);

		if (duration_ms > 0) {
			struct timespec now;

			clock_gettime(CLOCK_MONOTONIC, &now);
			long elapsed_ms =
				(long)(now.tv_sec - start.tv_sec) * 1000L +
				(now.tv_nsec - start.tv_nsec) / 1000000L;
			if (elapsed_ms >= duration_ms)
				break;
		}
	}

	if (sink->close)
		sink->close(NULL);
	intp_close(ctx);
	return exit_code;
}

int main(int argc, char *argv[])
{
	struct intp_options opts = {
		.target = {.kind = INTP_TARGET_SYSTEM},
		.interval_ms = 1000,
	};
	const char *format = "jsonl";
	long duration_ms = 0; /* 0 = run forever */
	bool do_print_schema = false;
	bool do_list_backends = false;
	bool do_serve = false;
	struct intp_serve_target serve_tgt = {.port = 9100};
	const char *disabled_backends[MAX_DISABLED + 1] = {NULL};
	size_t n_disabled = 0;
	const char *disabled_metrics[MAX_DISABLED + 1] = {NULL};
	size_t n_disabled_metrics = 0;
	const char *plugin_dirs[MAX_DISABLED + 1] = {NULL};
	size_t n_plugin_dirs = 0;
	const char *enabled_plugins[MAX_DISABLED + 1] = {NULL};
	size_t n_enabled_plugins = 0;
	const char *disabled_plugins[MAX_DISABLED + 1] = {NULL};
	size_t n_disabled_plugins = 0;
	struct intp_core_force forces[16];
	struct intp_core_extras extras = {0};
	static pid_t pid_list[INTP_CORE_MAX_PIDS];
	const char *cgroup_arg = NULL; /* -c / --target-container */
	bool vm_target = false;	       /* --target-vm given */
	bool want_portable = false;    /* --portable-metrics given */
	int opt;

	while ((opt = getopt_long(argc, argv, "p:c:i:d:f:vVh", long_opts,
				  NULL)) != -1) {
		switch (opt) {
		case 'V':
			print_version();
			return 0;
		case 'h':
			print_help();
			return 0;
		case OPT_PRINT_SCHEMA:
			do_print_schema = true;
			break;
		case OPT_PROBE:
			return run_probe_ebpf();
		case OPT_SERVE:
			if (intp_parse_serve_uri(optarg, &serve_tgt) != 0)
				return 2;
			do_serve = true;
			break;
		case 'p': {
			char *pend = NULL;
			long pid = strtol(optarg, &pend, 10);

			if (pend == optarg || *pend != '\0' || pid <= 0 ||
			    pid > INT_MAX) {
				fprintf(stderr, "intp: bad --pid: %s\n",
					optarg);
				return 2;
			}
			opts.target.kind = INTP_TARGET_PID;
			opts.target.pid = (pid_t)pid;
			break;
		}
		case 'c':
		case OPT_TARGET_CONTAINER:
			opts.target.kind = INTP_TARGET_CGROUP;
			opts.target.cgroup_path = optarg;
			cgroup_arg = optarg;
			break;
		case OPT_PIDS:
			extras.n_pids = intp_parse_pid_list(optarg, pid_list,
							    INTP_CORE_MAX_PIDS);
			if (extras.n_pids <= 0) {
				fprintf(stderr, "intp: bad --pids list: %s\n",
					optarg);
				return 2;
			}
			extras.pids = pid_list;
			break;
		case OPT_COMM:
			extras.n_pids =
				intp_find_pids_by_comm(optarg, pid_list,
						       INTP_CORE_MAX_PIDS);
			if (extras.n_pids == 0)
				fprintf(stderr,
					"intp: warning: no PIDs found for "
					"comm '%s'\n",
					optarg);
			extras.pids = pid_list;
			break;
		case OPT_TARGET_VM:
			/* VM netp comes from the host-side tap iface; the
			 * other six metrics are scoped by a separate
			 * -c <qemu scope> passed alongside (the union then
			 * carries cgroup_path while kind stays VM). */
			opts.target.tap_iface = optarg;
			vm_target = true;
			break;
		case OPT_TAP_IFACE:
			opts.target.tap_iface = optarg;
			break;
		case OPT_EXACT:
			extras.exact = 1;
			break;
		case OPT_EXPERIMENTAL:
			opts.experimental = true;
			break;
		case OPT_PORTABLE:
			want_portable = true;
			break;
		case 'i': {
			long ms = parse_duration_ms(optarg);

			if (ms <= 0) {
				fprintf(stderr, "intp: bad --interval: %s\n",
					optarg);
				return 2;
			}
			opts.interval_ms = (uint32_t)ms;
			break;
		}
		case 'd':
			duration_ms = parse_duration_ms(optarg);
			if (duration_ms <= 0) {
				fprintf(stderr, "intp: bad --duration: %s\n",
					optarg);
				return 2;
			}
			break;
		case 'f':
			format = optarg;
			break;
		case 'v':
			opts.log_level++;
			break;
		case OPT_DISABLE_BACKEND:
			if (n_disabled < MAX_DISABLED)
				disabled_backends[n_disabled++] = optarg;
			break;
		case OPT_LIST_BACKENDS:
			do_list_backends = true;
			break;
		case OPT_FORCE_BACKEND:
			if (extras.n_forces >=
				    sizeof(forces) / sizeof(forces[0]) ||
			    parse_force(optarg, &forces[extras.n_forces]) !=
				    0) {
				fprintf(stderr,
					"intp: bad --force-backend spec: %s\n",
					optarg);
				return 2;
			}
			extras.n_forces++;
			break;
		case OPT_DISABLE_METRIC:
			if (n_disabled_metrics < MAX_DISABLED)
				disabled_metrics[n_disabled_metrics++] = optarg;
			break;
		case OPT_NIC_SPEED:
			extras.nic_speed_bps_override = atol(optarg);
			break;
		case OPT_MEM_BW:
			extras.mem_bw_max_bps_override = atol(optarg);
			break;
		case OPT_LLC_SIZE:
			extras.llc_size_bytes_override = atol(optarg);
			break;
		case OPT_DISK_BW:
			extras.disk_bw_max_bps_override = atol(optarg);
			break;
		case OPT_IFACE:
			extras.iface = optarg;
			break;
		case OPT_DISK:
			extras.disk = optarg;
			break;
		case OPT_PLUGIN_DIR:
			if (n_plugin_dirs < MAX_DISABLED)
				plugin_dirs[n_plugin_dirs++] = optarg;
			break;
		case OPT_ENABLE_PLUGIN:
			n_enabled_plugins =
				add_csv(optarg, enabled_plugins,
					n_enabled_plugins, MAX_DISABLED);
			break;
		case OPT_DISABLE_PLUGIN:
			n_disabled_plugins =
				add_csv(optarg, disabled_plugins,
					n_disabled_plugins, MAX_DISABLED);
			break;
		default:
			fprintf(stderr, "Try 'intp --help' for usage.\n");
			return 2;
		}
	}

	/* --target-vm wins over any -c/-p kind set during parsing: the
	 * union carries the qemu-scope cgroup path (or NULL) while the
	 * kind stays VM (ported v2.1 --target-vm + --cgroup pairing). */
	if (vm_target) {
		opts.target.kind = INTP_TARGET_VM;
		opts.target.cgroup_path = cgroup_arg;
	}

	/* Experimental fences (ADR-0008): refuse BEFORE opening, with
	 * the friendly message; intp_core_open_ex enforces -EPERM too. */
	if (vm_target && !opts.experimental) {
		fprintf(stderr,
			"intp: --target-vm is experimental and requires "
			"--experimental.\n"
			"      VM targeting is unvalidated, pending the "
			"C26/C27 validation campaign\n"
			"      on the RDT testbed. See the \"stability\" "
			"fields in --print-schema for\n"
			"      what is validated today.\n");
		return 2;
	}
	if (want_portable && !opts.experimental) {
		fprintf(stderr,
			"intp: --portable-metrics is experimental and "
			"requires --experimental.\n"
			"      The system.portable.* set is unvalidated, "
			"pending the C26/C27\n"
			"      validation campaign on the RDT testbed. See "
			"the \"stability\" fields\n"
			"      in --print-schema for what is validated "
			"today.\n");
		return 2;
	}
	if (want_portable)
		extras.portable_metrics = 1;

	if (n_disabled > 0)
		opts.disabled_backends = disabled_backends;
	if (n_disabled_metrics > 0)
		extras.disabled_metrics = disabled_metrics;
	if (n_plugin_dirs > 0)
		opts.plugin_dirs = plugin_dirs;
	if (n_disabled_plugins > 0)
		opts.disabled_plugins = disabled_plugins;
	if (n_enabled_plugins > 0)
		extras.enabled_plugins = enabled_plugins;
	extras.forces = forces;

	return run_sampling(&opts, &extras, format, duration_ms,
			    do_print_schema, do_list_backends, do_serve,
			    &serve_tgt);
}
