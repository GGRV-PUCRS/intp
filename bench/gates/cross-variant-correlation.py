#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
#
# Ported from across-intp:
#   bench/plot/cross-variant-correlation.py @ 2e87b30fc762
# with ONLY the I/O plumbing + dependency surface adapted to the
# bench/gates run tree:
#
#   - input: RUNDIR/solo/<workload>/rep<R>/profiler.tsv (intp tsv-legacy-v1
#     with the 'netp..cpu' header prepended by run-gates.sh) plus
#     RUNDIR/overhead/<env>/<variant>/<ref>/rep<R>/throughput.tsv, instead
#     of the origin campaign's aggregate-means.tsv. The overhead subtree
#     layout and the 2-column throughput.tsv (`bogo_ops_per_s_real`) are
#     UNCHANGED from the origin (_read_bogo/overhead_bounds verbatim in
#     intent), so origin-shaped overhead trees also parse.
#   - the correlation "arms" are the per-rep fingerprints of the ONE
#     production binary under test (rep-to-rep fingerprint stability of
#     the [workload x metric] matrix) — the origin's four research
#     endpoints do not exist in the production tree. Per-rep central
#     tendency is the shared intp_metrics.rep_summary median (the origin
#     read per-rep means from aggregate-means.tsv; that file is a
#     research-harness artifact).
#   - pandas/numpy dropped: stdlib-only, deterministic inline Pearson
#     (pairwise-complete, like the origin's NaN mask); per-metric
#     zero-variance drop in the z-scored scope kept.
#   - the paper-number EXPECTED_VALUES/--verify machinery and the
#     matplotlib heatmaps are dropped (paper artifacts, not gates).
#   - gate emission for bench/gates/run-gates.sh:
#       GATE\tfingerprint\t<PASS|FAIL|SKIP>\t<detail>
#         mean pairwise raw Pearson r across rep fingerprints >= 0.8
#         (FINGERPRINT_R_MIN; the claim-class threshold, frozen)
#       GATE\toverhead\t<PASS|FAIL|SKIP>\t<detail>
#         mean bogo-ops/s throughput delta vs the no-profiler baseline
#         <= INTP_GATE_OVERHEAD_PCT percent (default 5.0) for EVERY
#         (variant, ref_load); overhead = (base - arm)/base*100,
#         positive = slower, per the origin's overhead_bounds.
#     A gate that cannot be computed (single rep, no finite features, no
#     throughput rows) is SKIP, never a crash.
#
# Usage:  cross-variant-correlation.py --campaign RUNDIR [--out DIR]
# Exit:   0 no gate FAILed (PASS/SKIP), 1 at least one gate FAILed,
#         2 usage error.
#
# Outputs (TSV, into --out, default the run dir):
#   correlation-pairs.tsv        rep-pair r_raw/r_zscored/n_features
#   correlation-per-metric.tsv   per-metric rep-pair r across workloads
#   overhead-bounds.tsv          variant, ref_load, mean/std/n delta pct
import argparse
import glob
import os
import sys
from itertools import combinations

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import intp_metrics as M  # noqa: E402  (shared metric model)

METRICS = M.METRICS_CANON

# Claim-class thresholds (frozen; see file header).
FINGERPRINT_R_MIN = 0.8
OVERHEAD_PCT_DEFAULT = 5.0


def log(msg):
    print(msg, flush=True)


# ---------------------------------------------------------------- inline stats
def _pearson(xs, ys):
    """Pearson r over pairwise-complete (both-not-None) values.
    Returns (r|None, n_used) — mirrors the origin's NaN-masked _pearson
    (None when n < 3 or either side has zero variance)."""
    pairs = [(x, y) for x, y in zip(xs, ys) if x is not None and y is not None]
    n = len(pairs)
    if n < 3:
        return None, n
    mx = sum(p[0] for p in pairs) / n
    my = sum(p[1] for p in pairs) / n
    sxx = sum((p[0] - mx) ** 2 for p in pairs)
    syy = sum((p[1] - my) ** 2 for p in pairs)
    sxy = sum((p[0] - mx) * (p[1] - my) for p in pairs)
    if sxx <= 0.0 or syy <= 0.0:
        return None, n
    return sxy / (sxx * syy) ** 0.5, n


def _mean(xs):
    return sum(xs) / len(xs) if xs else None


def _std(xs):
    """Sample std (ddof=1), None for n < 2 — matches pandas .std()."""
    n = len(xs)
    if n < 2:
        return None
    mu = sum(xs) / n
    return (sum((x - mu) ** 2 for x in xs) / (n - 1)) ** 0.5


