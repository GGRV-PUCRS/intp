#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# run-gates.sh — the four release gates over a small solo campaign.
#
# Reduced port of the intp-comparison campaign driver
# (bench/run-intp-bench.sh @ 2e87b30fc762): solo stage + overhead stage
# only, one binary under test, then the gate analyzers:
#
#   1. fidelity      analyze-faithfulness.py  (cpu ratio band 0.8-1.25,
#                    llcmr Spearman rho >= 0.6 — claim-class thresholds)
#   2. overhead      cross-variant-correlation.py (bogo-ops/s delta vs
#                    the no-profiler baseline <= INTP_GATE_OVERHEAD_PCT,
#                    default 5)
#   3. fingerprint   cross-variant-correlation.py (mean pairwise Pearson
#                    r across rep fingerprints >= 0.8)
#   4. sample loss   extract-fragility.py (loss <= 10 % on every rep)
#
# Context-switch ground truth, when needed, must come from vmstat or
# /proc/stat ctxt — NEVER `perf stat`: a BPF program co-attached to the
# same tracepoint makes perf under-count arbitrarily (documented kernel
# behavior since 2016; see the SBAC-PAD 2026 methodological caveat).
#
# Usage:  bench/gates/run-gates.sh [intp-binary] [out-dir]
# Env:    INTP_GATE_SECONDS (30)  per-run window
#         INTP_GATE_REPS (3)      reps per workload
#         INTP_GATE_INTERVAL (1)  sampling interval seconds
#         INTP_GATE_OVERHEAD_PCT (5)  overhead gate threshold
# Exit:   0 all gates pass, 1 any gate FAILs, 77 prerequisites missing.

set -u

BIN="${1:-./build/src/cli/intp}"
OUT="${2:-$(mktemp -d /tmp/intp-gates.XXXXXX)}"
HERE="$(cd "$(dirname "$0")" && pwd)"
SECS="${INTP_GATE_SECONDS:-30}"
REPS="${INTP_GATE_REPS:-3}"
IV="${INTP_GATE_INTERVAL:-1}"
NPROC="$(nproc 2>/dev/null || echo 2)"

command -v stress-ng >/dev/null 2>&1 || {
	echo "SKIP: stress-ng not installed" >&2
	exit 77
}
[ -x "$BIN" ] || {
	echo "SKIP: intp binary not found/executable: $BIN" >&2
	exit 77
}
BIN="$(cd "$(dirname "$BIN")" && pwd)/$(basename "$BIN")"

# Workload catalog: the three representative solo classes the plan
# names (cpu-, mem-, disk-bound). The cpu workload name is load-bearing:
# analyze-faithfulness's run-validity guard reads solo/cpu.
wl_args() {
	case "$1" in
	cpu) echo "--cpu $NPROC" ;;
	mem) echo "--vm 2 --vm-bytes 1G" ;;
	disk) echo "--hdd 1 --hdd-bytes 1G" ;;
	esac
}
WORKLOADS="cpu mem disk"

cleanup() {
	pkill -f "stress-ng.*intp-gates" 2>/dev/null
	jobs -p | xargs -r kill 2>/dev/null
}
trap cleanup EXIT

echo "== run-gates: bin=$BIN out=$OUT window=${SECS}s reps=$REPS =="

# ---------------------------------------------------------------- solo
HEADER='netp	nets	blk	mbw	llcmr	llcocc	cpu'
for wl in $WORKLOADS; do
	for rep in $(seq 1 "$REPS"); do
		dir="$OUT/solo/$wl/rep$rep"
		mkdir -p "$dir"
		echo "-- solo $wl rep$rep"

		# shellcheck disable=SC2046
		stress-ng $(wl_args "$wl") --timeout "${SECS}s" \
			--temp-path /tmp >/dev/null 2>&1 &
		SPID=$!

		INTERVAL="$IV" DURATION="$SECS" \
			"$HERE/capture-groundtruth.sh" \
			>"$dir/groundtruth.tsv" 2>/dev/null &
		GPID=$!

		{
			printf '%s\n' "$HEADER"
			"$BIN" -f tsv-legacy-v1 -i "${IV}s" -d "${SECS}s" \
				2>"$dir/intp.stderr"
		} >"$dir/profiler.tsv"

		printf '{"duration_s":%s,"interval_s":%s,"workload":"%s"}\n' \
			"$SECS" "$IV" "$wl" >"$dir/run.json"

		wait "$SPID" 2>/dev/null
		kill "$GPID" 2>/dev/null
		wait "$GPID" 2>/dev/null
	done
