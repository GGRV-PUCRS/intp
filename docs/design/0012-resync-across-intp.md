# ADR-0012 — Pinned re-sync with across-intp v0.2.0

- **Status:** Accepted; implemented for 0.10.0
- **Date:** 2026-09-22
- **Deciders:** Lead maintainer
- **Supersedes:** the port-and-freeze contract of [SYNC.md](../../SYNC.md) (2026-06-10)
- **Related:** [ADR-0003](0003-ebpf-acceleration.md), [ADR-0008](0008-experimental-feature-policy.md), [ADR-0009](0009-portable-metrics-plugin.md), [ADR-0010](0010-metric-semantic-models.md), across-intp `DECISIONS.md` D10 to D15, across-intp `docs/DECISIONS-container.md` C25 to C37

## Context

intp's engines were ported once from across-intp at `2e87b30fc762`
(2026-06-10, across-intp D10) under a port-and-freeze contract: after the
freeze the two repositories evolved independently, and any re-sync needed
an ADR here and a decision entry there.

Since then across-intp shipped the SBAC-PAD 2026 artifact (`v0.1.0`) and
the container and VM deployment study behind the JSA manuscript, released
as across-intp `v0.2.0`. Two upstream fixes (D11, D12) had already been
back-ported into intp before 0.9.0. The question for 0.10.0 was whether
anything else upstream should move, and how to record the relationship
so later syncs have a baseline.

## Decision

**1. Pin.** intp 0.10.0 is synced with across-intp
**`ce6600259a41`** (tagged `v0.2.0`, across-intp D15). The freeze commit
`2e87b30fc762` stays the origin named in each file's provenance header:
no ported file was re-ported from the pin, because the audit below found
nothing new to take.

**2. Audit.** Every row of the SYNC.md provenance map was diffed between
the freeze and the pin
(`git diff 2e87b30fc762 ce6600259a41 -- <origin paths>`), then against
intp. Upstream changed the ported origins in five commits only.

| Upstream change | Origin | intp state | Action |
| --- | --- | --- | --- |
| `wakeup_events=1` dropped from the eBPF llcmr sampling attr | D11, `8591e14` | Present (back-ported before 0.9.0) | None |
| Per-cgroup/PID `mbw` via resctrl, one refcounted monitoring group shared by `mbw` and `llcocc` | D12, `ca049b7` | Present (`resctrl_target_group_*`) | None |
| v3.2/v3.3 load-attach and ctxsw test robustness | D12, `ca049b7` | Present in `tests/regression/` | None |
| Capability banner strings name v2.1 / v3.3 | `7657801` | Not applicable (intp prints its own banner) | None |
| `cross-variant-correlation.py` heatmap file names (figure naming registry) | `920c72f` | Algorithm unchanged; `bench/gates/` keeps its own output names | None |
| `METRICS-ALIGNMENT.md` `mbw` row cites `docs/V3-OVERHEAD-FINDINGS.md` §3 instead of a draft section | D14, `4f9cfb5` | Documentation only | None |

**3. Items the audit raised in intp itself.** These are divergences from
the origin or fixes the origin never needed; each is its own commit.

| Item | Finding | Change in 0.10.0 |
| --- | --- | --- |
| Nested container cgroups | `resolve_target()` and the resctrl rescan read the target's `cgroup.procs` only. Under the cgroup v2 no-internal-processes rule an Incus or systemd container lists no PIDs at its top, so `n_pids` was 0 and `llcmr` perf_hwcache counted system-wide. v2.1 has the same non-recursive read (C25); v3.3 reads the subtree (`read_cgroup_pids_rec`). | Subtree read (`procutil_read_cgroup_procs_rec`) for cgroup and VM targets and for the rescan |
| Guest `llcmr` event family | In a KVM guest the LL-read events are refused and perfev opens the architectural cache-references/misses instead (C26). Samples still read `ok` under the host backend id. | Samples are `degraded` with note `arch_cache_refs`; `--print-schema` reports `event_family` |
| `mbw` ceiling | The formula (uncore_imc channels x configured MT/s x 8 B) matches v2.1 and was right on the testbed: 8 x 4400 MT/s = 281600 MB/s. The banked campaign's 42656 MB/s came from the silent DDR4 default when dmidecode returned no speed (C34, S16 R3). | Ceiling source recorded; `--print-schema` shows `ceiling_bps`, `ceiling_source`, `ceiling_fallback`; samples carry `ceiling_fallback` unless `--mem-bw-max-bps` is set |
| Guest `llcocc` | Proxy from the miss ratio (C36). | Unchanged: already status `proxy`, note `directional_only`; never counted as RDT coverage |
| RDT metrics in a guest | Must read unavailable, never 0 (C25 P5, C26). | Registry already does; the release gate gains a guest assertion |
| In-guest eBPF `cpu` grows with the sampling interval | JSA cadence sweep | Documented in METRICS.md; prefer the procfs backend in a guest |

The portable provider (`system.portable.*`, C26 to C28) and the guest
perf fallback were part of the original port: both predate the freeze
and are listed in the provenance map. They are not post-freeze drift.

**4. Research-only, not ported.** CV leakage analysis, the IADA
simulator and its S-campaigns, the figure pipelines (C35), and the
harness's `intp-detect.sh` ceiling capture stay in across-intp.

**5. Portable fence stays.** `system.portable.*` remains behind
`--experimental` in 0.10.0 (ADR-0008). The JSA campaign
(`p2-portable-15metric`) exists, but PSI was falsified as a
memory-bandwidth signal and `membw_est` over-reads on network workloads
under v3.3 (the C31 net-path caveat). The fence lifts per metric in
0.11.0 once the JSA verdicts are final.

**6. Rule for later syncs.** A re-sync names one across-intp commit
reachable from its `main` (a tag, preferably), repeats the audit above
against the previous pin, lands as a new ADR that supersedes this one,
and gets a matching entry in across-intp's `DECISIONS.md`. Fixes found
in intp stay in intp; research-only work stays in across-intp.

## Consequences

- `mbw` percentages from 0.10.0 can differ from the 0.9.0 gate run on a
  host where dmidecode was unavailable; the capability report and the
  schema now say so.
- Container targets with nested cgroups report per-container `llcmr`,
  `cpu` and resctrl values where 0.9.0 reported system-wide or
  unavailable ones.
- Consumers keying on `llcmr` status see `degraded` in a guest.
- Cross-variant parity in the release gate compares against v2.1 and
  v3.3 built at the pin.
