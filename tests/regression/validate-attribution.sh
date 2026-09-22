#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# validate-attribution.sh -- ground-truth validation of intp's per-cgroup
# attribution (-c / --cgroup).
#
# Ported from across-intp:
#   bench/validate-attribution.sh @ 2e87b30fc762
# adapted to the intp CLI (tsv-legacy-v1 parsing instead of the v2.1 JSON
# output) and REDUCED to plain cgroup-v2 legs: the LXD/Incus container
# legs and the iperf3 network sender are dropped. What remains is the
# load-bearing attribution experiment:
#
#   two sibling cgroup scopes on the host -- heavy stress-ng in one, an
#   idle sleeper in the other -- profiled concurrently with a system-wide
#   run. For each separable metric (cpu blk llcmr mbw llcocc):
#     track+ratio   heavy reading clearly non-trivial AND heavy/idle >= 8
#     isolation     idle reading <= 5 (pct units)
#     conservation  |(heavy+idle) - system| / system <= 0.25
#
#   netp/nets cells are recorded SKIP[upstream]: per-cgroup net split is
#   validated upstream in across-intp (per-cgroup nets is a byte-share
#   proxy, decision C2; netp-per-netns is the container legs' job).
#
# Origin thresholds and assertion structure are kept verbatim.
#
# Usage:
#   sudo tests/regression/validate-attribution.sh <intp-binary>
#
# Env knobs (all optional, origin names):
#   VAL_DURATION      profiler window seconds      (default 30)
#   VAL_INTERVAL      sample interval seconds      (default 1)
#   VAL_WARMUP        leading samples dropped      (default 5)
#   VAL_RATIO_MIN     min heavy/idle ratio         (default 8)
#   VAL_IDLE_MAX      max idle reading (pct units) (default 5)
#   VAL_CONSERVE_TOL  |sum-total|/total tolerance  (default 0.25)
#
# Exit codes: 0 PASS, 1 FAIL, 77 SKIP (no root / no writable cgroup v2 /
# no stress-ng).

set -euo pipefail

# --- config ------------------------------------------------------------------
DURATION="${VAL_DURATION:-30}"
INTERVAL="${VAL_INTERVAL:-1}"
WARMUP="${VAL_WARMUP:-5}"
RATIO_MIN="${VAL_RATIO_MIN:-8}"
IDLE_MAX="${VAL_IDLE_MAX:-5}"
CONSERVE_TOL="${VAL_CONSERVE_TOL:-0.25}"

# Metric groups (tsv-legacy-v1 column order: netp nets blk mbw llcmr llcocc cpu).
SEPARABLE_METRICS=(cpu blk llcmr mbw llcocc)   # per-cgroup attributable in intp
NET_METRICS=(netp nets)                        # per-cgroup split validated upstream (C2)

# Heavy stress-ng config exercises all five separable metrics at once:
#   --cpu (cpu) --vm (mbw) --cache (llcmr/llcocc) --hdd (blk).
# --vm 4 (not 1): one memory worker stays largely cache-resident and
# leaves mbw below the idle-floor; four workers drive enough sustained
# DRAM traffic for mbw to clear it and be clearly attributable.
HEAVY_STRESS="--cpu 2 --vm 4 --vm-bytes 256M --cache 2 --hdd 1 --hdd-bytes 128M"

INTP_BIN=""
WORKDIR=""
RESULTS=()        # "leg|metric|check|detail|RESULT"
N_FAIL=0
declare -a CLEANUP_PIDS=()
declare -a CLEANUP_CGROUPS=()
declare -a CLEANUP_SCOPES=()
USE_SYSTEMD=0

log()  { printf '[validate] %s\n' "$*" >&2; }
warn() { printf '[validate][WARN] %s\n' "$*" >&2; }
die()  { printf '[validate][FATAL] %s\n' "$*" >&2; exit 1; }
skip() { printf '[validate][SKIP] %s\n' "$*" >&2; exit 77; }

