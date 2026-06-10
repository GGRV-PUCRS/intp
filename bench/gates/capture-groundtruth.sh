#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# capture-groundtruth.sh -- per-interval ground-truth side-channel capture
# for the bench/gates fidelity adjudication.
#
# Mirrors the groundtruth capture method of intp-comparison
# bench/run-intp-bench.sh (start_groundtruth / merge_perf_into_groundtruth
# / stop_groundtruth @ 2e87b30fc762), packaged as a standalone synchronous
# script so run-gates.sh can background it next to the profiler.
#
# Output: OUTDIR/groundtruth.tsv with the origin's 12 columns:
#   ts cpu_busy_pct disk_read_mb disk_write_mb net_rx_mb net_tx_mb
#   instr cycles llc_ref llc_miss resctrl_mbw_bps resctrl_llcocc_bytes
#
#   - cpu_busy_pct       /proc/stat (100 * (1 - idle/total) per interval)
#   - disk_read/write_mb /proc/diskstats sector deltas (x512 bytes)
#   - net_rx/tx_mb       /proc/net/dev byte deltas over non-lo ifaces
#   - instr/cycles/llc_* interval-mode `perf stat -I -a` merged in at the
#                        end (written as '--' during capture)
#   - resctrl_*          direct read of /sys/fs/resctrl/intp-bench/mon_data
#                        when that mon group exists ('--' otherwise; CMT
#                        binds one RMID per task, so while a profiler holds
#                        the tenant RMID its own read IS the GT)
#
# The first data row carries cumulative-counter garbage by construction
# (prev=0); consumers (intp_metrics.parse_gt) drop it.
#
# CTXSW CAVEAT (preserved from the origin / paper section V-D): any
# context-switch ground truth MUST come from `vmstat` or /proc/stat `ctxt`
# -- NEVER from `perf stat -e context-switches`. perf's software event
# undercounts when BPF programs are co-attached to the sched tracepoints
# (documented BPF co-attach undercount), which is exactly the condition
# under test when intp's eBPF backends are active. This script therefore
# captures NO perf-based ctxsw column; see
# tests/regression/test-no-ctxsw-amplification.sh for the vmstat-based
# ctxsw gate.
#
# Deviation from the origin (bugfix, noted): the origin piped
# `grep ':' /proc/net/dev | tail -n +3`, which skipped the first two
# INTERFACE lines (the two /proc/net/dev header lines contain no ':');
# here the headers are dropped first (`tail -n +3 /proc/net/dev`).
#
# Usage: capture-groundtruth.sh OUTDIR DURATION [INTERVAL]

set -u

OUTDIR="${1:?usage: capture-groundtruth.sh OUTDIR DURATION [INTERVAL]}"
DURATION="${2:?usage: capture-groundtruth.sh OUTDIR DURATION [INTERVAL]}"
INTERVAL="${3:-1}"

GT="$OUTDIR/groundtruth.tsv"
mkdir -p "$OUTDIR"

PERF_PID=""
cleanup() { [ -n "$PERF_PID" ] && kill "$PERF_PID" 2>/dev/null; }
trap cleanup INT TERM

# Interval-mode, system-wide perf for the per-interval GT perf columns
# (instr/cycles/llc_ref/llc_miss). System-wide (-a) so it captures the
# tenant's forked workers; for SOLO runs the tenant is the only significant
# load, so this is its ground truth (like cpu_busy_pct, it is system-wide).
# '-x; -I <ms>' emits one CSV line per (interval, event); merged into
# groundtruth.tsv afterward. HW cache counters only -- NO context-switches
# event here (see the ctxsw caveat above).
if command -v perf >/dev/null 2>&1; then
    iv_ms=$(awk -v s="$INTERVAL" 'BEGIN{v=s*1000; printf "%d", (v<100?100:v)}')
    perf stat -x';' -I "$iv_ms" -a \
        -e instructions,cycles,cache-references,cache-misses \
        -- sleep "$DURATION" > "$OUTDIR/perf-stat.txt" 2>&1 &
    PERF_PID=$!
fi

