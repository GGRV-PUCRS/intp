#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
#
# Ported from across-intp:
#   bench/analyze-faithfulness.py @ 2e87b30fc762
# with ONLY the I/O plumbing adapted to the bench/gates run tree:
#
#   - input tree is RUNDIR/solo/<workload>/rep<R>/{profiler.tsv,
#     groundtruth.tsv} as produced by bench/gates/run-gates.sh (single
#     env "local", single arm = the intp binary under test) instead of
#     the origin's <campaign>/<env>/<variant>/solo/<workload>/rep*;
#     workloads are discovered from the tree. profiler.tsv is intp
#     tsv-legacy-v1 with the canonical 'netp..cpu' header line prepended
#     by run-gates.sh, exactly what intp_metrics.parse_capture expects.
#   - scipy is dropped entirely: the origin already degrades to the
#     Cliff-delta-only path when scipy is absent; the load-bearing llcmr
#     Spearman rank check (origin §1b, scipy.stats.spearmanr) is
#     reimplemented inline in pure python (average-rank ties ->
#     Pearson on ranks), deterministic, so the gate needs no
#     numpy/scipy on the gate host.
#   - gate emission: machine-readable lines
#       GATE\tfidelity:cpu-band\t<PASS|FAIL|SKIP>\t<detail>
#       GATE\tfidelity:llcmr-rank\t<PASS|FAIL|SKIP>\t<detail>
#     are appended to stdout for bench/gates/run-gates.sh. A gate that
#     cannot be computed (no GT, idle host, too few cells) is SKIP,
#     never a crash.
#
# Claim-class thresholds come verbatim from intp_metrics.py (the single
# source of truth, copied from the origin) and MUST NOT drift:
#   cpu   absolute ratio band RATIO_LO..RATIO_HI = 0.8..1.25 vs GT
#         cpu_busy_pct (only same-scale GT; idle guard IDLE_GT = 5.0,
#         run-validity floor INVALID_CPU_PCT = 10.0 on the cpu workload)
#   llcmr directional Spearman rho >= RHO_OK = 0.6, cells gated on
#         median GT llc_ref >= LLCREF_MIN = 1e6/interval
#
# Usage:  analyze-faithfulness.py RUNDIR [--out report.md]
# Exit:   0 no gate FAILed (PASS/SKIP), 1 at least one gate FAILed,
#         2 usage error.
"""Faithfulness adjudication for the intp release gates.

Reads each solo run's profiler.tsv (the 7 canonical metrics) and
groundtruth.tsv (the capture-groundtruth.sh side-channel) and adjudicates
faithfulness per workload using median/IQR across reps.

Scale discipline (critical, from the origin): of the GT columns, ONLY
cpu_busy_pct is on the same 0-100 scale as its profiler metric (cpu), so it
is the only column that admits a direct ratio adjudication. The profiler cpu
may be cgroup-scoped while GT cpu_busy_pct is /proc/stat system-wide; for
SOLO runs the single tenant is the only significant load so they align --
the ratio verdict is meaningful for solo only. blk/netp/nets are 0-100
normalized utilizations vs raw MB/interval GT => qualitative only. llcmr IS
adjudicated against the perf GT (100*llc_miss/llc_ref) but DIRECTIONALLY
(event-definition + scope differ); the Spearman rank check is the
load-bearing faithfulness measure. mbw/llcocc GT columns (resctrl CMT) stay
empty => UNVERIFIABLE (one RMID per task; the profiler holds it, so its own
read is the GT).
"""
import sys, os, glob, argparse

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import intp_metrics as M  # noqa: E402  (shared metric model + thresholds)

# Metric model + thresholds are shared (bench/gates/intp_metrics.py).
METRICS = M.METRICS_CANON
CLAIM_CLASS = M.CLAIM_CLASS
UNVERIFIABLE = M.UNVERIFIABLE
RATIO_LO, RATIO_HI = M.RATIO_LO, M.RATIO_HI
IDLE_GT = M.IDLE_GT
INVALID_CPU_PCT = M.INVALID_CPU_PCT
LLCREF_MIN = M.LLCREF_MIN
RHO_OK = M.RHO_OK

# Gates-tree workload spine: run-gates.sh's three solo reference loads.
# (The origin's app01..app13 W4 spine does not exist in the production
# tree; the cpu workload doubles as the run-validity guard like app10.)
CPU_WL = "cpu"
DISK_WL = "disk"
ARM = "intp"            # the single profiler arm under test

# Basic stats helpers shared (identical implementations) -- intp_metrics.py.
_median = M._median
_iqr = M._iqr
_fmt = M._fmt
cliffs_delta = M.cliffs_delta