done

# ------------------------------------------------------------ overhead
# Tree shape consumed by cross-variant-correlation.py overhead_bounds:
#   overhead/<env>/<variant>/<ref>/rep<R>/throughput.tsv
# with `_baseline` as the no-profiler arm. bogo-ops/s comes from
# stress-ng --metrics-brief (the origin's workload-visible axis).
bogo_run() { # args: ref_wl with_profiler(0/1) out_file
	local ref="$1" arm="$2" out="$3" ipid=
	if [ "$arm" = "1" ]; then
		"$BIN" -f tsv-legacy-v1 -i "${IV}s" \
			>/dev/null 2>&1 &
		ipid=$!
	fi
	# The stressor metrics row of --metrics-brief reads
	#   stress-ng: metrc: [pid] <name> <bogo> <real> <usr> <sys> \
	#     <bogo/s real> <bogo/s usr+sys>
	# so the real-time rate is $(NF-1). Fall back to 0 on a miss
	# (the analyzer treats 0/None as SKIP, never a crash).
	local bogo
	# shellcheck disable=SC2046
	bogo=$(stress-ng $(wl_args "$ref") --timeout "${SECS}s" \
		--temp-path /tmp --metrics-brief 2>&1 |
		awk '$2=="metrc:" && $4!~/^\(/ && NF>=10 {last=$(NF-1)} END{print last}')
	case "$bogo" in
	'' | *[!0-9.]*) bogo=0 ;;
	esac
	printf 'bogo_ops_per_s_real\t%s\n' "$bogo" >"$out"
	[ -n "$ipid" ] && kill "$ipid" 2>/dev/null && wait "$ipid" 2>/dev/null
	return 0
}

for ref in $WORKLOADS; do
	for rep in $(seq 1 "$REPS"); do
		bdir="$OUT/overhead/bare/_baseline/ref_$ref/rep$rep"
		adir="$OUT/overhead/bare/intp/ref_$ref/rep$rep"
		mkdir -p "$bdir" "$adir"
		echo "-- overhead ref_$ref rep$rep (baseline, then with profiler)"
		bogo_run "$ref" 0 "$bdir/throughput.tsv"
		bogo_run "$ref" 1 "$adir/throughput.tsv"
	done
done

# --------------------------------------------------------------- gates
echo
echo "== gate adjudication =="
FAIL=0

python3 "$HERE/analyze-faithfulness.py" "$OUT" \
	--out "$OUT/faithfulness.md" | tee "$OUT/gate-fidelity.txt" |
	grep '^GATE' || true
grep -q $'^GATE\t.*\tFAIL' "$OUT/gate-fidelity.txt" && FAIL=1

python3 "$HERE/cross-variant-correlation.py" --campaign "$OUT" \
	--out "$OUT" | tee "$OUT/gate-correlation.txt" |
	grep '^GATE' || true
grep -q $'^GATE\t.*\tFAIL' "$OUT/gate-correlation.txt" && FAIL=1

python3 "$HERE/extract-fragility.py" "$OUT" \
	--out "$OUT/fragility.md" | tee "$OUT/gate-fragility.txt" |
	grep '^GATE' || true
grep -q $'^GATE\t.*\tFAIL' "$OUT/gate-fragility.txt" && FAIL=1

echo
echo "== gate summary ($OUT) =="
grep -h '^GATE' "$OUT"/gate-*.txt | column -t -s "$(printf '\t')"

exit "$FAIL"
