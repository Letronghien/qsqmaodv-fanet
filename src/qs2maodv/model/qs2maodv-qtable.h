/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/**
 * QS-QMAODV Q-Table
 *
 * KEY DIFFERENCE from QMAODV:
 *   QMAODV state  : s = destination_ID
 *   QS-QMAODV state: s = (destination_ID, queue_bucket, energy_bucket)
 *
 * queue_bucket  = floor(q_t / 0.20)  in {0,1,2,3,4}
 * energy_bucket = floor(E_t / 0.20)  in {0,1,2,3,4}
 * → 25 states per destination → 25*|D| total (tabular OK)
 *
 * Queue-triggered adaptive epsilon (NOT RERR-triggered like SA/EA):
 *   q_t > θ_H=0.70 → ε += δ_q=0.15  (explore when congested)
 *   q_t < θ_L=0.30 → ε -= δ_decay=0.02 (exploit when stable)
 *
 * Hybrid path selection (NOT pure argmax-Q like QMAODV/SA/EA):
 *   score(s,a) = Q(s,a) * (1 - q_a)^β   β=0.50
 *   a* = argmax score(s,a)
 *
 * Reward: r = w1*ACK + w2*1/(delay+1) + w3*E_t
 *   w1=0.45, w2=0.45, w3=0.10  (fixed)
 *
 * Authors: QS-QMAODV Research Group — IUH
 */
#ifndef QS2MAODV_QTABLE_H
#define QS2MAODV_QTABLE_H

#include "qs2maodv-rtable.h"
#include "ns3/ipv4-address.h"
#include "ns3/nstime.h"
#include "ns3/random-variable-stream.h"
#include <map>
#include <tuple>
#include <vector>
#include <deque>
#include <functional>

namespace ns3
{
namespace qs2maodv
{

// ============================================================
// 3D State Key: (destination, queue_bucket, energy_bucket)
// ============================================================
struct QsStateKey
{
    Ipv4Address dst;
    int         qBucket;  ///< 0..4  (floor(q_t / 0.20))
    int         eBucket;  ///< 0..4  (floor(E_t / 0.20))

    QsStateKey() : qBucket(0), eBucket(0) {}
    QsStateKey(Ipv4Address d, int q, int e) : dst(d), qBucket(q), eBucket(e) {}

    bool operator<(const QsStateKey& o) const
    {
        if (dst < o.dst) return true;
        if (o.dst < dst) return false;
        if (qBucket < o.qBucket) return true;
        if (o.qBucket < qBucket) return false;
        return eBucket < o.eBucket;
    }

    bool operator==(const QsStateKey& o) const
    {
        return dst == o.dst && qBucket == o.qBucket && eBucket == o.eBucket;
    }
};

// ============================================================
// One Q-learning record per (QsStateKey, next-hop) pair
// ============================================================
struct QsRecord
{
    RoutingTableEntry rt;       ///< Full routing-table entry (for forwarding)
    double            qValue;   ///< Current Q(s, a)
    uint32_t          txCount;  ///< Times this action was taken
    uint32_t          ackCount; ///< Times feedback was positive
    Time              lastUpd;  ///< Last time Q was updated
    Time            lastAck;   ///< Last time ACK received for this next-hop

    QsRecord()
        : qValue(0.0), txCount(0), ackCount(0), lastUpd(Seconds(0)), lastAck(Seconds(0)) {}

    QsRecord(const RoutingTableEntry& e, double q)
        : rt(e), qValue(q), txCount(0), ackCount(0), lastUpd(Seconds(0)), lastAck(Seconds(0)) {}
};

// ============================================================
// Main Q-table class
// ============================================================
class QsQTable
{
  public:
    QsQTable(uint32_t maxPaths = 3);

    // -------- Capacity -------------------------------------------------------
    void     SetMaxPaths(uint32_t mp);
    uint32_t GetMaxPaths() const;

