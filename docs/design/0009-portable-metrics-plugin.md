# ADR-0009 — Portable metrics as an in-tree metric-provider plugin

- **Status:** Accepted
- **Date:** 2026-06-10
- **Deciders:** Lead maintainer
- **Related:** [ADR-0004](0004-plugin-abi.md), [ADR-0008](0008-experimental-feature-policy.md), across-intp `docs/reports/8th-metric-vm-portable-design.md` (C26/C27)

## Context

The research line added eight opt-in metrics that survive
environments where the RDT-backed core metrics are structurally
gapped (stock KVM guests): six VM-portable (`schedlat`, `psi_mem`,
`membw_est`, `psi_io`, `schedthr`, `steal`) and two scheduling-regime
(`psp`, `idle_preempt`). Upstream they are a SEPARATE flag-gated
benchmark — the canonical 7-metric fingerprint is byte-identical with
the flag off, and the validation campaign (C27) is still pending.

## Decision

Port `variants/v2.1-c-abi-cgroup/src/portable.c` as an **in-tree
`metric_provider` plugin** (`src/plugins/portable-metrics/`), not as
core backends:

- Namespace `system.portable.*` — fully outside the frozen
  `system.interference.*` namespace; consumers ignoring unknown keys
  are unaffected (ADR-0002).
- Marked `stability: experimental` and loadable only under
  `--experimental` (ADR-0008). `--portable-metrics` is sugar for
  `--experimental --enable-plugin portable-metrics`.
- Units per the upstream design: percentages for
  schedlat/psi_*/schedthr/steal, MB/s for membw_est, events/s for
  psp/idle_preempt. `idle_preempt` is eBPF-sourced and reports
  unavailable on the pure-C path, exactly as upstream. (ADR-0013 adds
  the eBPF backends that give it, `schedlat` and `psp` their v3.3
  values.)
- Packaged as its own binary package (`intp-plugin-portable`) so the
  default install carries no experimental code.

## Rationale

The plugin form dogfoods ADR-0004 with a real consumer, keeps the
canonical seven untouched in core, lets packaging exclude it by
default, and makes post-validation promotion (or removal) a non-core
change.

## Alternatives considered

- **Core backends behind a flag.** Rejected: grows the core metric
  set before validation and complicates the frozen-contract story.
- **Out-of-tree repository.** Rejected for now: the code is small,
  shares procutil/detect, and the dissertation timeline needs it
  versioned with the engine.
