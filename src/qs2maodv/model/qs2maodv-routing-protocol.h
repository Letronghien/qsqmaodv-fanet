/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/**
 * QS-QMAODV Routing Protocol Header
 *
 * Extends QMAODV with:
 *  1. Queue-state in state space: s = (dst, queue_bucket, energy_bucket)
 *  2. Queue-triggered adaptive epsilon (NOT RERR-triggered)
 *  3. Hybrid path selection: score = Q(s,a) * (1 - q_a)^β
 *
 * Authors: QS-QMAODV Research Group — IUH
 */
#ifndef QS2MAODV_ROUTING_PROTOCOL_H
#define QS2MAODV_ROUTING_PROTOCOL_H

#include "qs2maodv-dpd.h"
#include "qs2maodv-neighbor.h"
#include "qs2maodv-packet.h"
#include "qs2maodv-rqueue.h"
#include "qs2maodv-rtable.h"
#include "qs2maodv-qtable.h"
#include "ns3/ipv4-interface.h"
#include "ns3/ipv4-l3-protocol.h"
#include "ns3/ipv4-routing-protocol.h"
#include "ns3/node.h"
#include "ns3/event-id.h"
#include "ns3/output-stream-wrapper.h"
#include "ns3/random-variable-stream.h"
#include "ns3/energy-source.h"
#include <map>
#include <unordered_map>

namespace ns3
{

class WifiMpdu;
class WifiMacQueue;
enum WifiMacDropReason : uint8_t;

namespace qs2maodv
{

/**
 * \ingroup qs2maodv
 * \brief QS-QMAODV routing protocol
 *
 * Queue-State-Aware Q-Learning Multipath AODV.
 * State space: (destination, queue_bucket, energy_bucket)
 * Epsilon: queue-triggered (not RERR-triggered)
 * Selection: hybrid Q*queue_factor
 */
class RoutingProtocol : public Ipv4RoutingProtocol
{
  public:
    static TypeId GetTypeId();
    static const uint32_t QS2MAODV_PORT;

    RoutingProtocol();
    ~RoutingProtocol() override;
    void DoDispose() override;

    // -------- Ipv4RoutingProtocol interface ----------------------------------
    Ptr<Ipv4Route> RouteOutput(Ptr<Packet> p,
                               const Ipv4Header& header,
                               Ptr<NetDevice> oif,
                               Socket::SocketErrno& sockerr) override;

    bool RouteInput(Ptr<const Packet> p,
                    const Ipv4Header& header,
                    Ptr<const NetDevice> idev,
                    const UnicastForwardCallback& ucb,
                    const MulticastForwardCallback& mcb,
                    const LocalDeliverCallback& lcb,
                    const ErrorCallback& ecb) override;

    void NotifyInterfaceUp(uint32_t interface) override;
    void NotifyInterfaceDown(uint32_t interface) override;
    void NotifyAddAddress(uint32_t interface, Ipv4InterfaceAddress address) override;
    void NotifyRemoveAddress(uint32_t interface, Ipv4InterfaceAddress address) override;
    void SetIpv4(Ptr<Ipv4> ipv4) override;
    void PrintRoutingTable(Ptr<OutputStreamWrapper> stream,
                           Time::Unit unit = Time::S) const override;

    // -------- Parameter accessors -------------------------------------------
    Time     GetMaxQueueTime() const  { return m_maxQueueTime; }
    void     SetMaxQueueTime(Time t);
    uint32_t GetMaxQueueLen() const   { return m_maxQueueLen; }
    void     SetMaxQueueLen(uint32_t len);

    bool GetDestinationOnlyFlag() const  { return m_destinationOnly; }
    void SetDestinationOnlyFlag(bool f)  { m_destinationOnly = f; }
    bool GetGratuitousReplyFlag() const  { return m_gratuitousReply; }
    void SetGratuitousReplyFlag(bool f)  { m_gratuitousReply = f; }
    void SetHelloEnable(bool f)          { m_enableHello = f; }
    bool GetHelloEnable() const          { return m_enableHello; }
    void SetBroadcastEnable(bool f)      { m_enableBroadcast = f; }
    bool GetBroadcastEnable() const      { return m_enableBroadcast; }

    int64_t AssignStreams(int64_t stream);

    // -------- [FIX v2] Run-level diagnostics (read by the simulation script) --
    struct QueueStats
    {
        uint64_t samples{0};       ///< number of q_t samples (one per RouteOutput)
        double   macSum{0.0};      ///< sum of node-level MAC BE occupancy
        double   macMax{0.0};      ///< max MAC BE occupancy observed
        uint64_t macPositive{0};   ///< samples with MAC occupancy > 0
        double   routeSum{0.0};    ///< sum of routing (request-queue) occupancy
        uint64_t fbAck{0};         ///< MAC-level ACKed MPDUs matched to a Q decision
        uint64_t fbDrop{0};        ///< MAC-level dropped MPDUs matched to a Q decision
        uint64_t fbExpired{0};     ///< decisions with no MAC outcome before timeout
    };
    const QueueStats& GetQueueStats() const { return m_qstats; }

