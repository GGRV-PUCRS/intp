# intp

<img src="docs/images/intp.png" alt="intP — Linux interference profiler" width="720">

**Interference profiling. Precise observability. For Linux.**

<p align="center">
  <img alt="status" src="https://img.shields.io/badge/release-0.10.0-brightgreen?style=flat-square">
  <img alt="destination" src="https://img.shields.io/badge/destination-PPA%20%E2%86%92%20official%20package-success?style=flat-square">
  <img alt="kernel" src="https://img.shields.io/badge/Linux-kernel%206.8%2B-informational?style=flat-square">
  <img alt="license" src="https://img.shields.io/badge/license-MIT-blue?style=flat-square">
</p>

---

> **This repository carries the working profiler.** The engine was
> designed, compared, and hardened across measured variants in
> **[across-intp](https://github.com/ggrv-intp/across-intp)**
> (formerly intp-comparison; SBAC-PAD 2026), ported here in June 2026
> and re-synced with across-intp `v0.2.0` for release 0.10.0 — see
> [SYNC.md](SYNC.md) for the provenance map and
> [ADR-0012](docs/design/0012-resync-across-intp.md) for the re-sync
> audit. The pure-C stable-ABI engine (heir of variant v2.1) is the
> baseline; the eBPF/CO-RE engine (heir of v3.3) registers as
> higher-priority backends on capable kernels and falls through cleanly
> elsewhere (ADR-0003). Every release is gated on the RDT testbed
> (`tests/regression/run-remote-gate.sh`).

## The vision in one sentence

To measure, precisely and cheaply, **how one workload interferes with
another** when they share the same Linux hardware — CPU, last-level
cache, memory bandwidth, block I/O, and network. In production. Without
assembling a zoo of tools.

```bash
# today, on Ubuntu 22.04, 24.04 and 26.04
sudo add-apt-repository ppa:norohim/intp
sudo apt install intp
intp -p $(pgrep -f my-application) -i 100ms
```

## Seven metrics. One single utility.

`intp` is the direct heir of the original *interference profiler*
published by Xavier et al. (SBAC-PAD 2022, PUCRS), rewritten to
survive modern kernels and contemporary clouds. In short windows it
collects seven signals that answer the question *"who is stealing what
from whom?"*:

| Metric    | What it measures                                   | Source |
|-----------|----------------------------------------------------|--------|
| **netp**  | NIC physical utilization (TX + RX in bps)          | driver counters |
| **nets**  | Kernel network stack service time                  | softirq / NAPI |
| **blk**   | Block device busy percentage                       | block layer |
| **mbw**   | Memory bandwidth (LLC ↔ DRAM)                      | uncore IMC PMU |
| **llcmr** | LLC miss ratio                                     | perf_event |
| **llcocc**| LLC occupancy in bytes                             | Intel RDT / resctrl |
| **cpu**   | CPU utilization (user + sys)                       | scheduler / cgroup |

Each metric is a window; the set is the *fingerprint* of interference.
Where a host cannot provide a metric (no RDT, a KVM guest's vPMU), intp
reports it as unavailable, degraded or proxy instead of emitting a zero,
and `--print-schema` names the backend and semantic model behind every
reading ([METRICS.md](docs/METRICS.md)).

**Seven canonical metrics, plus an experimental portable set.** Behind
`--experimental --portable-metrics`, intp also publishes eight
`system.portable.*` signals — six that stay observable inside a KVM
guest and two scheduling-regime rates — pending final validation
verdicts ([PLUGINS.md](docs/PLUGINS.md)).
The technical deep-dive lives in
[METRICS-DEEP-DIVE](https://github.com/ggrv-intp/across-intp/blob/main/docs/METRICS-DEEP-DIVE.md).

## Why this exists

Every cloud — public or private — sells the illusion that vCPUs are
independent. They are not. Co-located workloads fight over cache, over
memory bandwidth, over NIC queues, over I/O queues. The symptom is
latency that appears with no obvious cause; the cause is the *noisy
neighbour* nobody can see.

Traditional tools (`top`, `sar`, `iostat`, `perf`) show *what each
thing consumes*. `intp` shows **how that consumption translates into
observable interference** — the signal that interference-aware
schedulers (such as
[IADA](https://www.sciencedirect.com/science/article/abs/pii/S0164121222001670))
need in order to decide.

`intp` is not a replacement for `htop`. It is the missing sensor
between *"this metric is high"* and *"this is hurting the neighbour"*.

## Relationship with `across-intp`

The sibling repository
[`across-intp`](https://github.com/ggrv-intp/across-intp) is the
test bench. Eleven implementations coexist there, each a different
combination of instrumentation paradigm, cost, portability, and
fidelity:

| Variant | Paradigm | Role in the study |
|---------|----------|-------------------|
| V0 | Classic SystemTap (original paper; needs `intel_cqm`, removed in 4.14) | Portability cliff; reference only |
| V0.1 | SystemTap, LLC disabled | Portability cliff (6/7 metrics) |
| V0.2 | SystemTap + userspace helper, kernel 5.15 | **Measured** legacy-faithful baseline |
| V1 | SystemTap native probes, 6.8+ | Reliability cliff |
| V1.1 | SystemTap + userspace helper, 6.8+ | Architectural proof |
| V2 | Pure C: procfs / perf_event / resctrl | **Measured** |
| **V2.1** | **Pure C, per-cgroup (cgroup v2 + perf cgroup mode)** | **Heir in `intp`: the pure-C engine** |
| V3 | eBPF/CO-RE, ring-buffer streaming | Predecessor (context-switch amplification) |
| V3.1 | bpftrace + Python orchestrator | Corroboration |
| V3.2 | eBPF/CO-RE, in-kernel aggregation | **Measured** |
| **V3.3** | **eBPF/CO-RE, per-cgroup, in-kernel aggregation** | **Heir in `intp`: the eBPF backends** |

Only kernel-module-free variants survive the upstream packaging bar —
official distributions do not accept out-of-tree modules without a
long-term maintenance path. The SBAC-PAD 2026 comparison selected the
C and eBPF/CO-RE lines, and their per-cgroup versions, v2.1 and v3.3,
became `intp`.

> The research behind that decision is the Master's dissertation of
> **André Sacilotto Santos**, advised by **Prof. Cesar De Rose**
> (PPGCC/PUCRS), with defense expected in **March 2027**.

## Roadmap to the official package

The journey from "GitHub placeholder" to `sudo apt install intp` has
well-defined stops. The full breakdown lives in
[docs/ROADMAP.md](docs/ROADMAP.md); at a glance:

1. **Public name reservation** — done.
2. **Variant selection** — done: the SBAC-PAD 2026 comparison selected
   the v2.1 stable-ABI + v3.3 eBPF pairing now living in this tree.
3. **Debian packaging** — done: lintian-clean packages, manpage,
   install/remove-tested. `v0.9.0` passed the RDT-testbed validation
   gate but was never published.
4. **Personal PPA on Launchpad** — *you are here*: `ppa:norohim/intp`
   carried a placeholder (0.2.0~placeholder, no metrics); 0.10.0 is
   the first release with the profiler, for 22.04, 24.04 and 26.04.
5. **Debian sponsorship** — *Intent To Package* (ITP),
   `mentors.debian.net` review, sponsored upload by a Debian Developer.
6. **Ubuntu universe** — automatic sync from Debian.
7. **Adjacent distributions** — Fedora (COPR → official), Arch (AUR →
   official), Alpine, Nix.

Every step has its own technical gate (quality, tests, distro policy)
and its own bureaucratic gate. None of them is trivial. That is why
the *"if all goes well"* is not false modesty — it is honesty about a
process that involves more than code.

## Installing

```bash
sudo add-apt-repository ppa:norohim/intp
sudo apt install intp
```

- Use only `ppa:norohim/intp`. `ppa:norohim/intp-staging` holds
  release candidates and untested builds.
- **Ubuntu 22.04:** the eBPF backends are not built (the archive's
  libbpf is too old); the pure-C engine serves all seven metrics with
  the same output schema.
- **arm64:** builds, but is not validated — the release gate runs on
  x86_64 only. Metrics a host cannot provide read unavailable.
- Upgrading from the 0.2.0 placeholder replaces its `libintp1` package
  with `libintp0` automatically.

Building from source: [docs/BUILDING.md](docs/BUILDING.md).

## How to follow along

The research record, benchmark campaigns, and historical variants live in
[`across-intp`](https://github.com/ggrv-intp/across-intp) —
home of the variants, the benchmarks, and the technical documents
([METRICS-DEEP-DIVE](https://github.com/ggrv-intp/across-intp/blob/main/docs/METRICS-DEEP-DIVE.md),
[KERNEL-6.8-CHANGES](https://github.com/ggrv-intp/across-intp/blob/main/docs/KERNEL-6.8-CHANGES.md),
[VARIANT-COMPARISON](https://github.com/ggrv-intp/across-intp/blob/main/docs/VARIANT-COMPARISON.md)).

Local documentation:

- [docs/VISION.md](docs/VISION.md) — *Why `intp`, and why now.*
- [docs/ROADMAP.md](docs/ROADMAP.md) — *The steps to the official package.*

## Citation

Please cite the SBAC-PAD 2026 paper; [CITATION.cff](CITATION.cff) has
the metadata, and the DOI will be added upon publication:

> A. Sacilotto, M. G. Xavier, S. Becker, C. A. F. De Rose and O. Kao.
> *Cross-Application Interference Profiling in Consolidated Cloud
> Servers: Improving Portability, Overhead and Fidelity in IntP*.
> SBAC-PAD 2026, Madrid, Spain. IEEE. To appear.

The research artifacts are the releases of
[across-intp](https://github.com/ggrv-intp/across-intp): `v0.1.0` for
the SBAC-PAD 2026 paper, `v0.2.0` for the container and VM deployment
study. The dissertation reference will be added after the defense,
expected in **March 2027**.

Original work:

> M. G. Xavier, C. H. C. Cano, V. Meyer and C. A. F. De Rose (2022).
> *IntP: Quantifying Cross-Application Interference via System-Level Instrumentation*.
> SBAC-PAD 2022, Bordeaux, France, pp. 231–240. IEEE.
> doi:[10.1109/SBAC-PAD55451.2022.00034](https://doi.org/10.1109/SBAC-PAD55451.2022.00034) ·
> [PDF](https://repositorio.pucrs.br/dspace/bitstream/10923/24018/2/IntP_Quantifying_crossapplication_interference_via_systemlevel_instrumentation.pdf)

## License

[MIT](LICENSE).

---

<p align="center">
  <em>The code is here. The package is on its way.</em>
</p>
