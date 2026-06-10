#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
#
# Ported from intp-comparison:
#   bench/plot/extract-fragility.py @ 2e87b30fc762
# with the I/O plumbing adapted to the bench/gates run tree and the
# loss DEFINITION reduced to the origin's stress-ng leg: expected
# samples come from the run's recorded target duration / interval
# (run.json {"duration_s","interval_s"}, the same fields D8 added
# upstream), actual samples are the data rows of profiler.tsv. The
# origin's SystemTap-specific machinery (stap.log probe-skip and
# stall-monitor forensics, the HiBench timestamp-gap fallback) does not
# apply to the production binary — intp's tsv-legacy-v1 carries no
# timestamp column and the eBPF/pure-C paths emit no per-rep stap log —
# so reliability is adjudicated from sample counts alone, exactly how
# the origin treated C-ABI/eBPF arms (their loss was "inferred from
# sample count (rows) and absence of run.json error").
#
# Gate (threshold frozen from the release plan / origin campaign):
#   sample loss > LOSS_MAX_PCT (10.0) on ANY rep  ->  FAIL
#   no reps found / no run.json metadata          ->  SKIP
#
# Usage:  extract-fragility.py RUNDIR [--out FILE]
# Exit:   0 no FAIL, 1 any FAIL, 2 usage.
# Emits:  Markdown report on stdout + GATE\tsample-loss\t... line.

import argparse
import glob
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from intp_metrics import parse_capture  # noqa: E402

LOSS_MAX_PCT = 10.0


def rep_loss(rep_dir):
    """(expected, actual, loss_pct) for one rep, or None when the rep
    carries no usable metadata/capture."""
    meta_path = os.path.join(rep_dir, "run.json")
    cap_path = os.path.join(rep_dir, "profiler.tsv")
    if not (os.path.exists(meta_path) and os.path.exists(cap_path)):
        return None
    try:
        meta = json.load(open(meta_path))
        duration = float(meta["duration_s"])
        interval = float(meta.get("interval_s", 1.0))
    except (OSError, ValueError, KeyError, json.JSONDecodeError):
        return None
    if interval <= 0 or duration <= 0:
        return None

    expected = int(duration / interval)
    if expected <= 0:
        return None
    # parse_capture drops the warmup row; account for it on the
    # expected side too so a lossless run reads 0 % (origin behavior:
    # first cumulative row is not a sample).
    expected = max(expected - 1, 1)
    actual = len(parse_capture(cap_path))
    loss = max(0.0, (expected - actual) / expected * 100.0)
    return expected, actual, loss


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("rundir", help="run tree from bench/gates/run-gates.sh")
    ap.add_argument("--out", default=None, help="also write the report here")
    args = ap.parse_args()
    base = args.rundir.rstrip("/")

    rows = []
    for rep_dir in sorted(glob.glob(f"{base}/solo/*/rep*")):
        wl = os.path.basename(os.path.dirname(rep_dir))
        rep = os.path.basename(rep_dir)
        r = rep_loss(rep_dir)
        if r is None:
            continue
        rows.append((wl, rep) + r)

    L = []
    em = L.append
    em(f"# Sample-loss / fragility — {base}")
    em("")

    if not rows:
        detail = (f"no adjudicable reps under {base} "
                  f"(need solo/<wl>/rep*/{{run.json,profiler.tsv}})")
        em(f"**SKIP: {detail}**")
        result = ("SKIP", detail)
    else:
        em("| workload | rep | expected | actual | loss % |")
        em("|----------|-----|----------|--------|--------|")
        worst = 0.0
        n_over = 0
        for wl, rep, expected, actual, loss in rows:
            flag = "  <-- OVER" if loss > LOSS_MAX_PCT else ""
            em(f"| {wl} | {rep} | {expected} | {actual} | "
               f"{loss:.2f}{flag} |")
            worst = max(worst, loss)
            n_over += 1 if loss > LOSS_MAX_PCT else 0
        em("")
        em(f"worst-rep loss {worst:.2f} % over {len(rows)} reps; "
           f"threshold {LOSS_MAX_PCT:.1f} % (frozen); {n_over} rep(s) over.")
        if n_over:
            result = ("FAIL",
                      f"{n_over}/{len(rows)} reps exceed "
                      f"{LOSS_MAX_PCT:.1f}% loss (worst {worst:.2f}%)")
        else:
            result = ("PASS",
                      f"worst-rep loss {worst:.2f}% <= "
                      f"{LOSS_MAX_PCT:.1f}% over {len(rows)} reps")

    report = "\n".join(L)
    print(report)
    print()
    print(f"GATE\tsample-loss\t{result[0]}\t{result[1]}")
    if args.out:
        open(args.out, "w").write(report + "\n")
        sys.stderr.write(f"[wrote {args.out}]\n")
    return 1 if result[0] == "FAIL" else 0


if __name__ == "__main__":
    sys.exit(main())