# ---------------------------------------------------------------- fingerprints
def rep_fingerprints(base):
    """Per-rep [workload x metric] fingerprint matrices from the solo tree.

    Returns (mats, workloads): mats[arm][wl][metric] = median|None where
    arm is the rep id (e.g. 'rep1'); workloads is the COMMON workload set
    across arms (origin: common app set), sorted.
    """
    per_arm = {}
    for wl_dir in sorted(glob.glob(os.path.join(base, "solo", "*"))):
        wl = os.path.basename(wl_dir)
        for rep_dir in sorted(glob.glob(os.path.join(wl_dir, "rep*"))):
            arm = os.path.basename(rep_dir)
            s = M.rep_summary(rep_dir)
            per_arm.setdefault(arm, {})[wl] = {m: s.get(m) for m in METRICS}
    if len(per_arm) < 2:
        return per_arm, sorted({wl for d in per_arm.values() for wl in d})
    common = sorted(set.intersection(*[set(d) for d in per_arm.values()]))
    mats = {arm: {wl: d[wl] for wl in common} for arm, d in per_arm.items()}
    return mats, common


def flatten(mat, workloads):
    return [mat[wl][m] for wl in workloads for m in METRICS]


def zscore_per_metric(mats, workloads):
    """Z-score each metric across the pooled (arm x workload) values.

    Pooling keeps a single shared scale per metric so the flattened
    fingerprint correlation reflects pattern agreement, not absolute level.
    A metric with zero pooled variance (or <2 finite values) is dropped
    (logged) rather than div-by-zero; None cells stay None."""
    pooled = {m: [] for m in METRICS}
    for mat in mats.values():
        for wl in workloads:
            for m in METRICS:
                v = mat[wl][m]
                if v is not None:
                    pooled[m].append(v)
    keep, stats = [], {}
    for m in METRICS:
        xs = pooled[m]
        if len(xs) < 2:
            continue
        mu = sum(xs) / len(xs)
        sd = (sum((x - mu) ** 2 for x in xs) / len(xs)) ** 0.5   # ddof=0
        if sd > 0.0:
            keep.append(m)
            stats[m] = (mu, sd)
    dropped = [m for m in METRICS if m not in keep]
    if dropped:
        log(f"[zscore] dropping zero-variance/empty metric(s): {dropped}")
    out = {}
    for arm, mat in mats.items():
        out[arm] = {wl: {m: ((mat[wl][m] - stats[m][0]) / stats[m][1]
                            if mat[wl][m] is not None else None)
                         for m in keep}
                    for wl in workloads}
    return out, keep


def pair_table(mats, mats_z, keep_z, workloads):
    rows = []
    for a, b in combinations(sorted(mats), 2):
        r_raw, nfeat = _pearson(flatten(mats[a], workloads),
                                flatten(mats[b], workloads))
        za = [mats_z[a][wl][m] for wl in workloads for m in keep_z]
        zb = [mats_z[b][wl][m] for wl in workloads for m in keep_z]
        r_z, _ = _pearson(za, zb)
        rows.append(dict(arm_a=a, arm_b=b,
                         r_raw=None if r_raw is None else round(r_raw, 4),
                         r_zscored=None if r_z is None else round(r_z, 4),
                         n_features=nfeat))
    return rows


def per_metric_table(mats, workloads):
    rows = []
    for m in METRICS:
        for a, b in combinations(sorted(mats), 2):
            r, n = _pearson([mats[a][wl][m] for wl in workloads],
                            [mats[b][wl][m] for wl in workloads])
            rows.append(dict(metric=m, arm_a=a, arm_b=b,
                             r=None if r is None else round(r, 4),
                             n_workloads=n))
    return rows


# ---------------------------------------------------------------- overhead
def _read_bogo(path):
    """bogo_ops_per_s_real from a 2-column throughput.tsv; None if absent."""
    try:
        with open(path) as fh:
            for line in fh:
                k, _, v = line.rstrip("\n").partition("\t")
                if k == "bogo_ops_per_s_real":
                    try:
                        return float(v)
                    except ValueError:
                        return None
    except OSError:
        pass
    return None


def overhead_bounds(base):
    """Throughput overhead %, per (variant, ref_load), with per-variant
    baseline. overhead = (baseline_bogo - arm_bogo)/baseline_bogo*100,
    positive = slower. A per-variant `_baseline.<variant>` dir overrides the
    shared `_baseline` when present (origin behavior). Tree:
    <base>/overhead/<env>/<variant>/<ref>/rep<R>/throughput.tsv."""
    rows = []
    for tsv in sorted(glob.glob(os.path.join(
            base, "overhead", "*", "*", "*", "rep*", "throughput.tsv"))):
        parts = tsv.split(os.sep)
        env, variant, ref = parts[-5], parts[-4], parts[-3]
        bogo = _read_bogo(tsv)
        if bogo is None:
            continue
        rows.append(dict(env=env, variant=variant, ref=ref, bogo=bogo))
    if not rows:
        return []

    base_means = {}   # (variant, env, ref) -> mean bogo
    grouped = {}
    for r in rows:
        if r["variant"].startswith("_baseline"):
            grouped.setdefault((r["variant"], r["env"], r["ref"]), []).append(r["bogo"])
    for k, xs in grouped.items():
        base_means[k] = _mean(xs)
    available = {k[0] for k in base_means}

    def base_for(v):
        return f"_baseline.{v}" if f"_baseline.{v}" in available else "_baseline"

    deltas = {}   # (variant, ref) -> [delta_pct]
    for r in rows:
        if r["variant"].startswith("_baseline"):
            continue
        bb = base_means.get((base_for(r["variant"]), r["env"], r["ref"]))
        if not bb:
            continue
        deltas.setdefault((r["variant"], r["ref"]), []).append(
            (bb - r["bogo"]) / bb * 100.0)

    out = []
    for (variant, ref), xs in sorted(deltas.items()):
        mu, sd = _mean(xs), _std(xs)
        out.append(dict(variant=variant, ref_load=ref,
                        throughput_delta_pct_mean=round(mu, 3),
                        throughput_delta_pct_std=(None if sd is None
                                                  else round(sd, 3)),
                        n=len(xs)))
    return out