# =============================================================================
# Profiler + parsing helpers
# =============================================================================

# profile_cgroup OUTFILE [CGROUP_PATH]
# Runs intp over the window in tsv-legacy-v1 mode. No CGROUP_PATH ->
# system-wide. Blocking; intended to be backgrounded by the caller so the
# heavy/idle/system units run concurrently.
profile_cgroup() {
    local outfile="$1" cgpath="${2:-}"
    local args=( -f tsv-legacy-v1 -i "${INTERVAL}s" -d "${DURATION}s" )
    [ -n "$cgpath" ] && args+=( -c "$cgpath" )
    # mbw: force the resctrl mbm backend on EVERY leg (cgroup AND
    # system-wide). The default system-wide mbw is the uncore IMC PMU,
    # whose absolute scale differs from resctrl's mbm_total_bytes, so
    # mixing them makes heavy(resctrl)+idle(resctrl) incomparable to
    # system(IMC) and the conservation check spuriously fails. resctrl's
    # root-group totals already include its sub-mon_groups (llcocc
    # conservation relies on the same), so forcing resctrl puts all
    # three legs on one scale.
    args+=( --force-backend mbw:resctrl_mbm )
    timeout "$((DURATION + 10))" "$INTP_BIN" "${args[@]}" \
        > "$outfile" 2>"${outfile}.log" || true
}

# Column index of a metric in the tsv-legacy-v1 row.
metric_col() {
    case "$1" in
        netp) echo 1 ;; nets) echo 2 ;; blk) echo 3 ;; mbw) echo 4 ;;
        llcmr) echo 5 ;; llcocc) echo 6 ;; cpu) echo 7 ;;
        *) echo 0 ;;
    esac
}

# mean_metric OUTFILE METRIC -> steady-state mean of the metric column
# ('--' cells dropped, first $WARMUP samples skipped). Echoes "na" if no
# numeric samples. (Origin parsed the v2.1 JSON .<metric>.v with jq; intp's
# tsv-legacy-v1 needs only awk.)
mean_metric() {
    local outfile="$1" metric="$2" col
    col=$(metric_col "$metric")
    [ -s "$outfile" ] || { echo "na"; return 0; }
    awk -F'\t' -v col="$col" -v w="$WARMUP" '
        /^[0-9-]/ {
            r++
            if (r <= w) next
            if ($col == "--" || $col == "") next
            s += $col; n++
        }
        END { if (n > 0) printf "%g\n", s / n; else print "na" }
    ' "$outfile" 2>/dev/null || echo "na"
}

# --- float predicates (awk; bash has no float compare) -----------------------
fcmp() { awk -v a="$1" -v op="$2" -v b="$3" 'BEGIN{
    if(op==">")  exit !(a>b);  if(op==">=") exit !(a>=b);
    if(op=="<")  exit !(a<b);  if(op=="<=") exit !(a<=b);
    exit 1 }'; }
is_num() { case "$1" in ''|na|null) return 1 ;; *) awk -v x="$1" 'BEGIN{exit !(x==x+0)}' ;; esac; }

# =============================================================================
# Assertions -- each records one row into RESULTS[] and bumps N_FAIL on FAIL.
# (Ported verbatim from the origin.)
# =============================================================================
record() {  # LEG METRIC CHECK DETAIL RESULT
    RESULTS+=("$1|$2|$3|$4|$5")
    [ "$5" = "FAIL" ] && N_FAIL=$((N_FAIL + 1)) || true
}