    // -------- RL hyper-parameters -------------------------------------------
    /// Fixed α=0.30, γ=0.90 per blueprint
    void SetLearningParameters(double alpha, double gamma, double epsilon);

    /// Reward weights: w1=0.45 (ACK), w2=0.45 (delay), w3=0.10 (energy)
    void SetRewardWeights(double w1, double w2, double w3);

    double GetAlpha()   const { return m_alpha; }
    double GetGamma()   const { return m_gamma; }
    double GetEpsilon() const { return m_epsilon; }

    // -------- Queue-triggered epsilon (blueprint §3.3) ----------------------
    /**
     * Update ε based on local queue occupancy (called before every forwarding).
     * q_t > θ_H → ε = min(ε_max, ε + δ_q)
     * q_t < θ_L → ε = max(ε_min, ε − δ_decay)
     * else      → unchanged
     */
    void UpdateEpsilon(double qt);
  void SetAdaptiveW3 (bool v) { m_adaptiveW3 = v; }
  void SetTrendEps   (bool v) { m_trendEps   = v; }

    // -------- [FIX v2] Ablation / correctness switches ----------------------
    /// Enable queue-triggered epsilon (theta_H / theta_L / trend). If false, epsilon stays constant.
    void SetQueueEpsilon(bool v) { m_queueEps = v; }
    /// Enable hybrid selection Q*(1-q_a)^beta. If false, pure argmax-Q exploitation.
    void SetHybridSelect(bool v) { m_hybrid = v; }
    /// Per-next-hop queue occupancy provider used by the hybrid score (replaces HC proxy).
    void SetNextHopQueueFn(std::function<double(Ipv4Address)> fn) { m_nhQueueFn = std::move(fn); }
    /// Delay normalisation: delay term = 1 / (1 + delay / scale). Legacy behaviour: scale = 1 s.
    void SetDelayScale(double s) { m_delayScale = (s > 0.0 ? s : 1.0); }
    /// Only decay positive Q-values (negative values are not pulled toward 0). Legacy: false.
    void SetDecayPositiveOnly(bool v) { m_decayPositiveOnly = v; }

    // -------- State construction (blueprint §3.2) ---------------------------
    /**
     * Build a QsStateKey from destination, current queue occupancy, and
     * current energy ratio.
     * \param dst destination IP address
     * \param qt  queue occupancy in [0,1]
     * \param Et  energy ratio E_rem/E_0 in [0,1]
     */
    static QsStateKey BuildState(Ipv4Address dst, double qt, double /*unused*/);

    // -------- Route management ----------------------------------------------
    /**
     * Add alternate route. Dedup by (dst, nextHop).
     * At capacity, evict worst (highest HC) if rt is strictly better.
     * \return true if added/changed.
     */
    bool AddRoute(const RoutingTableEntry& rt, const QsStateKey& state);

    /**
     * Ensure primary route is tracked (bypass capacity check).
     * Preserves learned Q if already present.
     * \return true if newly inserted.
     */
    bool EnsureRecord(const RoutingTableEntry& rt, const QsStateKey& state);

    /**
     * Update Q for (state, nextHop), creating record if missing.
     */
    void UpdateQValueOrCreate(const RoutingTableEntry& rt,
                              const QsStateKey& state,
                              double ackSuccess, double delaySec, double qt);

    /**
     * Get all valid alternate routes for dst (any state bucket).
     */
    uint32_t GetRoutes(Ipv4Address dst,
                       std::vector<RoutingTableEntry>& routes,
                       const RoutingTable* mainTable = nullptr) const;

    // -------- Hybrid selection (blueprint §3.5) -----------------------------
    /**
     * Hybrid ε-greedy selection: score = Q(s,a) * (1 - q_a)^β
     *
     * \param primary  Primary route from RoutingTable lookup
     * \param state    Current 3D state (dst, qBucket, eBucket)
     * \param qt_local Local queue occupancy (for f_avail approximation)
     * \param out      Selected entry
     * \param mainTable Optional revalidation
     * \return false only if no candidate is usable
     */
    bool SelectHybrid(const RoutingTableEntry& primary,
                      const QsStateKey& state,
                      double qt_local,
                      RoutingTableEntry& out,
                      const RoutingTable* mainTable = nullptr);

