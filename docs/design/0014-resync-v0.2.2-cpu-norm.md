# ADR-0014 — Re-sync with across-intp v0.2.2: opt-in cgroup cpu normalization

- **Status:** Accepted
- **Date:** 2026-11-24
- **Deciders:** Lead maintainer
- **Supersedes:** [ADR-0013](0013-engines-and-resync-v0.2.1.md) as the current pin (its audit stays the baseline for the v0.2.1 sync)
- **Related:** [ADR-0010](0010-metric-semantic-models.md), [ADR-0012](0012-resync-across-intp.md), across-intp `docs/DECISIONS-container.md` C37, C39 and C40, `DECISIONS.md` D22 and D23

## Context

The `cpu` metric divides a target's on-CPU time by interval × online
CPUs, so a tenant pinned to C of N CPUs saturates at C/N while the same
work in a guest of C vCPUs reads 100. The JSA measured this as the 3.0
host/guest ratio (across-intp C37) and corrected it after the fact.
across-intp v0.2.2 adds an opt-in `--cpu-norm cgroup` to v2.1 and v3.3
(C40) that divides by the CPU capacity of the target cgroup instead.
The default stays `online`, so published campaigns and the IADA training
data keep their scale. v0.2.2 also hardens the harness's memory-bandwidth
ceiling (C39) and relabels the JSA figures (D22).

## Decision

**1. Pin.** intp is synced with across-intp **`a1aad1a7277e`** (tag
`v0.2.2`, the merge of `release/v0.2.2` into `main`). This ADR audited
`release/v0.2.2` at `5d90acc`; the merge commit carries the same tree.

**2. Audit** (`git diff a214643cb1be 5d90acc` over every SYNC.md origin):

| Upstream change | Origin | Action |
| --- | --- | --- |
| `procutil_count_cpulist()`, `procutil_cgroup_cpu_capacity()`: CPU count of `cpuset.cpus.effective`, capped by the tightest `cpu.max` quota on the path or an ancestor | v2.1 `procutil.{c,h}` (C40) | Ported, same algorithm and unit cases |
| cgroup `cpu` backend divides by the capacity under `--cpu-norm cgroup` | v2.1 `src/cpu.c`, `include/backend.h` | Ported (`cpu_procfs.c`, `intp_core_target.cpu_norm_cgroup`) |
| eBPF `cpu` divides by the capacity in cgroup mode; `schedlat` keeps the online budget | v3.3 `intp_agg.c`, `cgroup_cap.{c,h}` | Ported (`cpu_ebpf.c`); intp's `schedlat` backends do not change |
| `--cpu-norm` option and a `# cpu normalization:` header line | v2.1 `intp-hybrid.c`, v3.3 `intp_agg_args.{c,h}`, `intp_agg.c` | Ported as `--cpu-norm`; the basis is reported by `--print-schema` (`normalization`, `normalization_cpus`) because intp's TSV keeps its legacy header (intentional divergence) |
| Placeholder memory-bandwidth ceiling flagged by `intp-detect.sh` and fatal in the harness; per-run ceiling in `run.json`; analysis rescale | `shared/intp-detect.sh` and the harness (C39) | Already present in kind: `--print-schema` reports the `mbw` ceiling and whether it is a fallback (ADR-0012). The harness and analysis parts are research-only |
| Variant READMEs, Makefile, `.gitignore`, upstream unit-test files | v2.1, v3.3 | Research-only; intp has its own tests (`test_procutil`, `cli/cpu-norm-*`) |
| Figure vocabulary (D22), release metadata (D23) | `bench/plot/`, docs | Research-only |

**3. Default.** `--cpu-norm online` remains the default; no output
changes unless the option is given.

## Consequences

- With `--cpu-norm cgroup`, `cpu` of a cgroup target is a share of the
  cgroup's own allotment, so a pinned container and a guest of the same
  size report on the same scale. Without a cgroup target the option has
  no effect.
- The basis is read once when the backend starts, as in both variants.
- Validation at this ADR (release/0.10.0-beta2, kernel 7.0, eBPF build
  with `-Dwerror=true`): 26 tests pass, including the unit cases on a
  fake cgroup tree and two CLI tests (a bad mode exits 2; the schema
  declares the basis). Under a user scope with `CPUQuota=150%` on 16
  CPUs, `cpu` reads 9.38 % with `online` and 100.00 % with `cgroup`, and
  `--print-schema` reports `cgroup`, 1.50. Not yet run on the RDT
  testbed; 0.10.0-beta2 skips the release gate like 0.10.0-beta1.
