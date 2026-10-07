#!/usr/bin/env python3
"""
analyze.py — statistics for the JADS revision (v2 CSVs, ns-3.48).

  python3 analysis/analyze.py --dir data/v2 --out reports/v2

Produces (in --out):
  sanity.txt                 MAC-signal / feedback / v1-reproduction / pairing checks
  main_all_conditions.csv    every family x condition x baseline x metric:
                             means, paired mean difference, 95% bootstrap CI,
                             Wilcoxon signed-rank (paired by seed), Holm-adjusted p,
                             two-sided Mann-Whitney p, Cliff's delta
  main_summary.md            compact tables for the paper (vs. every baseline)
  ablation_factorial.csv/.md main effects + interaction (synergy) of Q x D per scenario
  sens.csv                   sensitivity sweeps with 95% CI
Design notes
  * Runs are paired: the same seed gives the same mobility/traffic for every protocol
    (checked with TopoFingerprint). Paired tests are therefore used.
  * Holm correction is applied within (baseline, metric) across all conditions.
  * Delay is reported both per-flow-averaged (v1 definition) and packet-weighted.
"""
import argparse, os, glob
import numpy as np, pandas as pd
from scipy import stats

RNG = np.random.default_rng(2026)
METRICS = {"PDR": ("PDR (pp)", 100.0), "DelayPw_ms": ("Delay, packet-weighted (ms)", 1.0),
           "Delay_ms": ("Delay, per-flow mean (ms)", 1.0),
           "NRLall": ("Routing overhead, all control bytes incl. broadcast (% of data bytes)", 1.0),
           "NRLpkt": ("Control packets per delivered data packet", 1.0)}

def cliff(a, b):
    a = np.asarray(a)[:, None]; b = np.asarray(b)[None, :]
    return float(((a > b).sum() - (a < b).sum()) / (a.size * b.size))

def boot_ci(d, B=10000):
    d = np.asarray(d)
    if len(d) < 2: return (np.nan, np.nan)
    m = d[RNG.integers(0, len(d), (B, len(d)))].mean(1)
    return tuple(np.percentile(m, [2.5, 97.5]))

def holm(p):
    p = np.asarray(p, float); o = np.argsort(p); m = len(p); adj = np.empty(m); run = 0.0
    for k, i in enumerate(o):
        run = max(run, (m - k) * p[i]); adj[i] = min(1.0, run)
    return adj

def wilcox(a, b):
    d = np.asarray(a) - np.asarray(b)
    if len(d) < 5 or np.all(d == 0): return 1.0
    return stats.wilcoxon(a, b, zero_method="zsplit").pvalue

def paired(df, key_a, key_b, by, metric, scale):
    a = df[key_a].set_index("Seed")[metric] * scale
    b = df[key_b].set_index("Seed")[metric] * scale
    s = a.index.intersection(b.index)
    return a.loc[s].values, b.loc[s].values

def load(d, pattern):
    fs = sorted(glob.glob(os.path.join(d, pattern)))
    if not fs: return None
    return pd.concat([pd.read_csv(f) for f in fs], ignore_index=True)

