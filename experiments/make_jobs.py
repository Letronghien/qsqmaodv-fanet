#!/usr/bin/env python3
"""
make_jobs.py — generate the job list for the JADS revision experiments (ns-3.48).

Output: jobs.tsv with columns  job_id <TAB> csv_name <TAB> qsq-compare arguments
Run them with:  bash experiments/run_jobs.sh experiments/jobs.tsv
Every job writes ALL parameters into its CSV row (no more row-order reconstruction).

Experiment sets (select with --sets, default = all):
  smoke    : 20-s runs of every protocol (build/installation check, ~1 min)
  sanity   : 3-seed checks (MAC signal non-zero, MAC feedback active, v1-switch variant)
  ablation : 2x2 factorial  MAC-queue information (Q) x ACK-silence decay (D)
             + all-features-off core and legacy-v1 reference, 2 scenarios, 30 seeds
  main     : families N, L, S, C  x  AODV, PMAODV, QMAODV, QS2MAODV(v2), 30 seeds
  realism  : multi-hop variant (300 m radio range, 11 Mbps broadcasts), convergecast
             and 5 random pairs, N in {20,30,40}
  sens     : gamma, w3 (adaptive OFF so w3 is really w3), threshold, decay factor
"""
import argparse, itertools

SEEDS = range(1, 31)
PROTOS = ["AODV", "PMAODV", "QMAODV", "QS2MAODV"]
BASE = "--simTime=200 --energy=0"
V1_SWITCHES = "--macQueueSignal=0 --macFeedback=0 --nextHopQueue=0 --decayPosOnly=0"
CORE_OFF = ("--macQueueSignal=0 --nextHopQueue=0 --qsW3=0 --adaptiveW3=0 --trendEps=0 "
            "--queueEps=0 --queueState=0 --hybridSelect=0 --enableDecay=0")

def jobs():
    J = []
    def add(set_, csv, tag, args, seeds=SEEDS):
        for s in seeds:
            J.append((f"{set_}|{tag}|s{s}", csv, f"{BASE} --seed={s} --tag={tag} {args}"))

    # ---------------- sanity ----------------
    add("sanity", "v2_sanity.csv", "v2default",
        "--protocol=QS2MAODV --numNodes=20 --pktInterval=0.10 --meanVelMin=5 --meanVelMax=5", [1, 2, 3])
    # v1 behaviour under ns-3.48 (numbers will NOT equal the old ns-3.40 CSVs: different simulator version)
    add("sanity", "v2_sanity.csv", "v1switches",
        f"--protocol=QS2MAODV --numNodes=20 --pktInterval=0.10 --meanVelMin=5 --meanVelMax=5 "
        f"--ackSilenceThreshold=15 --decayFactor=0.92 {V1_SWITCHES}", [1, 2, 3])
    for pr in PROTOS:
        J.append((f"smoke|{pr}|s1", "smoke.csv",
                  f"--simTime=20 --energy=0 --seed=1 --tag=smoke --protocol={pr} --numNodes=10 --pktInterval=0.25"))

    # ---------------- factorial ablation ----------------
    scen = {"A_N20_pi010": "--numNodes=20 --pktInterval=0.10 --meanVelMin=5 --meanVelMax=5",
            "B_N40_pi025": "--numNodes=40 --pktInterval=0.25 --meanVelMin=5 --meanVelMax=5"}
    for sname, sargs in scen.items():
        p = f"--protocol=QS2MAODV {sargs}"
        for q, d in itertools.product([0, 1], [0, 1]):
            qa = "--macQueueSignal=1 --nextHopQueue=1" if q else "--macQueueSignal=0 --nextHopQueue=0"
            add("ablation", "v2_ablation.csv", f"{sname}|Q{q}D{d}", f"{p} {qa} --enableDecay={d}")
        add("ablation", "v2_ablation.csv", f"{sname}|CORE", f"{p} {CORE_OFF}")
        add("ablation", "v2_ablation.csv", f"{sname}|V1", f"{p} {V1_SWITCHES}")
        add("ablation", "v2_ablation.csv", f"{sname}|AODV", sargs.replace("--numNodes", "--protocol=AODV --numNodes"))

    # ---------------- main families ----------------
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

    # ---------------- realism (multi-hop) ----------------
    for n in (20, 30, 40):
        for traffic, targs in (("conv", "--numFlows=0"), ("pairs5", "--numFlows=5 --randomPairs=1")):
            for pr in PROTOS:
                add("realism", "v2_realism.csv", f"R{n}|{traffic}|{pr}",
                    f"--protocol={pr} --numNodes={n} --pktInterval=0.10 --meanVelMin=15 --meanVelMax=15 "
                    f"--rangeM=300 --bcast11=1 {targs}")

    # ---------------- sensitivity ----------------
    ref = "--protocol=QS2MAODV --numNodes=20 --pktInterval=0.10 --meanVelMin=5 --meanVelMax=5"
    for g in (0.0, 0.5, 0.9):
        add("sens", "v2_sens.csv", f"gamma:{g}", f"{ref} --qsGamma={g}")
    for w in (0.0, 0.1, 0.2, 0.3):
        add("sens", "v2_sens.csv", f"w3:{w}", f"{ref} --qsW3={w} --adaptiveW3=0")
    for t in (5, 10, 15, 20, 30):
        add("sens", "v2_sens.csv", f"thr:{t}", f"{ref} --ackSilenceThreshold={t}")
    for d in (0.85, 0.90, 0.92, 0.95, 0.99):
        add("sens", "v2_sens.csv", f"decay:{d}", f"{ref} --decayFactor={d}")
    return J

if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--sets", default="sanity,ablation,main,realism,sens",
                    help="comma list of: smoke,sanity,ablation,main,realism,sens")
    ap.add_argument("--protocols", default="AODV,PMAODV,QMAODV,QS2MAODV",
                    help="drop baselines that are not installed, e.g. AODV,QS2MAODV")
    ap.add_argument("--out", default="jobs.tsv")
    a = ap.parse_args()
    keep = set(a.sets.split(","))
    allowed = set(a.protocols.split(","))
    def proto_of(args):
        for tok in args.split():
            if tok.startswith("--protocol="): return tok.split("=", 1)[1]
        return "QS2MAODV"
    J = [j for j in jobs() if j[0].split("|")[0] in keep and proto_of(j[2]) in allowed]
    with open(a.out, "w") as f:
        for jid, csv, args in J:
            f.write(f"{jid}\t{csv}\t{args}\n")
    from collections import Counter
    print(f"{len(J)} jobs -> {a.out}")
    for k, v in Counter(j[0].split('|')[0] for j in J).items():
        print(f"  {k:9s} {v}")
