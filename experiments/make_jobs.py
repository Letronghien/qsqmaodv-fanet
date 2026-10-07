#!/usr/bin/env python3
"""
make_jobs.py — experiment design for the JADS revision (ns-3.48, QS-QMAODV v3).

QS-QMAODV v3 (protocol QSQMAODV) = QMAODV as published (hop-by-hop Q-learning,
MAC-ACK/delay reward) + queue-state extensions. Every extension is an attribute;
all off must reproduce QMAODV exactly (checked in the 'sanity' set).

Output: jobs_<set>.tsv  (job_id <TAB> csv_name <TAB> qsq-compare arguments)
Sets:
  diag     exploratory diagnosis on DEV seeds 1-20 (exploration, hop-by-hop, queue-driven exploration)
  smoke    20-s run of every protocol
  sanity   (a) QSQMAODV all-off == QMAODV (bit-identical), (b) queue signal active
  ablation_final  2x2 factorial S (source-side learning) x Q (queue information) + leave-one-out
                  + references, 3 scenarios, TEST seeds 31-60 (reported in the paper)
  main     families N, L, S, C x {AODV, PMAODV, QMAODV, QLAODV, QSQMAODV (+AOMDV)}, TEST seeds
  realism  multi-hop variant (300 m range, 11 Mbps broadcasts), convergecast / 5 random pairs
  sens     wq, beta, decay threshold, decay factor, gamma
"""
import argparse, itertools
from collections import Counter

# Seeds: 'dev' (1-30) was used while designing QS-QMAODV (first ablation);
# 'test' (31-60) is reserved for every result reported in the paper, so that no reported
# comparison uses runs that influenced the design.
DEV_SEEDS = range(1, 31)
TEST_SEEDS = range(31, 61)
SEEDS = TEST_SEEDS
# Compared protocols (restrict with --protocols).
PROTOS = ["AODV", "AOMDV", "PMAODV", "QMAODV", "QLAODV", "QSQMAODV"]
ALL_PROTOS = PROTOS
BASE = "--simTime=200"

def A(*dicts, **kw):
    """build a --qsqAttr string; later dicts / keywords override earlier ones"""
    merged = {}
    for d in dicts: merged.update(d)
    merged.update(kw)
    kw = merged
    def v(x): return ("true" if x else "false") if isinstance(x, bool) else str(x)
    return '--qsqAttr=' + ";".join(f"{k}={v(x)}" for k, x in kw.items())

# every QS-QMAODV extension off (incl. source-side learning) == QMAODV, bit-identical
OFF = dict(HopByHop=True, QueueRewardWeight=0, FailurePenalty=0, QueueAwareSelect=False,
           AckSilenceDecay=False, QueueDrivenExploration=False)
# queue information = queue reward + queue-aware selection + queue-driven exploration
Q_ON = dict(QueueRewardWeight=0.1, QueueAwareSelect=True, QueueDrivenExploration=True)
Q_OFF = dict(QueueRewardWeight=0, QueueAwareSelect=False, QueueDrivenExploration=False)
# v3 design (used by the diagnosis sets): hop-by-hop, no queue-driven exploration
V3 = dict(HopByHop=True, QueueDrivenExploration=False)