    // -------- Q-value update (blueprint §3.6) -------------------------------
    /**
     * Bellman update with 3-term reward: r = w1*ACK + w2*1/(delay+1) + w3*E_t
     */
    void UpdateQValue(const QsStateKey& state,
                      Ipv4Address nextHop,
                      double ackSuccess,
                      double delaySec,
                      double qt);

    // -------- Q-init (blueprint §3.6) ---------------------------------------
    /**
     * Re-compute initial Q-values for all records of dst using
     * normalize(1/HC) across current set — called after AddRoute.
     */
    void ReinitQValues(Ipv4Address dst);

    // -------- Lifecycle / inspection ----------------------------------------
    void     DeleteRoutes(Ipv4Address dst);
    void     DeleteRoute(Ipv4Address dst, Ipv4Address nextHop);
    void     RemoveNextHopGlobally(Ipv4Address nextHop);
    uint32_t Size() const;
    uint32_t CountFor(Ipv4Address dst) const;
    void    DecayStaleRoutes(ns3::Time now, double tauSec, double decayFactor);
    void     Clear();
    void     Print(std::ostream& os) const;

    double   GetQValue(const QsStateKey& state, Ipv4Address nextHop) const;

  private:
    // Inner storage: state_key → vector of QsRecord
    std::map<QsStateKey, std::vector<QsRecord>> m_records;

    // Per-destination set of known next-hops (for GetRoutes, dedup, etc.)
    // We also keep a flat dst→records map for lifecycle ops
    std::map<Ipv4Address, std::vector<QsStateKey>> m_dstToKeys;

    // Capacity
    uint32_t m_maxPaths;

    // RL hyper-parameters (fixed per blueprint)
    double m_alpha;       ///< 0.30
    double m_gamma;       ///< 0.90
    double m_epsilon;     ///< starts 0.30
    double m_epsilonMin;  ///< 0.10
    double m_epsilonMax;  ///< 0.50

    // Queue-triggered epsilon params (blueprint §3.3)
    double m_thetaH;      ///< 0.70 high queue threshold
    double m_thetaL;      ///< 0.30 low queue threshold
    double m_deltaQ;      ///< 0.15 bump when congested
    std::deque<double> m_qtHistory;
  bool m_adaptiveW3{true};   ///< ablation: enable adaptive w3 scaling
  bool m_trendEps  {true};   ///< ablation: enable trend-based epsilon bump  ///< queue trend window (3 samples)
    double m_deltaDecay;  ///< 0.02 decay when stable

    // Reward weights (blueprint §3.4)
    double m_w1;  ///< 0.45 ACK
    double m_w2;  ///< 0.45 delay
    double m_w3;  ///< 0.10 energy

    // Hybrid selection β (blueprint §3.5)
    double m_beta;  ///< 0.50

    // [FIX v2]
    bool   m_queueEps{true};
    bool   m_hybrid{true};
    double m_delayScale{1.0};
    bool   m_decayPositiveOnly{false};
    std::function<double(Ipv4Address)> m_nhQueueFn;

    Ptr<UniformRandomVariable> m_uniform;

    // -------- Private helpers -----------------------------------------------
    std::vector<QsRecord>::iterator FindWorst(std::vector<QsRecord>& vec);

    std::vector<QsRecord> BuildCandidates(const RoutingTableEntry& primary,
                                          const QsStateKey& state,
                                          const RoutingTable* mainTable) const;

    /// Approximate queue occupancy of next-hop `a` using HC proxy
    double ApproxNextHopQueue(uint32_t hcA, uint32_t hcMax) const;
};

} // namespace qs2maodv
} // namespace ns3

#endif /* QS2MAODV_QTABLE_H */
