#!/usr/bin/env python3
"""
make_jobs.py — experiment design for the JADS revision (ns-3.48, QS-QMAODV v3).

QS-QMAODV v3 (protocol QSQMAODV) = QMAODV as published (hop-by-hop Q-learning,
MAC-ACK/delay reward) + queue-state extensions. Every extension is an attribute;
all off must reproduce QMAODV exactly (checked in the 'sanity' set).

Output: jobs_<set>.tsv  (job_id <TAB> csv_name <TAB> qsq-compare arguments)
Sets:
  smoke    20-s run of every protocol
  sanity   (a) QSQMAODV all-off == QMAODV (bit-identical), (b) queue signal active
  ablation 2x2 factorial  Q (queue information: reward + selection + trend) x D (decay)
           + leave-one-out from FULL + references, 2 scenarios x 30 seeds
  main     families N, L, S, C x {AODV, PMAODV, QMAODV, QSQMAODV} x 30 seeds
  realism  multi-hop variant (300 m range, 11 Mbps broadcasts), convergecast / 5 random pairs
  sens     wq, beta, decay threshold, decay factor, gamma
"""
import argparse, itertools
from collections import Counter

SEEDS = range(1, 31)
PROTOS = ["AODV", "PMAODV", "QMAODV", "QSQMAODV"]
BASE = "--simTime=200 --energy=0"

def A(**kw):
    """build a --qsqAttr string"""
    def v(x): return ("true" if x else "false") if isinstance(x, bool) else str(x)
    return '--qsqAttr=' + ";".join(f"{k}={v(x)}" for k, x in kw.items())

OFF = dict(QueueRewardWeight=0, FailurePenalty=0, QueueAwareSelect=False,
           AckSilenceDecay=False, TrendEpsilon=False)
Q_ON = dict(QueueRewardWeight=0.1, AdaptiveQueueWeight=True, QueueAwareSelect=True, TrendEpsilon=True)
Q_OFF = dict(QueueRewardWeight=0, QueueAwareSelect=False, TrendEpsilon=False)

def jobs():
    J = []
    def add(set_, csv, tag, args, seeds=SEEDS):
        for s in seeds:
            J.append((f"{set_}|{tag}|s{s}", csv, f"{BASE} --seed={s} --tag={tag} {args}"))

    # ---- smoke ----
    for pr in PROTOS:
        J.append((f"smoke|{pr}|s1", "smoke.csv",
                  f"--simTime=20 --energy=0 --seed=1 --tag=smoke --protocol={pr} --numNodes=10 --pktInterval=0.25"))

    # ---- sanity ----
    ref = "--numNodes=20 --pktInterval=0.10 --meanVelMin=5 --meanVelMax=5"
    add("sanity", "v2_sanity.csv", "qmaodv", f"--protocol=QMAODV {ref}", [1, 2, 3])
    add("sanity", "v2_sanity.csv", "qsq_off", f"--protocol=QSQMAODV {ref} {A(**OFF)}", [1, 2, 3])
    add("sanity", "v2_sanity.csv", "qsq_full", f"--protocol=QSQMAODV {ref}", [1, 2, 3])

    # ---- ablation ----
    scen = {"A_N20_pi010": "--numNodes=20 --pktInterval=0.10 --meanVelMin=5 --meanVelMax=5",
            "B_N40_pi025": "--numNodes=40 --pktInterval=0.25 --meanVelMin=5 --meanVelMax=5"}
    for sname, sargs in scen.items():
        p = f"--protocol=QSQMAODV {sargs}"
        for q, d in itertools.product([0, 1], [0, 1]):
            attrs = dict(Q_ON if q else Q_OFF, AckSilenceDecay=bool(d), FailurePenalty=0.5)
            add("ablation", "v2_ablation.csv", f"{sname}|Q{q}D{d}", f"{p} {A(**attrs)}")
        add("ablation", "v2_ablation.csv", f"{sname}|OFF", f"{p} {A(**OFF)}")
        for name, attrs in {"FULL-R": dict(QueueRewardWeight=0), "FULL-S": dict(QueueAwareSelect=False),
                            "FULL-T": dict(TrendEpsilon=False), "FULL-P": dict(FailurePenalty=0)}.items():
            add("ablation", "v2_ablation.csv", f"{sname}|{name}", f"{p} {A(**attrs)}")
        add("ablation", "v2_ablation.csv", f"{sname}|QMAODV", f"--protocol=QMAODV {sargs}")
        add("ablation", "v2_ablation.csv", f"{sname}|AODV", f"--protocol=AODV {sargs}")

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
            for pr in PROTOS:
                add("main", f"v2_family_{fam}.csv", f"{fam}{k}|{pr}", f"--protocol={pr} {cargs}")

    # ---- realism (multi-hop) ----
    for n in (20, 30, 40):
        for traffic, targs in (("conv", "--numFlows=0"), ("pairs5", "--numFlows=5 --randomPairs=1")):
            for pr in PROTOS:
                add("realism", "v2_realism.csv", f"R{n}|{traffic}|{pr}",
                    f"--protocol={pr} --numNodes={n} --pktInterval=0.10 --meanVelMin=15 --meanVelMax=15 "
                    f"--rangeM=300 --bcast11=1 {targs}")

    # ---- sensitivity (QSQMAODV, reference scenario) ----
    p = f"--protocol=QSQMAODV {ref}"
    for w in (0.0, 0.05, 0.10, 0.20, 0.30):
        add("sens", "v2_sens.csv", f"wq:{w}", f"{p} {A(QueueRewardWeight=w, AdaptiveQueueWeight=False)}")
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
    ap.add_argument("--sets", default="sanity,ablation,main,realism,sens",
                    help="comma list of: smoke,sanity,ablation,main,realism,sens")
    ap.add_argument("--protocols", default=",".join(PROTOS),
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
