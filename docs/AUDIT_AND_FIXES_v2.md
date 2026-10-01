> **Note (ns-3.48 layout):** paths in this document refer to the old repository. In the new project: `compare-sim-v2.cc` = `scratch/qsq-compare.cc`, `scripts/v2/*` = `experiments/` and `analysis/`, results = `data/v2`. Under ns-3.48 the v1 reproduction check is qualitative only (different simulator version).

# QS-QMAODV — technical revision (v2) for the JADS submission

This folder is a drop-in update for `github.com/Letronghien/qs-qmaodv-ns3`.
All C++ files were syntax-checked against the ns-3.40 headers (`g++ -std=c++20 -fsyntax-only`);
they have **not** been linked or executed here — run the sanity set first (section 4).

## 1. What the audit of v1 (code + CSV) found

| # | Finding | Evidence | Consequence for the paper |
|---|---------|----------|---------------------------|
| 1 | **MAC queue term was always 0.** `GetTxopQueue(AC_BE)` returns `nullptr` on an 802.11b `AdhocWifiMac` (QosSupported = false ⇒ no EDCA queues); the code then `continue`s. | ns-3.40 `WifiMac::GetTxopQueue` / `SetQosSupported`; `compare-sim.cc` never enables QoS | q_t was only the AODV *route-discovery buffer* (`RequestQueue`). The central claim (MAC BE queue in the reward) was not tested by any v1 experiment. |
| 2 | "ACK" = neighbour-freshness check at send time (`LookupRoute(nextHop)` valid ⇒ ack=1, delay fixed 0.005 s). | `RouteOutput`, lines ~479–487 | No delivery feedback; the delay term is a constant. |
| 3 | Q-learning only at the **source** (`RouteOutput`); intermediate nodes forward with plain AODV (`Forwarding`). | `Forwarding()` | "per-forwarding-event" must become "per source packet". |
| 4 | Reward uses **1/(1+delay)**, not 1/(HC+1); state = (dst, queue bucket); selection = **Q·(1−q_a)^0.5 with q_a = HC/HC_max** (a hop-count proxy, not a queue). | `qs2maodv-qtable.cc` | Paper equations and Algorithm 1 do not describe the code. |
| 5 | ACK-silence decay **resets negative Q to 0** (multiply then clamp). Failing routes stop being selected ⇒ no lastAck ⇒ they are exactly the ones "decayed" ⇒ their penalty is erased. | `DecayStaleRoutes` | Mechanism is the opposite of what the paper says. |
| 6 | Parameters differ from Table 5: α=0.30, ε0=0.30 (0.10–0.50, queue-triggered), w1=0.40, w2=0.50; 3-D Gauss-Markov 1000×1000×300 m; **N−1 sources → sink 0** (convergecast), apps 5–200 s; MAC queue 500 p; no 250 m range (Friis 16 dBm: data@11 Mbps ≈1 km, broadcasts@1 Mbps reach the whole box). | `compare-sim.cc`, CSV `nFlows=19` | Table 5 must be rewritten. Network is mostly 1–2 hops. |
| 7 | QMAODV baseline is the **separate qmaodv module** (α=0.5, ε=0.5, w1=0.6, w2=0.4), not "qs2maodv with w3=0". | `compare-sim.cc` | Statement in Implementation section is wrong. |
| 8 | **Family E2:** results bit-identical for E0 = 1, 2, 5, 10, 20 J; consumed energy = 72–73 % of E0 in every run (also 50 J) ⇒ every battery hit the depletion cutoff but networking was unaffected; `LiveNodes` (rem > 1e-9) can never drop. | `family_E2.csv`, all families | Remove E2 and all energy claims (or redo with a working energy model). |
| 9 | w3 = 0.40 and 0.50 give identical runs (cap 0.40); with AdaptiveW3 on, w3 = 0 is not "off". Family W does not match W2/W3 at the same nominal config (different code version). | `family_W.csv` | Family W cannot be used as is. |
| 10 | Families overlap: N@20 = L@0.25, C-L3 = S@25, E2 = S@5 (identical runs). | CSV | Not 5 independent families. |
| 11 | vs **AODV / PMAODV**: QS-QMAODV is never significantly better (PDR: 0/23 better, 2–3/23 worse after Holm). vs QMAODV: better in 11/23 PDR and 8/23 delay cells. | `reanalysis_v1_all_conditions.csv` | Abstract/conclusion cannot claim improvement over the reactive baselines. |

