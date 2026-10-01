/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/**
 * Self-Adaptive Q-Table for SA-QSQMAODV (Self-Adaptive Q-learning Multipath AODV).
 *
 * Inherits the full structure of QSQMAODV's QTable and ADDS a Self-Adaptive
 * Controller that dynamically adjusts three groups of parameters at runtime,
 * following the SA-QSQMAODV paper (ICIT 2026, Section 4):
 *
 *   (1) Adaptive Exploration ε_t  (§4.2):
 *         on RERR  : ε_t = min(0.5, ε_t + 0.2)        // re-explore
 *         periodic : ε_t = max(0.1, ε_t − 0.02)       // stabilise
 *
 *   (2) Adaptive Learning Rate α_t  (§4.3):
 *         α_t = 0.1 + 0.8·(1 − e^(−λ·Δ_Seq))          // ∈ [0.1, 0.9]
 *         where Δ_Seq = number of destination-SeqNo updates in a short window,
 *         λ = sensitivity coefficient (default 0.1).
 *
 *   (3) Adaptive Reward Function  (§4.4):
 *         r_t = w₁(t)·ACK_success + w₂(t)·1/(delay+1) + w₃(t)·Energy_residual
 *         Normal weights:   (w₁, w₂, w₃) = (0.5, 0.4, 0.1)
 *         Low-energy mode:  (w₁, w₂, w₃) = (0.1, 0.1, 0.8)  when E_res < 20 %
 *
 *   (4) Q-update rule (Eq. 4): same as QSQMAODV but with α_t and r_t as above.
 *
 * The class API mirrors QSQMAODV::QTable so the same patch chain (2.3a-d, fix-v2)
 * can be re-used with only one substitution: `qtable` → `saqtable`.
 */

#ifndef QSQMAODV_QTABLE_H
#define QSQMAODV_QTABLE_H

#include "qsqmaodv-rtable.h"

#include "ns3/ipv4-address.h"
#include "ns3/nstime.h"
#include "ns3/random-variable-stream.h"

#include <map>
#include <vector>
#include <deque>
#include <functional>

namespace ns3
{
namespace qsqmaodv
{

/// One Q-learning record per (destination, next-hop) pair.
struct QRecord
{
    RoutingTableEntry rt;
    double            qValue;
    uint32_t          txCount;
    uint32_t          ackCount;
    Time              lastUpd;
    Time              lastAck{Seconds(0)};   ///< QS: time of the last MAC ACK on this pair

    QRecord() : qValue(0.0), txCount(0), ackCount(0), lastUpd(Seconds(0)) {}
    QRecord(const RoutingTableEntry& e, double q)
        : rt(e), qValue(q), txCount(0), ackCount(0), lastUpd(Seconds(0)) {}
};

/**
 * \brief Self-Adaptive Q-Table.
 */
class QTable
{
  public:
    QTable(uint32_t maxPaths = 3);

    void SetMaxPaths(uint32_t mp);
    uint32_t GetMaxPaths() const;

    // -------- Static (initial) hyper-parameters -----------------------------
    /// Set initial values; the Self-Adaptive Controller may later override them.
    void SetLearningParameters(double alpha0, double gamma, double epsilon0);
    /// Set initial reward weights (sum should equal 1).
    void SetRewardWeights(double w1, double w2, double w3 = 0.0);
    void SetRewardWeights(double w1, double w2, double w3, double w4) { SetRewardWeights(w1, w2, w3); }
    void SetTdErrorParams(double /*mu*/, double /*kappa*/) {}

    // -------- STEP3: switches for the adaptive controller ---------------------
    /**
     * \brief Enable/disable each self-adaptive component.
     * All false  -> QSQMAODV as published at ICIT 2025 (fixed alpha, fixed reward
     *               weights, epsilon decays by m_epsilonStep per tick down to 0).
     * All true   -> SA-QSQMAODV behaviour (unchanged).
     */
    void SetAdaptiveFlags(bool adaptEpsilon, bool adaptAlpha, bool adaptReward);

    /**
     * \brief STEP7: next-hop selection policy.
     * false -> epsilon-greedy on Q (QSQMAODV / SA-QSQMAODV);
     * true  -> PMAODV: roulette wheel with p_i = (1/HC_i) / sum_k (1/HC_k).
     */
    void SetProbabilistic(bool on) { m_probabilistic = on; }

