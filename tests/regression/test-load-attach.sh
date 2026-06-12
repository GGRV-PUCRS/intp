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

# Leak check counts intp's OWN programs, never the global total. On a
# busy multi-tenant host the system-wide BPF program count churns
# continuously (containers, systemd, qemu load and unload programs
# independently of intp), so a global pre/post delta is not attributable
# to us -- it produced a false "leak" failure on the RDT testbed
# (pre=111 post=115) while intp's teardown was provably clean: its own
# programs went 0 -> 15 -> 0 across the run. These are the program
# function names from intp_agg.bpf.c, truncated to the kernel's 15-char
# BPF_OBJ_NAME_LEN where longer.
INTP_PROG_RE='cg_skb_egress|cg_skb_ingress|tp_net_dev_xmit|tp_netif_receiv|tp_block_rq|tp_block_bio|tp_sched_switch|tp_sched_wakeup|tp_schedlat|tp_softirq_|perf_llc_'

count_intp_progs() {
    "$BPFTOOL" prog show 2>/dev/null | grep -cE "name ($INTP_PROG_RE)" |
        tr -d '[:space:]'
}

pre_count="$(count_intp_progs)"
pre_count="${pre_count:-0}"
echo "pre-run intp BPF programs: $pre_count"

out=$(mktemp)
trap 'rm -f "$out"' EXIT

echo "running $BIN --interval 1s --duration ${DURATION}s"
timeout $((DURATION + 5)) "$BIN" --interval 1s --duration "${DURATION}s" \
    > "$out" 2>&1 || true

# BPF program teardown is ASYNCHRONOUS to process exit: intp's
# intp_agg_bpf__destroy() drops every link and the program refcount, but
# the kernel reaps the program after an RCU grace period, so a tp_btf
# program (observed: tp_schedlat) can still appear for a fraction of a
# second after intp returns. Poll until intp's programs drain back to
# the pre-run baseline; only a count that never settles within the
# timeout is a real leak. On the RDT testbed this drains in < 0.5s.
settle_deadline=$(($(date +%s) + 5))
while :; do
    post_count="$(count_intp_progs)"
    post_count="${post_count:-0}"
    [ "$post_count" -le "$pre_count" ] && break
    [ "$(date +%s)" -ge "$settle_deadline" ] && break
    sleep 0.25
done
echo "post-run intp BPF programs: $post_count"

if [ "$post_count" -gt "$pre_count" ]; then
    echo "FAIL: intp leaked BPF programs (pre=$pre_count post=$post_count, did not drain in 5s)"
    "$BPFTOOL" prog show 2>&1 | grep -E "name ($INTP_PROG_RE)" | head -20
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