# heavy reading must be clearly non-trivial AND >> idle.
assert_track_and_ratio() {  # LEG METRIC HEAVY IDLE
    local leg="$1" m="$2" heavy="$3" idle="$4"
    if ! is_num "$heavy"; then
        record "$leg" "$m" "track" "heavy=$heavy (unavailable backend)" "SKIP"; return
    fi
    if ! fcmp "$heavy" ">" "$IDLE_MAX"; then
        record "$leg" "$m" "track" "heavy=$heavy not above idle-floor $IDLE_MAX" "FAIL"; return
    fi
    if is_num "$idle" && fcmp "$idle" ">" 0; then
        local ratio; ratio=$(awk -v h="$heavy" -v i="$idle" 'BEGIN{printf "%.2f", h/i}')
        if fcmp "$ratio" ">=" "$RATIO_MIN"; then
            record "$leg" "$m" "track+ratio" "heavy=$heavy idle=$idle ratio=$ratio (>= $RATIO_MIN)" "PASS"
        else
            record "$leg" "$m" "track+ratio" "heavy=$heavy idle=$idle ratio=$ratio (< $RATIO_MIN)" "FAIL"
        fi
    else
        record "$leg" "$m" "track" "heavy=$heavy idle=${idle} (~0)" "PASS"
    fi
}

# idle reading must sit under the floor.
assert_idle() {  # LEG METRIC IDLE
    local leg="$1" m="$2" idle="$3"
    if ! is_num "$idle"; then
        record "$leg" "$m" "isolation" "idle=$idle (unavailable)" "SKIP"; return
    fi
    if fcmp "$idle" "<=" "$IDLE_MAX"; then
        record "$leg" "$m" "isolation" "idle=$idle (<= $IDLE_MAX)" "PASS"
    else
        record "$leg" "$m" "isolation" "idle=$idle (> $IDLE_MAX, leakage)" "FAIL"
    fi
}

# sum of units must reconcile with the whole.
assert_conserve() {  # LEG METRIC TOTAL UNIT...
    local leg="$1" m="$2" total="$3"; shift 3
    if ! is_num "$total"; then
        record "$leg" "$m" "conservation" "total=$total (unavailable)" "SKIP"; return
    fi
    local sum=0 u
    for u in "$@"; do is_num "$u" && sum=$(awk -v s="$sum" -v x="$u" 'BEGIN{printf "%.3f", s+x}'); done
    if ! fcmp "$total" ">" 0; then
        record "$leg" "$m" "conservation" "sum=$sum total=$total (both ~0)" "SKIP"; return
    fi
    local rel; rel=$(awk -v s="$sum" -v t="$total" 'BEGIN{printf "%.3f", (s>t?s-t:t-s)/t}')
    if fcmp "$rel" "<=" "$CONSERVE_TOL"; then
        record "$leg" "$m" "conservation" "sum=$sum total=$total relerr=$rel (<= $CONSERVE_TOL)" "PASS"
    else
        record "$leg" "$m" "conservation" "sum=$sum total=$total relerr=$rel (> $CONSERVE_TOL)" "FAIL"
    fi
}

# =============================================================================
# Cgroup plumbing -- plain cgroup-v2 (no LXD)
# =============================================================================

# Transient-scope workload placement when systemd is PID 1 (origin
# sdrun_host): systemd owns delegation, yielding a LEAF cgroup whose
# cgroup.procs lists the workload pids. Fallback: a raw child of the
# cgroup-v2 root, with the launcher shell writing ITSELF into cgroup.procs
# before exec'ing the workload so every fork inherits the cgroup.
launch_unit() {  # UNIT CMD...
    local unit="$1"; shift
    if [ "$USE_SYSTEMD" -eq 1 ]; then
        systemd-run --scope --slice=intp-val.slice --unit="$unit" --collect --quiet "$@" \
            >"$WORKDIR/$unit.log" 2>&1 &
        CLEANUP_PIDS+=("$!")
        CLEANUP_SCOPES+=("$unit.scope")
    else
        local cg="/sys/fs/cgroup/$unit"
        mkdir -p "$cg" || return 1
        CLEANUP_CGROUPS+=("$cg")
        bash -c "echo \$\$ > '$cg/cgroup.procs' && exec $(printf '%q ' "$@")" \
            >"$WORKDIR/$unit.log" 2>&1 &
        CLEANUP_PIDS+=("$!")
    fi
}

