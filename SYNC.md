# SYNC.md — relationship to the research repository

`intp` is the production line. [**across-intp**](https://github.com/GGRV-PUCRS/across-intp)
(formerly intp-comparison) is the public research repository where the
measured variants, the benchmark campaigns and the papers live: its
`v0.1.0` release is the SBAC-PAD 2026 artifact, and the JSA article cites
its `v0.2.2` release for the code and the `v0.2.1` record for the data. Decision codes in comments here refer to across-intp's
logs unless another log is named: `D` entries to `DECISIONS.md`, `C`
entries to `docs/DECISIONS-container.md`, `S` entries to
`bench/iada/DECISIONS-sim-experiments.md`.

## The contract: pinned re-sync

- **Current pin:** across-intp tag **`v0.2.2`**, recorded in
  [ADR-0014](docs/design/0014-resync-v0.2.2-cpu-norm.md). The audit read
  `release/v0.2.2` at `5d90acc`; the tag's commit goes here once it is cut.
  Earlier pins: `a214643cb1be` (tag `v0.2.1`,
  [ADR-0013](docs/design/0013-engines-and-resync-v0.2.1.md)) and
  `24f26e5a5df1` (tag `v0.2.0`,
  [ADR-0012](docs/design/0012-resync-across-intp.md), across-intp D15).
- **History:** the first port was a one-way freeze at **`2e87b30fc762`**
  (branch `feat/container-based-interference`, 2026-06-10, across-intp
  D10). Every ported file carries a provenance header naming its origin
  path at that commit. The port was verified file by file against the
  originals (formulas, constants, vendor event codes, branch order,
  status assignments); the only intended additions are documented in the
  file headers and commit messages (RMID hygiene; ADR-0001
  registration). D11 and D12 were back-ported before 0.9.0.
- **Between syncs** the two repositories evolve independently. Fixes
  belong in `intp`; experiments belong in across-intp.
- **A re-sync** names one across-intp commit reachable from its `main`
  (preferably a tag), diffs every origin below between the previous pin
  and the new one, classifies each hunk (already present, port,
  intentional divergence, research-only), and lands as a new ADR here
  plus a decision entry there. Not a cherry-pick.

## Intentional divergences at the current pin

Recorded in ADR-0012: intp resolves cgroup targets over the whole
subtree (v2.1 reads the top cgroup only), marks the guest
architectural-cache `llcmr` fallback as degraded, and reports where the
`mbw` ceiling came from.

Recorded in ADR-0013: intp runs both lineages in one binary
(`--engine auto|stable|ebpf`); under `auto` the eBPF `cpu`, `llcmr` and
`netp` backends step aside for per-PID targets, and the eBPF `cpu`
backend inside a KVM guest. `--engine=stable` and `--engine=ebpf`
reproduce v2.1 and v3.3 without those rules.

Recorded in ADR-0014: the `--cpu-norm` basis is reported by
`--print-schema` instead of a `# cpu normalization:` line in the TSV,
which keeps its legacy header.

## Provenance map

Origins are paths in across-intp. Each was first ported at
`2e87b30fc762`, audited at `24f26e5a5df1` (ADR-0012), at `a214643cb1be`
(ADR-0013), where the v0.2.1 changes were ported, and at v0.2.2
(ADR-0014), where the cgroup cpu normalization was ported.

| intp | across-intp origin |
|---|---|
| `src/core/registry.c` (selection semantics) | `variants/v2.1-c-abi-cgroup/src/backend_registry.c` |
| `src/core/dispatcher.c` (cadence) | `variants/v2.1-c-abi-cgroup/intp-hybrid.c` (main loop) |
| `src/core/detect.{c,h}` | `variants/v2.1-c-abi-cgroup/{src/detect.c,include/detect.h}` |
| `src/backends/common/procutil.{c,h}` | `variants/v2.1-c-abi-cgroup/{src/procutil.c,include/procutil.h}` |
| `src/backends/common/resctrl.{c,h}` | `variants/v2.1-c-abi-cgroup/{src/resctrl.c,include/resctrl.h}` (+ hygiene additions: `resctrl_reap_stale`, `resctrl_rmid_budget_ok`) |
| `src/backends/common/perfev.{c,h}` | `variants/v2.1-c-abi-cgroup/{src/perfev.c,include/perfev.h}` |
| `src/backends/netp/netp_stable.c` | `variants/v2.1-c-abi-cgroup/src/netp.c` |
| `src/backends/nets/nets_softirq.c` | `variants/v2.1-c-abi-cgroup/src/nets.c` |
| `src/backends/blk/blk_stable.c` | `variants/v2.1-c-abi-cgroup/src/blk.c` |
| `src/backends/mbw/mbw_stable.c` | `variants/v2.1-c-abi-cgroup/src/mbw.c` (+ hygiene in init) |
| `src/backends/llcmr/llcmr_perf.c` | `variants/v2.1-c-abi-cgroup/src/llcmr.c` |
| `src/backends/llcocc/llcocc_stable.c` | `variants/v2.1-c-abi-cgroup/src/llcocc.c` (+ hygiene in init) |
| `src/backends/cpu/cpu_procfs.c` | `variants/v2.1-c-abi-cgroup/src/cpu.c` |
| `src/backends/ebpf/cpu_ebpf.c` (cpu basis) | `variants/v3.3-ebpf-core-cgroup/src/{intp_agg.c,cgroup_cap.c}` (`compute_sample`, `--cpu-norm`) |
| `src/backends/ebpf/intp_agg.bpf.{c,h}` (byte-frozen) | `variants/v3.3-ebpf-core-cgroup/src/intp_agg.bpf.{c,h}` |
| `src/backends/ebpf/ebpf_loader.c` + shims | `variants/v3.3-ebpf-core-cgroup/src/intp_agg.c` (split: skeleton lifecycle / map polling / compute_sample slices) |
| `src/output/tsv_legacy.c` (rendering) | `variants/v2.1-c-abi-cgroup/intp-hybrid.c` (`emit_tsv`/`format_value`) |
| `src/plugins/portable-metrics/` (stable-ABI backends) | `variants/v2.1-c-abi-cgroup/src/portable.c` |
| `src/plugins/portable-metrics/` (eBPF backends) | `variants/v3.3-ebpf-core-cgroup/src/intp_agg.c` (`--portable-metrics` block of the main loop) |
| `tests/unit/test_procutil.c`, `test_detect.c` | `variants/v2.1-c-abi-cgroup/tests/` |
| `tests/regression/xval.sh` | `shared/validate-cross-variant.sh` |
| `tests/regression/preflight.sh` | `shared/intp-preflight.sh` |
| `tests/regression/validate-attribution.sh` | `bench/validate-attribution.sh` |
| `bench/gates/` | `bench/plot/{analyze-faithfulness,cross-variant-correlation,extract-fragility}.py` |

Variants v0.x/v1.x (SystemTap) and v3/v3.1 (ring-buffer eBPF,
bpftrace) deliberately stay in across-intp as historical references
and structural evidence — see ARCHITECTURE.md "Heritage".

## Semantic ground truth

Metric formula questions are settled by across-intp's
`METRICS-ALIGNMENT.md` and `docs/METRICS-DEEP-DIVE.md` at the pin, surfaced here through ADR-0010 (semantic models). The
per-cgroup `nets` byte-share proxy divergence (upstream decision C2)
is encoded in the parity tooling, not just documented.

## Re-running cross-repo parity

`tests/regression/xval.sh --cross` compares this repo's binary
against the v2.1/v3.3 binaries built from an across-intp checkout at the pin
(`INTP_XVAL_ACROSS_DIR`), with pinned normalizer overrides, the nets
exemption under cgroup scope, and blk compared within model families.
The release-blocking run happens on the RDT testbed via
`tests/regression/run-remote-gate.sh` (step 5).
