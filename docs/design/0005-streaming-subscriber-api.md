# ADR-0005 — Streaming subscriber API (Unix socket / gRPC)

- **Status:** Proposed (deferred post-1.0)
- **Date:** 2026-06-10
- **Deciders:** Lead maintainer
- **Related:** [ARCHITECTURE.md](../ARCHITECTURE.md), [ADR-0002](0002-output-schema-and-versioning.md)

## Context

ARCHITECTURE.md sketches live transports for consumers that need
samples without polling files: a Unix-domain socket
(`/run/intp/sample.sock`, SO_PEERCRED auth) and a gRPC endpoint
(`intp serve --grpc`, mTLS / bearer token) — the integration points
for a Kubernetes scheduler plugin, IADA online mode, or a SLURM SPANK
plugin.

## Decision

Defer both transports past 1.0. What ships now:

- **In-process streaming** via `intp_subscribe()` (public C API) — a
  linked consumer gets every sample with no transport at all.
- **Pull**: the `--serve` Prometheus endpoint.
- **Pipe**: JSONL on stdout is losslessly consumable by any
  supervisor.

The socket/gRPC transports require an authentication story, a
versioned wire schema (`intp.proto`), and daemon lifecycle decisions
(systemd unit, ADR pending) that should not gate the first release.
This ADR records the intended shape so the public API does not paint
itself out of it: `intp_subscribe`'s callback contract is exactly the
per-sample hook the future transport server will consume.

## Consequences

- ARCHITECTURE.md's reference to this design is now a real document
  with an honest status instead of a dangling link.
- Downstream consumers needing live data today link libintp or read
  JSONL; nothing in the current ABI blocks the future transports.
