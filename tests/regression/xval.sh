#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# xval.sh -- cross-/self-variant parity check for intp.
#
# Ported from across-intp:
#   shared/validate-cross-variant.sh @ 2e87b30fc762
# reshaped for intp: instead of comparing six research variants
# sequentially, it runs all arms CONCURRENTLY under one identical
# stress-ng window and compares the 7 canonical tsv-legacy-v1 columns
# (netp nets blk mbw llcmr llcocc cpu) within a per-column tolerance.
#
# Modes:
#   --self <bin>    run <bin> twice side-by-side: default (eBPF-preferred)
#                   backend selection vs all-eBPF-disabled
#                   (--disable-backend ebpf_bytes/ebpf_softirq/ebpf_svctm/
#                   ebpf_sched/ebpf_perf).
#   --cross <bin>   --self arms PLUS the research v2.1 binary
#                   (make -C $INTP_XVAL_ACROSS_DIR/variants/v2.1-c-abi-cgroup;
#                   intp-c-abi-cgroup --output tsv --no-header) and, when
#                   root + BTF, the v3.3 binary (variants/
#                   v3.3-ebpf-core-cgroup; intp-ebpf-core-cgroup
#                   --no-diag-cols). Used by run-remote-gate.sh step 5.
#
# All arms get IDENTICAL pinned normalizer overrides (the variant CLIs
# use the same long flag names), so detect-path divergence cannot leak
# into the comparison.
#
# Baked-in comparison rules:
#   (a) a column that is all-'--' on either side of a pair is SKIPped;
#   (b) blk is REPORT-ONLY, never a magnitude failure: intp's canonical
#       blk is the svctm busy fraction while the procfs fallback and the
#       v2.1 binary use io_ticks -- different declared semantic models;
#   (c) with INTP_XVAL_CGROUP set, nets is EXEMPT entirely: per-cgroup
#       nets is a byte-share proxy that diverges by design (upstream
#       decision C2, DECISIONS-container.md).
#
# Environment:
#   INTP_XVAL_SECONDS     measurement window seconds        (default 30)
#   INTP_XVAL_WARMUP      workload warmup before window     (default 5)
#   INTP_XVAL_TOL         per-column tolerance, %-points    (default 15)
#   INTP_XVAL_REPORT      write the Markdown report here    (default stdout)
#   INTP_XVAL_CGROUP      cgroup-v2 path: profile it on all arms; the
#                         stress workload is moved into it when possible
#   INTP_XVAL_ACROSS_DIR  across-intp checkout (--cross only)
#   INTP_XVAL_NIC_BPS     pinned NIC speed       (default 125000000, 1GbE)
#   INTP_XVAL_MEMBW_BPS   pinned DRAM bandwidth  (default 25600000000)
#   INTP_XVAL_LLC_BYTES   pinned LLC size        (default 33554432)
#   INTP_XVAL_DISK_BPS    pinned disk bandwidth  (default 500000000;
#                         intp-only flag, affects cgroup blk only)
#
# Exit codes: 0 parity holds, 1 divergent, 77 SKIP (missing prerequisite).

set -euo pipefail

usage() {
    echo "usage: xval.sh --self|--cross <intp-binary>" >&2
    exit 2
}

[ $# -eq 2 ] || usage
case "$1" in
    --self)  MODE=self ;;
    --cross) MODE=cross ;;
    *) usage ;;
esac
BIN="$2"

DUR="${INTP_XVAL_SECONDS:-30}"
WARMUP="${INTP_XVAL_WARMUP:-5}"
TOL="${INTP_XVAL_TOL:-15}"
REPORT="${INTP_XVAL_REPORT:-}"
CGROUP="${INTP_XVAL_CGROUP:-}"
ACROSS="${INTP_XVAL_ACROSS_DIR:-}"
NIC_BPS="${INTP_XVAL_NIC_BPS:-125000000}"
MEMBW_BPS="${INTP_XVAL_MEMBW_BPS:-25600000000}"
LLC_BYTES="${INTP_XVAL_LLC_BYTES:-33554432}"
DISK_BPS="${INTP_XVAL_DISK_BPS:-500000000}"