# ------------------------------------------------------------------------------------------
def sanity(d, out, v1dir):
    lines = []
    df = load(d, "v2_sanity.csv")
    if df is not None:
        qm = df[df.Tag == "qmaodv"].set_index("Seed")
        off = df[df.Tag == "qsq_off"].set_index("Seed")
        full = df[df.Tag == "qsq_full"]
        for s in sorted(set(qm.index) & set(off.index)):
            same = all(qm.loc[s, k] == off.loc[s, k] for k in ("RxPkts", "TxPkts", "CtrlPktsAll"))
            lines.append(f"[equivalence] seed {s}: QMAODV RxPkts={qm.loc[s,'RxPkts']} "
                         f"QSQMAODV(all off) RxPkts={off.loc[s,'RxPkts']} -> {'IDENTICAL' if same else 'DIFFERENT'}")
        lines.append("   -> must be IDENTICAL: QS-QMAODV with every extension off is QMAODV.")
        if len(full):
            lines.append(f"[QSQ full] node MAC occupancy: mean={full.MeanMacQt.mean():.4f} "
                         f"frac>0={full.FracMacQtPos.mean():.3f} max={full.MaxMacQt.max():.3f}")
            if "MeanNhQ" in full:
                lines.append(f"[QSQ full] next-hop occupancy q_n: mean={full.MeanNhQ.mean():.4f} "
                             f"frac>0={full.FracNhQPos.mean():.3f}")
            lines.append(f"[QSQ full] MAC feedback ACK={full.FbAck.sum():.0f} drop={full.FbDrop.sum():.0f}"
                         + (f"  decayed={full.QsDecayed.sum():.0f}" if "QsDecayed" in full else ""))
            ql = df[df.Tag == "qlaodv"]
            if len(ql) and "QlMeanCandidates" in ql:
                lines.append(f"[QL-AODV] candidate routes per selection={ql.QlMeanCandidates.mean():.2f} "
                             f"(>1 needed for multipath learning)  selections/run={ql.QlSelections.mean():.0f}  "
                             f"MAC ACK={ql.FbAck.sum():.0f} fail={ql.FbDrop.sum():.0f} timeout={ql.FbExpired.sum():.0f}  "
                             f"PDR={100*ql.PDR.mean():.2f} %")
            for tg, nm in (("aodv", "AODV"), ("aomdv", "AOMDV"), ("pmaodv", "PMAODV")):
                am = df[df.Tag == tg]
                if len(am):
                    lines.append(f"[{nm}] PDR={100*am.PDR.mean():.2f} %  NRLall={am.NRLall.mean():.1f}")
            lines.append(f"[PDR] QMAODV={100*qm.PDR.mean():.2f} %  QSQ-off={100*off.PDR.mean():.2f} %  "
                         f"QSQ-full={100*full.PDR.mean():.2f} %  (3 seeds: not a result)")
    # pairing check on main families
    m = load(d, "v2_family_*.csv")
    if m is not None and "TopoFingerprint" in m:
        m["cond"] = m.Tag.str.split("|").str[0]
        nun = m.groupby(["cond", "Seed"]).TopoFingerprint.apply(lambda x: np.ptp(x.values))
        lines.append(f"[pairing] max spread of topology fingerprint across protocols (same cond, seed): {nun.max():.6f}"
                     f"  -> 0 means identical mobility for all protocols (paired tests valid)")
        lines.append(f"[connectivity] mean degree @250/500/1000 m: "
                     f"{m.Deg250.mean():.2f} / {m.Deg500.mean():.2f} / {m.Deg1000.mean():.2f}")
    path = os.path.join(out, "sanity.txt")
    open(path, "w").write("\n".join(lines) + "\n")
    lines.append(f"(saved to {os.path.abspath(path)})")
    print("\n".join(lines))

