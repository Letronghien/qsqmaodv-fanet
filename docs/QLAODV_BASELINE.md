# External baseline: QL-AODV (Ateya et al., 2025)

A. A. Ateya, N. D. Tu, A. Muthanna, A. Koucheryavy, D. Kozyrev, J. Sztrik,
"QL-AODV: Q-Learning-Enhanced Multi-Path Routing Protocol for 6G-Enabled Autonomous Aerial
Vehicle Networks," *Future Internet*, vol. 17, no. 10, 473, 2025. doi:10.3390/fi17100473

## Why this baseline
* Independent group, journal paper, 2025, same problem (Q-learning multipath AODV for UAV networks),
  same simulator family (ns-3, AODV-based), **and it is congestion-aware** — the closest competitor.
* Clean contrast for the paper: QL-AODV measures the *routing-layer* RequestQueue, aggregated along
  the path inside the RREP, and decides once per route discovery at the source; QS-QMAODV measures the
  *MAC* best-effort queue towards each next hop and decides hop-by-hop at every packet.
* No source code was published, so it is re-implemented from the paper (module `src/qlaodv`, a fork of
  the ns-3.48 AODV module, as the authors forked ns-3.34 AODV).

## Mapping paper -> code
| Paper | Implementation |
|---|---|
| RREP 19 -> 27 B, `m_totalBuffer`, `m_maxBuffer` (Sec. 3.1, Fig. 2) | `RrepHeader` + 2×uint32; originator writes its own values, each forwarder adds its mean and maxes its max |
| Buffer = AODV RequestQueue occupancy in %, windowed mean and max (Sec. 3.1) | sampled every 100 ms, window 1 s (*) |
| State s = (h/Hmax, Btotal/(h·100), Bmax/100), Hmax = 10 (Eq. 1–4) | same; each component discretised to 0.1 for the Q-table key (*) |
| Action = route index among ≤ 10 RREPs sorted by hop count (Alg. 2) | same (`routeId` = rank) |
| Source waits m_rrepWaitTime = 300 ms, then selects ε-greedy and installs the route, sends the queue (Alg. 2) | same; RREPs that arrive within 2×300 ms after a selection while the route is valid are ignored (*) |
| ε0 = 0.5, ×0.995 per decision, εmin = 0.1; α = 0.2, γ = 0.7 (Table 2) | same |
| Reward +1 MAC ACK / −1 MAC failure or no ACK within a timeout (Eq. 5) | `AckedMpdu` / `DroppedMpdu`; timeout 1 s (*) |
| Q update with max over the current candidate routes of D (Eq. 6, Alg. 3) | same |
| Q initialised to 0 | same |

(*) = not specified in the paper; reconstruction choice, exposed as an ns-3 attribute
(`QlBufSampleInterval`, `QlBufWindow`, `QlFeedbackTimeout`) and to be stated in our paper.

## One necessary assumption
Standard AODV lets the destination answer only the first copy of a RREQ, so the source rarely sees
more than one RREP and "up to ten candidate routes" cannot occur. The paper does not say how its
implementation obtained several RREPs. We let the destination also answer duplicate RREQs arriving
from new neighbours (up to 10), attribute `QlDuplicateReplies` (default true). The CSV column
`QlMeanCandidates` reports how many candidates were actually available per selection; report it.

## Fairness notes for the paper
* QL-AODV's own parameters (Table 2) are used unchanged; it is not tuned on our scenarios, and
  neither is QS-QMAODV beyond the sensitivity study.
* Its routing overhead includes the 8 extra bytes in every RREP and HELLO (both use the RREP format).
* Its absolute numbers differ from the original paper because the scenario differs
  (802.11b at 11 Mbps, Friis, convergecast here; 802.11g, log-distance, 250 m range there).