def _stat(a, b):
    """Compact 'cliff=..' suffix comparing rep-lists a vs b (None-safe).
    Origin printed a Mann-Whitney p when scipy was present; this port is
    stdlib-only, i.e. the origin's HAVE_SCIPY=False path."""
    a = [x for x in a if x is not None]
    b = [x for x in b if x is not None]
    d = cliffs_delta(a, b)
    return f"cliff={_fmt(d)}" if d is not None else ""


# ---------------------------------------------------------------- inline stats
# Pure-python, deterministic replacements for scipy.stats.spearmanr's rho.
# (No p-value: the gate is the RHO_OK threshold on rho per the claim class;
# with the default 3-workload spine n is too small for a meaningful p anyway.)

def _pearson_r(xs, ys):
    n = len(xs)
    if n < 3:
        return None
    mx = sum(xs) / n
    my = sum(ys) / n
    sxx = sum((x - mx) ** 2 for x in xs)
    syy = sum((y - my) ** 2 for y in ys)
    sxy = sum((x - mx) * (y - my) for x, y in zip(xs, ys))
    if sxx <= 0.0 or syy <= 0.0:
        return None
    return sxy / (sxx * syy) ** 0.5


def _ranks(xs):
    """Average ranks (1-based) with ties sharing the mean rank."""
    order = sorted(range(len(xs)), key=lambda i: xs[i])
    ranks = [0.0] * len(xs)
    i = 0
    while i < len(order):
        j = i
        while j + 1 < len(order) and xs[order[j + 1]] == xs[order[i]]:
            j += 1
        r = (i + j) / 2.0 + 1.0
        for k in range(i, j + 1):
            ranks[order[k]] = r
        i = j + 1
    return ranks


def spearman_rho(xs, ys):
    if len(xs) != len(ys) or len(xs) < 3:
        return None
    return _pearson_r(_ranks(xs), _ranks(ys))