# ------------------------------------------------------------------------------------------
def main_families(d, out):
    m = load(d, "v2_family_*.csv")
    if m is None: print("[main] no data"); return
    m["cond"] = m.Tag.str.split("|").str[0]
    m["fam"] = m.cond.str[0]
    rows = []
    for (fam, cond), g in m.groupby(["fam", "cond"], sort=False):
        qs = g.Protocol == "QSQMAODV"
        for base in ("QMAODV", "QLAODV", "PMAODV", "AOMDV", "AODV"):
            bs = g.Protocol == base
            if not bs.any(): continue
            for met, (lab, sc) in METRICS.items():
                if met not in g: continue
                a, b = paired(g, qs, bs, "Seed", met, sc)
                if len(a) == 0: continue
                dlt = a - b; lo, hi = boot_ci(dlt)
                rows.append(dict(Family=fam, Cond=cond, Baseline=base, Metric=met, n=len(a),
                                 mean_QS=a.mean(), mean_base=b.mean(), delta=dlt.mean(), ci_lo=lo, ci_hi=hi,
                                 p_wilcoxon=wilcox(a, b),
                                 p_mw_two_sided=stats.mannwhitneyu(a, b, alternative="two-sided").pvalue,
                                 cliff=cliff(a, b)))
    T = pd.DataFrame(rows)
    T["p_holm"] = T.groupby(["Baseline", "Metric"]).p_wilcoxon.transform(holm)
    T.to_csv(os.path.join(out, "main_all_conditions.csv"), index=False, float_format="%.4f")
    md = ["# QS-QMAODV v2 vs baselines (paired by seed; Holm-adjusted Wilcoxon)\n"]
    for base in ("QMAODV", "QLAODV", "PMAODV", "AOMDV", "AODV"):
        for met in ("PDR", "DelayPw_ms", "NRLall", "NRLpkt"):
            t = T[(T.Baseline == base) & (T.Metric == met)]
            if t.empty: continue
            md.append(f"\n## {METRICS[met][0]} — QS-QMAODV minus {base}\n")
            md.append("| Family | Condition | QS | Base | Δ [95% CI] | p (Holm) | Cliff's δ |")
            md.append("|---|---|---|---|---|---|---|")
            for _, r in t.iterrows():
                md.append(f"| {r.Family} | {r.Cond} | {r.mean_QS:.2f} | {r.mean_base:.2f} | "
                          f"{r.delta:+.2f} [{r.ci_lo:+.2f}, {r.ci_hi:+.2f}] | {r.p_holm:.4f} | {r.cliff:+.3f} |")
            win = ((t.p_holm < 0.05) & (np.sign(t.delta) == (1 if met == "PDR" else -1))).sum()
            lose = ((t.p_holm < 0.05) & (np.sign(t.delta) == (-1 if met == "PDR" else 1))).sum()
            md.append(f"\nSignificantly better in {win}/{len(t)} conditions, significantly worse in {lose}/{len(t)}.\n")
    open(os.path.join(out, "main_summary.md"), "w").write("\n".join(md))
    print(f"[main] {len(T)} comparisons -> main_all_conditions.csv, main_summary.md")