# ---------------------------------------------------------------- output
def write_tsv(path, rows, columns):
    with open(path, "w") as fh:
        fh.write("\t".join(columns) + "\n")
        for r in rows:
            fh.write("\t".join("-" if r.get(c) is None else str(r.get(c))
                               for c in columns) + "\n")


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--campaign", required=True,
                    help="run tree from bench/gates/run-gates.sh")
    ap.add_argument("--out", default=None,
                    help="output directory (default: the run tree)")
    args = ap.parse_args()
    base = args.campaign.rstrip("/")
    out_dir = args.out or base
    os.makedirs(out_dir, exist_ok=True)

    overhead_limit = float(os.environ.get("INTP_GATE_OVERHEAD_PCT",
                                          str(OVERHEAD_PCT_DEFAULT)))
    gates = []

    # ---- fingerprint stability (rep-to-rep Pearson) -------------------------
    mats, workloads = rep_fingerprints(base)
    log(f"[load] {len(mats)} rep arm(s), {len(workloads)} common workload(s) "
        f"under {base}/solo")
    pairs = []
    if len(mats) >= 2 and workloads:
        mats_z, keep_z = zscore_per_metric(mats, workloads)
        pairs = pair_table(mats, mats_z, keep_z, workloads)
        pm = per_metric_table(mats, workloads)
        write_tsv(os.path.join(out_dir, "correlation-pairs.tsv"), pairs,
                  ["arm_a", "arm_b", "r_raw", "r_zscored", "n_features"])
        write_tsv(os.path.join(out_dir, "correlation-per-metric.tsv"), pm,
                  ["metric", "arm_a", "arm_b", "r", "n_workloads"])
        log(f"[fingerprint] {len(pairs)} rep pair(s) over "
            f"{len(workloads) * len(METRICS)} features -> "
            f"correlation-pairs.tsv, correlation-per-metric.tsv")

    finite = [p["r_raw"] for p in pairs if p["r_raw"] is not None]
    if len(mats) < 2:
        gates.append(("fingerprint", "SKIP",
                      f"need >=2 reps for rep-to-rep fingerprint correlation "
                      f"(have {len(mats)})"))
    elif not finite:
        gates.append(("fingerprint", "SKIP",
                      "no rep pair with >=3 shared finite features "
                      "(metrics mostly '--' on this host?)"))
    else:
        mean_r = _mean(finite)
        detail = (f"mean pairwise raw Pearson r={mean_r:.3f} over "
                  f"{len(finite)} pair(s) "
                  f"(min={min(finite):.3f}, threshold >= {FINGERPRINT_R_MIN})")
        gates.append(("fingerprint",
                      "PASS" if mean_r >= FINGERPRINT_R_MIN else "FAIL",
                      detail))

    # ---- overhead envelope ----------------------------------------------------
    ob = overhead_bounds(base)
    if ob:
        write_tsv(os.path.join(out_dir, "overhead-bounds.tsv"), ob,
                  ["variant", "ref_load", "throughput_delta_pct_mean",
                   "throughput_delta_pct_std", "n"])
        log(f"[overhead] overhead-bounds.tsv ({len(ob)} rows) from "
            f"{base}/overhead")
        bad = [r for r in ob
               if r["throughput_delta_pct_mean"] > overhead_limit]
        if bad:
            gates.append(("overhead", "FAIL",
                          f"{len(bad)}/{len(ob)} (variant,ref) cell(s) exceed "
                          f"{overhead_limit}%: " + ", ".join(
                              f"{r['variant']}/{r['ref_load']}="
                              f"{r['throughput_delta_pct_mean']:+.2f}%"
                              for r in bad)))
        else:
            worst = max(ob, key=lambda r: r["throughput_delta_pct_mean"])
            gates.append(("overhead", "PASS",
                          f"all {len(ob)} (variant,ref) cell(s) <= "
                          f"{overhead_limit}% (worst {worst['variant']}/"
                          f"{worst['ref_load']}="
                          f"{worst['throughput_delta_pct_mean']:+.2f}%)"))
    else:
        gates.append(("overhead", "SKIP",
                      f"no baseline/arm throughput.tsv pairs under "
                      f"{base}/overhead"))

    print()
    for name, result, detail in gates:
        print(f"GATE\t{name}\t{result}\t{detail}")
    return 1 if any(r == "FAIL" for _, r, _ in gates) else 0


if __name__ == "__main__":
    sys.exit(main())
