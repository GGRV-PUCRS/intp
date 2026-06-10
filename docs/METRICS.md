# Metrics

`intp` collects **seven core metrics** that, taken together, describe
how a workload competes for shared hardware on a Linux host. The set
is the one settled on by Xavier & De Rose in the original IntP paper
(SBAC-PAD 2022) after a longer empirical exploration, lightly updated
to track kernel evolution and to harmonise across variants.

This document is the canonical, slim reference. The deep, line-by-line
walkthrough — kernel probes, normalisation formulas, hardware
constants, kernel-version specifics — lives in
[`intp-comparison/docs/METRICS-DEEP-DIVE.md`](https://github.com/ggrv-intp/intp-comparison/blob/main/docs/METRICS-DEEP-DIVE.md)
and [`intp-comparison/METRICS-ALIGNMENT.md`](https://github.com/ggrv-intp/intp-comparison/blob/main/METRICS-ALIGNMENT.md).
Treat those as the source of truth; this document is the index.

---

## The seven metrics, at a glance

| Metric     | Unit | What it measures                                                       | Stable-ABI backend                 | eBPF-accelerated? |
| ---------- | ---- | ---------------------------------------------------------------------- | ---------------------------------- | :---------------: |
| **netp**   | %    | NIC physical utilization (TX + RX bps / link bps).                     | `/sys/class/net/` byte counters    | yes               |
| **nets**   | %    | Kernel network-stack service time (softirq NET_RX/NET_TX).             | `/proc/softirqs` + `/proc/stat`    | yes               |
| **blk**    | %    | Block I/O utilization (see semantic models below).                     | `/proc/diskstats` `io_ticks`       | yes               |
| **mbw**    | %    | Memory bandwidth utilisation (LLC ↔ DRAM, summed across channels).     | uncore IMC / DF / CMN → resctrl    | no                |
| **llcmr**  | %    | LLC miss ratio (misses / references).                                  | `perf_event_open` HW_CACHE / raw   | yes               |
| **llcocc** | %    | LLC occupancy / total LLC, via Intel RDT or vendor analogue.           | `/sys/fs/resctrl/mon_groups/…`     | no                |
| **cpu**    | %    | CPU utilisation (busy share, system / per-PID / per-cgroup).           | `/proc/stat` family or `cpu.stat`  | yes               |

Each metric is rendered as integer percent for the legacy TSV output
and carried as a fractional double in `[0.0, 1.0]` in the C API,
JSON-Lines, and Prometheus surfaces (ADR-0010). Plugins are free to
publish in their natural unit; the schema descriptor records it.

---

## Why these seven (and no others, in core)

Each one closes a specific blind spot that `top`, `sar`, `iostat`,
`perf stat`, or cgroup accounting alone cannot fill:

- **`cpu`** is the baseline against which the other six are read. A
  workload at `cpu=80` and `mbw=10` is CPU-bound; same workload at
  `cpu=80` and `mbw=85` is bandwidth-bound; same workload at `cpu=20`
  and `llcocc=95` is the noisy neighbour you were looking for.
- **`netp` vs `nets`** separates wire-level saturation from kernel-
  side packet handling. A NIC running at 95% with low `nets` means
  the network is the bottleneck *for the application*; high `nets`
  with moderate `netp` means the kernel is paying for traffic the
  application never explicitly asked about (incoming SYN floods,
  noisy neighbours).
- **`blk`** is the disk-side counterpart of `nets`: how much of the
  block layer's queue capacity is in use.
- **`mbw` and `llcmr` and `llcocc`** are the three faces of the
  memory subsystem: bandwidth saturation (LLC↔DRAM), how often a
  request paid the full DRAM round-trip, and *who is sitting in the
  cache right now*. They are independent: a streaming workload has
  high `mbw` and high `llcmr` but low `llcocc`; an in-memory key-
  value store has high `llcocc` but low `mbw`. Together they
  diagnose memory-subsystem contention.

Anything beyond these seven — GPU SM occupancy, database lock-wait,
vector-index recall under load, application request-queue depth — is
a *plugin-defined metric* (see [PLUGINS.md](PLUGINS.md)), not a core
metric. Adding a new core metric needs an ADR and very strong
justification.

---

## Backend selection per metric

`intp --print-schema` shows which backend was selected for each
metric on the running host. The selection logic is described in
[ARCHITECTURE.md](ARCHITECTURE.md#backend-registry--runtime-selection);
the per-metric defaults below assume eBPF is available.

The shipped chains (backend ids as reported by `--list-backends`):

| Metric | Backend candidates (in priority order)                                  |
| ------ | ----------------------------------------------------------------------- |
| netp   | `ebpf_bytes` → `tap` (VM) → `cgroup` (netns) → `sysfs` → `procfs`       |
| nets   | `ebpf_softirq` → `procfs_softirq` → `procfs_throughput`                 |
| blk    | `ebpf_svctm` → `cgroup` (io.stat) → `diskstats` (io_ticks) → `sysfs`    |
| mbw    | `perf_uncore_imc` → `perf_amd_df` → `perf_arm_cmn` → `resctrl_mbm`      |
| llcmr  | `ebpf_perf` → `cgroup` (perf cgroup mode) → `perf_hwcache` → `perf_raw` |
| llcocc | `resctrl` → `proxy_from_miss_ratio`                                     |
| cpu    | `ebpf_sched` → `cgroup` (cpu.stat) → `procfs_pid` → `procfs_system`     |

When an eBPF backend wins, fidelity matches the measured v3.3 endpoint
in `intp-comparison`. When it loses (eBPF disabled at build time, BTF
absent, kernel < 5.8, missing capability), the stable-ABI chain takes
over and matches v2.1.

## Semantic models (ADR-0010)

Two backends of the same metric can measure different latent
quantities; the selected backend's **model** is declared in
`--print-schema` and fidelity comparisons are valid only within a
model family:

- **blk** has three declared models: `svctm_busy_fraction` (canonical
  — Σ per-request service time / interval, queue-depth aware),
  `io_ticks_util` (the pure-C fallback — "device had ≥ 1 outstanding
  I/O", saturates at 100 % on parallel NVMe and can read the same disk
  activity *opposite* to svctm), and `cgroup_io_throughput` (io.stat
  bytes over a detected device ceiling). The campaign behind this
  split is in across-intp `METRICS-ALIGNMENT.md`.
- **nets** per-cgroup exists only as the eBPF byte-share proxy
  (`softirq_byteshare_proxy`, status PROXY) — divergent **by design**
  from the system-wide softirq figure; parity tooling exempts it.
- **llcocc**'s `proxy_from_miss_ratio` fallback is directional-only
  (status PROXY), never comparable to occupancy bytes.

Units: the C API/JSONL/Prometheus carry ratios [0,1]; `tsv-legacy-v1`
renders integer percent (the frozen IADA contract). Canonical formula
derivations live in across-intp `docs/METRICS-DEEP-DIVE.md` at the
freeze commit (see [SYNC.md](../SYNC.md)).

---

## Cross-vendor portability

| Vendor           | `mbw`                                                          | `llcmr`                      | `llcocc`                                 |
| ---------------- | -------------------------------------------------------------- | ---------------------------- | ---------------------------------------- |
| Intel (Skylake+) | uncore IMC `cas_count_read/write`; resctrl MBM where available | `MEM_LOAD_RETIRED.L3_*`      | resctrl `llc_occupancy` (CMT)            |
| AMD (Rome+)      | uncore `umc_cas_*` events                                      | `MISS_*` family              | resctrl `llc_occupancy` (AMD L3 monitor) |
| AMD (Zen 3+)     | as above; per-CCX bandwidth aggregated                         | as above                     | as above                                 |
| ARM Neoverse     | PMU `LLC_*` events; vendor uncore varies                       | `LL_CACHE_MISS_RD` / similar | MPAM (where supported)                   |
| Other / older    | metric reported as unavailable; not silently zeroed            | reported as unavailable      | reported as unavailable                  |

`tools/intp-detect.sh` (ported from `intp-comparison/shared/intp-detect.sh`)
is the single source of truth for hardware capability probing. It
emits a shell-sourceable file that the build and runtime read.

The full hardware compatibility matrix is in
[`intp-comparison/docs/HARDWARE-COMPATIBILITY.md`](https://github.com/ggrv-intp/intp-comparison/blob/main/docs/HARDWARE-COMPATIBILITY.md).

---

## Sampling cadence and PID attribution

- Default sampling interval: **1 s** (matches the original IntP paper).
  Configurable via `--interval` down to 100 ms.
- **System-wide** mode: `intp` (no target). Aggregates across all
  PIDs; uses `/proc/stat`, system-wide `perf_event_open`, root
  resctrl monitor.
- **Per-PID** mode: `intp -p <pid>` or `intp -p $(pgrep -f my-app)`.
  Uses `/proc/<pid>/stat`, PID-attached `perf_event_open`, and a
  per-PID resctrl `mon_group`.
- **Per-cgroup** mode (post-1.0): `intp -c /sys/fs/cgroup/<path>`.
  Uses cgroup-aware `perf_event_open` and the cgroup's resctrl group.

The per-PID resctrl plumbing is the trickiest part because resctrl
groups are mutually exclusive — a PID can be in exactly one
`mon_group` at a time. `intp` either creates and tears down its own
`mon_group` for the target (default) or attaches to an existing one
(`--resctrl-group=<name>`).

---

## Cross-variant equivalence (heritage from `intp-comparison`)

The metric definitions above are the *aligned* ones documented in
[`intp-comparison/METRICS-ALIGNMENT.md`](https://github.com/ggrv-intp/intp-comparison/blob/main/METRICS-ALIGNMENT.md).
That file documents the small patches applied across variants to
harmonise post-kernel-6.8 behaviour:

- All variants standardised on softirq tracepoints (vec=2, vec=3) for
  `nets`, replacing the older `__napi_schedule_irqoff` probe that
  broke on kernel 6.8 / veth.
- v0/v0.1/v1 retain the original ~100× amplification on `blk` for
  bit-for-bit fidelity with the 2022 paper; v1.1/v3/v3.1 use the
  modernised `svctm_sum / interval`. v2 uses `io_ticks` (no
  queue-depth signal). `intp`'s eBPF backend reproduces the
  v3-style queue-depth integration.
- Hardware constants (NIC speed, DRAM bandwidth, LLC size) are
  **autodetected** in `intp` via `tools/intp-detect.sh`; v0's
  hardcoded constants are documented but not used.

`tests/regression/` ports the cross-variant equivalence harness from
`intp-comparison/shared/validate-cross-variant.sh` so we can keep the
alignment honest as the codebase evolves.

---

## What about plugin-defined metrics?

Plugins publish their metrics under their own schema namespace
(`gpu.nvidia.*`, `db.postgres.*`, `app.<service>.*`) and are
documented in their own `README.md`. The plugin loader merges them
into the same `intp_sample` stream as the seven core metrics, so
downstream consumers see them through the same interface.

See [PLUGINS.md](PLUGINS.md) for the metric-provider plugin contract.