# ------------------------------------------------------------------------------------------
def ablation(d, out, name="v2_ablation", label="ablation"):
    """Factorial ablation with two factors encoded in the tags as e.g. 'S1Q0' or 'Q1D0'
    (FULL = both factors 1), leave-one-out variants 'FULL-x', and reference variants.
    name: v2_ablation (design phase, v3, DEV seeds) or v2_ablation_final (TEST seeds)."""
    import re as _re
    a = load(d, name + ".csv")
    if a is None: print(f"[{label}] no data"); return
    a["scen"] = a.Tag.str.split("|").str[0]; a["var"] = a.Tag.str.split("|").str[1]
    nice = {"R": "queue reward", "S": "queue-aware selection",
            "E": "queue-driven exploration", "D": "ACK-silence decay", "P": "failure penalty"}
    rows, md = [], [f"# Ablation ({label})\n"]
    for scen, g in a.groupby("scen"):
        piv = {v: g[g["var"] == v].set_index("Seed") for v in g["var"].unique()}
        cells = {v: _re.fullmatch(r"([A-Z])(\d)([A-Z])(\d)", v) for v in piv}
        cells = {v: m.groups() for v, m in cells.items() if m}
        f1 = f2 = None
        if len(cells) == 4:
            f1, f2 = next(iter(cells.values()))[0], next(iter(cells.values()))[2]
        full = f"{f1}1{f2}1" if f1 else None
        refs = [v for v in ("AODV", "QMAODV", "QMAODV_src", "OFF") if v in piv]
        order = refs + sorted(cells) + sorted(v for v in piv if v.startswith("FULL-"))
        md.append(f"\n## Scenario {scen}\n\n| Variant | PDR (%) | Delay pw (ms) | Overhead NRLall (%) |\n|---|---|---|---|")
        for v in order:
            p = piv[v]
            md.append(f"| {v}{' (FULL)' if v == full else ''} | {100*p.PDR.mean():.2f} ± {100*p.PDR.std():.2f} | "
                      f"{p.DelayPw_ms.mean():.1f} | {p.NRLall.mean():.2f} |")
        if full:
            c00, c10, c01, c11 = f"{f1}0{f2}0", f"{f1}1{f2}0", f"{f1}0{f2}1", full
            seeds = sorted(set.intersection(*[set(piv[k].index) for k in (c00, c10, c01, c11)]))
            md.append(f"\n| Metric | Effect | Estimate [95% CI] | p (Wilcoxon) |\n|---|---|---|---|")
            for met, sc in (("PDR", 100), ("DelayPw_ms", 1), ("NRLall", 1)):
                y = {k: piv[k].loc[seeds, met].values * sc for k in (c00, c10, c01, c11)}
                eff = {f"{f1} main effect": (y[c10] + y[c11] - y[c00] - y[c01]) / 2,
                       f"{f2} main effect": (y[c01] + y[c11] - y[c00] - y[c10]) / 2,
                       f"{f1} x {f2} interaction": y[c11] - y[c10] - y[c01] + y[c00]}
                for nm, e in eff.items():
                    lo, hi = boot_ci(e); pv = wilcox(e, np.zeros_like(e))
                    rows.append(dict(Scenario=scen, Metric=met, Effect=nm, estimate=e.mean(),
                                     ci_lo=lo, ci_hi=hi, p=pv, n=len(e)))
                    md.append(f"| {met} | {nm} | {e.mean():+.2f} [{lo:+.2f}, {hi:+.2f}] | {pv:.4f} |")
            # FULL versus every other variant (leave-one-out and references), paired by seed
            md.append("\n| Compared with FULL | dPDR (pp) [95% CI] | p | dDelay (ms) | p | dOverhead | p |\n"
                      "|---|---|---|---|---|---|---|")
            others = [v for v in order if v != full]
            ps = []
            for v in others:
                sd = sorted(set(piv[v].index) & set(piv[full].index))
                dp = (piv[v].loc[sd, "PDR"].values - piv[full].loc[sd, "PDR"].values) * 100
                dd = piv[v].loc[sd, "DelayPw_ms"].values - piv[full].loc[sd, "DelayPw_ms"].values
                do = piv[v].loc[sd, "NRLall"].values - piv[full].loc[sd, "NRLall"].values
                ps.append((v, dp, dd, do))
            padj = holm([wilcox(dp, np.zeros_like(dp)) for _, dp, _, _ in ps]) if ps else []
            for (v, dp, dd, do), pa in zip(ps, padj):
                lab = v
                if v.startswith("FULL-"): lab = f"without {nice.get(v[5:], v[5:])}"
                lo, hi = boot_ci(dp)
                md.append(f"| {lab} | {dp.mean():+.2f} [{lo:+.2f}, {hi:+.2f}] | {pa:.4f} (Holm) | {dd.mean():+.1f} | "
                          f"{wilcox(dd, np.zeros_like(dd)):.4f} | {do.mean():+.2f} | {wilcox(do, np.zeros_like(do)):.4f} |")
                rows.append(dict(Scenario=scen, Metric="PDR", Effect=f"{lab} - FULL", estimate=dp.mean(),
                                 ci_lo=lo, ci_hi=hi, p=pa, n=len(dp)))
            md.append("\nNegative dPDR = the variant delivers less than FULL (the removed part helps).")
    pd.DataFrame(rows).to_csv(os.path.join(out, f"{label}_factorial.csv"), index=False, float_format="%.4f")
    open(os.path.join(out, f"{label}_factorial.md"), "w").write("\n".join(md))
    print(f"[{label}] -> {label}_factorial.csv/.md")


# ------------------------------------------------------------------------------------------
def sens(d, out):
    s = load(d, "v2_sens.csv")
    if s is None: print("[sens] no data"); return
    s["param"] = s.Tag.str.split(":").str[0]; s["value"] = s.Tag.str.split(":").str[1].astype(float)
    rows = []
    for (p, v), g in s.groupby(["param", "value"]):
        lo, hi = boot_ci(g.PDR.values * 100)
        rows.append(dict(param=p, value=v, PDR=g.PDR.mean() * 100, PDR_lo=lo, PDR_hi=hi,
                         DelayPw_ms=g.DelayPw_ms.mean(), NRLall=g.NRLall.mean(), n=len(g)))
    R = pd.DataFrame(rows)
    R.to_csv(os.path.join(out, "sens.csv"), index=False, float_format="%.3f")
    for p, g in R.groupby("param"):
        print(f"[sens] {p}: PDR range {g.PDR.min():.2f}–{g.PDR.max():.2f} % (spread {g.PDR.max()-g.PDR.min():.2f} pp)")