# Parsers + per-rep summary are shared (intp_metrics.py): rep_summary reads
# rundir/{profiler.tsv,groundtruth.tsv} and yields per-run medians + gt_*.
rep_summary = M.rep_summary


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("rundir", help="run tree from bench/gates/run-gates.sh")
    ap.add_argument("--out", default=None, help="also write the report here")
    args = ap.parse_args()
    base = args.rundir.rstrip("/")

    gates = []   # (name, result, detail)

    def gate(name, result, detail):
        gates.append((name, result, detail))

    # ---- discover the tree -------------------------------------------------
    workloads = sorted(
        os.path.basename(p) for p in glob.glob(f"{base}/solo/*")
        if glob.glob(f"{p}/rep*"))
    cell = {wl: [rep_summary(rep)
                 for rep in sorted(glob.glob(f"{base}/solo/{wl}/rep*"))]
            for wl in workloads}

    def vals(wl, key):
        return [s.get(key) for s in cell.get(wl, []) if s.get(key) is not None]

    L = []
    em = L.append
    em(f"# Faithfulness adjudication — {base}")
    em("")

    if not workloads:
        msg = (f"no data under {base} "
               f"(expected {base}/solo/<workload>/rep*/profiler.tsv)")
        em(f"**{msg}**")
        gate("fidelity:cpu-band", "SKIP", msg)
        gate("fidelity:llcmr-rank", "SKIP", msg)
        _emit(L, gates, args.out)
        return 0

    reps = [len(v) for v in cell.values() if v]
    em(f"stats: stdlib-only port (Cliff delta; inline Spearman); "
       f"reps/workload median: {int(_median(reps) or 0)}")
    em("")

    # ---- run-validity guard (origin env-validity, single-env form) ---------
    # A run whose cpu workload shows GT cpu_busy_pct < INVALID_CPU_PCT never
    # loaded the host (workload failed to start) => INVALID, gates SKIP.
    invalid_reason = None
    if CPU_WL in workloads:
        gtc = _median(vals(CPU_WL, "gt_cpu"))
        if gtc is None:
            em(f"_(no GT cpu_busy_pct for the '{CPU_WL}' workload — validity "
               f"guard inconclusive, keeping the run)_")
            em("")
        elif gtc < INVALID_CPU_PCT:
            invalid_reason = (f"run INVALID: '{CPU_WL}' workload GT "
                              f"cpu_busy_pct={_fmt(gtc)} < {INVALID_CPU_PCT}"
                              f" (host never loaded)")
            em(f"**{invalid_reason}**")
            em("")

    # ---- 1. CPU faithfulness ------------------------------------------------
    em("## 1. CPU faithfulness — `cpu` vs GT `cpu_busy_pct` (only same-scale GT; all workloads)")
    em("")
    em(f"Verdict: FAITHFUL {RATIO_LO}<=ratio<={RATIO_HI}, else UNDER/OVERREPORTS. "
       f"NOTE: a cgroup-scoped profiler `cpu` vs the system-wide GT aligns for "
       f"SOLO runs only; workloads with GT cpu% < {IDLE_GT} are too idle to ratio.")
    em("")
    em(f"| workload | GT cpu% | {ARM} cpu | verdict |")
    em("|---|---|---|---|")
    band_checked = []   # (wl, ratio)
    band_bad = []
    for wl in workloads:
        pr = vals(wl, "cpu")
        gvr = vals(wl, "gt_cpu")
        cm, gvm = _median(pr), _median(gvr)
        r = (cm / gvm) if (cm is not None and gvm) else None
        if gvm is not None and gvm < IDLE_GT:
            v = "n/a (idle wl)"
        elif r is None:
            v = "-"
        else:
            tag = ("FAITHFUL" if RATIO_LO <= r <= RATIO_HI
                   else ("**UNDER**" if r < RATIO_LO else "**OVER**"))
            st = _stat(pr, gvr)
            v = f"{tag} ({r:.2f})" + (f" [{st}]" if st else "")
            band_checked.append((wl, r))
            if not (RATIO_LO <= r <= RATIO_HI):
                band_bad.append(f"{wl}(r={r:.2f})")
        em(f"| {wl} | {_fmt(gvm)} | {_fmt(cm)} | {v} |")
    em("")

    if invalid_reason:
        gate("fidelity:cpu-band", "SKIP", invalid_reason)
    elif not band_checked:
        gate("fidelity:cpu-band", "SKIP",
             "no adjudicable cell (no profiler cpu + GT cpu_busy_pct pair "
             f"above the {IDLE_GT}% idle guard)")
    elif band_bad:
        gate("fidelity:cpu-band", "FAIL",
             f"{len(band_bad)}/{len(band_checked)} cell(s) outside "
             f"[{RATIO_LO},{RATIO_HI}]: " + ", ".join(band_bad))
    else:
        rats = ", ".join(f"{wl}={r:.2f}" for wl, r in band_checked)
        gate("fidelity:cpu-band", "PASS",
             f"{len(band_checked)}/{len(band_checked)} cell(s) in "
             f"[{RATIO_LO},{RATIO_HI}]: {rats}")

    # ---- 1b. LLC miss-ratio (DIRECTIONAL) ------------------------------------
    em("## 1b. LLC miss-ratio — `llcmr` vs perf GT `100×llc_miss/llc_ref` (DIRECTIONAL)")
    em("")
    em("**Read these verdicts as DIRECTIONAL, not as a same-scale pass/fail.** "
       "The profiler and the GT do not count the same physical quantity: the GT "
       "denominator is perf `cache-references` (ALL request ops) while the "
       "profiler uses a read-only LL-access event, and the GT is system-wide "
       "(`perf -a`) vs a possibly cgroup-scoped profiler. Absolute band labels "
       "are kept for reference only; the Spearman rank check below is the "
       "load-bearing faithfulness measure.")
    em("")
    em(f"Band FAITHFUL {RATIO_LO}<=ratio<={RATIO_HI} indicative only; cells gated "
       f"on median GT llc_ref >= {LLCREF_MIN:.0g}/interval.")
    em("")
    em(f"| workload | GT llcmr% | GT llc_ref | {ARM} llcmr | verdict |")
    em("|---|---|---|---|---|")
    xs, ys = [], []         # adjudicated cells for the rank check
    have_llcmr_gt = False
    for wl in workloads:
        pr = vals(wl, "llcmr")
        gvr = vals(wl, "gt_llcmr")
        cm, gvm = _median(pr), _median(gvr)
        grefm = _median(vals(wl, "gt_llc_ref"))
        if gvm is not None:
            have_llcmr_gt = True
        r = (cm / gvm) if (cm is not None and gvm) else None
        if gvm is None or grefm is None:
            v = "-"
        elif grefm < LLCREF_MIN:
            v = "n/a (low LLC traffic)"
        elif r is None:
            v = "-"
        else:
            tag = ("FAITHFUL" if RATIO_LO <= r <= RATIO_HI
                   else ("**UNDER**" if r < RATIO_LO else "**OVER**"))
            st = _stat(pr, gvr)
            v = f"{tag} ({r:.2f})" + (f" [{st}]" if st else "")
            xs.append(cm)
            ys.append(gvm)
        em(f"| {wl} | {_fmt(gvm)} | {_fmt(grefm)} | {_fmt(cm)} | {v} |")
    if not have_llcmr_gt:
        em("")
        em("_(No populated GT llc_ref/llc_miss in this run — perf GT path "
           "absent or all '--'; needs perf counter access.)_")
    em("")

    rho = None
    if len(xs) >= 3 and len(set(xs)) > 1 and len(set(ys)) > 1:
        rho = spearman_rho(xs, ys)
    if rho is not None:
        ok = rho >= RHO_OK
        em(f"**Directional faithfulness (Spearman rank, inline): "
           f"rho={rho:.2f} (n={len(xs)} cells) — "
           + ("directionally FAITHFUL" if ok else "weak rank agreement") + "**")
        em("(Absolute UNDER/OVER labels above reflect the event-definition + "
           "scope differences, not an established absolute miss-ratio error.)")
        em("")
    if invalid_reason:
        gate("fidelity:llcmr-rank", "SKIP", invalid_reason)
    elif rho is None:
        gate("fidelity:llcmr-rank", "SKIP",
             f"insufficient adjudicable cells/spread for rank correlation "
             f"(n={len(xs)}, need >=3 with llcmr + perf GT and llc_ref >= "
             f"{LLCREF_MIN:.0g})")
    elif rho >= RHO_OK:
        gate("fidelity:llcmr-rank", "PASS",
             f"Spearman rho={rho:.2f} >= {RHO_OK} over n={len(xs)} cells")
    else:
        gate("fidelity:llcmr-rank", "FAIL",
             f"Spearman rho={rho:.2f} < {RHO_OK} over n={len(xs)} cells")

    # ---- 2. Normalized-metric activation (qualitative) -----------------------
    em("## 2. Normalized metrics (blk) — qualitative activation")
    em("")
    em("0-100 utilizations; GT `disk_*` is raw MB/interval (C7 normalization) "
       "=> no ratio. (The origin's netp/nets loopback check has no workload in "
       "the default gate spine — the gates run no network reference load.)")
    em("")
    if DISK_WL in workloads:
        on = _median(vals(DISK_WL, "blk"))
        off = max([_median(vals(w, "blk")) or 0
                   for w in workloads if w != DISK_WL] or [0])
        tag = "tracks disk" if (on or 0) > 50 else "CHECK"
        em(f"- **blk on {DISK_WL} (disk)** = {_fmt(on)} "
           f"(max on non-disk workloads {_fmt(off)}) — {tag}")
    else:
        em("- _(no disk workload in this tree)_")
    em("")

    em(f"## 3. GT-UNVERIFIABLE metrics: {', '.join(UNVERIFIABLE)}")
    em("resctrl GT columns (`resctrl_mbw_bps`,`resctrl_llcocc_bytes`) are empty "
       "BY DESIGN: Intel RDT CMT/MBM bind one RMID per task and the profiler "
       "holds the tenant's RMID, so no independent collector can read the same "
       "tasks' occupancy/bandwidth concurrently — the profiler's own resctrl "
       "read IS the ground truth. `llcmr` IS compared (§1b) but directionally.")
    em("")

    # ---- 4. dominant-metric signature ----------------------------------------
    em("## 4. Per-workload dominant metric (qualitative signature)")
    em("")
    em("| arm | " + " | ".join(workloads) + " |")
    em("|" + "---|" * (len(workloads) + 1))
    cells = []
    for wl in workloads:
        meds = {m: _median(vals(wl, m)) for m in METRICS}
        meds = {m: v for m, v in meds.items() if v is not None}
        dom = max(meds, key=meds.get) if meds else "-"
        cells.append(f"{dom}({_fmt(meds.get(dom))})")
    em(f"| {ARM} | " + " | ".join(cells) + " |")
    em("")

    # ---- 5. full per-cell table ------------------------------------------------
    em("## 5. Per-workload median (spread = q3-q1 nearest-rank) — all metrics")
    em("")
    em("| workload | " + " | ".join(METRICS) + " | gt_cpu | gt_disk | gt_net | gt_llcmr |")
    em("|" + "---|" * (len(METRICS) + 5))
    for wl in workloads:
        row = [wl]
        for m in METRICS:
            xs2 = vals(wl, m)
            row.append(f"{_fmt(_median(xs2))}({_fmt(_iqr(xs2))})")
        for k in ("gt_cpu", "gt_disk", "gt_net", "gt_llcmr"):
            row.append(_fmt(_median(vals(wl, k))))
        em("| " + " | ".join(row) + " |")

    _emit(L, gates, args.out)
    return 1 if any(r == "FAIL" for _, r, _ in gates) else 0


def _emit(lines, gates, out):
    report = "\n".join(lines)
    print(report)
    print()
    for name, result, detail in gates:
        print(f"GATE\t{name}\t{result}\t{detail}")
    if out:
        open(out, "w").write(report + "\n")
        sys.stderr.write(f"[wrote {out}]\n")


if __name__ == "__main__":
    sys.exit(main())
