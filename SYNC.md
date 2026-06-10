# SYNC.md — relationship to the research repository

`intp` is the production line; **across-intp** (the working name of
the comparison/research monorepo, `github.com/Saccilotto/across-local-intp`,
publicly referenced as `intp-comparison`) is the experiment harness
where the measured variants, the benchmark campaigns, and the papers
live.

## The contract: port-and-freeze

Code moved **one way**, once, from across-intp into this repository:

- **Freeze baseline:** branch `feat/container-based-interference` at
  commit **`2e87b30fc762`** (2026-06-10).
- Every ported file carries a provenance header naming its origin path
  at that commit. The port was adversarially verified file-by-file
  against the originals (formulas, constants, vendor event codes,
  branch order, status assignments) with zero behavioral drift; the
  only intended additions are documented in the file headers and the
  commit messages (RMID hygiene; ADR-0001 registration).
- After the freeze, the two repositories evolve **independently**.
  Fixes belong in `intp`; experiments belong in across-intp. A future
  re-sync (in either direction) is a consequential decision that
  requires a new ADR here and a decision-log entry there — not a
  cherry-pick.

## Provenance map

| intp | across-intp origin (@ 2e87b30fc762) |
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
| `src/backends/ebpf/intp_agg.bpf.{c,h}` (byte-frozen) | `variants/v3.3-ebpf-core-cgroup/src/intp_agg.bpf.{c,h}` |
| `src/backends/ebpf/ebpf_loader.c` + shims | `variants/v3.3-ebpf-core-cgroup/src/intp_agg.c` (split: skeleton lifecycle / map polling / compute_sample slices) |
| `src/output/tsv_legacy.c` (rendering) | `variants/v2.1-c-abi-cgroup/intp-hybrid.c` (`emit_tsv`/`format_value`) |
| `src/plugins/portable-metrics/` | `variants/v2.1-c-abi-cgroup/src/portable.c` |
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
`METRICS-ALIGNMENT.md` and `docs/METRICS-DEEP-DIVE.md` at the freeze
commit, surfaced here through ADR-0010 (semantic models). The
per-cgroup `nets` byte-share proxy divergence (upstream decision C2)
is encoded in the parity tooling, not just documented.

## Re-running cross-repo parity

`tests/regression/xval.sh --cross` compares this repo's binary
against the v2.1/v3.3 binaries built from an across-intp checkout
(`INTP_XVAL_ACROSS_DIR`), with pinned normalizer overrides, the nets
exemption under cgroup scope, and blk compared within model families.
The release-blocking run happens on the RDT testbed via
`tests/regression/run-remote-gate.sh` (step 5).