# Locate a unit's cgroup on the host and wait until it is populated.
# systemd path: the unit name is unique per run, so a `find` by name is
# robust to the exact slice nesting. NB: cgroup.procs is a kernfs file --
# st_size is 0 even when it lists pids, so check content, not `test -s`.
await_unit_cg() {  # UNIT -> host cgroup path once populated
    local unit="$1" cg i
    for ((i = 0; i < 25; i++)); do
        if [ "$USE_SYSTEMD" -eq 1 ]; then
            # `|| true`: find keeps walking after head closes -> SIGPIPE under pipefail.
            cg=$(find /sys/fs/cgroup -type d -name "$unit.scope" 2>/dev/null | head -1) || true
        else
            cg="/sys/fs/cgroup/$unit"
        fi
        [ -n "$cg" ] && [ -n "$(head -n1 "$cg/cgroup.procs" 2>/dev/null)" ] && { printf '%s\n' "$cg"; return 0; }
        sleep 0.4
    done
    return 1
}

dump_unit_logs() {  # UNIT...
    local u f
    for u in "$@"; do
        f="$WORKDIR/$u.log"
        [ -s "$f" ] && { warn "---- $u.log ----"; sed 's/^/[validate]   /' "$f" >&2 || true; }
    done
    return 0
}

cleanup() {
    local p s cg
    for p in "${CLEANUP_PIDS[@]:-}"; do [ -n "$p" ] && kill "$p" 2>/dev/null || true; done
    for s in "${CLEANUP_SCOPES[@]:-}"; do [ -n "$s" ] && systemctl stop "$s" 2>/dev/null || true; done
    for cg in "${CLEANUP_CGROUPS[@]:-}"; do
        [ -n "$cg" ] && [ -d "$cg" ] || continue
        # kernel >= 5.14 has cgroup.kill; otherwise kill listed pids.
        if [ -f "$cg/cgroup.kill" ]; then
            echo 1 > "$cg/cgroup.kill" 2>/dev/null || true
        else
            local pid
            while read -r pid; do kill -9 "$pid" 2>/dev/null || true; done < "$cg/cgroup.procs" 2>/dev/null || true
        fi
        sleep 0.2
        rmdir "$cg" 2>/dev/null || true
    done
    [ -n "$WORKDIR" ] && [ -d "$WORKDIR" ] && rm -rf "$WORKDIR" 2>/dev/null || true
}

# =============================================================================
# The leg: heavy + idle sibling cgroups vs system-wide
# =============================================================================
run_leg_cgroup() {
    log "=== LEG cgroup: heavy + idle sibling scopes vs system-wide ==="
    local heavy_cg idle_cg pp=()
    local out_h="$WORKDIR/heavy.tsv" out_i="$WORKDIR/idle.tsv" out_s="$WORKDIR/system.tsv"
    local uh="intp-val-heavy-$$" ui="intp-val-idle-$$"

    # shellcheck disable=SC2086  # HEAVY_STRESS is intentionally word-split
    launch_unit "$uh" stress-ng $HEAVY_STRESS --timeout "$((DURATION + 10))s" \
        || skip "cannot create a cgroup scope (no write access to cgroup v2)"
    launch_unit "$ui" sleep "$((DURATION + 10))" \
        || skip "cannot create a cgroup scope (no write access to cgroup v2)"

    heavy_cg=$(await_unit_cg "$uh") || heavy_cg=""
    idle_cg=$(await_unit_cg "$ui")  || idle_cg=""
    if [ -z "$heavy_cg" ] || [ -z "$idle_cg" ]; then
        dump_unit_logs "$uh" "$ui"
        skip "workload cgroups never populated (systemd-run/delegation?)"
    fi
    log "heavy=$heavy_cg"
    log "idle=$idle_cg"

    log "profiling heavy, idle, system-wide concurrently (${DURATION}s)"
    profile_cgroup "$out_h" "$heavy_cg" & pp+=("$!"); CLEANUP_PIDS+=("$!")
    profile_cgroup "$out_i" "$idle_cg"  & pp+=("$!"); CLEANUP_PIDS+=("$!")
    profile_cgroup "$out_s" ""          & pp+=("$!"); CLEANUP_PIDS+=("$!")
    wait "${pp[@]}"

    local m h i t
    for m in "${SEPARABLE_METRICS[@]}"; do
        h=$(mean_metric "$out_h" "$m"); i=$(mean_metric "$out_i" "$m"); t=$(mean_metric "$out_s" "$m")
        assert_track_and_ratio "cgroup" "$m" "$h" "$i"
        assert_idle            "cgroup" "$m" "$i"
        assert_conserve        "cgroup" "$m" "$t" "$h" "$i"
    done
    # netp/nets: per-cgroup net split is validated upstream (across-intp
    # container legs); per-cgroup nets is a byte-share proxy by design (C2).
    for m in "${NET_METRICS[@]}"; do
        record "cgroup" "$m" "net-split" "validated upstream (C2 byte-share proxy / netns legs)" "SKIP[upstream]"
    done
}