  protected:
    void DoInitialize() override;

    // -------- QS-QMAODV specific setters ------------------------------------
    void SetMaxPaths(uint32_t mp);
    uint32_t GetMaxPaths() const;

    void SetQLearningParameters(double alpha, double gamma, double epsilon);
    void SetRewardWeights(double w1, double w2, double w3);

    // Initial energy (J) — needed for E_t = E_rem / E_0
    void SetInitialEnergy(double e0)  { m_initialEnergy = e0; }
    double GetInitialEnergy() const   { return m_initialEnergy; }

  private:
    void NotifyTxError(WifiMacDropReason reason, Ptr<const WifiMpdu> mpdu);

    // -------- Cross-layer queue & energy monitoring -------------------------
    /**
     * Get local queue occupancy q_t in [0,1].
     * Uses AODV RequestQueue fill ratio.
     * (Cross-layer WifiMacQueue can be added later for higher accuracy.)
     */
    double GetQueueOccupancy();

    // [FIX v2] cross-layer helpers
    Ptr<WifiMacQueue> GetBeQueue();                  ///< AC_BE (QoS) or AC_BE_NQOS (non-QoS) queue
    bool   GetBeQueueIsQos();                        ///< true if the MAC is QoS-enabled
    double GetMacQueueOccupancy();                   ///< |Q_BE| / Q_max,BE
    double GetRoutingQueueOccupancy();               ///< |Q_routing| / Q_max,route
    double GetNextHopQueueOccupancy(Ipv4Address nh); ///< max(q_route, |Q_BE(nh)| / ref)
    bool   LookupMac(Ipv4Address ip, Mac48Address& mac);

    // [FIX v2] real hop-level feedback from the MAC (replaces "neighbor freshness" ACK)
    struct PendingTx
    {
        Ipv4Address  nextHop;
        QsStateKey   state;
        double       qt;
        Time         t0;
    };
    std::unordered_map<uint64_t, PendingTx> m_pending;   ///< keyed by packet UID
    void NotifyAckedMpdu(Ptr<const WifiMpdu> mpdu);
    void ResolvePending(uint64_t uid, bool success);
    void PurgePending();
    void PurgeTick();
    EventId m_purgeEvent;

    /**
     * Get remaining energy ratio E_t = E_rem / E_0 in [0,1].
     * Uses ns-3 BasicEnergySource if attached; returns 1.0 otherwise.
     */
    double GetEnergyRatio() const;

    // -------- Protocol parameters -------------------------------------------
    uint32_t m_rreqRetries;
    uint16_t m_ttlStart;
    uint16_t m_ttlIncrement;
    uint16_t m_ttlThreshold;
    uint16_t m_timeoutBuffer;
    uint16_t m_rreqRateLimit;
    uint16_t m_rerrRateLimit;
    Time     m_activeRouteTimeout;
    uint32_t m_netDiameter;
    Time     m_nodeTraversalTime;
    Time     m_netTraversalTime;
    Time     m_pathDiscoveryTime;
    Time     m_myRouteTimeout;
    Time     m_helloInterval;
    uint32_t m_allowedHelloLoss;
    Time     m_deletePeriod;
    Time     m_nextHopWait;
    Time     m_blackListTimeout;
    uint32_t m_maxQueueLen;
    Time     m_maxQueueTime;
    bool     m_destinationOnly;
    bool     m_gratuitousReply;
    bool     m_enableHello;
    bool     m_enableBroadcast;

    // -------- NS-3 objects --------------------------------------------------
    Ptr<Ipv4> m_ipv4;
    std::map<Ptr<Socket>, Ipv4InterfaceAddress> m_socketAddresses;
    std::map<Ptr<Socket>, Ipv4InterfaceAddress> m_socketSubnetBroadcastAddresses;
    Ptr<NetDevice> m_lo;

    // -------- Routing tables ------------------------------------------------
    RoutingTable m_routingTable;

    // -------- QS-QMAODV: Q-table with 3D state ------------------------------
    QsQTable m_qtable;

    // -------- QS-QMAODV hyper-parameters ------------------------------------
    uint32_t m_maxPaths{3};
    double   m_alpha{0.30};
    double   m_gamma{0.90};
    double   m_epsilon{0.30};
    double   m_w1{0.45};
    double   m_w2{0.45};
    double   m_w3{0.10};
    double   m_initialEnergy{50.0};  ///< E_0 = 50 J per blueprint Table 3

    // -------- Packet buffer & bookkeeping -----------------------------------
    RequestQueue m_queue;
    uint32_t     m_requestId;
    uint32_t     m_seqNo;
    IdCache      m_rreqIdCache;
    DuplicatePacketDetection m_dpd;
    Neighbors    m_nb;
    uint16_t     m_rreqCount;
    uint16_t     m_rerrCount;

