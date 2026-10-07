#!/usr/bin/env python3
"""
make_figures.py — publication figures (PDF + 300-dpi PNG) from data/v2.

  python3 analysis/make_figures.py --dir data/v2 --out reports/v2/figures

Figures carry no titles (captions belong to the manuscript); every point is the mean over
seeds with a 95 % confidence interval (t distribution). Colours: Okabe-Ito (colour-blind safe),
and every protocol also has its own marker so the plots stay readable in grey scale.
"""
import argparse, glob, os
import numpy as np, pandas as pd
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from scipy import stats

ORDER = ["AODV", "AOMDV", "PMAODV", "QMAODV", "QLAODV", "QSQMAODV"]
LABEL = {"AODV": "AODV", "AOMDV": "AOMDV", "PMAODV": "PMAODV", "QMAODV": "QMAODV",
         "QLAODV": "QL-AODV", "QSQMAODV": "QS-QMAODV (proposed)"}
COLOR = {"AODV": "#999999", "AOMDV": "#56B4E9", "PMAODV": "#009E73", "QMAODV": "#E69F00",
         "QLAODV": "#0072B2", "QSQMAODV": "#D55E00"}
MARK = {"AODV": "o", "AOMDV": "v", "PMAODV": "s", "QMAODV": "^", "QLAODV": "D", "QSQMAODV": "*"}
METRICS = [("PDR", 100, "Packet delivery ratio (%)"),
           ("DelayPw_ms", 1, "End-to-end delay (ms)"),
           ("NRLall", 1, "Overhead (% of data bytes)")]
FAMILY_X = {"N": ("nNodes", "Number of UAVs"),
            "L": ("PktInterval_s", "Packet interval per source (s)"),
            "S": ("Speed_ms", "Mean UAV speed (m/s)"),
            "C": (None, "Combined load / mobility level")}

plt.rcParams.update({"font.family": "serif", "font.size": 9, "axes.grid": True,
                     "grid.alpha": 0.3, "legend.fontsize": 8, "figure.dpi": 100})


def ci95(x):
    x = np.asarray(x, float)
    if len(x) < 2: return 0.0
    return stats.t.ppf(0.975, len(x) - 1) * x.std(ddof=1) / np.sqrt(len(x))


def save(fig, out, name):
    os.makedirs(out, exist_ok=True)
    fig.savefig(os.path.join(out, name + ".pdf"), bbox_inches="tight")
    fig.savefig(os.path.join(out, name + ".png"), dpi=300, bbox_inches="tight")
    plt.close(fig)
    print("  figure:", name)


def load(d, pattern):
    fs = sorted(glob.glob(os.path.join(d, pattern)))
    return pd.concat([pd.read_csv(f) for f in fs], ignore_index=True) if fs else None


def families(d, out):
    m = load(d, "v2_family_*.csv")
    if m is None: return
    m["cond"] = m.Tag.str.split("|").str[0]
    m["fam"] = m.cond.str[0]
    for fam, g in m.groupby("fam"):
        xcol, xlab = FAMILY_X[fam]
        if xcol is None:
            g = g.assign(xv=g.cond.str[1:].astype(int) + 1)
            xticks = sorted(g.xv.unique())
        else:
            g = g.assign(xv=g[xcol])
            xticks = None
        fig, axes = plt.subplots(1, 3, figsize=(7.2, 2.4))
        for ax, (met, sc, ylab) in zip(axes, METRICS):
            for pr in [p for p in ORDER if p in set(g.Protocol)]:
                s = g[g.Protocol == pr].groupby("xv")[met]
                mu, ci = s.mean() * sc, s.apply(ci95) * sc
                ax.errorbar(mu.index, mu.values, yerr=ci.values, label=LABEL[pr], color=COLOR[pr],
                            marker=MARK[pr], ms=4, lw=1.2 if pr != "QSQMAODV" else 1.8, capsize=2)
            ax.set_xlabel(xlab if xcol else "Level (L1 ... L5)")
            ax.set_ylabel(ylab)
            if xticks: ax.set_xticks(xticks)
        h, l = axes[0].get_legend_handles_labels()
        fig.legend(h, l, loc="upper center", ncol=len(l), frameon=False, bbox_to_anchor=(0.5, 1.08))
        fig.tight_layout()
        save(fig, out, f"family_{fam}")