METRICS=(netp nets blk mbw llcmr llcocc cpu)
EBPF_BACKENDS=(ebpf_bytes ebpf_softirq ebpf_svctm ebpf_sched ebpf_perf)

log()  { echo "[xval] $*" >&2; }
warn() { echo "[xval] WARNING: $*" >&2; }
die()  { echo "[xval] FATAL: $*" >&2; exit 1; }
skip() { echo "[xval] SKIP: $*" >&2; exit 77; }

[ -x "$BIN" ] || die "intp binary not executable: $BIN (meson compile first)"
command -v stress-ng >/dev/null 2>&1 || skip "stress-ng not installed (apt install stress-ng)"

OUTDIR=$(mktemp -d /tmp/intp-xval-XXXXXX)
WORKLOAD_PID=""

cleanup() {
    # Arms run in the foreground, so an interrupted arm dies with the
    # script's process group; only the shared workload needs reaping.
    # shellcheck disable=SC2317  # body reached via `trap cleanup` below
    if [ -n "$WORKLOAD_PID" ]; then
        kill "$WORKLOAD_PID" 2>/dev/null || true
        wait "$WORKLOAD_PID" 2>/dev/null || true
    fi
}
trap cleanup EXIT INT TERM

# -- Arm discovery -------------------------------------------------------------
# ARMS holds arm names; per-arm command construction lives in run_arm.
ARMS=(intp-default intp-noebpf)
V21_BIN=""
V33_BIN=""

if [ "$MODE" = cross ]; then
    [ -n "$ACROSS" ] || skip "INTP_XVAL_ACROSS_DIR not set (across-intp checkout required for --cross)"
    [ -d "$ACROSS" ] || skip "across-intp checkout missing: $ACROSS"

    log "building v2.1 (make -C $ACROSS/variants/v2.1-c-abi-cgroup)"
    if make -C "$ACROSS/variants/v2.1-c-abi-cgroup" >"$OUTDIR/v2.1-build.log" 2>&1; then
        V21_BIN="$ACROSS/variants/v2.1-c-abi-cgroup/intp-c-abi-cgroup"
        [ -x "$V21_BIN" ] || { warn "v2.1 build produced no binary"; V21_BIN=""; }
    else
        warn "v2.1 build failed (see $OUTDIR/v2.1-build.log) -- skipping the v2.1 piece"
    fi
    [ -n "$V21_BIN" ] && ARMS+=(v2.1)

    # v3.3 needs root + BTF to load its CO-RE programs.
    if [ "$(id -u)" -eq 0 ] && [ -f /sys/kernel/btf/vmlinux ]; then
        log "building v3.3 (make -C $ACROSS/variants/v3.3-ebpf-core-cgroup)"
        if make -C "$ACROSS/variants/v3.3-ebpf-core-cgroup" >"$OUTDIR/v3.3-build.log" 2>&1; then
            V33_BIN="$ACROSS/variants/v3.3-ebpf-core-cgroup/intp-ebpf-core-cgroup"
            [ -x "$V33_BIN" ] || { warn "v3.3 build produced no binary"; V33_BIN=""; }
        else
            warn "v3.3 build failed (see $OUTDIR/v3.3-build.log) -- skipping the v3.3 piece"
        fi
    else
        warn "not root or no BTF -- skipping the v3.3 piece"
    fi
    [ -n "$V33_BIN" ] && ARMS+=(v3.3)

    if [ -z "$V21_BIN" ] && [ -z "$V33_BIN" ]; then
        skip "--cross: no research variant available (v2.1 and v3.3 both unavailable)"
    fi
fi

