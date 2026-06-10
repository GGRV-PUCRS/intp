# ADR-0006 — Cgroup and VM attribution in core

- **Status:** Accepted
- **Date:** 2026-06-10
- **Deciders:** Lead maintainer
- **Related:** [ADR-0001](0001-backend-registry.md), [ADR-0010](0010-metric-semantic-models.md), across-intp `docs/DECISIONS-container.md` (C1–C15), `variants/v2.1-c-abi-cgroup/DESIGN.md`, `variants/v3.3-ebpf-core-cgroup/DESIGN.md`

## Context

"A container is a cgroup": the same per-cgroup code path serves bare
co-located processes, containers (docker/podman/lxc-incus/k8s), and —
with a host tap interface for netp — VM guests. The research line
validated this in two sibling engines (v2.1 stable-ABI, v3.3 eBPF)
whose per-metric attribution differs in mechanism but not in
question. `intp` ships both as backends of the same metrics.

## Decision

### Target model

`INTP_TARGET_CGROUP` resolves the cgroup-v2 path at open: member PIDs
are read from `cgroup.procs` (non-recursive — the resctrl path's
contract-preserving reader), and the eBPF loader additionally resolves
the cgroup identity (`stat(path).st_ino`) with ancestor-inclusive
gating by default (`--exact` switches to leaf-only). Container
runtimes are addressed by passing their cgroup path
(`--target-container` is an alias). `INTP_TARGET_VM` (experimental,
ADR-0008) is a cgroup scope (the hypervisor's qemu scope) plus a host
tap/vnet interface for netp.

### Per-metric attribution sources (cgroup scope)

| Metric | pure-C backend | eBPF backend |
|---|---|---|
| cpu | cgroup v2 `cpu.stat` usage_usec (child-inclusive, continuous) | `sched_switch` gated by cgroup id |
| blk | cgroup v2 `io.stat` bytes / disk-bw ceiling (`cgroup_io_throughput`) | `block_rq_issue/complete` svctm with `bio->bi_blkg` owner attribution (`svctm_busy_fraction`) |
| llcmr | `perf_event_open` cgroup mode (`PERF_FLAG_PID_CGROUP`, one pair per CPU) | perf-event BPF counters, cgroup-gated |
| mbw / llcocc | resctrl mon_group re-populated from `cgroup.procs` on a cadence (eager at startup, then periodic) | same resctrl path (BPF cannot do file I/O) |
| netp | the container's netns via `/proc/<pid>/net/dev`, normalized by the **host** NIC speed | `cgroup_skb` egress/ingress byte counters (covers shared-netns cases the netns reader cannot) |
| nets | **system-wide only** (softirq time is host-global; no per-cgroup counter without eBPF) — declared `scope=system` | per-cgroup **byte-share proxy**: `nets_sys × bytes(cg) / bytes_total` |

### The nets exemption

The per-cgroup `nets` proxy is divergent **by design** from the
system-wide softirq figure (decision C2 upstream). Every parity or
equivalence check between backends/variants must exempt `nets` under
cgroup scope; the exemption lives in the regression scripts, not only
in prose. Whether the proxy is more faithful to true per-cgroup
softirq cost is a hypothesis pending measurement, never an asserted
advantage.

### resctrl under cgroup scope

mon_groups are re-scanned from `cgroup.procs` on a cadence (eager for
the first samples to catch fork storms, then periodic) so
late-spawned workers and cgroup migrations are picked up; RMID
inheritance alone misses tasks that forked before attach. RMID
hygiene (75 % budget, stale-group reaping) applies unchanged.

## Consequences

- One binary serves system, PID, container, and (experimental) VM
  scopes with the scope declared per metric in `--print-schema`.
- Per-cgroup `nets` exists only on the eBPF path — the documented
  irreducible eBPF advantage; pure-C reports the system-wide value
  with `scope=system` so consumers are never silently misled.
- VM `nets` is N/A (guest softirq cost is not host-observable as a
  cgroup signal, C8 upstream).
