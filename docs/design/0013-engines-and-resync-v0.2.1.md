# ADR-0013 — Backend engines, portable eBPF backends, and re-sync with across-intp v0.2.1

- **Status:** Accepted
- **Date:** 2026-11-23
- **Deciders:** Lead maintainer
- **Supersedes:** [ADR-0012](0012-resync-across-intp.md) as the current pin (its audit stays the baseline for the v0.2.0 sync)
- **Related:** [ADR-0001](0001-backend-registry.md), [ADR-0003](0003-ebpf-acceleration.md), [ADR-0008](0008-experimental-feature-policy.md), [ADR-0009](0009-portable-metrics-plugin.md), across-intp `docs/DECISIONS-container.md` C31 and C38

## Context

intp's two engines come from across-intp: the stable-ABI chains from
v2.1 and the in-kernel-aggregating eBPF backends from v3.3. The JSA
manuscript evaluates every metric under both variants. Re-reading both
variants against intp at the v0.2.0 pin showed four gaps:

1. **Portable metrics ran only the v2.1 code.** The eBPF loader attached
   v3.3's `tp_sched_wakeup{,_new}` and `tp_schedlat` programs and summed
   `schedlat_wait_ns_sum`, `psp_count` and `idle_preempt_count`, but no
   backend read them. `idle_preempt` was always unavailable, and
   `schedlat`/`psp` always came from `/proc` even on an eBPF host. An
   intp user could reproduce only the C column of the JSA figures.
2. **The v0.2.1 fix (C38) was missing.** `/proc/<pid>/schedstat` and the
   `*_ctxt_switches` lines describe one thread. Reading them per process
   left the worker threads of a multi-threaded target uncounted, and a
   delta over the sum of counters dropped to 0 whenever a task exited.
   A four-thread workload whose leader sleeps read `schedlat = 0`,
   `psp = 0` on intp 0.10.0-beta1; the JSA recommends v0.2.1 or later
   for multi-threaded targets.
3. **eBPF silently widened per-PID targets.** The BPF programs gate on
   cgroup identity or count host-wide; v3.3 has no per-PID scope. For
   `-p`/`--pids`/`--comm` the eBPF `cpu`, `llcmr` and `netp` backends
   still won selection and reported host-wide values under a `pid`
   target, although stable per-PID backends existed.
4. **No way to run one lineage.** A researcher could not ask intp for
   "v2.1 only" or "v3.3 only" without listing every backend to disable,
   so the separate across-intp binaries stayed necessary for
   cross-variant work.

## Decision

**1. Pin.** intp is synced with across-intp **`a214643cb1be`** (tag
`v0.2.1`). Its variant trees are identical to `main` at the time of
this ADR (`5151ec1`).

**2. Audit** (`git diff 24f26e5a5df1 a214643cb1be` over every SYNC.md
origin):

| Upstream change | Origin | Action |
| --- | --- | --- |
| Per-thread `schedlat`/`psp` (per-TID baselines over `cgroup.threads` / `/proc/<pid>/task`; `tid_cap` degradation) | v2.1 `portable.c`, `procutil.{c,h}`, `intp.h` (C38, `3067a0a`) | Ported |
| Recursive resctrl rescan and immediate enrollment of a parent slice | v2.1 `resctrl.{c,h}` (C38) | Already present (ADR-0012 subtree read) |
| `is_partition` keeps the `strrchr` result `const` | v2.1 `blk.c` (`2ab09e1`) | Ported (it was a build warning here too) |
| Comments: `psp` is not Volpert's PSP; `schedthr` reads 0 without `cpu.max` | v3.3 `intp_agg.bpf.{c,h}`, `intp_agg.c`; v2.1 `portable.c` | Ported; the BPF files stay byte-identical to upstream apart from the provenance header and one earlier comment correction |
| CLI usage text for `--portable-metrics` | v2.1 `intp-hybrid.c`, v3.3 `intp_agg_args.c` | Not applicable (intp has its own CLI) |
| C38 test harness and logs | v2.1 `tests/` | Research-only |

**3. Lineage and engines.** Every core backend declares its lineage
(`stable`, `ebpf`, or both for the resctrl backends v2.1 and v3.3
share). `--engine` selects which may run:

- `auto` (default): today's hybrid, eBPF first where it probes;
- `stable` (aliases `c`, `v2.1`): only stable-ABI backends;
- `ebpf` (alias `v3.3`): only the eBPF lineage and the shared resctrl
  backends; the CLI refuses to sample when eBPF cannot load.

Under a single lineage, a metric the lineage cannot serve reads
unavailable instead of borrowing from the other one, so a run matches
one variant end to end. `--force-backend metric:id` still picks any
backend for one metric and overrides the engine, now for portable
metrics too.

**4. Portable eBPF backends.** The provider gains `ebpf_schedlat`,
`ebpf_psp`, `ebpf_idle_preempt` and `ebpf_llc_misses`, reading the
loader's counters with v3.3's formulas. They lead the `schedlat`, `psp`
and `idle_preempt` chains. For `membw_est` the stable perf backends stay
first: on network-heavy targets the eBPF network hooks charge their own
cache misses to the target cgroup (C31), which the JSA measured as a
6–7x higher reading. File-based metrics (`psi_*`, `schedthr`, `steal`)
are shared by both lineages.

**5. Defer rule (auto only).** A backend may decline selection with a
recorded reason even when its probe passes. The eBPF `cpu`, `llcmr` and
`netp` backends and the portable eBPF backends decline per-PID targets.
The eBPF `cpu` backend also declines inside a KVM guest, where the JSA
showed its reading depends on the sampling interval. `--engine=ebpf` and
`--force-backend` ignore the rule.

**6. Introspection.** `--list-backends` shows each candidate's lineage
and rejection reason and, with the portable provider, its chains.
`--print-schema` reports the engine and the bound backend of every
portable metric.

## Consequences

- `--engine=stable` and `--engine=ebpf` let one intp build replace the
  v2.1 and v3.3 binaries for experiments; the JSA's C and eBPF columns
  can be produced with the same CLI and output formats.
- On eBPF hosts, `system.portable.schedlat` and `psp` now come from BPF
  by default, as in v3.3, and `idle_preempt` has a value. Consumers that
  need v2.1 values pass `--engine=stable` or force the per-thread
  backends.
- Per-PID runs on eBPF hosts report per-PID `cpu`, `llcmr` and `netp`
  where 0.10.0-beta1 reported host-wide values.
- KVM guests get the procfs `cpu` backend without the manual
  `--disable-backend cpu:ebpf_sched` ADR-0012 recommended.
- Without a cgroup target v3.3 reads 0 for its cgroup-scoped columns;
  `--engine=ebpf` reads the host-wide counters instead, as v3.2 does.
  Equivalence with v3.3 is therefore checked on cgroup targets only
  (`xval.sh` skips the v3.3 arm otherwise).
- Validation at this ADR (kernel 7.0, cgroup target, `xval.sh
  --cross`): all six pairs within tolerance; `--engine=stable` vs v2.1
  and `--engine=ebpf` vs v3.3 agree within 0.5 pp on `cpu`, 1 pp on
  `llcmr`, 3.3 pp on `llcocc` (the v2.1 proxy), and on `blk` once both
  sides use the same disk ceiling.
- The `system.portable.*` fence (ADR-0008) is unchanged.