# -- Arm launch ----------------------------------------------------------------
run_arm() {
    # run_arm <name> <outfile>; runs the arm in the FOREGROUND for one
    # measurement window. Arms must NOT overlap (see the sequential loop
    # below) -- concurrent per-cgroup resctrl monitors contend for the
    # cgroup's RMID.
    local name="$1" outfile="$2"
    local args=()
    case "$name" in
    intp-default|intp-noebpf)
        args=(-f tsv-legacy-v1 -i 1s -d "${DUR}s"
              --nic-speed-bps "$NIC_BPS" --mem-bw-max-bps "$MEMBW_BPS"
              --llc-size-bytes "$LLC_BYTES" --disk-bw-max-bps "$DISK_BPS")
        [ -n "$CGROUP" ] && args+=(-c "$CGROUP")
        if [ "$name" = intp-noebpf ]; then
            local b
            for b in "${EBPF_BACKENDS[@]}"; do args+=(--disable-backend "$b"); done
        fi
        timeout "$((DUR + 10))" "$BIN" "${args[@]}" \
            > "$outfile" 2>"${outfile}.err" || true
        ;;
    v2.1|v3.3)
        # The research CLIs share the long normalizer flag names. v3.3 needs
        # --no-diag-cols at EVERY capture site (upstream C13) so the TSV is
        # exactly the 7 canonical columns; do not drop that flag.
        local vbin="$V21_BIN"
        [ "$name" = v3.3 ] && vbin="$V33_BIN"
        args=(--output tsv --no-header --interval 1 --duration "$DUR"
              --nic-speed-bps "$NIC_BPS" --mem-bw-max-bps "$MEMBW_BPS"
              --llc-size-bytes "$LLC_BYTES")
        [ "$name" = v3.3 ] && args+=(--no-diag-cols)
        [ -n "$CGROUP" ] && args+=(--cgroup "$CGROUP")
        timeout "$((DUR + 10))" "$vbin" "${args[@]}" \
            > "$outfile" 2>"${outfile}.err" || true
        ;;
    esac
    log "  arm $name captured -> $outfile"
}

# -- TSV column statistics -------------------------------------------------------
count_rows() { grep -cE '^[0-9-]' "$1" 2>/dev/null || true; }

column_stats() {
    # Echo "mean count" x7 (space-separated; mean = 'na' when the column is
    # all-'--'). Rows may start with a digit or with '-' ('--' cell first).
    awk -F'\t' '
    /^[0-9-]/ {
        for (i = 1; i <= 7; i++) {
            if ($i != "--" && $i != "") { s[i] += $i; n[i]++ }
        }
    }
    END {
        for (i = 1; i <= 7; i++) {
            if (n[i] > 0) printf "%.2f %d", s[i] / n[i], n[i]
            else          printf "na 0"
            printf (i < 7 ? " " : "\n")
        }
    }' "$1"
}

# -- Pair comparison -------------------------------------------------------------
# compare_pair writes one Markdown section to $MD and returns 1 on divergence.
MD="$OUTDIR/report.md"

