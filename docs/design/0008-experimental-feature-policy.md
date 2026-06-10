# ADR-0008 — Experimental-feature policy

- **Status:** Accepted
- **Date:** 2026-06-10
- **Deciders:** Lead maintainer
- **Related:** [ADR-0002](0002-output-schema-and-versioning.md), [ADR-0009](0009-portable-metrics-plugin.md), across-intp `docs/DECISIONS-container.md` (C26/C27)

## Context

The release scope includes capabilities whose validation campaign has
not yet run (VM targeting via `--target-vm`/`--tap-iface`; the
portable-metrics set). The project invariant is "measured, not
asserted": shipping them unmarked would launder unvalidated paths
into the trusted surface; cutting them would discard working code the
dissertation work actively exercises.

## Decision

1. **Stability field.** Every metric/feature carries
   `stability: stable | experimental` in `--print-schema`. The seven
   core metrics are stable; `--target-vm` and `system.portable.*` are
   experimental until promoted.
2. **CLI fence.** Experimental features refuse to run without
   `--experimental`, printing what is unvalidated and where the
   validation plan lives. The fence is a deliberate speed bump, not a
   hidden flag.
3. **No contract contamination.** Experimental output never enters
   `tsv-legacy-v1` or the frozen `system.interference.*` namespace.
4. **Promotion criteria.** A feature is promoted (fence removed,
   stability flipped) only when its validation campaign passes on the
   reference testbed — for the current set: the portable/VM first-pass
   validation (step 8 of `tests/regression/run-remote-gate.sh`,
   adjudicated by across-intp's `bench/analyze-portable.py`
   methodology). Promotion is a CHANGELOG-visible decision referencing
   the campaign results.
5. **Demotion/removal** of an experimental feature is a minor-version
   change (it was never part of the stable contract).

## Consequences

- 0.9.0 can ship the full scope without weakening the "fidelity
  demonstrated, not asserted" invariant.
- Consumers scripting against experimental surfaces opted in
  explicitly and were told the risk.
