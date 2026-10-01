# QS-QMAODV v3 — design (module `src/qsqmaodv`, protocol `QSQMAODV`)

## Why v3
The published QMAODV [4] already (i) runs an independent Q-learning agent at every node
(hop-by-hop) and (ii) learns from MAC-layer feedback (ACK success and one-hop delay).
The v2 module `qs2maodv` was derived from an older, source-only QMAODV with a
neighbour-freshness reward, i.e. it was *weaker* than the published baseline.
v3 is therefore a fork of the exact QMAODV module used as baseline
(`~/nbqmaodv-fanet/ns-3-nbq/src/qmaodv`) to which ONLY the queue-state extensions are added.
With every extension switched off, QSQMAODV runs the same code path as QMAODV
(the `sanity` set checks that both give bit-identical results for the same seed).

## Extensions (ns-3 attributes of `ns3::qsqmaodv::RoutingProtocol`)
| Component | Attributes (default) | Effect |
|---|---|---|
| R — queue reward | `QueueRewardWeight` (0.10), `AdaptiveQueueWeight` (true), `QueueWeightMax` (0.40), `QueueWeightKappa` (0.20) | r = s·(w1·ACK + w2·D) + w3·E + wq_eff·ACK·(1 − q_n) − P·(1 − ACK), s = (w1+w2−wq_eff)/(w1+w2) |
| S — queue-aware selection | `QueueAwareSelect` (true), `QueueSelectBeta` (0.5) | exploitation picks argmax [Q − β·q_n·max|Q|] |
| D — ACK-silence decay | `AckSilenceDecay` (true), `AckSilenceThreshold` (15 s), `DecayFactor` (0.92), `DecayMinTx` (3), `DecayInterval` (10 s) | positive Q of (dst, n) without MAC ACK for > τ shrinks; penalties are kept |
| T — trend exploration | `TrendEpsilon` (true), `TrendInterval` (1 s), `TrendDelta` (0.05), `TrendWindow` (3), `TrendBump` (0.10), `TrendCap` (0.50) | ε rises when the node's MAC occupancy grows in 3 consecutive samples |
| P — failure penalty | `FailurePenalty` (0.5) | MAC drop is punished (QMAODV gives a drop the delay term only) |

q_n = |Q_BE(n)| / Q_max,BE: frames waiting in the node's best-effort MAC queue whose receiver
is next hop n (read-only, `MakeWifiUnicastQueueId`; AC_BE_NQOS on the non-QoS 802.11b MAC).
q_n is recorded when the next hop is chosen and carried to the MAC-feedback update in the
packet tag, so the reward refers to the queue state the agent actually saw.

All QMAODV parameters (α0 = 0.5, γ = 0.9, ε0 = 0.5, w1 = 0.6, w2 = 0.4, MaxPaths = 3,
HopByHop, UseMacFeedback, DelayRef = 10 ms) are identical for QMAODV and QSQMAODV.

## Novelty statement for the paper (proposed)
Relative to QMAODV [4], the contribution is the *queue-state awareness of the
next-hop decision*: a per-next-hop MAC queue signal used both in the reward (R) and in
the exploitation rule (S), plus two stability mechanisms (D, T) and an explicit failure
penalty (P). The factorial and leave-one-out ablations quantify each part.