compare_pair() {
    local name_a="$1" file_a="$2" name_b="$3" file_b="$4"
    local stats_a stats_b
    stats_a=$(column_stats "$file_a")
    stats_b=$(column_stats "$file_b")
    read -ra A <<< "$stats_a"   # A[2i]=mean, A[2i+1]=count
    read -ra B <<< "$stats_b"

    local fail=0 checked=0 i metric ma ca mb cb delta status
    {
        echo "### $name_a vs $name_b"
        echo ""
        echo "| metric | $name_a mean | $name_b mean | delta (pp) | verdict |"
        echo "|---|---|---|---|---|"
    } >> "$MD"

    for i in "${!METRICS[@]}"; do
        metric="${METRICS[$i]}"
        ma="${A[$((2 * i))]}";  ca="${A[$((2 * i + 1))]}"
        mb="${B[$((2 * i))]}";  cb="${B[$((2 * i + 1))]}"
        delta="-"
        if [ "$ca" -eq 0 ] || [ "$cb" -eq 0 ]; then
            # Rule (a): unavailable on either side -> not comparable here.
            status="SKIP (all '--' on $( [ "$ca" -eq 0 ] && echo "$name_a" || echo "$name_b"))"
        else
            delta=$(awk -v a="$ma" -v b="$mb" 'BEGIN { d = a - b; printf "%.2f", (d < 0 ? -d : d) }')
            if [ "$metric" = blk ]; then
                # Rule (b): svctm vs io_ticks semantic models -- report only.
                status="REPORT-ONLY (blk model: svctm vs io_ticks)"
            elif [ "$metric" = netp ] && [ -n "$CGROUP" ]; then
                # Rule (d): per-cgroup netp via cgroup_skb counts loopback
                # traffic (documented cgroup_skb=100 artifact), while the
                # pure-C arms report system-wide NIC netp -- different
                # quantities under cgroup targeting, so report only.
                status="REPORT-ONLY (per-cgroup netp cgroup_skb loopback)"
            elif [ "$metric" = nets ] && [ -n "$CGROUP" ]; then
                # Rule (c): per-cgroup nets byte-share proxy (upstream C2).
                status="EXEMPT (per-cgroup nets byte-share proxy, C2)"
            elif awk -v d="$delta" -v t="$TOL" 'BEGIN { exit !(d <= t) }'; then
                status="OK"
                checked=$((checked + 1))
            else
                status="**DIVERGENT**"
                fail=$((fail + 1))
                checked=$((checked + 1))
            fi
        fi
        echo "| $metric | $ma | $mb | $delta | $status |" >> "$MD"
    done

    {
        echo ""
        if [ "$fail" -gt 0 ]; then
            echo "Verdict: **FAIL** -- $fail of $checked compared metric(s) exceed ${TOL}pp."
        else
            echo "Verdict: PASS -- all $checked compared metric(s) within ${TOL}pp."
        fi
        echo ""
    } >> "$MD"

    [ "$fail" -eq 0 ]
}

# -- Workload --------------------------------------------------------------------
# One mixed stress-ng load (origin mix) shared by every arm; with a cgroup
# target the launcher shell moves ITSELF into the cgroup before exec'ing
# stress-ng, so every forked worker inherits the target cgroup.
# Arms run sequentially, so the one shared workload must outlast warmup
# plus one window PER arm (plus slack for build/launch jitter).
STRESS_ARGS=(--cpu 2 --vm 1 --vm-bytes 64M --hdd 1 --sock 1
             --timeout "$((WARMUP + ${#ARMS[@]} * (DUR + 12) + 20))s" --quiet)

log "mode=$MODE arms=[${ARMS[*]}] window=${DUR}s warmup=${WARMUP}s tol=${TOL}pp outdir=$OUTDIR"

if [ -n "$CGROUP" ] && [ -w "$CGROUP/cgroup.procs" ]; then
    bash -c "echo \$\$ > '$CGROUP/cgroup.procs' && exec stress-ng $(printf '%q ' "${STRESS_ARGS[@]}")" &
    WORKLOAD_PID=$!
    log "started stress-ng inside cgroup $CGROUP (pid $WORKLOAD_PID)"
elif [ -n "$CGROUP" ]; then
    warn "$CGROUP/cgroup.procs not writable -- workload runs OUTSIDE the profiled cgroup (caller must place load there)"
    stress-ng "${STRESS_ARGS[@]}" &
    WORKLOAD_PID=$!
else
    stress-ng "${STRESS_ARGS[@]}" &
    WORKLOAD_PID=$!
    log "started stress-ng workload (pid $WORKLOAD_PID)"
fi

log "warmup ${WARMUP}s before the measurement window"
sleep "$WARMUP"

# SEQUENTIAL by design: per-cgroup resctrl monitoring reassigns the
# cgroup's tasks to the profiler's mon_group RMID, and a task can be in
# only one RMID at a time -- two profilers on the same cgroup contend
# and the loser reads ~0 (verified on the RDT testbed: v2.1 llcocc
# collapsed to 0 while intp ran concurrently; both read ~99 alone). So
# each arm gets the cgroup's RMID exclusively for its own window. The
# shared workload runs throughout, so every arm sees the same
# steady-state interference.
for arm in "${ARMS[@]}"; do
    log "measuring arm $arm (${DUR}s window, exclusive RMID)"
    run_arm "$arm" "$OUTDIR/$arm.tsv"
done

