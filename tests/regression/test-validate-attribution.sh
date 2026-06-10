#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# Self-test for validate-attribution.sh parse + assertion logic. Sources
# the (guarded) harness and feeds synthetic intp tsv-legacy-v1 output --
# no root/cgroups needed, so it runs anywhere (including CI-adjacent
# local checks).
#
# Ported from intp-comparison:
#   bench/test-validate-attribution.sh @ 2e87b30fc762
# adapted to the intp port: synthetic input is tsv-legacy-v1 (7 columns,
# '--' for unavailable) instead of v2.1 JSON; the jq dependency and the
# backend_of/assert_unsplittable tests fall away with the JSON parser and
# the dropped LXD intra leg.
#
# Run: tests/regression/test-validate-attribution.sh
set -uo pipefail
SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
HARNESS="$SCRIPT_DIR/validate-attribution.sh"
# shellcheck disable=SC1090
source "$HARNESS"
set +e   # the harness sets -e; assertions below probe failure paths

WORKDIR=$(mktemp -d -t intp-vtest.XXXXXX)
trap 'rm -rf "$WORKDIR"' EXIT
WARMUP=2; IDLE_MAX=5; RATIO_MIN=8; CONSERVE_TOL=0.25

T_PASS=0; T_FAIL=0
ok()   { printf '  ok   %s\n' "$1"; T_PASS=$((T_PASS+1)); }
bad()  { printf '  FAIL %s\n' "$1"; T_FAIL=$((T_FAIL+1)); }
check(){ [ "$2" = "$3" ] && ok "$1 (=$3)" || bad "$1 (want $3, got $2)"; }

# One tsv-legacy-v1 sample line: netp nets blk mbw llcmr llcocc cpu
# (pass numeric values, or the literal token -- for unavailable).
tline() { printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$@"; }

# 2 warmup lines (value 0) then 4 steady lines at the target values.
mkfile() { # FILE netp nets blk mbw llcmr llcocc cpu
  local f="$1"; shift
  : > "$f"
  tline 0 0 0 0 0 0 0 >> "$f"
  tline 0 0 0 0 0 0 0 >> "$f"
  local k; for k in 2 3 4 5; do tline "$@" >> "$f"; done
}

H="$WORKDIR/h.tsv" I="$WORKDIR/i.tsv" Tt="$WORKDIR/t.tsv" N="$WORKDIR/null.tsv"
#       netp nets blk mbw llcmr llcocc cpu
mkfile "$H"  12 3  50 40 30 25 80
mkfile "$I"  12 3   0  1  1  1  2
mkfile "$Tt" 12 3  50 41 31 26 82
# '--'-mbw file: metric unavailable on this host. First cell '--' too, so
# the row starts with '-' (the regression that ^[0-9] row matching missed).
: > "$N"; tline 0 0 0 0 0 0 0 >> "$N"; tline 0 0 0 0 0 0 0 >> "$N"
k=2; while [ "$k" -le 5 ]; do tline -- 3 50 -- 30 25 80 >> "$N"; k=$((k+1)); done

echo "== mean_metric (WARMUP skip + '--' drop) =="
check "mean cpu heavy"          "$(mean_metric "$H" cpu)"      "80"
check "mean blk idle"           "$(mean_metric "$I" blk)"      "0"
check "mean netp total"         "$(mean_metric "$Tt" netp)"    "12"
check "mean mbw '--'->na"       "$(mean_metric "$N" mbw)"      "na"
check "mean netp '--'->na"      "$(mean_metric "$N" netp)"     "na"
check "mean cpu '--'-file ok"   "$(mean_metric "$N" cpu)"      "80"
check "mean missing file na"    "$(mean_metric "$WORKDIR/nope.tsv" cpu)" "na"

echo "== separable-metric assertions =="
RESULTS=(); N_FAIL=0
# Helper: read the RESULT token (5th |-field) of the last recorded row.
last() { echo "${RESULTS[-1]##*|}"; }

assert_track_and_ratio x cpu 80 2 ; check "track cpu 80vs2"       "$(last)" "PASS"
assert_track_and_ratio x cpu 80 20; check "track cpu 80vs20(r=4)" "$(last)" "FAIL"
assert_track_and_ratio x blk 50 0 ; check "track blk 50vs0"       "$(last)" "PASS"
assert_track_and_ratio x mbw na 1 ; check "track mbw na->skip"    "$(last)" "SKIP"
assert_track_and_ratio x cpu 3 1  ; check "track cpu below floor" "$(last)" "FAIL"

assert_idle x cpu 2  ; check "idle cpu 2<=5"      "$(last)" "PASS"
assert_idle x cpu 10 ; check "idle cpu 10>5"      "$(last)" "FAIL"
assert_idle x mbw na ; check "idle mbw na->skip"  "$(last)" "SKIP"

assert_conserve x cpu 82 80 2  ; check "conserve exact"        "$(last)" "PASS"
assert_conserve x cpu 100 80 2 ; check "conserve 18% in tol"   "$(last)" "PASS"
assert_conserve x cpu 200 80 2 ; check "conserve 59% out tol"  "$(last)" "FAIL"
assert_conserve x mbw na 40 1  ; check "conserve total na"     "$(last)" "SKIP"
assert_conserve x cpu 0 0 0    ; check "conserve both ~0"      "$(last)" "SKIP"

echo "== N_FAIL bookkeeping =="
RESULTS=(); N_FAIL=0
assert_idle x cpu 10   # one FAIL
assert_idle x cpu 2    # one PASS
check "N_FAIL counts 1 fail" "$N_FAIL" "1"

echo
printf 'TEST TOTAL: %d ok, %d FAIL\n' "$T_PASS" "$T_FAIL"
[ "$T_FAIL" -eq 0 ]