def jobs():
    J = []
    def add(set_, csv, tag, args, seeds=SEEDS):
        for s in seeds:
            J.append((f"{set_}|{tag}|s{s}", csv, f"{BASE} --seed={s} --tag={tag} {args}"))

    def ablation(set_, csv, seeds):
        """Final design: 2x2 factorial  S (source-side learning) x Q (queue information),
        decay and failure penalty on in all four cells; leave-one-out from FULL; references."""
        scen = {"A_N20_pi010": "--numNodes=20 --pktInterval=0.10 --meanVelMin=5 --meanVelMax=5",
                "B_N40_pi025": "--numNodes=40 --pktInterval=0.25 --meanVelMin=5 --meanVelMax=5",
                "M_N30_r300": "--numNodes=30 --pktInterval=0.10 --meanVelMin=15 --meanVelMax=15 "
                              "--rangeM=300 --bcast11=1"}
        for sname, sargs in scen.items():
            p = f"--protocol=QSQMAODV {sargs}"
            for s_, q in itertools.product([0, 1], [0, 1]):
                attrs = dict(Q_ON if q else Q_OFF, HopByHop=not bool(s_),
                             AckSilenceDecay=True, FailurePenalty=0.5)
                add(set_, csv, f"{sname}|S{s_}Q{q}", f"{p} {A(attrs)}", seeds)
            add(set_, csv, f"{sname}|OFF", f"{p} {A(OFF)}", seeds)
            for name, attrs in {"FULL-R": dict(QueueRewardWeight=0),
                                "FULL-S": dict(QueueAwareSelect=False),
                                "FULL-E": dict(QueueDrivenExploration=False),
                                "FULL-D": dict(AckSilenceDecay=False),
                                "FULL-P": dict(FailurePenalty=0)}.items():
                add(set_, csv, f"{sname}|{name}", f"{p} {A(attrs)}", seeds)
            add(set_, csv, f"{sname}|QMAODV", f"--protocol=QMAODV {sargs}", seeds)
            # QMAODV with source-side learning only (= QS-QMAODV with every queue feature off)
            add(set_, csv, f"{sname}|QMAODV_src", f"--protocol=QSQMAODV {A(OFF, HopByHop=False)} {sargs}", seeds)
            add(set_, csv, f"{sname}|AODV", f"--protocol=AODV {sargs}", seeds)

    # ---- smoke ----
    for pr in ALL_PROTOS:
        J.append((f"smoke|{pr}|s1", "smoke.csv",
                  f"--simTime=20 --seed=1 --tag=smoke --protocol={pr} --numNodes=10 --pktInterval=0.25"))

    # ---- sanity ----
    ref = "--numNodes=20 --pktInterval=0.10 --meanVelMin=5 --meanVelMax=5"
    add("sanity", "v2_sanity.csv", "qmaodv", f"--protocol=QMAODV {ref}", [1, 2, 3])
    add("sanity", "v2_sanity.csv", "qsq_off", f"--protocol=QSQMAODV {ref} {A(OFF)}", [1, 2, 3])
    add("sanity", "v2_sanity.csv", "qsq_full", f"--protocol=QSQMAODV {ref}", [1, 2, 3])
    add("sanity", "v2_sanity.csv", "qlaodv", f"--protocol=QLAODV {ref}", [1, 2, 3])
    add("sanity", "v2_sanity.csv", "aomdv", f"--protocol=AOMDV {ref}", [1, 2, 3])
    add("sanity", "v2_sanity.csv", "pmaodv", f"--protocol=PMAODV {ref}", [1, 2, 3])
    add("sanity", "v2_sanity.csv", "aodv", f"--protocol=AODV {ref}", [1, 2, 3])

    # ---- diagnosis 3 (DEV seeds 1-20) on the PUBLISHED baselines (mpaodv/aomdv): do the
    #      design conclusions of diag/diag2 hold?  Pre-specified rule: keep the final design
    #      if FULL beats QMAODV significantly (Holm) in at least 2 of the 3 scenarios. ----
    scen_d3 = {"A_N20_pi010": "--numNodes=20 --pktInterval=0.10 --meanVelMin=5 --meanVelMax=5",
               "B_N40_pi025": "--numNodes=40 --pktInterval=0.25 --meanVelMin=5 --meanVelMax=5",
               "M_N30_r300": "--numNodes=30 --pktInterval=0.10 --meanVelMin=15 --meanVelMax=15 "
                             "--rangeM=300 --bcast11=1"}
    for sname, sargs in scen_d3.items():
        variants = {
            "AODV":          "--protocol=AODV",
            "AOMDV":         "--protocol=AOMDV",
            "PMAODV":        "--protocol=PMAODV",
            "QMAODV":        "--protocol=QMAODV",
            "QLAODV":        "--protocol=QLAODV",
            "QSQ_src_only":  f"--protocol=QSQMAODV {A(OFF, HopByHop=False)}",
            "QSQ_hbh_queue": f"--protocol=QSQMAODV {A(HopByHop=True)}",
            "QSQ_FULL":      "--protocol=QSQMAODV",
        }
        for v, args in variants.items():
            add("diag3", "v2_diag3.csv", f"{sname}|{v}", f"{args} {sargs}", range(1, 21))

    # ---- diag / diag2 below: HISTORICAL (v3 module forked from the nbq QMAODV; results kept in
    #      data/v2). They cannot be re-run with the current module. ----
    # ---- diagnosis (DEV seeds 1-20): why is the QMAODV family below AODV, and does
    #      queue-driven exploration fix it?  Exploratory, not reported as a result. ----
    scen_d = {"A_N20_pi010": "--numNodes=20 --pktInterval=0.10 --meanVelMin=5 --meanVelMax=5",
              "B_N40_pi025": "--numNodes=40 --pktInterval=0.25 --meanVelMin=5 --meanVelMax=5",
              # genuinely multi-hop: 300 m radio range, broadcasts at the data rate
              "M_N30_r300": "--numNodes=30 --pktInterval=0.10 --meanVelMin=15 --meanVelMax=15 "
                            "--rangeM=300 --bcast11=1"}
    for sname, sargs in scen_d.items():
        variants = {
            "AODV":            "--protocol=AODV",
            "QMAODV":          "--protocol=QMAODV",
            "QMAODV_eps0.1":   "--protocol=QMAODV --qmEpsilon=0.1",
            "QMAODV_eps0.02":  "--protocol=QMAODV --qmEpsilon=0.02",
            "QMAODV_srconly":  "--protocol=QMAODV --qmAttr=HopByHop=false",
            "QSQ_FULL":        f"--protocol=QSQMAODV {A(V3)}",
            "QSQ_QDE":         f"--protocol=QSQMAODV {A(V3, QueueDrivenExploration=True)}",
            "QSQ_QDEonly":     f"--protocol=QSQMAODV {A(OFF, QueueDrivenExploration=True)}",
        }
        for v, args in variants.items():
            add("diag", "v2_diag.csv", f"{sname}|{v}", f"{args} {sargs}", range(1, 21))

    # ---- diagnosis 2 (DEV seeds 1-20): root cause found in diag = hop-by-hop forwarding into
    #      stale Q-table alternates (no-route drops). Test (a) pruning stale alternates and
    #      (b) source-only learning, each with and without the queue-state extensions. ----
    for sname, sargs in scen_d.items():
        variants = {
            "AODV":             "--protocol=AODV",
            "QMAODV":           "--protocol=QMAODV",
            "QMAODV_srconly":   "--protocol=QMAODV --qmAttr=HopByHop=false",
            "QSQ_prune_only":   f"--protocol=QSQMAODV {A(OFF, PruneStaleAlternates=True)}",
            "QSQ_prune_FULL":   f"--protocol=QSQMAODV {A(V3, PruneStaleAlternates=True)}",
            "QSQ_prune_FULL_QDE": f"--protocol=QSQMAODV {A(V3, PruneStaleAlternates=True, QueueDrivenExploration=True)}",
            "QSQ_src_FULL":     f"--protocol=QSQMAODV {A(V3, HopByHop=False)}",
            "QSQ_src_FULL_QDE": f"--protocol=QSQMAODV {A(V3, HopByHop=False, QueueDrivenExploration=True)}",
        }
        for v, args in variants.items():
            add("diag2", "v2_diag2.csv", f"{sname}|{v}", f"{args} {sargs}", range(1, 21))

    # ---- ablation ----
    # (the design-phase ablation on DEV seeds was run with the v3 design; its results are kept
    #  in data/v2/v2_ablation.csv and are not regenerated)
    ablation("ablation_final", "v2_ablation_final.csv", TEST_SEEDS) # reported in the paper

    # ---- main families ----
    fams = {
        "N": [f"--numNodes={n} --pktInterval=0.25 --meanVelMin=5 --meanVelMax=5" for n in (5, 10, 15, 20, 25, 30, 40, 50)],
        "L": [f"--numNodes=20 --pktInterval={pi} --meanVelMin=5 --meanVelMax=5" for pi in (0.05, 0.10, 0.25, 0.50, 1.00)],
        "S": [f"--numNodes=20 --pktInterval=0.25 --meanVelMin={v} --meanVelMax={v}" for v in (5, 15, 25, 35, 45)],
        "C": [f"--numNodes=20 --pktInterval={pi} --meanVelMin={v} --meanVelMax={v}"
              for pi, v in ((1.0, 5), (0.5, 15), (0.25, 25), (0.10, 35), (0.05, 45))],
    }
    for fam, conds in fams.items():
        for k, cargs in enumerate(conds):
            for pr in ALL_PROTOS:
                add("main", f"v2_family_{fam}.csv", f"{fam}{k}|{pr}", f"--protocol={pr} {cargs}")

    # ---- realism (multi-hop) ----
    for n in (20, 30, 40):
        for traffic, targs in (("conv", "--numFlows=0"), ("pairs5", "--numFlows=5")):
            for pr in ALL_PROTOS:
                add("realism", "v2_realism.csv", f"R{n}|{traffic}|{pr}",
                    f"--protocol={pr} --numNodes={n} --pktInterval=0.10 --meanVelMin=15 --meanVelMax=15 "
                    f"--rangeM=300 --bcast11=1 {targs}")

    # ---- sensitivity (QSQMAODV, reference scenario) ----
    p = f"--protocol=QSQMAODV {ref}"
    for w in (0.0, 0.05, 0.10, 0.20, 0.30):
        add("sens", "v2_sens.csv", f"wq:{w}", f"{p} {A(QueueRewardWeight=w)}")
    for b in (0.0, 0.25, 0.5, 1.0):
        add("sens", "v2_sens.csv", f"beta:{b}", f"{p} {A(QueueSelectBeta=b)}")
    for t in (5, 10, 15, 20, 30):
        add("sens", "v2_sens.csv", f"thr:{t}", f"{p} {A(AckSilenceThreshold=f'{t}s')}")
    for d in (0.85, 0.90, 0.92, 0.95, 0.99):
        add("sens", "v2_sens.csv", f"decay:{d}", f"{p} {A(DecayFactor=d)}")
    for r in (5, 10, 20, 50, 0):          # 0 = MAC queue MaxSize (500)
        add("sens", "v2_sens.csv", f"qref:{r}", f"{p} {A(QueueRefPackets=r)}")
    for g in (0.0, 0.5, 0.9):
        add("sens", "v2_sens.csv", f"gamma:{g}", f"{p} --qmGamma={g}")
    return J

if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--sets", default="sanity,ablation_final,main,realism,sens",
                    help="comma list of: smoke,sanity,diag3,ablation_final,main,realism,sens (diag, diag2: historical)")
    ap.add_argument("--protocols", default=",".join(PROTOS),   # AOMDV opt-in
                    help="restrict protocols, e.g. AODV,QMAODV,QSQMAODV")
    ap.add_argument("--out", default="jobs.tsv")
    a = ap.parse_args()
    keep, allowed = set(a.sets.split(",")), set(a.protocols.split(","))
    def proto_of(args):
        for tok in args.split():
            if tok.startswith("--protocol="): return tok.split("=", 1)[1]
        return "QSQMAODV"
    J = [j for j in jobs() if j[0].split("|")[0] in keep and proto_of(j[2]) in allowed]
    with open(a.out, "w") as f:
        for jid, csv, args in J:
            f.write(f"{jid}\t{csv}\t{args}\n")
    print(f"{len(J)} jobs -> {a.out}")
    for k, v in Counter(j[0].split('|')[0] for j in J).items():
        print(f"  {k:9s} {v}")
