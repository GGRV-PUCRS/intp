#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# bisect-amplification.sh — localize a ctxsw-amplification gate failure.
#
# Runs the amplification measurement four times with different backend
# selections so the offending component is identified empirically:
#
#   A  no profiler at all            (re-baseline sanity)
#   B  pure-C only                   (all five eBPF backends disabled)
#   C  eBPF minus llcmr              (the perf-overflow sampling path off)
#   D  full default selection        (what the gate measures)
#
# Reading the table:
#   B ~= A             the pure-C tier is clean
#   C ~= A but D >> A  the perf-event sampling path (ebpf_perf, the
#                      sample_period=1000 + wakeup_events=1 attr both
#                      v3.3 and this port use) is the amplifier on
#                      this host
#   C >> A             a tracepoint path amplifies (unexpected: the
#                      counter-map design does no per-event userspace
#                      work) — investigate the loader
#
# Usage: sudo tests/regression/bisect-amplification.sh [intp-binary]
# Env:   INTP_AMPL_SECONDS (default 30 here, for four quick windows)

set -u

BIN="${1:-./build/src/cli/intp}"
HERE="$(cd "$(dirname "$0")" && pwd)"
export INTP_AMPL_SECONDS="${INTP_AMPL_SECONDS:-30}"

[ "$(id -u)" -eq 0 ] || {
	echo "must run as root" >&2
	exit 1
}

EBPF_ALL="--disable-backend ebpf_bytes --disable-backend ebpf_softirq \
--disable-backend ebpf_svctm --disable-backend ebpf_sched \
--disable-backend ebpf_perf"

declare -A ratio

run_arm() { # label intp_args ("-" = skip profiler entirely)
	local label="$1" args="$2" out
	echo "==== arm $label ===="
	if [ "$args" = "-" ]; then
		# Baseline-only sanity: run the test but point it at
		# /bin/true so the with-arm adds no profiler.
		out=$(INTP_ARGS='' MAX_RATIO=999 \
			"$HERE/test-no-ctxsw-amplification.sh" /bin/true \
			2>&1 || true)
	else
		out=$(INTP_ARGS="$args" MAX_RATIO=999 \
			"$HERE/test-no-ctxsw-amplification.sh" "$BIN" \
			2>&1 || true)
	fi
	echo "$out" | grep -E "baseline ctxt|with-profiler ctxt|RESULT"
	ratio[$label]=$(echo "$out" |
		sed -n 's/.*ratio=\([0-9.]*\).*/\1/p' | head -1)
	echo
}

run_arm A "-"
run_arm B "$EBPF_ALL"
run_arm C "--disable-backend ebpf_perf"
run_arm D ""

echo "==== bisect summary (window=${INTP_AMPL_SECONDS}s) ===="
printf '%-44s %s\n' "A  no profiler (noise floor)" "${ratio[A]:-?}"
printf '%-44s %s\n' "B  pure-C only" "${ratio[B]:-?}"
printf '%-44s %s\n' "C  eBPF minus llcmr (no perf sampling)" "${ratio[C]:-?}"
printf '%-44s %s\n' "D  full default selection" "${ratio[D]:-?}"