def ablation(d, out, name):
    a = load(d, name + ".csv")
    if a is None: return
    a["scen"] = a.Tag.str.split("|").str[0]; a["var"] = a.Tag.str.split("|").str[1]
    import re as _re
    refs = ["AODV", "QMAODV", "QMAODV_src", "OFF"]
    cells = sorted(v for v in a["var"].unique() if _re.fullmatch(r"[A-Z]\d[A-Z]\d", v))
    order = refs + cells + sorted(v for v in a["var"].unique() if v.startswith("FULL-"))
    fullcell = next((v for v in cells if v.endswith("1") and v[1] == "1"), None)
    nice = {"OFF": "QS all off", "QMAODV_src": "QMAODV (source)"}
    if fullcell: nice[fullcell] = "FULL"
    scens = sorted(a.scen.unique())
    fig, axes = plt.subplots(len(scens), 2, figsize=(7.2, 2.3 * len(scens)), squeeze=False)
    for r, sc_ in enumerate(scens):
        g = a[a.scen == sc_]
        vs = [v for v in order if v in set(g["var"])]
        for c, (met, k, ylab) in enumerate(METRICS[:2]):
            mu = [g[g["var"] == v][met].mean() * k for v in vs]
            ci = [ci95(g[g["var"] == v][met] * k) for v in vs]
            col = ["#D55E00" if v == fullcell else "#999999" if v in refs else "#56B4E9" for v in vs]
            axes[r][c].bar(range(len(vs)), mu, yerr=ci, color=col, capsize=2)
            axes[r][c].set_xticks(range(len(vs)), [nice.get(v, v) for v in vs], rotation=45, ha="right")
            axes[r][c].set_ylabel(ylab)
            axes[r][c].text(0.01, 0.97, sc_, transform=axes[r][c].transAxes, va="top", fontsize=7)
    fig.tight_layout()
    save(fig, out, name.replace("v2_", ""))


def sensitivity(d, out):
    s = load(d, "v2_sens.csv")
    if s is None: return
    s["param"] = s.Tag.str.split(":").str[0]; s["value"] = s.Tag.str.split(":").str[1].astype(float)
    params = sorted(s.param.unique())
    lab = {"wq": "Queue reward weight $w_q$", "beta": r"Selection weight $\beta$",
           "thr": r"ACK-silence threshold $\tau$ (s)", "decay": "Decay factor",
           "gamma": r"Discount $\gamma$", "qref": "$Q_{ref}$ (packets; 0 = 500)"}
    fig, axes = plt.subplots(1, len(params), figsize=(1.9 * len(params), 2.2), squeeze=False)
    for ax, p in zip(axes[0], params):
        g = s[s.param == p].groupby("value").PDR
        ax.errorbar(g.mean().index, g.mean().values * 100, yerr=g.apply(ci95).values * 100,
                    color=COLOR["QSQMAODV"], marker="o", ms=3, capsize=2)
        ax.set_xlabel(lab.get(p, p)); ax.set_ylabel("PDR (%)")
    fig.tight_layout()
    save(fig, out, "sensitivity")


def realism(d, out):
    r = load(d, "v2_realism.csv")
    if r is None: return
    r["n"] = r.Tag.str.split("|").str[0].str[1:].astype(int); r["traffic"] = r.Tag.str.split("|").str[1]
    fig, axes = plt.subplots(1, 2, figsize=(7.2, 2.4))
    for ax, tr in zip(axes, ["conv", "pairs5"]):
        g = r[r.traffic == tr]
        prs = [p for p in ORDER if p in set(g.Protocol)]
        ns = sorted(g.n.unique()); w = 0.8 / max(1, len(prs))
        for i, pr in enumerate(prs):
            s = g[g.Protocol == pr].groupby("n").PDR
            ax.bar(np.arange(len(ns)) + i * w, s.mean().values * 100, w, yerr=s.apply(ci95).values * 100,
                   color=COLOR[pr], label=LABEL[pr], capsize=1.5)
        ax.set_xticks(np.arange(len(ns)) + w * (len(prs) - 1) / 2, [str(n) for n in ns])
        ax.set_xlabel("Number of UAVs (" + ("convergecast" if tr == "conv" else "5 random flows") + ")")
        ax.set_ylabel("PDR (%)")
    h, l = axes[0].get_legend_handles_labels()
    fig.legend(h, l, loc="upper center", ncol=len(l), frameon=False, bbox_to_anchor=(0.5, 1.08))
    fig.tight_layout()
    save(fig, out, "multihop")


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--dir", default="data/v2")
    ap.add_argument("--out", default="reports/v2/figures")
    a = ap.parse_args()
    families(a.dir, a.out)
    ablation(a.dir, a.out, "v2_ablation_final")
    ablation(a.dir, a.out, "v2_ablation")
    sensitivity(a.dir, a.out)
    realism(a.dir, a.out)
