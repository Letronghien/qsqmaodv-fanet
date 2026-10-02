# Text the manuscript must change (English, ready to adapt)

## A. Simulation setup — what v1 actually simulated (replace Table 5)

| Parameter | Value used in the code |
|---|---|
| Simulator | NS-3.40 |
| PHY / MAC | IEEE 802.11b, ad hoc, non-QoS DCF; data 11 Mbps (DSSS-CCK), control and broadcast frames 1 Mbps |
| Channel | Friis free-space, TxPower 16 dBm, no hard range limit (≈1 km for 11 Mbps data, broadcasts reach the whole area) |
| Area / mobility | 1000 × 1000 × 300 m, 3-D Gauss–Markov (α = 0.85, time step 0.5 s), mean speed fixed per scenario |
| Traffic | Convergecast: N−1 UDP CBR sources → node 0, 512-byte packets; sink starts at 1 s, sources at 5 s (+ small stagger), stop at 200 s |
| MAC queue / routing queue | 500 packets (ns-3 default) / 64 packets, 30 s |
| QS-QMAODV | α = 0.30, γ = 0.90, ε0 = 0.30 adapted in [0.10, 0.50]; w1 = 0.40, w2 = 0.50, w3 = 0.10 (adaptive, cap 0.40); MaxPaths = 3; decay every 10 s, threshold 15 s, factor 0.92 |
| QMAODV (separate module) | α = 0.5, γ = 0.9, ε0 = 0.5 decaying by 0.02 every 10 s, w1 = 0.6, w2 = 0.4, MaxPaths = 3 |
| Seeds | 30 runs per configuration (RngRun 1–30, common random numbers across protocols) |

## B. Protocol description for v2 (replace Sections 3.3–3.8 / Algorithm 1)

*Scope.* Q-learning is applied by the **source node** to choose the first hop among up to
MaxPaths candidate next hops learned from RREP and duplicate RREQ messages; intermediate nodes forward along their AODV route.

*Cross-layer signal.* For a candidate next hop n,
q_{t,n} = max( |Q_route| / Q_max,route ,  |Q_BE(n)| / Q_max,BE ),
where |Q_BE(n)| is the number of unicast frames waiting in the node's best-effort MAC queue whose receiver address is n
(read-only via the WifiMacQueue container identifier; no MAC modification, no header change).

*State and action.* s = (d, b(q_t)), with b(·) a 5-level bucket of the node-level occupancy; action = next hop n.

*Selection.* With probability ε a random candidate; otherwise argmax_n Q(s,n)·(1 − q_{t,n})^β, β = 0.5.

*Reward (on the MAC outcome of the forwarded packet).*
r = w1 + w2 / (1 + D/τ) + w3,eff (1 − q_{t,n})  if the MAC reports an ACK,
r = −0.5  if the MAC drops the frame after the retry limit,
where D is the measured time from the routing decision to the MAC ACK (queueing + channel access + retransmissions) and τ = 50 ms.

*Update.* Q(s,n) ← (1−α) Q(s,n) + α [ r + γ max_{n'} Q(s,n') ].
State the choice explicitly: the bootstrap uses the node's own estimate for the same state
(report the γ sensitivity, γ ∈ {0, 0.5, 0.9}; γ = 0 is the contextual-bandit special case).

*ACK-silence decay.* Every 10 s, entries with ≥3 transmissions and no MAC ACK for more than 15 s are multiplied by 0.92 **if positive**; negative estimates are kept.

## C. Claims to remove or rewrite
* "Implemented with 250 m range", "2-D", "5 random flows", "applications 10–190 s", "α = 0.1, ε0 = 0.1" — not what was simulated.
* "QMAODV uses the same module with w3 = 0" — false for the main families.
* Energy model, Family E2, LiveNodes, energy per packet — results were independent of E0.
* "Per-forwarding-event at every hop" — source-only.
* Any improvement claim versus AODV / PMAODV — not supported by v1 data (0 of 23 conditions significantly better after Holm correction).