# =============================================================================
# Report (ported verbatim, LIMIT tier dropped with the intra leg)
# =============================================================================
print_report() {
    local n_pass=0 n_fail=0 n_skip=0 row
    printf '\n'
    printf '%-7s %-7s %-18s %-6s %s\n' LEG METRIC CHECK RESULT DETAIL
    printf '%-7s %-7s %-18s %-6s %s\n' "------" "------" "----------------" "----" "------"
    for row in "${RESULTS[@]:-}"; do
        [ -z "$row" ] && continue
        IFS='|' read -r leg metric check detail result <<<"$row"
        printf '%-7s %-7s %-18s %-6s %s\n' "$leg" "$metric" "$check" "$result" "$detail"
        case "$result" in
            PASS) n_pass=$((n_pass+1)) ;;
            FAIL) n_fail=$((n_fail+1)) ;;
            SKIP*) n_skip=$((n_skip+1)) ;;
        esac
    done
    printf '\nSummary: %d PASS, %d FAIL, %d SKIP\n' "$n_pass" "$n_fail" "$n_skip"
    if [ "$n_fail" -gt 0 ]; then
        printf 'RESULT: FAIL -- per-cgroup attribution did not hold.\n'
        return 1
    fi
    printf 'RESULT: PASS -- per-cgroup attribution validated.\n'
    return 0
}

# =============================================================================
main() {
    INTP_BIN="${1:?usage: validate-attribution.sh <intp-binary>}"
    trap cleanup EXIT INT TERM

    [ -x "$INTP_BIN" ] || die "intp binary not executable: $INTP_BIN (meson compile first)"
    [ "$(id -u)" -eq 0 ] || skip "must run as root for cgroup creation + full backend access"
    grep -q 'cgroup2 /sys/fs/cgroup ' /proc/mounts 2>/dev/null \
        || skip "no cgroup v2 unified hierarchy at /sys/fs/cgroup"
    [ -w /sys/fs/cgroup ] || skip "no write access to /sys/fs/cgroup"
    command -v stress-ng >/dev/null 2>&1 || skip "stress-ng required (apt install stress-ng)"

    if command -v systemd-run >/dev/null 2>&1 && [ -d /run/systemd/system ]; then
        USE_SYSTEMD=1
    fi

    WORKDIR=$(mktemp -d -t intp-validate.XXXXXX)
    log "binary=$INTP_BIN duration=${DURATION}s interval=${INTERVAL}s warmup=${WARMUP} systemd=$USE_SYSTEMD workdir=$WORKDIR"
    log "thresholds: ratio>=${RATIO_MIN} idle<=${IDLE_MAX} conserve<=${CONSERVE_TOL}"

    run_leg_cgroup
    print_report
}

# Run only when executed directly; sourcing (the self-test) loads the
# functions without running the campaign.
if [ "${BASH_SOURCE[0]}" = "${0}" ]; then
    main "$@"
fi