# -- Validate captures -----------------------------------------------------------
declare -a LIVE_ARMS=()
HARD_FAIL=0
for arm in "${ARMS[@]}"; do
    rows=$(count_rows "$OUTDIR/$arm.tsv")
    log "arm $arm: $rows data rows"
    if [ "${rows:-0}" -gt 0 ]; then
        LIVE_ARMS+=("$arm")
    else
        warn "arm $arm produced no data rows; stderr follows"
        sed 's/^/[xval]   /' "$OUTDIR/$arm.tsv.err" >&2 || true
        HARD_FAIL=1
    fi
done

# -- Report ------------------------------------------------------------------------
{
    echo "# intp cross-variant parity report"
    echo ""
    echo "Date: $(date -Iseconds)"
    echo ""
    echo "| parameter | value |"
    echo "|---|---|"
    echo "| mode | $MODE |"
    echo "| binary | $BIN |"
    echo "| window | ${DUR}s (warmup ${WARMUP}s) |"
    echo "| tolerance | ${TOL} %-points |"
    echo "| arms | ${ARMS[*]} |"
    echo "| cgroup target | ${CGROUP:-system-wide} |"
    echo "| nic-speed-bps | $NIC_BPS (pinned, all arms) |"
    echo "| mem-bw-max-bps | $MEMBW_BPS (pinned, all arms) |"
    echo "| llc-size-bytes | $LLC_BYTES (pinned, all arms) |"
    echo "| disk-bw-max-bps | $DISK_BPS (pinned, intp arms) |"
    echo "| captures | $OUTDIR |"
    echo ""
    echo "## Per-arm capture"
    echo ""
    for arm in "${ARMS[@]}"; do
        echo "- \`$arm\`: $(count_rows "$OUTDIR/$arm.tsv") rows, means \`$(column_stats "$OUTDIR/$arm.tsv")\` (mean count, x7: ${METRICS[*]})"
    done
    echo ""
    echo "## Pair comparisons"
    echo ""
} > "$MD"

# Pair set: the self pair, then intp-default vs each research variant.
declare -a PAIRS=()
have_arm() { local a; for a in "${LIVE_ARMS[@]:-}"; do [ "$a" = "$1" ] && return 0; done; return 1; }
if have_arm intp-default && have_arm intp-noebpf; then
    PAIRS+=("intp-default intp-noebpf")
fi
for v in v2.1 v3.3; do
    if have_arm intp-default && have_arm "$v"; then
        PAIRS+=("intp-default $v")
    fi
done

if [ "${#PAIRS[@]}" -eq 0 ]; then
    warn "no comparable arm pair survived capture"
    HARD_FAIL=1
fi

TOTAL_FAIL=0
for pair in "${PAIRS[@]:-}"; do
    [ -n "$pair" ] || continue
    read -r a b <<< "$pair"
    if compare_pair "$a" "$OUTDIR/$a.tsv" "$b" "$OUTDIR/$b.tsv"; then
        log "pair $a vs $b: PASS"
    else
        log "pair $a vs $b: FAIL"
        TOTAL_FAIL=$((TOTAL_FAIL + 1))
    fi
done

{
    echo "## Result"
    echo ""
    if [ "$HARD_FAIL" -ne 0 ]; then
        echo "**FAIL** -- at least one arm produced no data (see capture dir)."
    elif [ "$TOTAL_FAIL" -eq 0 ]; then
        echo "**PASS** -- all ${#PAIRS[@]} pair(s) agree within ${TOL}pp"
        echo "(blk report-only; nets exempt under cgroup targeting)."
    else
        echo "**FAIL** -- $TOTAL_FAIL of ${#PAIRS[@]} pair(s) exceed the ${TOL}pp tolerance."
    fi
} >> "$MD"

if [ -n "$REPORT" ]; then
    cp "$MD" "$REPORT"
    log "report written to $REPORT"
else
    cat "$MD"
fi

if [ "$HARD_FAIL" -ne 0 ] || [ "$TOTAL_FAIL" -gt 0 ]; then
    exit 1
fi
exit 0
