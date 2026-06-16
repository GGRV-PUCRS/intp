# ADR-0011 — OTLP metrics output sink

- **Status:** Accepted; implemented (CLI-local, behind `-Dwith_otel`)
- **Date:** 2026-06-16
- **Deciders:** Lead maintainer
- **Supersedes:** —
- **Related:** [ADR-0002](0002-output-schema-and-versioning.md), [ADR-0004](0004-plugin-abi.md), [ADR-0007](0007-extension-domains-gpu-db-app.md), [INTEGRATIONS.md](../INTEGRATIONS.md)

## Context

Operators want intp's metrics in their existing observability backend
without bespoke glue. OpenTelemetry's OTLP is the vendor-neutral way to
ship them. A `with_otel` build option has existed since 0.9.0 but had no
implementation, and its meson recipe wrongly pulled in **protobuf** — the
exact heavy dependency the `src/lib/prometheus.c:16-20` remote_write note
warns must never enter `libintp.so` for every consumer.

Two questions had to be answered: which OTLP transport/encoding, and
where the exporter lives relative to `libintp.so`.

## Decision

**1. Transport/encoding: OTLP/HTTP + JSON over libcurl.** Not gRPC (drags
libgrpc + abseil + protobuf), not OTLP/HTTP+protobuf (still protobuf). The
OTLP-JSON encoding is the protobuf-JSON mapping of the same
`ExportMetricsServiceRequest`, POSTed to `/v1/metrics`; every compliant
collector accepts it. intp emits ~7 core gauges plus a handful of
namespaced metrics per tick, so protobuf's size/CPU edge is irrelevant
while its dependency cost is disqualifying. A serialize-only JSON writer
(~150 lines) plus libcurl is the whole footprint.

**2. Placement: compiled into the `intp` CLI binary, never into
`libintp.so`.** The in-tree sinks compile into `intp_internal`, which
`libintp.so` `link_whole`s — so an in-tree OTLP sink would put libcurl in
`libintp.so`'s `NEEDED` set for *every* consumer, including those that
only call `intp_sample()`. That violates the dependency-containment rule.
Instead the sink (`src/cli/otlp.c` + `src/cli/otlp_json.c`) and `curl_dep`
are added to the **CLI target only**, behind `-Dwith_otel`/`INTP_HAVE_OTEL`;
libcurl is in the `intp` binary's closure, never `libintp.so`'s (verified:
`objdump -p libintp.so` shows no libcurl `NEEDED`). The CLI resolves
`--format otlp` to the sink's `intp_output_vtable` directly, before the
`libintp.so` sink registry. Shipping it instead as an out-of-tree
`OUTPUT_SINK` plugin (ADR-0004) — so a libintp consumer that is not the
CLI can also use it — remains a future packaging option once the loader's
output-sink path lands; the sink already conforms to the plugin vtable, so
that lift is mechanical.

**3. Config follows the OTel env conventions** (so it works with existing
operator setups), overridable by flags the plugin parses itself:
`OTEL_EXPORTER_OTLP_METRICS_ENDPOINT` → `OTEL_EXPORTER_OTLP_ENDPOINT`
(+`/v1/metrics`) → `http://localhost:4318/v1/metrics`;
`OTEL_EXPORTER_OTLP_HEADERS` for auth/tenant routing;
`OTEL_RESOURCE_ATTRIBUTES` merged over the auto-derived resource.

**4. Metric mapping.** Names are emitted **verbatim** (they are already
OTel-style dotted, e.g. `system.interference.cpu` — no `_` mangling like
Prometheus). DOUBLE → Gauge `as_double`; INT64/UINT64 → `as_int`
(saturating UINT64 > INT64_MAX with a one-time diagnostic); `INTP_UNIT_COUNT`
monotonic metrics → `Sum{is_monotonic=true, CUMULATIVE}`; everything else
→ Gauge. `INTP_VALUE_NONE` is omitted, never exported as 0 (ADR-0001).
Units map to UCUM (RATIO→`1`, BYTES→`By`, …). The loop iterates
`sample->values[]`, so portable and `db.<engine>.*` plugin metrics export
for free.

**5. Resource attributes carry target identity for correlation.**
`host.name` ← `sample->host`; `service.name=intp`, `service.version`;
and the target identity (`intp.target.cgroup` / `intp.target.vm` /
`process.pid`) read at `open()` from the active target — the same tuple
JSONL's `target` object and the Prometheus labels carry. A consumer such
as **expdb** supplies `run.id` / `target.id` via
`OTEL_RESOURCE_ATTRIBUTES` to join interference series to a benchmark run.

**6. Batching/failure semantics.** A bounded in-plugin batch flushed by
count or interval; one POST per flush over a reused `CURL*` handle;
drop-oldest on overflow with a `intp.otlp.dropped_points` counter;
capped exponential backoff honoring `Retry-After`; a hard per-flush
deadline. Profiling must never stall the workload it measures, and a dead
collector degrades to dropped exports, never a crash.

## Consequences

- `libintp.so`'s default dependency closure is unchanged — no libcurl, no
  protobuf. A CI guard should assert libcurl is absent from
  `libintp.so`'s `NEEDED` even when the plugin is built.
- `-Dwith_otel=true` builds the exporter into the CLI; `--format otlp`
  streams metrics to the collector, and `--version` reports the sink as
  enabled. The default build is unchanged (`INTP_HAVE_OTEL=0`, no libcurl,
  `--format otlp` → "unknown format").
- The wrong protobuf build dependency is removed; `with_otel` now resolves
  `libcurl` instead.
- A dead/unreachable collector logs one error and drops exports; intp
  exits cleanly (verified) — profiling is never aborted by the sink.
- OTLP is an open standard, not a vendor — so this stays consistent with
  the "standard formats, no per-vendor code in core" integration policy.

## Alternatives considered

- **OTLP in core (in-tree sink).** Rejected: libcurl in `libintp.so` for
  everyone (§2).
- **OTLP/gRPC or OTLP/HTTP+protobuf.** Rejected: heavy closure (§1).
- **No in-process exporter; rely on an external OTel Collector** scraping
  the Prometheus `--serve` endpoint or tailing JSONL. This remains the
  zero-dependency fallback for builds that decline the plugin, and is the
  recommended path today.