{
    printf 'ts\tcpu_busy_pct\tdisk_read_mb\tdisk_write_mb\tnet_rx_mb\tnet_tx_mb\tinstr\tcycles\tllc_ref\tllc_miss\tresctrl_mbw_bps\tresctrl_llcocc_bytes\n'
    prev_d_r=0; prev_d_w=0; prev_n_r=0; prev_n_t=0; prev_cpu_idle=0; prev_cpu_total=0
    end=$(($(date +%s) + DURATION))
    while [ "$(date +%s)" -lt "$end" ]; do
        ts=$(date +%s.%N)

        # /proc/stat -- cpu busy fraction
        read -r _ user nice system idle iowait irq softirq steal _ < /proc/stat
        total=$((user+nice+system+idle+iowait+irq+softirq+steal))
        idle_now=$((idle+iowait))
        d_total=$((total-prev_cpu_total))
        d_idle=$((idle_now-prev_cpu_idle))
        cpu_busy="--"
        if [ "$d_total" -gt 0 ]; then
            cpu_busy=$(awk -v t=$d_total -v i=$d_idle 'BEGIN{printf "%.2f",100*(t-i)/t}')
        fi
        prev_cpu_total=$total; prev_cpu_idle=$idle_now

        # /proc/diskstats -- delta MB read/written across non-loop devices
        d_r=0; d_w=0
        while read -r _ _ name _ _ r_sec _ _ _ w_sec _; do
            case "$name" in loop*|ram*|sr*|fd*) continue ;; esac
            d_r=$((d_r + r_sec)); d_w=$((d_w + w_sec))
        done < <(awk '{print $1,$2,$3,$4,$5,$6,$7,$8,$9,$10,$11}' /proc/diskstats)
        mb_r=$(awk -v c=$d_r -v p=$prev_d_r 'BEGIN{printf "%.2f",(c-p)*512/1048576}')
        mb_w=$(awk -v c=$d_w -v p=$prev_d_w 'BEGIN{printf "%.2f",(c-p)*512/1048576}')
        prev_d_r=$d_r; prev_d_w=$d_w

        # /proc/net/dev -- rx/tx bytes over all non-lo ifaces
        n_r=0; n_t=0
        while read -r line; do
            iface=$(echo "$line" | awk -F: '{print $1}' | xargs)
            [ "$iface" = "lo" ] && continue
            bytes_r=$(echo "$line" | awk -F: '{print $2}' | awk '{print $1}')
            bytes_t=$(echo "$line" | awk -F: '{print $2}' | awk '{print $9}')
            n_r=$((n_r + bytes_r)); n_t=$((n_t + bytes_t))
        done < <(tail -n +3 /proc/net/dev | grep ':')
        mb_n_r=$(awk -v c=$n_r -v p=$prev_n_r 'BEGIN{printf "%.2f",(c-p)/1048576}')
        mb_n_t=$(awk -v c=$n_t -v p=$prev_n_t 'BEGIN{printf "%.2f",(c-p)/1048576}')
        prev_n_r=$n_r; prev_n_t=$n_t

        # resctrl direct counters (the ground truth for mbw and llcocc)
        mbw_bps="--"; llcocc_bytes="--"
        if [ -d /sys/fs/resctrl/intp-bench/mon_data ]; then
            # Sum across L3 monitoring domains
            mbw_bps=$(cat /sys/fs/resctrl/intp-bench/mon_data/mon_L3_*/mbm_total_bytes 2>/dev/null | awk '{s+=$1}END{printf "%d",s}')
            llcocc_bytes=$(cat /sys/fs/resctrl/intp-bench/mon_data/mon_L3_*/llc_occupancy 2>/dev/null | awk '{s+=$1}END{printf "%d",s}')
        fi

        # perf counters: filled in at end via perf stat -- empty here
        printf '%s\t%s\t%s\t%s\t%s\t%s\t--\t--\t--\t--\t%s\t%s\n' \
            "$ts" "$cpu_busy" "$mb_r" "$mb_w" "$mb_n_r" "$mb_n_t" "$mbw_bps" "$llcocc_bytes"
        sleep "$INTERVAL"
    done
} > "$GT" 2>/dev/null

[ -n "$PERF_PID" ] && wait "$PERF_PID" 2>/dev/null

# Merge interval-mode perf counts (perf-stat.txt) into the groundtruth.tsv
# perf columns (instr/cycles/llc_ref/llc_miss), which the loop above wrote
# as '--' (perf runs as a separate process, so its counters must be folded
# in here). perf '-x; -I' emits one CSV line per (interval,event):
# time;value;unit;event;... Group by interval (time-ordered) and fill GT
# data rows by index; this makes the llcmr ground truth (llc_miss/llc_ref)
# adjudicable. resctrl_mbw_bps/resctrl_llcocc_bytes stay as-is: resctrl CMT
# is exclusive per-task, so while the profiler-under-test holds the
# tenant's RMID an independent occupancy read is not possible -- the
# profiler's own direct resctrl read is the occupancy GT.
merge_perf_into_groundtruth() {
    local gt="$GT" pf="$OUTDIR/perf-stat.txt"
    [ -f "$gt" ] && [ -f "$pf" ] && command -v python3 >/dev/null 2>&1 || return 0
    python3 - "$gt" "$pf" <<'PY' 2>/dev/null || true
import sys, collections
gt, pf = sys.argv[1], sys.argv[2]
ev = {"instructions": 0, "cycles": 1, "cache-references": 2, "cache-misses": 3}
per = collections.OrderedDict()           # interval-time -> [instr,cycles,ref,miss]
for line in open(pf):
    s = line.strip()
    if not s or s[0] == '#':
        continue
    f = s.split(';')
    if len(f) < 4:
        continue
    name = f[3].strip()
    if name not in ev:
        continue
    try:
        val = float(f[1])
    except ValueError:
        continue
    per.setdefault(f[0].strip(), [None, None, None, None])[ev[name]] = val
rows = list(per.values())
lines = open(gt).read().splitlines()
if not lines:
    sys.exit(0)
out, di = [lines[0]], 0
def fmt(x): return ("%d" % x) if x is not None else "--"
for ln in lines[1:]:
    c = ln.split('\t')
    if len(c) >= 12 and di < len(rows):
        r = rows[di]
        c[6], c[7], c[8], c[9] = fmt(r[0]), fmt(r[1]), fmt(r[2]), fmt(r[3])
        ln = '\t'.join(c); di += 1
    out.append(ln)
open(gt, 'w').write('\n'.join(out) + '\n')
PY
}
merge_perf_into_groundtruth
exit 0
