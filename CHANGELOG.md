# Changelog

All notable changes to `intp` are documented here.

The format follows [Keep a Changelog 1.1.0](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning 2.0.0](https://semver.org/spec/v2.0.0.html).

Pre-1.0 releases (`0.x`) have no API or ABI stability guarantees. The
plugin ABI freezes at `1.0.0`; before that, breaking changes can land
in any minor release and will be flagged in the relevant section below.

## [Unreleased]

### Added

- _(nothing yet)_

---

## [0.9.0-rc1] — 2026-06-10

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
- `--serve` keeps its placeholder values until a libmicrohttpd build
  environment is available; the one-shot `--format prometheus` sink
  carries live values.

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

## [0.2.0-scaffold] — 2026-05-09

### Added

- Project scaffolding under [docs/ROADMAP.md](docs/ROADMAP.md) Stage 1.5:
  governance docs (`CONTRIBUTING.md`, `GOVERNANCE.md`, `MAINTAINERS`,
  `SECURITY.md`, `code_of_conduct.txt`), local `CITATION.cff`, and this
  changelog.

---

## Release history

No releases yet. The first tagged release will be `0.1.0` once the
build skeleton, plugin ABI, and CI bootstrap (Steps 4–6 of the plan)
land. `1.0.0` is reserved for the first stable, distro-packageable
release per [docs/ROADMAP.md](docs/ROADMAP.md) Stage 2.
