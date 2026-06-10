# Regression & validation tooling — execution scoping

Ported from the intp-comparison harness (`shared/`, `bench/`) at the
freeze commit `2e87b30fc762` (see [SYNC.md](../../SYNC.md)). Three
execution tiers; nothing here runs in CI.

## What runs where

### CI (GitHub Actions)

**None of this directory.** CI runs the meson default suites only
(unit / plugin-abi / integration / golden-format), the build matrix,
and the lint gates. The scripts here need root, real hardware, or a
research-repo checkout.

### Local, root (developer laptop, no RDT required)

| What | How | Needs |
|------|-----|-------|
| eBPF live suite (load-attach + ctxsw amplification gate ≤ 1.10) | `meson test -C build --setup= --suite live` | root, BTF, stress-ng; `INTP_AMPL_SECONDS` shortens the 90 s window |
| Self-parity: eBPF vs pure-C backends of the same binary | `tests/regression/xval.sh --self ./build/src/cli/intp` | stress-ng; `INTP_XVAL_SECONDS`, `INTP_XVAL_TOL` (15 pp) |
| Per-cgroup attribution validation | `sudo tests/regression/validate-attribution.sh ./build/src/cli/intp` | root + cgroup v2 (SKIPs 77 otherwise) |
| Host capability matrix | `tests/regression/preflight.sh` | nothing (exit 0 all-present / 2 missing) |
| Release gates, degraded | `bench/gates/run-gates.sh ./build/src/cli/intp` | stress-ng; without resctrl the RDT-backed cells SKIP |

### Remote only (the RDT testbed; release-blocking)

Run through `tests/regression/run-remote-gate.sh <user@host>` — and
only when the testbed is **not** running a research campaign:

1. preflight (full RDT matrix expected)
2. build + default suites
3. live suite on the reference hardware
4. 7/7-metric smoke + multi-domain resctrl sanity + RMID crash-reap
5. **cross-variant parity** vs the in-repo v2.1/v3.3 binaries —
   `xval.sh --cross` with `INTP_XVAL_ACROSS_DIR` (release-blocking)
6. full attribution validation
7. (optional, human-coordinated reboot) ub22 / kernel-5.15 leg
8. portable/VM experimental validation (non-blocking; promotion
   evidence for ADR-0008)

Steps 1–6 green ⇒ stamp `v0.9.0`.

## Encoded comparison rules (not just documentation)

- `nets` under a cgroup scope is **exempt** from parity checks — the
  eBPF byte-share proxy is divergent by design (upstream decision C2,
  ADR-0006). The exemption lives in `xval.sh`.
- `blk` magnitudes are compared only **within a semantic model family**
  (`svctm_busy_fraction` vs `io_ticks_util` vs `cgroup_io_throughput`,
  ADR-0010); across families only direction/sanity is reported.
- Context-switch ground truth comes from `vmstat` / `/proc/stat ctxt`,
  never `perf stat` (BPF co-attach undercount — the SBAC-PAD 2026
  methodological caveat).

## Gate thresholds (frozen; single source: `bench/gates/intp_metrics.py`)

| Gate | Threshold |
|------|-----------|
| fidelity: cpu | absolute ratio band 0.8–1.25 vs ground-truth cpu_busy |
| fidelity: llcmr | directional, Spearman ρ ≥ 0.6 |
| fingerprint stability | mean pairwise Pearson r ≥ 0.8 |
| overhead envelope | throughput delta ≤ 5 % (`INTP_GATE_OVERHEAD_PCT`) |
| sample loss | ≤ 10 % per rep |