    // -------- Self-Adaptive controller --------------------------------------
    /**
     * \brief RERR-triggered ε bump (Eq. ε_t = min(0.5, ε_t + 0.2)).
     * Call from RecvError().
     */
    void OnRouteError();

    /**
     * \brief Periodic ε decay (Eq. ε_t = max(ε_min, ε_t − 0.02)).
     * Call periodically (e.g. every 10 s) from the routing protocol.
     */
    void PeriodicEpsilonDecay();

    /**
     * \brief Record a destination-SeqNo update.
     * The window-length running count drives α_t (paper §4.3).
     */
    void RecordSeqNoUpdate();

    /**
     * \brief Recompute α_t from the current Δ_Seq count and the sensitivity λ.
     * α_t = 0.1 + 0.8·(1 − exp(−λ·Δ_Seq))
     */
    void RecomputeAdaptiveAlpha();

    /**
     * \brief Update reward-weights from the node's remaining energy fraction.
     * If energyFraction < 0.20  → low-energy mode (w₁,w₂,w₃)=(0.1,0.1,0.8).
     * Otherwise                 → normal mode.
     *
     * The caller (routing protocol) is responsible for reading the energy
     * source periodically (e.g. every PeriodicEpsilonDecay tick) and passing
     * the residual fraction here.
     */
    void RecomputeAdaptiveRewardWeights(double energyFraction);

    /// Configurable: low-energy threshold (default 0.20).
    void SetLowEnergyThreshold(double frac);
    /// Configurable: sensitivity λ in α_t formula (default 0.1).
    void SetSensitivityLambda(double lambda);
    /// Configurable: SeqNo window size (default 5 s).
    void SetSeqNoWindow(Time window);
    /// STEP6: delay normalisation 1/(1 + d/dRef); dRef <= 0 keeps the legacy 1/(d+1)
    void SetDelayRef(double dRefSeconds) { m_delayRef = dRefSeconds; }

    // Read accessors (for logging / paper traces)
    double GetAlpha()   const { return m_alpha; }
    double GetGamma()   const { return m_gamma; }
    double GetEpsilon() const { return m_epsilon; }
    double GetW1()      const { return m_w1; }
    double GetW2()      const { return m_w2; }
    double GetW3()      const { return m_w3; }
    uint32_t GetDeltaSeq() const;

    // -------- Standard QTable operations (same shape as QSQMAODV) -------------
    bool AddRoute(const RoutingTableEntry& rt);
    void ReinitQValues(Ipv4Address dst);
    uint32_t GetRoutes(Ipv4Address dst,
                       std::vector<RoutingTableEntry>& routes,
                       const RoutingTable* mainTable = nullptr) const;

    /// STEP5: 'exclude' = previous hop of the packet (never chosen as next hop);
    /// the default (unset) address excludes nothing.
    bool SelectEpsilonGreedy(const RoutingTableEntry& primary,
                             RoutingTableEntry& out,
                             const RoutingTable* mainTable = nullptr,
                             Ipv4Address exclude = Ipv4Address());

    /**
     * \brief Update Q for (dst, nextHop) using the *current* adaptive α_t and
     *        3-term reward r_t computed from the supplied feedback.
     */
    void UpdateQValue(Ipv4Address dst,
                      Ipv4Address nextHop,
                      double ackSuccess,
                      double delaySec,
                      double energyFraction = 1.0);

