# QS-QMAODV — final design (module `src/qsqmaodv`, protocol `QSQMAODV`)

## How the design was reached (DEV seeds 1–30, not used for any reported result)
1. **v3** = published QMAODV (hop-by-hop Q-learning, MAC-ACK/delay reward) + queue-state terms.
   Design-phase ablation: no queue component had a significant effect; QMAODV and every v3
   variant delivered 8–9 pp less than plain AODV (`reports/v2/ablation_factorial.md`).
2. **diag** (20 seeds, 3 scenarios incl. a 300 m multi-hop one): lowering ε did not help;
   **switching off hop-by-hop learning recovered +5 to +7.5 pp** in all scenarios.
   Data-packet losses were dominated by *no-route* drops (QMAODV 6 008 vs AODV 3 406 in A),
   not TTL expiry: intermediate nodes kept choosing Q-table alternates through neighbours
   that had already reported the destination unreachable.
3. **diag2**: pruning those alternates (`PruneStaleAlternates`) was not consistent across
   scenarios; **source-side learning + queue state + queue-driven exploration** was the only
   variant better than QMAODV in all three scenarios (+9.5, +5.5, +7.1 pp; p ≤ 0.002) and
   statistically indistinguishable from AODV (+1.7, −2.9, −1.9 pp; p ≥ 0.13).
   It also had the lowest MAC drops and routing overhead in scenario A.

## Change of base implementation (v5)
Steps 1–3 used a QMAODV module taken from the NBQ-MAODV project tree. The authors' published
implementations are in github.com/Letronghien/ao-am-pm-qm-sa (module `mpaodv`: PMAODV, QMAODV;
module `aomdv`), vendored in `src/`. The module previously imported as "PMAODV" was in fact a PM-AOMDV
re-implementation on top of AOMDV and was dropped. QS-QMAODV v5 is a fork of `mpaodv` with the
same extensions; the diff is `docs/diffs/qsqmaodv_vs_mpaodv.diff`. Because the base changed,
the conclusions of steps 2–3 are re-checked on DEV seeds with `make run SETS=diag3` before any
TEST run (pre-specified rule: keep the design if FULL beats QMAODV significantly, Holm-adjusted,
in at least 2 of the 3 scenarios).

## Final protocol (defaults of `ns3::qsqmaodv::RoutingProtocol`)
| Component | Attributes (default) | Effect |
|---|---|---|
| Source-side learning | `HopByHop` (**false**) | the source chooses the first hop with Q-learning; intermediate nodes forward on their AODV route (QMAODV: true) |
| R — queue reward | `QueueRewardWeight` (0.10, fixed) | r = s·(w1·ACK + w2/(1 + d_ms)) + wq·ACK·(1 − q_n) − P·(1 − ACK), s = (w1 + w2 − wq)/(w1 + w2) |
| S — queue-aware selection | `QueueAwareSelect` (true), `QueueSelectBeta` (0.5) | exploitation picks argmax [Q − β·q_n·max|Q|] |
| E — queue-driven exploration | `QueueDrivenExploration` (true), `ExplorationFloor` (0.01) | per decision: explore with probability floor + (ε − floor)·q of the greedy next hop |
| D — ACK-silence decay | `AckSilenceDecay` (true), `AckSilenceThreshold` (15 s), `DecayFactor` (0.92) | positive Q without MAC ACK for > τ shrinks; penalties are kept |
| P — failure penalty | `FailurePenalty` (0.5) | a MAC drop is punished |

ε follows the QMAODV schedule (0.5, −0.02 every 10 s); α, γ and w1, w2 are QMAODV's.

## Base module
`src/mpaodv` is the authors' published module (PMAODV, QMAODV) from which the code of a separate
self-adaptive variant was removed by `tools/strip_sa.py`; only options that are off for PMAODV and
QMAODV were deleted (`make verify-baseline` shows bit-identical results against the unmodified
module). `src/qsqmaodv` is forked from this cleaned module, so QS-QMAODV contains no other
adaptation mechanism than the queue-state components listed above.

q_n = min(1, |Q_BE(n)| / Q_ref), Q_ref = `QueueRefPackets` (20): frames waiting in the node's
best-effort MAC queue for next hop n (AC_BE_NQOS on the non-QoS 802.11b MAC), recorded at the
decision and carried to the MAC-feedback update in the packet tag.

Every component is an attribute; with `HopByHop=true` and all queue components off the module
runs the QMAODV code path (bit-identical results, checked by `make sanity`).

## What the paper may claim (pre-specified before the TEST runs)
* Primary: QS-QMAODV vs QMAODV and vs QL-AODV (Q-learning baselines), paired Wilcoxon + Holm.
* Secondary: QS-QMAODV vs AODV / AOMDV / PMAODV — expected "not worse"; report honestly.
* Ablation (`ablation_final`, TEST seeds): S (source-side) × Q (queue information) factorial and
  leave-one-out; the contribution of the queue components is whatever this shows.
