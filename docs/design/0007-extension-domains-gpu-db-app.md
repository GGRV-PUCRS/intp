# ADR-0007 — Extension domains: GPU, database, application metrics

- **Status:** Proposed (stub)
- **Date:** 2026-06-10
- **Deciders:** Lead maintainer
- **Related:** [ADR-0002](0002-output-schema-and-versioning.md), [ADR-0004](0004-plugin-abi.md)

## Context

ADR-0002 reserves plugin-defined metric namespaces beyond the OS-level
core: `gpu.<vendor>.*`, `db.<engine>.*`, `app.<service>.*`. The meson
options (`with_gpu_nvml`, `with_gpu_rocm`, `with_gpu_zes`) and the
plugin ABI already anticipate them.

## Decision (to be made)

This ADR is a placeholder reserving the numbering and the namespace
conventions. The concrete design — which domains ship in-tree as
reference plugins, their schema fragments, their capability
declarations — lands when the first extension-domain plugin is
implemented. Until then the only normative content is:

- Extension metrics live under their own namespace, never under
  `system.interference.*`.
- Each domain plugin is a `metric_provider` (ADR-0004) shipped as a
  separate package with its own dependency footprint.

## Status

No extension-domain plugin is scheduled for the 0.9.x line.
