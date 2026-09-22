# ADR-0010 — Metric semantic models and degradation surfacing

- **Status:** Accepted
- **Date:** 2026-06-09
- **Deciders:** Lead maintainer
- **Supersedes:** —
- **Related:** [ADR-0001](0001-backend-registry.md), [ADR-0002](0002-output-schema-and-versioning.md), [ADR-0003](0003-ebpf-acceleration.md), `across-intp/METRICS-ALIGNMENT.md`

## Context

The seven-metric contract froze the metric *names*, but the comparison
campaign (across-intp, SBAC-PAD 2026) demonstrated
that two backends emitting the same column can measure different
latent quantities. The canonical example is `blk`: the eBPF backend
integrates per-request service time (queue-depth-aware, the
physically meaningful disk-busy fraction), while the pure-C fallback
reads `io_ticks` from /proc/diskstats — a "device had ≥1 outstanding
I/O" fraction that saturates at 100 % on parallel NVMe. These produce
*opposite* interpretations of the same disk activity. A consumer that
cannot tell which model produced a value cannot trust the value.

A second ambiguity: the engines compute percent [0,100], the original
TSV contract renders integer percent, but ADR-0002's JSONL example and
the `intp_<metric>_ratio` Prometheus naming use ratios [0,1].

## Decision

### 1. Every backend declares a named semantic model

The backend descriptor carries a `model` string recorded in
`--print-schema` next to the selected backend. Models in the initial
set:

| Metric | Model | Meaning | Backends |
|---|---|---|---|
| blk | `svctm_busy_fraction` | Σ per-request service time / interval; queue-depth aware, may exceed 1 (canonical) | eBPF `block_rq_issue/complete` |
| blk | `io_ticks_util` | % of time the device had ≥1 outstanding I/O (iostat %util; saturates on parallel NVMe) | diskstats fallback |
| blk | `cgroup_io_throughput` | cgroup io.stat bytes / detected device-bandwidth ceiling | cgroup backend |
| nets | `softirq_time_share` | measured CPU time in NET_TX/NET_RX softirq | eBPF tracepoints |
| nets | `softirq_count_time_estimate` | /proc/softirqs counts scaled by /proc/stat softirq jiffies (approximation) | procfs |
| nets | `softirq_byteshare_proxy` | system-wide softirq time split by the cgroup's byte share (per-cgroup PROXY, divergent by design) | eBPF cgroup |
| netp | `link_share_bytes` | observed bytes/s over link capacity | sysfs/procfs/eBPF |
| mbw | `mbm_bandwidth_share` | resctrl MBM bytes/s over detected max DRAM bandwidth | resctrl |
| mbw | `imc_cas_bandwidth_share` | uncore IMC CAS_COUNT × 64 B over max bandwidth | perf uncore |
| llcmr | `miss_per_reference` | LLC misses / references | perf |
| llcocc | `occupancy_share` | resctrl llc_occupancy over LLC size | resctrl |
| llcocc | `proxy_from_miss_ratio` | directional proxy derived from llcmr (no byte semantics) | fallback |
| cpu | `proc_stat_busy_fraction` / `proc_pid_stat_share` / `cgroup_cpustat_share` | busy share of total CPU time | procfs/cgroup |

Fidelity comparisons (the regression parity gates) compare values
**within a model family**; across families only direction/sanity is
asserted. The per-cgroup `nets` proxy is exempt from cross-variant
equivalence checks entirely (documented divergence, decision C2 in
across-intp).

### 2. Status surfacing

Backends report `ok / degraded / proxy / unavailable` per read
(ported v2.1 status model). Mapping to the output surfaces:

- **unavailable** → the metric key is *absent* from JSONL and rendered
  `--` in tsv-legacy-v1; the reason (failed probes per candidate) is
  in `--print-schema`. Never 0.
- **ok / degraded / proxy** → the value is emitted; the *status* and
  *model* are selection-time properties visible in `--print-schema`
  and as a one-line stderr banner at startup. The JSONL `metrics{}`
  object stays plain numbers (ADR-0002); per-sample status does not
  pollute the stream.

### 3. Units: ratio in the schema-facing surfaces, percent in legacy

- The `system.interference.*` values are **ratios [0,1]** in the C API
  (`intp_metric_value`, unit `ratio`), JSONL, Prometheus
  (`intp_<metric>_ratio`), and OTLP. This matches ADR-0002's example
  and the 0..1 framing of the original paper.
- The engines internally compute percent [0,100] (preserved verbatim
  from the ported variants); the dispatcher divides by 100.
- `tsv-legacy-v1` renders `%.0f` of (ratio × 100) with `--` for
  unavailable — the exact `emit_tsv()`/`format_value()` behavior of
  v2.1 with `--no-header`. The golden fixture generated from the
  v2.1 binary is the arbiter of this format. Values may render as
  `100`; the "0–99" phrasing in older docs described the typical
  clamped range of the stap-era variants, not a format guarantee.
- Rounding note: percent → ratio → percent round-trips through IEEE
  doubles can differ by 1 ulp near `%.0f` rounding boundaries; the
  unit suite pins representative values and live comparisons use
  tolerances, so this is bounded and accepted.

## Consequences

- Consumers can finally distinguish "blk from io_ticks" from "blk
  from service time" mechanically (`--print-schema`), which the
  across-intp campaign showed is the difference between opposite
  readings on NVMe.
- Parity tooling carries the model-family and nets-proxy exemptions
  in code, not in prose.
- The ratio decision is a pre-1.0 semantic commitment; tsv-legacy-v1
  remains bit-compatible for IADA.

## Alternatives considered

- **Percent everywhere.** Rejected: contradicts ADR-0002's committed
  JSONL example and Prometheus `_ratio` naming.
- **Per-sample status in JSONL.** Rejected for the core stream
  (noise, breaks the "plain numbers" consumer contract); diagnostic
  keys may appear under a separate namespace behind `--verbose`.
- **One forced model per metric.** Rejected: would either ban the
  pure-C fallback (breaking the no-eBPF envelope) or ban the
  canonical eBPF model (breaking fidelity). Declared models keep both
  honest.