## 2. Code changes (all switchable — set every switch false to reproduce v1 exactly)

| Attribute (`ns3::qs2maodv::RoutingProtocol`) | Default v2 | v1 behaviour | Purpose |
|---|---|---|---|
| `MacQueueSignal` | true | false | read AC_BE (QoS) **or AC_BE_NQOS (non-QoS)** queue |
| `NextHopQueue` | true | false | occupancy of the BE queue **towards each candidate next hop** (`WifiContainerQueueId`, RA = next-hop MAC via ARP); used in the hybrid score (replaces HC proxy) and in the reward |
| `MacFeedback` | true | false | reward computed when the MAC reports `AckedMpdu` (ack=1) or `DroppedMpdu` (ack=0) for that packet; delay = real hop time (queue + access + retries) |
| `DelayScale` | 0.05 s | 1 s | delay term 1/(1+d/scale) — otherwise ≈1 for ms delays |
| `DecayPositiveOnly` | true | false | decay shrinks positive Q only; negative penalties are kept |
| `QueueState`, `QueueEpsilon`, `HybridSelect` | true | true | ablation switches |
| `PendingTimeout` | 2 s | – | forget decisions with no MAC outcome |

`GetQueueStats()` exposes per-node diagnostics (mean/max MAC occupancy, fraction of samples > 0,
MAC ACK/drop counts) that `compare-sim-v2` writes to the CSV — this is the evidence the reviewers will ask for.

`scratch/compare-sim-v2.cc` (the v1 `compare-sim.cc` is untouched):
every swept parameter in the CSV (no row-order reconstruction), energy OFF by default, packet-weighted delay,
topology fingerprint (verifies paired seeds), node degree at 250/500/1000 m, options
`--rangeM`, `--bcast11`, `--randomPairs`, `--qos`, `--macQueuePkts`, `--tag`.

## 3. Install

```bash
cp -r src/qs2maodv/*            ~/ns-allinone-3.40-qs2maodv/ns-3.40/src/qs2maodv/
cp scratch/compare-sim-v2.cc    ~/ns-allinone-3.40-qs2maodv/ns-3.40/scratch/
cd ~/ns-allinone-3.40-qs2maodv/ns-3.40 && ./ns3 build
# or: git apply qs-qmaodv-v2.patch   (from the repo root)
```

## 4. Run (order matters)

```bash
cd scripts/v2
python3 make_jobs_v2.py --sets sanity --out jobs_sanity.tsv
bash run_jobs_v2.sh jobs_sanity.tsv
python3 analyze_v2.py --dir ~/QS-QMAODV-results/v2 --v1dir ../../results --out report_v2
```
**Go/no-go:** `sanity.txt` must show `FracMacQtPos > 0`, `FbAck > 0`, and the `v1repro` lines
`IDENTICAL` (proves the switches reproduce v1). Then:

```bash
python3 make_jobs_v2.py --sets ablation,main,realism,sens      # 4410 runs
MAX_JOBS=3 bash run_jobs_v2.sh jobs_v2.tsv
python3 analyze_v2.py --dir ~/QS-QMAODV-results/v2 --out report_v2
```
Start with `--sets ablation` (420 runs): it answers whether MAC-queue information helps at all
(Q main effect) and whether there is real synergy with decay (Q×D interaction, per-seed contrast).
Decide the paper's storyline from that result before running everything else.

## 5. Re-analysis of v1 data
`python3 scripts/v2/reanalyze_v1.py results` → paired Wilcoxon (seeds are common random numbers),
95 % bootstrap CI, Holm correction across all conditions, vs all three baselines.