    // -------- Forwarding timestamp for delay measurement --------------------
    std::map<uint32_t, Time> m_txTimestamps;  ///< uid → send time

  private:
    void Start();
    void DeferredRouteOutput(Ptr<const Packet> p,
                             const Ipv4Header& header,
                             UnicastForwardCallback ucb,
                             ErrorCallback ecb);
    bool Forwarding(Ptr<const Packet> p,
                    const Ipv4Header& header,
                    UnicastForwardCallback ucb,
                    ErrorCallback ecb);
    void ScheduleRreqRetry(Ipv4Address dst);
    bool UpdateRouteLifeTime(Ipv4Address addr, Time lt);
    void UpdateRouteToNeighbor(Ipv4Address sender, Ipv4Address receiver);
    bool IsMyOwnAddress(Ipv4Address src);
    Ptr<Socket> FindSocketWithInterfaceAddress(Ipv4InterfaceAddress iface) const;
    Ptr<Socket> FindSubnetBroadcastSocketWithInterfaceAddress(Ipv4InterfaceAddress iface) const;
    void ProcessHello(const RrepHeader& rrepHeader, Ipv4Address receiverIfaceAddr);
    Ptr<Ipv4Route> LoopbackRoute(const Ipv4Header& header, Ptr<NetDevice> oif) const;

    // Control packet handlers
    void RecvQs2maodv(Ptr<Socket> socket);
    void RecvRequest(Ptr<Packet> p, Ipv4Address receiver, Ipv4Address src);
    void RecvReply(Ptr<Packet> p, Ipv4Address my, Ipv4Address src);
    void RecvReplyAck(Ipv4Address neighbor);
    void RecvError(Ptr<Packet> p, Ipv4Address src);

    // Send methods
    void SendPacketFromQueue(Ipv4Address dst, Ptr<Ipv4Route> route);
    void SendHello();
    void SendRequest(Ipv4Address dst);
    void SendReply(const RreqHeader& rreqHeader, const RoutingTableEntry& toOrigin);
    void SendReplyByIntermediateNode(RoutingTableEntry& toDst,
                                     RoutingTableEntry& toOrigin,
                                     bool gratRep);
    void SendReplyAck(Ipv4Address neighbor);
    void SendRerrWhenBreaksLinkToNextHop(Ipv4Address nextHop);
    void SendRerrMessage(Ptr<Packet> packet, std::vector<Ipv4Address> precursors);
    void SendRerrWhenNoRouteToForward(Ipv4Address dst, uint32_t dstSeqNo, Ipv4Address origin);
    void SendTo(Ptr<Socket> socket, Ptr<Packet> packet, Ipv4Address destination);

    // Timers
    Timer   m_htimer;
    void    HelloTimerExpire();
    Timer   m_rreqRateLimitTimer;
    void    RreqRateLimitTimerExpire();
    Timer   m_rerrRateLimitTimer;
    Timer               m_decayTimer;
  bool        m_enableDecay{true};  ///< ablation: enable ACK-silence decay
  double   m_silenceThreshold{15.0}; ///< ACK-silence threshold (s) for Q-value decay
  double   m_decayFactor{0.92};      ///< Q-value decay multiplier (applied every decay pass)
  bool        m_adaptiveW3 {true};  ///< ablation: enable adaptive w3
  bool        m_trendEps   {true};  ///< ablation: enable trend epsilon
  // [FIX v2] switches (defaults = corrected protocol; set all false to reproduce v1 behaviour)
  bool        m_macQueueSignal{true}; ///< read MAC BE queue (false reproduces v1)
  bool        m_macFeedback  {true};  ///< reward from MAC ACK / drop instead of neighbor freshness
  bool        m_nextHopQueue {true};  ///< per-next-hop queue in hybrid score and reward
  bool        m_queueState   {true};  ///< include queue bucket in the Q-table state
  bool        m_queueEps     {true};  ///< queue-triggered epsilon
  bool        m_hybridSelect {true};  ///< hybrid Q*(1-q_a)^beta selection
  bool        m_decayPosOnly {true};  ///< decay only positive Q-values
  double      m_delayScale   {0.05};  ///< delay term = 1/(1+d/scale), seconds
  double      m_nhQueueRef   {0.0};   ///< packets; 0 => use MAC queue MaxSize
  Time        m_pendingTimeout{Seconds(2.0)};
  Time        m_purgePeriod{MilliSeconds(500)};
  bool        m_expiredFail{true};
  QueueStats  m_qstats;
    void    RerrRateLimitTimerExpire();
    void DecayTimerExpire();
    std::map<Ipv4Address, Timer> m_addressReqTimer;
    void    RouteRequestTimerExpire(Ipv4Address dst);
    void    AckTimerExpire(Ipv4Address neighbor, Time blacklistTimeout);

    Ptr<UniformRandomVariable> m_uniformRandomVariable;
    Time m_lastBcastTime;
};

} // namespace qs2maodv
} // namespace ns3

#endif /* QS2MAODV_ROUTING_PROTOCOL_H */