    // ======================= QS-QMAODV extensions ===========================
    /// Queue-state configuration. All-zero/false = QMAODV behaviour (bit-identical).
    struct QsConfig
    {
        double wq{0.0};          ///< weight of the queue term  wq·ACK·(1 − q_n)
        bool   adaptiveWq{false};///< wq_eff = min(wqMax, wq + wqKappa·q_n)
        double wqMax{0.40};
        double wqKappa{0.20};
        double failPenalty{0.0}; ///< subtracted from r when the MAC drops the frame
        bool   queueSelect{false};///< exploitation score Q − beta·q_n·max|Q|
        double beta{0.5};
    };
    void SetQsConfig(const QsConfig& c) { m_qs = c; }
    const QsConfig& GetQsConfig() const { return m_qs; }
    /// q_n provider: occupancy of the MAC BE queue towards next hop n, in [0,1]
    void SetNextHopQueueFn(std::function<double(Ipv4Address)> fn) { m_nhQueueFn = std::move(fn); }
    /// Q-update with the queue occupancy q_n observed when the decision was made
    void UpdateQValueQs(Ipv4Address dst, Ipv4Address nextHop, double ackSuccess,
                        double delaySec, double energyFraction, double qn);
    /// ACK-silence decay: positive Q of pairs with >= minTx transmissions and no
    /// MAC ACK for more than tauSec is multiplied by factor. Returns #decayed entries.
    uint32_t DecayStale(Time now, double tauSec, double factor, uint32_t minTx);
    /// Trend-triggered exploration bump (bounded by cap)
    void BumpEpsilon(double bump, double cap) { m_epsilon = std::min(cap, m_epsilon + bump); }

    bool EnsureRecord(const RoutingTableEntry& rt);
    void UpdateQValueOrCreate(const RoutingTableEntry& rt,
                              double ackSuccess, double delaySec,
                              double energyFraction = 1.0);

    void DeleteRoutes(Ipv4Address dst);
    void DeleteRoute(Ipv4Address dst, Ipv4Address nextHop);
    void RemoveNextHopGlobally(Ipv4Address nextHop);

    uint32_t Size() const;
    uint32_t CountFor(Ipv4Address dst) const;
    bool     IsFull(Ipv4Address dst) const;
    void     Clear();
    void     Print(std::ostream& os) const;
    double   GetQValue(Ipv4Address dst, Ipv4Address nextHop) const;

  private:
    std::vector<QRecord>::iterator FindWorst(std::vector<QRecord>& vec);
    std::vector<QRecord> BuildCandidates(const RoutingTableEntry& primary,
                                         const RoutingTable* mainTable) const;
    /// Compute the 3-term reward r_t.
    double ComputeReward(double ackSuccess, double delaySec, double energyFrac) const;
    /// QS reward; reduces exactly to ComputeReward when the QS terms are off
    double ComputeRewardQs(double ackSuccess, double delaySec, double energyFrac, double qn) const;
    /// Prune SeqNo-update timestamps older than m_seqNoWindow.
    void PurgeSeqNoEvents();

    // Per-destination alternates with learned Q.
    std::map<Ipv4Address, std::vector<QRecord>> m_records;
    uint32_t m_maxPaths;

    // ---- Adaptive hyper-parameters (live state) ----
    double m_alpha;       // current α_t
    double m_gamma;       // discount (fixed in paper)
    double m_epsilon;     // current ε_t
    double m_w1, m_w2, m_w3;
    bool   m_lowEnergyMode;
    // STEP3: adaptive-controller switches (default true = SA behaviour)
    bool   m_adaptEpsilon{true};
    bool   m_adaptAlpha{true};
    bool   m_adaptReward{true};
    bool   m_probabilistic{false};   // STEP7: PMAODV policy

    // ---- Adaptation knobs ----
    double m_epsilonMin;       // 0.10  (floor for ε_t)
    double m_epsilonMax;       // 0.50  (ceiling on RERR-bump)
    double m_epsilonStep;      // 0.02  (periodic decay)
    double m_epsilonBump;      // 0.20  (on-RERR)
    double m_lambda;           // 0.1   (sensitivity in α_t formula)
    Time   m_seqNoWindow;      // 5 s
    double m_delayRef{0.0};    // STEP6: 0 = legacy 1/(delay+1)
    double m_lowEnergyThresh;  // 0.20
    // normal-mode reward weights
    double m_w1Normal, m_w2Normal, m_w3Normal;
    // low-energy mode reward weights
    double m_w1Low, m_w2Low, m_w3Low;

    // Δ_Seq sliding window: timestamps of recent destination SeqNo updates.
    mutable std::deque<Time> m_seqEvents;

    Ptr<UniformRandomVariable> m_uniform;

    QsConfig m_qs;
    std::function<double(Ipv4Address)> m_nhQueueFn;
};

} // namespace qsqmaodv
} // namespace ns3

#endif /* QSQMAODV_QTABLE_H */
