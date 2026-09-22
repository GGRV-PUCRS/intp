# Changelog

All notable changes to `intp` are documented here.

The format follows [Keep a Changelog 1.1.0](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning 2.0.0](https://semver.org/spec/v2.0.0.html).

Pre-1.0 releases (`0.x`) have no API or ABI stability guarantees. The
plugin ABI freezes at `1.0.0`; before that, breaking changes can land
in any minor release and will be flagged in the relevant section below.

## [Unreleased]

## [0.10.0] — 2026-09-22

Re-synced with across-intp `v0.2.0` (commit `ce6600259a41`; ADR-0012)
and the first release published to `ppa:norohim/intp`, for Ubuntu
22.04, 24.04 and 26.04 on amd64 and arm64. The research repository is
now named across-intp throughout (formerly intp-comparison).

### Fixed

- **Container targets with nested cgroups.** Cgroup and VM targets are
  resolved over the whole cgroup subtree. A container whose top cgroup
  holds no processes (Incus `.lxc`, systemd `init.scope`) left the PID
  set empty: `llcmr` counted system-wide, `cpu` read unavailable, and
  resctrl enrolled nothing.

### Added

- `--print-schema` reports the `llcmr` `event_family` (`ll_read`,
  `vendor_raw`, `arch_cache_refs`) and the `mbw` normalization
  ceiling with its source (`ceiling_bps`, `ceiling_source`,
  `ceiling_fallback`); the capability report shows the same source.
- `packaging/make-source-package.sh --series`: one source package per
  Ubuntu series from a single orig tarball, versioned
  `<v>~ppaN~ubuntuYY.MM.1`, with a jammy variant that builds without
  eBPF.
- ADR-0012 (pinned re-sync), portable-metrics caveats in PLUGINS.md,
  claim classes and a KVM-guest availability table in METRICS.md.

- **Plugin loader** (ADR-0004): `--plugin-dir` / `--enable-plugin` /
  `--disable-plugin` load metric-provider plugins via dlopen, with a
  directory trust model (root- or owner-owned, never world-writable),
  ABI/kind/requirements validation before any plugin code runs, an optional
  self-test gate, and a guard refusing any plugin that publishes a frozen
  `system.interference.*` metric. Real `intp_plugin_ctx` private storage.
- **Per-target output identity**: the JSONL sink emits the documented
  `target` object (`kind`, `pid`/`cgroup`), and both Prometheus renderers
  (`--serve` and `--format prometheus`) attach `{instance,target_kind,
  cgroup,pid}` labels, so a multi-tenant scraper can attribute each series.
- **OTLP/HTTP+JSON metrics exporter** (`--format otlp`, ADR-0011): built
  with `-Dwith_otel=true`, streams to an OpenTelemetry collector over
  libcurl, with resource attributes (`host.name`, `intp.target.*`, plus
  `OTEL_RESOURCE_ATTRIBUTES`). libcurl is contained to the CLI binary,
  never `libintp.so`.
- Hardened example systemd unit + `sysusers.d` service account.
- Tests: JSONL and Prometheus golden tests, the serve-URI parser, the
  OTLP-JSON serializer, and a plugin-load integration test.

### Changed

- In a KVM guest, `llcmr` on the architectural cache-references/misses
  fallback reports status `degraded` with the note `arch_cache_refs`
  instead of `ok`: it is not comparable with a host LL-read value.
- `mbw` readings normalized against a guessed ceiling (no uncore_imc
  PMU, or no dmidecode speed) carry the note `ceiling_fallback`, and
  dmidecode's older `Configured Clock Speed` label is accepted. Where
  dmidecode was unavailable, percentages can differ from 0.9.0's.
- `libintp0` Breaks/Replaces `libintp1 (<< 0.10.0~)`, the binary
  package the PPA placeholder shipped with the same library paths.
- `--serve` binds **127.0.0.1 by default** (was 0.0.0.0); pass an explicit
  host in the URI (e.g. `prometheus://0.0.0.0:9100`) to expose it. The
  Prometheus render buffer grows on demand instead of failing a scrape,
  INT64/UINT64 metrics are exported (no longer dropped), and every metric
  carries `# HELP`/`# TYPE` with complete unit suffixes.
- `-v/--verbose` is wired (libbpf print level + perf-path warnings);
  `--pid` validates its argument; the CLI uses `sigaction`.
- Packaging: DEP-5 `copyright` declares the GPL-2.0 vmlinux BTF dump and the
  dual-licensed BPF objects; `debian/watch` matches pre-release tags;
  `debian/control` deduped, `Standards-Version 4.7.0`, autopkgtest smoke.
- `with_otel` resolves libcurl (was: protobuf).

---

## [0.9.0] — 2026-06-12 (gated, not published)

Tagged upstream only; never uploaded to the PPA. Stamped after the release-blocking RDT-testbed gate
(steps 1–6 of `tests/regression/run-remote-gate.sh`) passed in full on
the Sapphire Rapids validation host (Xeon Gold 5412U, kernel 6.17,
full CMT/MBM): build + 16/16 default suites, live suite incl. the
ctxsw-amplification gate at **1.02** (limit 1.10), 7/7-metric smoke +
RMID crash-reap, cross-variant parity vs the frozen v2.1/v3.3 research
binaries (3/3 pairs within 15 pp, cgroup-targeted sequential
methodology), and per-cgroup attribution (15 PASS / 0 FAIL:
track + isolation + conservation for cpu, blk, llcmr, mbw, llcocc).

### Fixed (since 0.9.0-rc1)

- **Per-cgroup `mbw` now isolates.** Two defects found by the gate
  against kernel ground truth: (1) the uncore memory-controller PMUs
  (Intel IMC / AMD DF / ARM CMN) count total socket DRAM traffic and
  were selected even for cgroup/PID targets, silently reporting the
  system-wide figure as the target's mbw; they now probe-reject
  non-system targets so resctrl mbm (per-RMID) wins. (2) mbw and
  llcocc each created their own resctrl mon_group and contended for
  the target's single RMID (the loser read 0); they now share one
  refcounted per-target group. On a non-RDT host, per-cgroup mbw is
  honestly unavailable rather than silently system-wide.
- `--serve` exposes live values (sampling thread + honest omission +
  `intp_scrape_staleness_seconds`) and honors the full targeting
  surface; libmicrohttpd is in the default build.
- eBPF llcmr perf attr drops `wakeup_events=1` (documented deviation
  from the frozen origin): nothing consumes the perf fd, and the
  per-overflow wakeups added ~6.7k ctxsw/s on hybrid client CPUs
  (amplification 6.8× → 1.06× on a Core 7 240H; upstream D11).
- Test/gate robustness: unit selection tests are deterministic under
  root+eBPF; the eBPF leak check counts intp's own programs and waits
  out asynchronous BPF teardown; cross-variant parity runs
  cgroup-targeted and sequential (per-cgroup resctrl monitors cannot
  run concurrently — RMID contention); attribution compares mbw on a
  single backend scale; the gate preserves its exit code through the
  cleanup trap; the checked-in `vmlinux.h` BTF fallback is actually
  tracked (a bare `.gitignore` rule had swallowed it — Launchpad
  builds would have FTBFS'd).

---

## [0.9.0-rc1] — 2026-06-10 (not published)

Stage 2 of the roadmap: the working profiler. Engines ported-and-frozen
from intp-comparison @ `2e87b30fc762` (provenance: [SYNC.md](SYNC.md)).

### Added

- **Backend registry & dispatcher** (ADR-0001): linker-section
  registration, ordered per-metric fallback chains with deterministic
  tie-breaks, probe-driven selection with recorded rejection reasons,
  absolute-deadline sampling. A metric with no usable backend is
  reported unavailable — never silently zeroed.
- **Pure-C backend set** (heir of v2.1): netp/nets/blk/mbw/llcmr/
  llcocc/cpu over procfs, sysfs, perf_event_open, and resctrl, with
  capability detection and hardware-normalizer autodetection
  (overridable via CLI flags).
- **eBPF/CO-RE backends** (heir of v3.3, ADR-0003): in-kernel counter
  aggregation (no ring buffer), per-cgroup attribution incl. the
  documented per-cgroup `nets` byte-share proxy (ADR-0006); registered
  above the pure-C chains, with clean per-program degradation and full
  fall-through on verifier rejection. BPF object byte-frozen from the
  measured v3.3 variant.
- **Output sinks** (ADR-0002): `jsonl` (primary), `tsv-legacy-v1`
  (bit-compatible with IADA / intp-comparison consumers; golden-file
  tested), `prometheus` text format. `--print-schema` now reflects the
  live selection: backend, semantic model (ADR-0010), scope, stability,
  rejection reasons.
- **CLI**: full targeting/backend-control/normalizer surface plus
  `--list-backends`; `intp(1)` manpage.
- **Experimental fences** (ADR-0008): `--target-vm` and
  `--portable-metrics` (`system.portable.*`, ADR-0009) require
  `--experimental` until their validation campaign passes.
- **resctrl RMID hygiene**: stale mon_group reaping, 75 % RMID budget
  preflight with fall-through.
- **Regression tooling**: cross-backend/cross-variant parity
  (`tests/regression/xval.sh`), host preflight, per-cgroup attribution
  validation, fidelity/overhead/correlation/sample-loss gates under
  `bench/gates/`, and the operator-run RDT testbed gate
  (`tests/regression/run-remote-gate.sh`).
- ADRs 0005–0010.

### Changed

- `intp_open`/`intp_sample`/`intp_subscribe`/`intp_print_schema` and
  the schema introspection now have real implementations (previously
  `-ENOSYS` stubs).
- Public ABI additions (pre-1.0): `INTP_TARGET_VM`, `intp_target.tap_iface`,
  `intp_options.experimental`. `INTP_TARGET_CGROUP` is no longer
  marked post-1.0.
- The seven core metrics are carried as **ratios [0,1]**
  (`INTP_UNIT_RATIO`) in the C API/JSONL/Prometheus; `tsv-legacy-v1`
  renders integer percent (ADR-0010).
- `--serve` now exposes live values: a background sampling thread
  feeds each scrape, unavailable metrics are omitted (never 0),
  `intp_metrics_implemented` counts the real core metrics, and
  `intp_scrape_staleness_seconds` ages the sample for alerting. The
  CLI defers `--serve` until after option parsing, so it honors the
  full targeting/backend-control surface.

### Deferred

- OTLP sink (post-0.9; Prometheus covers the pull path).
- dlopen plugin loader (`--plugin-dir`/`--enable-plugin` exit with a
  clear message; the portable-metrics provider is statically linked).
- Streaming transports (Unix socket / gRPC) — ADR-0005, post-1.0.

### Release gate

`v0.9.0` final is stamped only after the RDT-testbed validation
(`run-remote-gate.sh` steps 1–6: preflight, build, live suite, 7/7
smoke + RMID reaping, cross-variant parity, attribution) passes on the
reference hardware.

---

## [0.2.0-placeholder] — 2026-05-09

Published to `ppa:norohim/intp` (noble; binaries `intp`, `libintp1`,
`libintp-dev`, `intp-doc`, `intp-plugin-bpftrace`). No metrics.

### Added

- Project scaffolding under [docs/ROADMAP.md](docs/ROADMAP.md) Stage 1.5:
  governance docs (`CONTRIBUTING.md`, `GOVERNANCE.md`, `MAINTAINERS`,
  `SECURITY.md`, `code_of_conduct.txt`), local `CITATION.cff`, and this
  changelog.
- `intp_probe_ebpf()` / `intp --probe` and `intp_serve_prometheus()` /
  `intp --serve` public API, serving placeholder values.

---

## [0.1.0-placeholder2] — 2026-05-09

Published to `ppa:norohim/intp` (noble), now superseded: a re-roll of
the first placeholder with a correct orig tarball. `0.1.0~placeholder-1`
failed to build on Launchpad and its version is burned.

---

## Release history

`0.1.0-placeholder2` and `0.2.0-placeholder` were PPA placeholders
without metrics. `0.9.0-rc1` and `0.9.0` passed the RDT-testbed gate
but were never published. `0.10.0` is the first release on the PPA.
`1.0.0` is reserved for the plugin ABI freeze, per
[docs/ROADMAP.md](docs/ROADMAP.md).