# ------------------------------------------------------------------------------------------
def diag(d, out, name="v2_diag", label="diag"):
    """Exploratory diagnosis (DEV seeds): exploration level, hop-by-hop vs source-only,
    queue-driven exploration; loss reasons (TTL = loops/detours, no route, MAC)."""
    g0 = load(d, name + ".csv")
    if g0 is None: print(f"[{label}] no data"); return
    g0["scen"] = g0.Tag.str.split("|").str[0]; g0["var"] = g0.Tag.str.split("|").str[1]
    order = ["AODV", "QMAODV", "QMAODV_eps0.1", "QMAODV_eps0.02", "QMAODV_srconly",
             "QSQ_FULL", "QSQ_QDE", "QSQ_QDEonly", "QSQ_prune_only", "QSQ_prune_FULL",
             "QSQ_prune_FULL_QDE", "QSQ_src_FULL", "QSQ_src_FULL_QDE",
             "AOMDV", "PMAODV", "QLAODV", "QSQ_src_only", "QSQ_hbh_queue"]
    md = [f"# {label} (DEV seeds 1-20, exploratory)\n"]
    for scen, g in g0.groupby("scen"):
        piv = {v: g[g["var"] == v].set_index("Seed") for v in g["var"].unique()}
        md.append(f"\n## {scen}\n")
        md.append("| Variant | PDR % (SD) | dPDR vs AODV [95% CI] p | dPDR vs QMAODV [95% CI] p | Delay ms | "
                  "Overhead % | Fwd/Tx | TTL drops | NoRoute drops | MAC drops |")
        md.append("|---|---|---|---|---|---|---|---|---|---|")
        def cmp(v, ref):
            if ref not in piv or v == ref: return "-"
            sd = sorted(set(piv[v].index) & set(piv[ref].index))
            x = (piv[v].loc[sd, "PDR"].values - piv[ref].loc[sd, "PDR"].values) * 100
            lo, hi = boot_ci(x)
            return f"{x.mean():+.2f} [{lo:+.2f}, {hi:+.2f}] p={wilcox(x, np.zeros_like(x)):.3f}"
        for v in [v for v in order if v in piv]:
            q = piv[v]
            col = lambda k, f="{:.1f}": f.format(q[k].mean()) if k in q else "-"
            md.append(f"| {v} | {100*q.PDR.mean():.2f} ({100*q.PDR.std():.1f}) | {cmp(v, 'AODV')} | "
                      f"{cmp(v, 'QMAODV')} | {col('DelayPw_ms')} | {col('NRLall')} | {col('FwdPerTx', '{:.2f}')} | "
                      f"{col('DropTtl', '{:.0f}')} | {col('DropNoRoute', '{:.0f}')} | {col('MacDrops', '{:.0f}')} |")
    open(os.path.join(out, f"{label}.md"), "w").write("\n".join(md) + "\n")
    print(f"[{label}] -> {label}.md")

if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--dir", default="data/v2")
    ap.add_argument("--v1dir", default=None, help="(unused since ns-3.48; kept for compatibility)")
    ap.add_argument("--out", default="reports/v2")
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    sanity(a.dir, a.out, a.v1dir)
    diag(a.dir, a.out)
    diag(a.dir, a.out, "v2_diag2", "diag2")
    diag(a.dir, a.out, "v2_diag3", "diag3")
    main_families(a.dir, a.out)
    ablation(a.dir, a.out, "v2_ablation", "ablation")              # design phase
    ablation(a.dir, a.out, "v2_ablation_final", "ablation_final")  # reported
    sens(a.dir, a.out)
