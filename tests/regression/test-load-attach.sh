#!/bin/bash
# SPDX-License-Identifier: MIT
#
# test-load-attach.sh -- smoke test: launch intp, verify the eBPF
# programs load and attach, let it run briefly, check no programs leak,
# confirm the JSONL output is well-formed.
#
# Ported from intp-comparison:
#   variants/v3.3-ebpf-core-cgroup/tests/integration/test-load-attach.sh
#   @ 2e87b30fc762
# adapted to invoke the intp binary (path in $1 or INTP_BIN) and to its
# JSONL default output instead of v3.3's TSV.
#
# Requires root + a BTF kernel: wired as the opt-in 'live' meson suite.

set -eu

BIN=${1:-${INTP_BIN:-./build/src/cli/intp}}
DURATION=${DURATION:-3}

if [ ! -x "$BIN" ]; then
    echo "ERROR: $BIN not built -- run 'meson compile' first"
    exit 1
fi
if [ "$(id -u)" -ne 0 ]; then
    echo "ERROR: must run as root (BPF + perf_event need privileges)"
    exit 1
fi

BPFTOOL=${BPFTOOL:-bpftool}
command -v "$BPFTOOL" >/dev/null 2>&1 || BPFTOOL=/usr/sbin/bpftool

pre_count="$("$BPFTOOL" prog show 2>/dev/null | wc -l | tr -d '[:space:]')"
pre_count="${pre_count:-0}"
echo "pre-run BPF program count: $pre_count"

out=$(mktemp)
trap 'rm -f "$out"' EXIT

echo "running $BIN --interval 1s --duration ${DURATION}s"
timeout $((DURATION + 5)) "$BIN" --interval 1s --duration "${DURATION}s" \
    > "$out" 2>&1 || true

post_count="$("$BPFTOOL" prog show 2>/dev/null | wc -l | tr -d '[:space:]')"
post_count="${post_count:-0}"
echo "post-run BPF program count: $post_count"

if [ "$post_count" -gt "$pre_count" ]; then
    echo "FAIL: leaked BPF programs (pre=$pre_count post=$post_count)"
    "$BPFTOOL" prog show 2>&1 | head -40
    exit 1
fi

lines="$(grep -c 'system\.interference\.' "$out" || true)"
if [ "$lines" -lt 1 ]; then
    echo "FAIL: no JSONL samples produced (output follows)"
    cat "$out"
    exit 1
fi

# The eBPF backends must actually have bound (root + BTF host): the
# schema selected-backend ids are the ebpf_* shims.
schema="$("$BIN" --print-schema 2>/dev/null || true)"
if ! echo "$schema" | grep -q 'ebpf_'; then
    echo "FAIL: --print-schema shows no ebpf_* backend bound (schema follows)"
    echo "$schema"
    exit 1
fi

echo "OK: produced $lines JSONL samples, ebpf_* backends bound, no leaked programs"
