/*
 * QS-QMAODV Routing Protocol — Implementation
 *
 * Based on QMAODV (qmaodv-routing-protocol.cc).
 * QS-specific changes are marked with // [QS] comments.
 *
 * THREE KEY DIFFERENCES from QMAODV:
 *   [QS-1] State space: s = (dst, queue_bucket, energy_bucket)
 *   [QS-2] Epsilon: queue-triggered (θ_H=0.70 bump, θ_L=0.30 decay)
 *   [QS-3] Selection: hybrid score = Q(s,a) * (1 - q_a)^β  instead of pure ε-greedy
 *
 * All AODV routing logic (RREQ, RREP, RERR, hello, timers) is identical to QMAODV.
 *
 * Authors: QS-QMAODV Research Group — IUH
 */
#include "ns3/wifi-net-device.h"
#include "ns3/wifi-mac.h"
#include "ns3/wifi-mac-queue.h"
#include "ns3/wifi-mac-queue-container.h"
#include "ns3/arp-cache.h"
#include "ns3/ipv4-l3-protocol.h"
#include "ns3/mac48-address.h"
#include "ns3/nstime.h"

#define NS_LOG_APPEND_CONTEXT                                                                      \
    if (m_ipv4)                                                                                    \
    {                                                                                              \
        std::clog << "[node " << m_ipv4->GetObject<Node>()->GetId() << "] ";                       \
    }

#include "qs2maodv-routing-protocol.h"

#include "ns3/adhoc-wifi-mac.h"
#include "ns3/boolean.h"
#include "ns3/double.h"
#include "ns3/inet-socket-address.h"
#include "ns3/log.h"
#include "ns3/pointer.h"
#include "ns3/random-variable-stream.h"
#include "ns3/string.h"
#include "ns3/trace-source-accessor.h"
#include "ns3/udp-header.h"
#include "ns3/udp-l4-protocol.h"
#include "ns3/udp-socket-factory.h"
#include "ns3/wifi-mpdu.h"
#include "ns3/wifi-net-device.h"
#include "ns3/energy-source-container.h"
#include "ns3/basic-energy-source.h"

#include <algorithm>
#include <limits>
#include <cmath>

namespace ns3
{

NS_LOG_COMPONENT_DEFINE("Qs2maodvRoutingProtocol");

namespace qs2maodv
{

NS_OBJECT_ENSURE_REGISTERED(RoutingProtocol);

const uint32_t RoutingProtocol::QS2MAODV_PORT = 655;

// ============================================================
// DeferredRouteOutputTag — identical to QMAODV, renamed namespace
// ============================================================
class DeferredRouteOutputTag : public Tag
{
  public:
    DeferredRouteOutputTag(int32_t o = -1) : Tag(), m_oif(o) {}

    static TypeId GetTypeId()
    {
        static TypeId tid = TypeId("ns3::qs2maodv::DeferredRouteOutputTag")
                                .SetParent<Tag>()
                                .SetGroupName("Qs2Maodv")
                                .AddConstructor<DeferredRouteOutputTag>();
        return tid;
    }
    TypeId GetInstanceTypeId() const override { return GetTypeId(); }
    int32_t GetInterface() const { return m_oif; }
    void    SetInterface(int32_t oif) { m_oif = oif; }
    uint32_t GetSerializedSize() const override { return sizeof(int32_t); }
    void Serialize(TagBuffer i) const override { i.WriteU32(m_oif); }
    void Deserialize(TagBuffer i) override { m_oif = i.ReadU32(); }
    void Print(std::ostream& os) const override
    { os << "DeferredRouteOutputTag: output interface = " << m_oif; }
  private:
    int32_t m_oif;
};
NS_OBJECT_ENSURE_REGISTERED(DeferredRouteOutputTag);

// ============================================================
// Constructor
// ============================================================
RoutingProtocol::RoutingProtocol()
    : m_rreqRetries(2),
      m_ttlStart(2),
      m_ttlIncrement(2),
      m_ttlThreshold(7),
      m_timeoutBuffer(2),
      m_rreqRateLimit(10),
      m_rerrRateLimit(10),
      m_activeRouteTimeout(Seconds(3)),
      m_netDiameter(35),
      m_nodeTraversalTime(MilliSeconds(40)),
      m_netTraversalTime(Time((2 * m_netDiameter) * m_nodeTraversalTime)),
      m_pathDiscoveryTime(Time(2 * m_netTraversalTime)),
      m_myRouteTimeout(Time(2 * std::max(m_pathDiscoveryTime, m_activeRouteTimeout))),
      m_helloInterval(Seconds(1)),
      m_allowedHelloLoss(2),
      m_deletePeriod(Time(5 * std::max(m_activeRouteTimeout, m_helloInterval))),
      m_nextHopWait(m_nodeTraversalTime + MilliSeconds(10)),
      m_blackListTimeout(Time(m_rreqRetries * m_netTraversalTime)),
      m_maxQueueLen(64),
      m_maxQueueTime(Seconds(30)),
      m_destinationOnly(false),
      m_gratuitousReply(true),
      m_enableHello(true),
      m_enableBroadcast(true),
      m_routingTable(m_deletePeriod),
      m_queue(m_maxQueueLen, m_maxQueueTime),
      m_requestId(0),
      m_seqNo(0),
      m_rreqIdCache(m_pathDiscoveryTime),
      m_dpd(m_pathDiscoveryTime),
      m_nb(m_helloInterval),
      m_rreqCount(0),
      m_rerrCount(0),
      m_htimer(Timer::CANCEL_ON_DESTROY),
      m_rreqRateLimitTimer(Timer::CANCEL_ON_DESTROY),
      m_rerrRateLimitTimer(Timer::CANCEL_ON_DESTROY),
      m_lastBcastTime(Seconds(0))
{
    m_nb.SetCallback(MakeCallback(&RoutingProtocol::SendRerrWhenBreaksLinkToNextHop, this));
}

// ============================================================
// TypeId
// ============================================================
TypeId RoutingProtocol::GetTypeId()
{
    static TypeId tid =
        TypeId("ns3::qs2maodv::RoutingProtocol")
            .SetParent<Ipv4RoutingProtocol>()
            .SetGroupName("Qs2Maodv")
            .AddConstructor<RoutingProtocol>()
            // [QS] QS-QMAODV specific parameters
            .AddAttribute("MaxPaths",
                          "Maximum routes per destination.",
                          UintegerValue(3),
                          MakeUintegerAccessor(&RoutingProtocol::SetMaxPaths,
                                               &RoutingProtocol::GetMaxPaths),
                          MakeUintegerChecker<uint32_t>(1))
            .AddAttribute("Alpha", "Q-learning rate (fixed 0.30)",
                          DoubleValue(0.30),
                          MakeDoubleAccessor(&RoutingProtocol::m_alpha),
                          MakeDoubleChecker<double>(0.0, 1.0))
            .AddAttribute("Gamma", "Q-learning discount (fixed 0.90)",
                          DoubleValue(0.90),
                          MakeDoubleAccessor(&RoutingProtocol::m_gamma),
                          MakeDoubleChecker<double>(0.0, 1.0))
            .AddAttribute("Epsilon", "Initial ε (queue-triggered update)",
                          DoubleValue(0.30),
                          MakeDoubleAccessor(&RoutingProtocol::m_epsilon),
                          MakeDoubleChecker<double>(0.0, 1.0))
            .AddAttribute("RewardW1", "Reward weight for ACK (0.45)",
                          DoubleValue(0.45),
                          MakeDoubleAccessor(&RoutingProtocol::m_w1),
                          MakeDoubleChecker<double>())
            .AddAttribute("RewardW2", "Reward weight for delay (0.45)",
                          DoubleValue(0.45),
                          MakeDoubleAccessor(&RoutingProtocol::m_w2),
                          MakeDoubleChecker<double>())
            .AddAttribute("RewardW3", "Reward weight for queue-state: w3*(1-QueueLoad) (0.10)",
                          DoubleValue(0.10),
                          MakeDoubleAccessor(&RoutingProtocol::m_w3),
                          MakeDoubleChecker<double>())
            .AddAttribute("EnableDecay",
             "Ablation: enable ACK-silence Q-value decay",
             BooleanValue(true),
             MakeBooleanAccessor(&RoutingProtocol::m_enableDecay),
             MakeBooleanChecker())
        .AddAttribute ("SilenceThreshold",
                       "ACK-silence threshold (seconds) before Q-value decay is applied.",
                       DoubleValue (15.0),
                       MakeDoubleAccessor (&RoutingProtocol::m_silenceThreshold),
                       MakeDoubleChecker<double> (1.0, 120.0))
        .AddAttribute ("DecayFactor",
                       "Q-value decay multiplier applied to stale Q-table entries (0-1).",
                       DoubleValue (0.92),
                       MakeDoubleAccessor (&RoutingProtocol::m_decayFactor),
                       MakeDoubleChecker<double> (0.01, 1.0))
        .AddAttribute("AdaptiveW3",
             "Ablation: enable adaptive w3 congestion scaling",
             BooleanValue(true),
             MakeBooleanAccessor(&RoutingProtocol::m_adaptiveW3),
             MakeBooleanChecker())
        .AddAttribute("TrendEps",
             "Ablation: enable trend-based epsilon bump",
             BooleanValue(true),
             MakeBooleanAccessor(&RoutingProtocol::m_trendEps),
             MakeBooleanChecker())
        // ---------------- [FIX v2] -------------------------------------------
        .AddAttribute("MacQueueSignal",
             "Read the MAC best-effort queue (false = MAC term forced to 0, which is what "
             "v1 effectively did because GetTxopQueue(AC_BE) returned nullptr on a non-QoS MAC)",
             BooleanValue(true),
             MakeBooleanAccessor(&RoutingProtocol::m_macQueueSignal),
             MakeBooleanChecker())
        .AddAttribute("MacFeedback",
             "Use MAC-level ACK/drop of the forwarded MPDU as the reward signal "
             "(false = legacy 'neighbor freshness' reward computed at send time)",
             BooleanValue(true),
             MakeBooleanAccessor(&RoutingProtocol::m_macFeedback),
             MakeBooleanChecker())
        .AddAttribute("NextHopQueue",
             "Use the per-next-hop MAC queue occupancy in the hybrid score and reward "
             "(false = legacy node-level q_t and hop-count proxy)",
             BooleanValue(true),
             MakeBooleanAccessor(&RoutingProtocol::m_nextHopQueue),
             MakeBooleanChecker())
        .AddAttribute("QueueState",
             "Include the queue bucket in the Q-table state (dst, qBucket)",
             BooleanValue(true),
             MakeBooleanAccessor(&RoutingProtocol::m_queueState),
             MakeBooleanChecker())
        .AddAttribute("QueueEpsilon",
             "Enable queue-triggered epsilon adaptation",
             BooleanValue(true),
             MakeBooleanAccessor(&RoutingProtocol::m_queueEps),
             MakeBooleanChecker())
        .AddAttribute("HybridSelect",
             "Enable hybrid selection score Q*(1-q_a)^beta (false = argmax Q)",
             BooleanValue(true),
             MakeBooleanAccessor(&RoutingProtocol::m_hybridSelect),
             MakeBooleanChecker())
        .AddAttribute("DecayPositiveOnly",
             "ACK-silence decay only shrinks positive Q-values "
             "(false = legacy: negative Q-values are reset to 0)",
             BooleanValue(true),
             MakeBooleanAccessor(&RoutingProtocol::m_decayPosOnly),
             MakeBooleanChecker())
        .AddAttribute("DelayScale",
             "Delay normalisation (s): delay term = 1/(1+d/DelayScale)",
             DoubleValue(0.05),
             MakeDoubleAccessor(&RoutingProtocol::m_delayScale),
             MakeDoubleChecker<double>(1e-6))
        .AddAttribute("NextHopQueueRef",
             "Reference length (packets) for per-next-hop occupancy; 0 = MAC queue MaxSize",
             DoubleValue(0.0),
             MakeDoubleAccessor(&RoutingProtocol::m_nhQueueRef),
             MakeDoubleChecker<double>(0.0))
        .AddAttribute("ExpiredAsFailure",
             "Treat a forwarding decision without any MAC outcome after PendingTimeout "
             "as a failed delivery (reward -0.5)",
             BooleanValue(true),
             MakeBooleanAccessor(&RoutingProtocol::m_expiredFail),
             MakeBooleanChecker())
        .AddAttribute("PendingCheckPeriod",
             "How often undecided forwarding decisions are checked",
             TimeValue(MilliSeconds(500)),
             MakeTimeAccessor(&RoutingProtocol::m_purgePeriod),
             MakeTimeChecker())
        .AddAttribute("PendingTimeout",
             "Forget a forwarding decision if the MAC reports no outcome within this time",
             TimeValue(Seconds(2.0)),
             MakeTimeAccessor(&RoutingProtocol::m_pendingTimeout),
             MakeTimeChecker())
        .AddAttribute("InitialEnergy", "Initial node energy E_0 (Joules, unused in reward — kept for backward compat)",
                          DoubleValue(50.0),
                          MakeDoubleAccessor(&RoutingProtocol::m_initialEnergy),
                          MakeDoubleChecker<double>(0.0))
            // Standard AODV parameters (identical to QMAODV)
            .AddAttribute("HelloInterval", "HELLO messages emission interval.",
                          TimeValue(Seconds(1)),
                          MakeTimeAccessor(&RoutingProtocol::m_helloInterval),
                          MakeTimeChecker())
            .AddAttribute("TtlStart", "Initial TTL value for RREQ.",
                          UintegerValue(1),
                          MakeUintegerAccessor(&RoutingProtocol::m_ttlStart),
                          MakeUintegerChecker<uint16_t>())
            .AddAttribute("TtlIncrement", "TTL increment for expanding ring search.",
                          UintegerValue(2),
                          MakeUintegerAccessor(&RoutingProtocol::m_ttlIncrement),
                          MakeUintegerChecker<uint16_t>())
            .AddAttribute("TtlThreshold", "Maximum TTL for expanding ring search.",
                          UintegerValue(7),
                          MakeUintegerAccessor(&RoutingProtocol::m_ttlThreshold),
                          MakeUintegerChecker<uint16_t>())
            .AddAttribute("TimeoutBuffer", "Buffer for timeout.",
                          UintegerValue(2),
                          MakeUintegerAccessor(&RoutingProtocol::m_timeoutBuffer),
                          MakeUintegerChecker<uint16_t>())
            .AddAttribute("RreqRetries", "Maximum RREQ retransmissions.",
                          UintegerValue(2),
                          MakeUintegerAccessor(&RoutingProtocol::m_rreqRetries),
                          MakeUintegerChecker<uint32_t>())
            .AddAttribute("RreqRateLimit", "Maximum RREQ per second.",
                          UintegerValue(10),
                          MakeUintegerAccessor(&RoutingProtocol::m_rreqRateLimit),
                          MakeUintegerChecker<uint32_t>())
            .AddAttribute("RerrRateLimit", "Maximum RERR per second.",
                          UintegerValue(10),
                          MakeUintegerAccessor(&RoutingProtocol::m_rerrRateLimit),
                          MakeUintegerChecker<uint32_t>())
            .AddAttribute("NodeTraversalTime", "One hop traversal time estimate.",
                          TimeValue(MilliSeconds(40)),
                          MakeTimeAccessor(&RoutingProtocol::m_nodeTraversalTime),
                          MakeTimeChecker())
            .AddAttribute("NextHopWait", "Wait for neighbour RREP_ACK.",
                          TimeValue(MilliSeconds(50)),
                          MakeTimeAccessor(&RoutingProtocol::m_nextHopWait),
                          MakeTimeChecker())
            .AddAttribute("ActiveRouteTimeout", "Period route is considered valid.",
                          TimeValue(Seconds(3)),
                          MakeTimeAccessor(&RoutingProtocol::m_activeRouteTimeout),
                          MakeTimeChecker())
            .AddAttribute("MyRouteTimeout",
                          "Lifetime field in RREP generated by this node.",
                          TimeValue(Seconds(11.2)),
                          MakeTimeAccessor(&RoutingProtocol::m_myRouteTimeout),
                          MakeTimeChecker())
            .AddAttribute("BlackListTimeout", "Time node is in blacklist.",
                          TimeValue(Seconds(5.6)),
                          MakeTimeAccessor(&RoutingProtocol::m_blackListTimeout),
                          MakeTimeChecker())
            .AddAttribute("DeletePeriod", "Upper bound for route invalidation propagation.",
                          TimeValue(Seconds(15)),
                          MakeTimeAccessor(&RoutingProtocol::m_deletePeriod),
                          MakeTimeChecker())
            .AddAttribute("NetDiameter", "Max possible hops between two nodes.",
                          UintegerValue(35),
                          MakeUintegerAccessor(&RoutingProtocol::m_netDiameter),
                          MakeUintegerChecker<uint32_t>())
            .AddAttribute("NetTraversalTime",
                          "Estimate of average net traversal time.",
                          TimeValue(Seconds(2.8)),
                          MakeTimeAccessor(&RoutingProtocol::m_netTraversalTime),
                          MakeTimeChecker())
            .AddAttribute("PathDiscoveryTime",
                          "Estimate of maximum route discovery time.",
                          TimeValue(Seconds(5.6)),
                          MakeTimeAccessor(&RoutingProtocol::m_pathDiscoveryTime),
                          MakeTimeChecker())
            .AddAttribute("MaxQueueLen", "Maximum buffered packets.",
                          UintegerValue(64),
                          MakeUintegerAccessor(&RoutingProtocol::SetMaxQueueLen,
                                               &RoutingProtocol::GetMaxQueueLen),
                          MakeUintegerChecker<uint32_t>())
            .AddAttribute("MaxQueueTime", "Maximum packet buffering time.",
                          TimeValue(Seconds(30)),
                          MakeTimeAccessor(&RoutingProtocol::SetMaxQueueTime,
                                           &RoutingProtocol::GetMaxQueueTime),
                          MakeTimeChecker())
            .AddAttribute("AllowedHelloLoss", "Hello messages allowed to miss.",
                          UintegerValue(2),
                          MakeUintegerAccessor(&RoutingProtocol::m_allowedHelloLoss),
                          MakeUintegerChecker<uint16_t>())
            .AddAttribute("GratuitousReply", "Send gratuitous RREP.",
                          BooleanValue(true),
                          MakeBooleanAccessor(&RoutingProtocol::SetGratuitousReplyFlag,
                                              &RoutingProtocol::GetGratuitousReplyFlag),
                          MakeBooleanChecker())
            .AddAttribute("DestinationOnly", "Only destination may respond to RREQ.",
                          BooleanValue(false),
                          MakeBooleanAccessor(&RoutingProtocol::SetDestinationOnlyFlag,
                                              &RoutingProtocol::GetDestinationOnlyFlag),
                          MakeBooleanChecker())
            .AddAttribute("EnableHello", "Enable hello messages.",
                          BooleanValue(true),
                          MakeBooleanAccessor(&RoutingProtocol::SetHelloEnable,
                                              &RoutingProtocol::GetHelloEnable),
                          MakeBooleanChecker())
            .AddAttribute("EnableBroadcast", "Enable broadcast data forwarding.",
                          BooleanValue(true),
                          MakeBooleanAccessor(&RoutingProtocol::SetBroadcastEnable,
                                              &RoutingProtocol::GetBroadcastEnable),
                          MakeBooleanChecker())
            .AddAttribute("UniformRv", "Underlying uniform random variable.",
                          StringValue("ns3::UniformRandomVariable"),
                          MakePointerAccessor(&RoutingProtocol::m_uniformRandomVariable),
                          MakePointerChecker<UniformRandomVariable>());
    return tid;
}

void RoutingProtocol::SetMaxQueueLen(uint32_t len)
{ m_maxQueueLen = len; m_queue.SetMaxQueueLen(len); }

void RoutingProtocol::SetMaxQueueTime(Time t)
{ m_maxQueueTime = t; m_queue.SetQueueTimeout(t); }

RoutingProtocol::~RoutingProtocol() {}

void RoutingProtocol::DoDispose()
{
    m_purgeEvent.Cancel();
    m_ipv4 = nullptr;
    for (auto iter = m_socketAddresses.begin(); iter != m_socketAddresses.end(); iter++)
        iter->first->Close();
    m_socketAddresses.clear();
    for (auto iter = m_socketSubnetBroadcastAddresses.begin();
         iter != m_socketSubnetBroadcastAddresses.end(); iter++)
        iter->first->Close();
    m_socketSubnetBroadcastAddresses.clear();
    Ipv4RoutingProtocol::DoDispose();
}

void RoutingProtocol::PrintRoutingTable(Ptr<OutputStreamWrapper> stream, Time::Unit unit) const
{
    *stream->GetStream() << "Node: " << m_ipv4->GetObject<Node>()->GetId()
                         << "; Time: " << Now().As(unit)
                         << ", Local time: " << m_ipv4->GetObject<Node>()->GetLocalTime().As(unit)
                         << ", QS2MAODV Routing table" << std::endl;
    m_routingTable.Print(stream, unit);
    *stream->GetStream() << std::endl;
    m_qtable.Print(*stream->GetStream());  // [QS] also print Q-table
}

int64_t RoutingProtocol::AssignStreams(int64_t stream)
{
    NS_LOG_FUNCTION(this << stream);
    m_uniformRandomVariable->SetStream(stream);
    return 1;
}

// ============================================================
// [QS-1] Cross-layer: queue occupancy
// ============================================================
double RoutingProtocol::GetRoutingQueueOccupancy()
{
    if (m_maxQueueLen == 0) return 0.0;
    return std::min(1.0, static_cast<double>(m_queue.GetSize()) /
                         static_cast<double>(m_maxQueueLen));
}

// [FIX v2] The original code called GetTxopQueue(AC_BE). With IEEE 802.11b and
// AdhocWifiMac, QosSupported defaults to false, so the MAC has no EDCA queues:
// GetQosTxop(AC_BE) returns nullptr and the MAC term was ALWAYS 0. The non-QoS
// DCF queue must be fetched with AC_BE_NQOS.
Ptr<WifiMacQueue> RoutingProtocol::GetBeQueue()
{
    if (!m_ipv4) return nullptr;
    Ptr<Node> node = m_ipv4->GetObject<Node>();
    if (!node) return nullptr;
    for (uint32_t i = 0; i < node->GetNDevices(); i++)
    {
        Ptr<WifiNetDevice> wifiDev = DynamicCast<WifiNetDevice>(node->GetDevice(i));
        if (!wifiDev) continue;
        Ptr<WifiMac> mac = wifiDev->GetMac();
        if (!mac) continue;
        Ptr<WifiMacQueue> q = mac->GetQosSupported() ? mac->GetTxopQueue(AC_BE)
                                                     : mac->GetTxopQueue(AC_BE_NQOS);
        if (q) return q;
    }
    return nullptr;
}

bool RoutingProtocol::GetBeQueueIsQos()
{
    if (!m_ipv4) return false;
    Ptr<Node> node = m_ipv4->GetObject<Node>();
    for (uint32_t i = 0; node && i < node->GetNDevices(); i++)
    {
        Ptr<WifiNetDevice> wifiDev = DynamicCast<WifiNetDevice>(node->GetDevice(i));
        if (wifiDev && wifiDev->GetMac()) return wifiDev->GetMac()->GetQosSupported();
    }
    return false;
}

double RoutingProtocol::GetMacQueueOccupancy()
{
    if (!m_macQueueSignal) return 0.0;   // ablation / exact v1 reproduction (v1 always read 0)
    Ptr<WifiMacQueue> q = GetBeQueue();
    if (!q) return 0.0;
    uint32_t maxPkt = q->GetMaxSize().GetValue();
    if (maxPkt == 0) return 0.0;
    return std::min(1.0, static_cast<double>(q->GetNPackets()) / static_cast<double>(maxPkt));
}

double RoutingProtocol::GetQueueOccupancy()
{
    double routingQt = GetRoutingQueueOccupancy();
    double macQt     = GetMacQueueOccupancy();
    // [FIX v2] diagnostics: lets the paper report how often the MAC signal is non-zero
    m_qstats.samples++;
    m_qstats.macSum   += macQt;
    m_qstats.routeSum += routingQt;
    m_qstats.macMax    = std::max(m_qstats.macMax, macQt);
    if (macQt > 0.0) m_qstats.macPositive++;
    return std::max(routingQt, macQt);
}

bool RoutingProtocol::LookupMac(Ipv4Address ip, Mac48Address& mac)
{
    if (!m_ipv4) return false;
    Ptr<Ipv4L3Protocol> l3 = m_ipv4->GetObject<Ipv4L3Protocol>();
    if (!l3) return false;
    for (uint32_t i = 0; i < l3->GetNInterfaces(); i++)
    {
        Ptr<ArpCache> arp = l3->GetInterface(i)->GetArpCache();
        if (!arp) continue;
        ArpCache::Entry* entry = arp->Lookup(ip);
        if (entry && (entry->IsAlive() || entry->IsPermanent() || entry->IsAutoGenerated()))
        {
            mac = Mac48Address::ConvertFrom(entry->GetMacAddress());
            return true;
        }
    }
    return false;
}

double RoutingProtocol::GetNextHopQueueOccupancy(Ipv4Address nh)
{
    double rq = GetRoutingQueueOccupancy();
    Ptr<WifiMacQueue> q = GetBeQueue();
    Mac48Address mac;
    if (!m_macQueueSignal || !q || !LookupMac(nh, mac)) return rq;
    uint32_t n = 0;
    if (GetBeQueueIsQos())
    {
        // Best-effort user priorities map to TID 0 and TID 3
        for (uint8_t tid : {uint8_t(0), uint8_t(3)})
            n += q->GetNPackets(MakeWifiUnicastQueueId(WIFI_QOSDATA_QUEUE, mac, tid));
    }
    else
    {
        n = q->GetNPackets(MakeWifiUnicastQueueId(WIFI_DATA_QUEUE, mac));
    }
    double ref = (m_nhQueueRef > 0.0) ? m_nhQueueRef
                                      : static_cast<double>(q->GetMaxSize().GetValue());
    double qn = (ref > 0.0) ? std::min(1.0, n / ref) : 0.0;
    return std::max(rq, qn);
}

// ============================================================
// [QS-1] Legacy: energy ratio (no longer used in reward; kept for API compat)
// ============================================================
double RoutingProtocol::GetEnergyRatio() const
{
    if (!m_ipv4) return 1.0;
    Ptr<Node> node = m_ipv4->GetObject<Node>();
    if (!node) return 1.0;
    Ptr<energy::EnergySourceContainer> esc = node->GetObject<energy::EnergySourceContainer>();
    if (esc && esc->GetN() > 0)
    {
        Ptr<energy::EnergySource> src = esc->Get(0);
        if (src && m_initialEnergy > 0.0)
            return std::max(0.0, std::min(1.0, src->GetRemainingEnergy() / m_initialEnergy));
    }
    return 1.0;
}

// ============================================================
// Start — configure Q-table
// ============================================================
void RoutingProtocol::Start()
{
    // [QS] Push hyper-parameters into Q-table
    m_qtable.SetMaxPaths(m_maxPaths);
    m_qtable.SetLearningParameters(m_alpha, m_gamma, m_epsilon);
    m_qtable.SetRewardWeights(m_w1, m_w2, m_w3);
    m_qtable.SetAdaptiveW3(m_adaptiveW3);
    m_qtable.SetTrendEps(m_trendEps);
    // [FIX v2]
    m_qtable.SetQueueEpsilon(m_queueEps);
    m_qtable.SetHybridSelect(m_hybridSelect);
    m_qtable.SetDecayPositiveOnly(m_decayPosOnly);
    m_qtable.SetDelayScale(m_macFeedback ? m_delayScale : 1.0);
    if (m_nextHopQueue)
        m_qtable.SetNextHopQueueFn([this](Ipv4Address nh) { return GetNextHopQueueOccupancy(nh); });
    if (m_macFeedback)
        m_purgeEvent = Simulator::Schedule(m_purgePeriod, &RoutingProtocol::PurgeTick, this);

    NS_LOG_FUNCTION(this);
    if (m_enableHello)
        m_nb.ScheduleTimer();
    m_rreqRateLimitTimer.SetFunction(&RoutingProtocol::RreqRateLimitTimerExpire, this);
    m_rreqRateLimitTimer.Schedule(Seconds(1));
    m_rerrRateLimitTimer.SetFunction(&RoutingProtocol::RerrRateLimitTimerExpire, this);
    m_rerrRateLimitTimer.Schedule(Seconds(1));
}

// ============================================================
// RouteOutput
// [QS-1] Build 3D state  [QS-2] Update ε  [QS-3] Hybrid select
// ============================================================
Ptr<Ipv4Route> RoutingProtocol::RouteOutput(Ptr<Packet> p,
                                              const Ipv4Header& header,
                                              Ptr<NetDevice> oif,
                                              Socket::SocketErrno& sockerr)
{
    NS_LOG_FUNCTION(this << header << (oif ? oif->GetIfIndex() : 0));
    if (!p)
    {
        NS_LOG_DEBUG("Packet is == 0");
        return LoopbackRoute(header, oif);
    }
    if (m_socketAddresses.empty())
    {
        sockerr = Socket::ERROR_NOROUTETOHOST;
        NS_LOG_LOGIC("No qs2maodv interfaces");
        Ptr<Ipv4Route> route;
        return route;
    }
    sockerr = Socket::ERROR_NOTERROR;
    Ptr<Ipv4Route> route;
    Ipv4Address dst = header.GetDestination();
    RoutingTableEntry rt;

    if (m_routingTable.LookupValidRoute(dst, rt))
    {
        // [QS-1] Read cross-layer signals
        double qt = GetQueueOccupancy();

        // [QS-2] Queue-triggered epsilon update
        m_qtable.UpdateEpsilon(qt);

        // [QS-1] Build state key (dst, queueBucket)
        double qs = m_queueState ? qt : 0.0;   // [FIX v2] ablation switch
        QsStateKey state = QsQTable::BuildState(dst, qs, qs);

        // [QS-3] Hybrid selection: score = Q(s,a) * (1-q_a)^β
        RoutingTableEntry chosenRt = rt;
        m_qtable.EnsureRecord(rt, state);
        m_qtable.SelectHybrid(rt, state, qt, chosenRt, &m_routingTable);

        // [FIX v2] Reward from the MAC outcome of THIS packet on the chosen next hop.
        // The decision is stored and resolved in NotifyAckedMpdu / NotifyTxError.
        if (m_macFeedback)
        {
            double qn = m_nextHopQueue ? GetNextHopQueueOccupancy(chosenRt.GetNextHop()) : qt;
            m_qtable.EnsureRecord(chosenRt, state);
            m_pending[p->GetUid()] = PendingTx{chosenRt.GetNextHop(), state, qn, Simulator::Now()};
            if (m_pending.size() > 20000) PurgePending();
        }
        else
        // Legacy per-packet Q-update (neighbor freshness reward — same pattern as QMAODV fix-v2)
        {
            RoutingTableEntry nbrCheck;
            bool fresh = m_routingTable.LookupRoute(chosenRt.GetNextHop(), nbrCheck)
                         && nbrCheck.GetFlag() == VALID
                         && nbrCheck.GetLifeTime() > Seconds(0);
            double ack    = fresh ? 1.0 : 0.0;
            double delayS = fresh ? 0.005 : 1.0;
            m_qtable.UpdateQValueOrCreate(chosenRt, state, ack, delayS, qt);
        }

        route = chosenRt.GetRoute();
        NS_ASSERT(route);
        NS_LOG_DEBUG("QS RouteOutput dst=" << dst
                     << " via=" << route->GetGateway()
                     << " qt=" << qt << " qt=" << qt
                     << " ε=" << m_qtable.GetEpsilon()
                     << " qBkt=" << state.qBucket << " eBkt=" << state.eBucket);

        if (oif && route->GetOutputDevice() != oif)
        {
            NS_LOG_DEBUG("Output device doesn't match. Dropped.");
            sockerr = Socket::ERROR_NOROUTETOHOST;
            return Ptr<Ipv4Route>();
        }
        UpdateRouteLifeTime(dst, m_activeRouteTimeout);
        UpdateRouteLifeTime(route->GetGateway(), m_activeRouteTimeout);
        return route;
    }

    // No valid route → loopback + defer
    uint32_t iif = (oif ? m_ipv4->GetInterfaceForDevice(oif) : -1);
    DeferredRouteOutputTag tag(iif);
    NS_LOG_DEBUG("Valid Route not found");
    if (!p->PeekPacketTag(tag))
        p->AddPacketTag(tag);
    return LoopbackRoute(header, oif);
}

// ============================================================
// DeferredRouteOutput — identical to QMAODV
// ============================================================
void RoutingProtocol::DeferredRouteOutput(Ptr<const Packet> p,
                                           const Ipv4Header& header,
                                           UnicastForwardCallback ucb,
                                           ErrorCallback ecb)
{
    NS_LOG_FUNCTION(this << p << header);
    NS_ASSERT(p && p != Ptr<Packet>());

    QueueEntry newEntry(p, header, ucb, ecb);
    bool result = m_queue.Enqueue(newEntry);
    if (result)
    {
        NS_LOG_LOGIC("Add packet " << p->GetUid() << " to queue. Protocol "
                                   << (uint16_t)header.GetProtocol());
        RoutingTableEntry rt;
        bool found = m_routingTable.LookupRoute(header.GetDestination(), rt);
        if (!found || ((rt.GetFlag() != IN_SEARCH) && found))
        {
            NS_LOG_LOGIC("Send new RREQ for outbound packet to " << header.GetDestination());
            SendRequest(header.GetDestination());
        }
    }
}

// ============================================================
// RouteInput — identical to QMAODV
// ============================================================
bool RoutingProtocol::RouteInput(Ptr<const Packet> p,
                                  const Ipv4Header& header,
                                  Ptr<const NetDevice> idev,
                                  const UnicastForwardCallback& ucb,
                                  const MulticastForwardCallback& mcb,
                                  const LocalDeliverCallback& lcb,
                                  const ErrorCallback& ecb)
{
    NS_LOG_FUNCTION(this << p->GetUid() << header.GetDestination() << idev->GetAddress());
    if (m_socketAddresses.empty())
    {
        NS_LOG_LOGIC("No qs2maodv interfaces");
        return false;
    }
    NS_ASSERT(m_ipv4);
    NS_ASSERT(p);
    NS_ASSERT(m_ipv4->GetInterfaceForDevice(idev) >= 0);
    int32_t iif = m_ipv4->GetInterfaceForDevice(idev);

    Ipv4Address dst    = header.GetDestination();
    Ipv4Address origin = header.GetSource();

    // Deferred route request
    if (idev == m_lo)
    {
        DeferredRouteOutputTag tag;
        if (p->PeekPacketTag(tag))
        {
            DeferredRouteOutput(p, header, ucb, ecb);
            return true;
        }
    }

    if (IsMyOwnAddress(origin))
        return true;

    if (dst.IsMulticast())
        return false;

    // Broadcast local delivery/forwarding
    for (auto j = m_socketAddresses.begin(); j != m_socketAddresses.end(); ++j)
    {
        Ipv4InterfaceAddress iface = j->second;
        if (m_ipv4->GetInterfaceForAddress(iface.GetLocal()) == iif)
        {
            if (dst == iface.GetBroadcast() || dst.IsBroadcast())
            {
                if (m_dpd.IsDuplicate(p, header))
                {
                    NS_LOG_DEBUG("Duplicated packet " << p->GetUid() << " from " << origin << ". Drop.");
                    return true;
                }
                UpdateRouteLifeTime(origin, m_activeRouteTimeout);
                Ptr<Packet> packet = p->Copy();
                if (!lcb.IsNull())
                {
                    NS_LOG_LOGIC("Broadcast local delivery to " << iface.GetLocal());
                    lcb(p, header, iif);
                }
                else
                {
                    NS_LOG_ERROR("Unable to deliver packet locally due to null callback "
                                 << p->GetUid() << " from " << origin);
                    ecb(p, header, Socket::ERROR_NOROUTETOHOST);
                }
                if (!m_enableBroadcast)
                    return true;
                if (header.GetProtocol() == UdpL4Protocol::PROT_NUMBER)
                {
                    UdpHeader udpHeader;
                    p->PeekHeader(udpHeader);
                    if (udpHeader.GetDestinationPort() == QS2MAODV_PORT)
                        return true;
                }
                if (header.GetTtl() > 1)
                {
                    NS_LOG_LOGIC("Forward broadcast. TTL " << (uint16_t)header.GetTtl());
                    RoutingTableEntry toBroadcast;
                    if (m_routingTable.LookupRoute(dst, toBroadcast))
                    {
                        Ptr<Ipv4Route> route = toBroadcast.GetRoute();
                        ucb(route, packet, header);
                    }
                    else
                    {
                        NS_LOG_DEBUG("No route to forward broadcast. Drop packet " << p->GetUid());
                    }
                }
                else
                {
                    NS_LOG_DEBUG("TTL exceeded. Drop packet " << p->GetUid());
                }
                return true;
            }
        }
    }

    // Unicast local delivery
    if (m_ipv4->IsDestinationAddress(dst, iif))
    {
        UpdateRouteLifeTime(origin, m_activeRouteTimeout);
        RoutingTableEntry toOrigin;
        if (m_routingTable.LookupValidRoute(origin, toOrigin))
        {
            UpdateRouteLifeTime(toOrigin.GetNextHop(), m_activeRouteTimeout);
            m_nb.Update(toOrigin.GetNextHop(), m_activeRouteTimeout);
        }
        if (!lcb.IsNull())
        {
            NS_LOG_LOGIC("Unicast local delivery to " << dst);
            lcb(p, header, iif);
        }
        else
        {
            NS_LOG_ERROR("Unable to deliver packet locally due to null callback "
                         << p->GetUid() << " from " << origin);
            ecb(p, header, Socket::ERROR_NOROUTETOHOST);
        }
        return true;
    }

    if (!m_ipv4->IsForwarding(iif))
    {
        NS_LOG_LOGIC("Forwarding disabled for this interface");
        ecb(p, header, Socket::ERROR_NOROUTETOHOST);
        return true;
    }

    return Forwarding(p, header, ucb, ecb);
}

// ============================================================
// Forwarding — identical to QMAODV (no QS changes needed here;
// QS selection already happened at RouteOutput)
// ============================================================
bool RoutingProtocol::Forwarding(Ptr<const Packet> p,
                                  const Ipv4Header& header,
                                  UnicastForwardCallback ucb,
                                  ErrorCallback ecb)
{
    NS_LOG_FUNCTION(this);
    Ipv4Address dst    = header.GetDestination();
    Ipv4Address origin = header.GetSource();
    m_routingTable.Purge();
    RoutingTableEntry toDst;
    if (m_routingTable.LookupRoute(dst, toDst))
    {
        if (toDst.GetFlag() == VALID)
        {
            Ptr<Ipv4Route> route = toDst.GetRoute();
            NS_LOG_LOGIC(route->GetSource() << " forwarding to " << dst
                         << " from " << origin << " packet " << p->GetUid());
            UpdateRouteLifeTime(origin, m_activeRouteTimeout);
            UpdateRouteLifeTime(dst, m_activeRouteTimeout);
            UpdateRouteLifeTime(route->GetGateway(), m_activeRouteTimeout);
            RoutingTableEntry toOrigin;
            m_routingTable.LookupRoute(origin, toOrigin);
            UpdateRouteLifeTime(toOrigin.GetNextHop(), m_activeRouteTimeout);
            m_nb.Update(route->GetGateway(), m_activeRouteTimeout);
            m_nb.Update(toOrigin.GetNextHop(), m_activeRouteTimeout);
            ucb(route, p, header);
            return true;
        }
        else
        {
            if (toDst.GetValidSeqNo())
            {
                SendRerrWhenNoRouteToForward(dst, toDst.GetSeqNo(), origin);
                NS_LOG_DEBUG("Drop packet " << p->GetUid() << " because no route to forward it.");
                return false;
            }
        }
    }
    NS_LOG_LOGIC("route not found to " << dst << ". Send RERR message.");
    NS_LOG_DEBUG("Drop packet " << p->GetUid() << " because no route to forward it.");
    SendRerrWhenNoRouteToForward(dst, 0, origin);
    return false;
}

// ============================================================
// SetIpv4 / NotifyInterface* — identical to QMAODV, port renamed
// ============================================================
void RoutingProtocol::SetIpv4(Ptr<Ipv4> ipv4)
{
    NS_ASSERT(ipv4);
    NS_ASSERT(!m_ipv4);
    m_ipv4 = ipv4;
    NS_ASSERT(m_ipv4->GetNInterfaces() == 1 &&
              m_ipv4->GetAddress(0, 0).GetLocal() == Ipv4Address("127.0.0.1"));
    m_lo = m_ipv4->GetNetDevice(0);
    NS_ASSERT(m_lo);
    RoutingTableEntry rt(m_lo,
                         Ipv4Address::GetLoopback(), true, 0,
                         Ipv4InterfaceAddress(Ipv4Address::GetLoopback(), Ipv4Mask("255.0.0.0")),
                         1, Ipv4Address::GetLoopback(),
                         Simulator::GetMaximumSimulationTime());
    m_routingTable.AddRoute(rt);
    Simulator::ScheduleNow(&RoutingProtocol::Start, this);
}

void RoutingProtocol::NotifyInterfaceUp(uint32_t i)
{
    NS_LOG_FUNCTION(this << m_ipv4->GetAddress(i, 0).GetLocal());
    Ptr<Ipv4L3Protocol> l3 = m_ipv4->GetObject<Ipv4L3Protocol>();
    if (l3->GetNAddresses(i) > 1)
        NS_LOG_WARN("QS2MAODV does not work with more then one address per each interface.");
    Ipv4InterfaceAddress iface = l3->GetAddress(i, 0);
    if (iface.GetLocal() == Ipv4Address("127.0.0.1"))
        return;

    // [FIX v13] Single socket per interface bound to 0.0.0.0:port.
    // Previously two sockets (unicast + broadcast) caused a conflict:
    // the 0.0.0.0 socket captured all inbound packets and the unicast
    // socket never fired its RecvCallback → RecvQs2maodv never called
    // → no RREQ/RREP processed → PDR = 0%.
    // One socket per interface matches upstream AODV/QMAODV design.
    Ptr<Socket> socket = Socket::CreateSocket(GetObject<Node>(), UdpSocketFactory::GetTypeId());
    NS_ASSERT(socket);
    socket->SetRecvCallback(MakeCallback(&RoutingProtocol::RecvQs2maodv, this));
    socket->BindToNetDevice(l3->GetNetDevice(i));
    socket->Bind(InetSocketAddress(Ipv4Address::GetAny(), QS2MAODV_PORT));
    socket->SetAllowBroadcast(true);
    socket->SetIpRecvTtl(true);
    m_socketAddresses.insert(std::make_pair(socket, iface));
    // m_socketSubnetBroadcastAddresses intentionally left empty.

    Ptr<NetDevice> dev = m_ipv4->GetNetDevice(m_ipv4->GetInterfaceForAddress(iface.GetLocal()));
    RoutingTableEntry rt(dev, iface.GetBroadcast(), true, 0, iface, 1, iface.GetBroadcast(),
                         Simulator::GetMaximumSimulationTime());
    m_routingTable.AddRoute(rt);
    // Route for limited broadcast 255.255.255.255
    RoutingTableEntry rtBcast(dev, Ipv4Address("255.255.255.255"), true, 0, iface, 1,
                              Ipv4Address("255.255.255.255"),
                              Simulator::GetMaximumSimulationTime());
    m_routingTable.AddRoute(rtBcast);

    if (l3->GetInterface(i)->GetArpCache())
        m_nb.AddArpCache(l3->GetInterface(i)->GetArpCache());

    Ptr<WifiNetDevice> wifi = dev->GetObject<WifiNetDevice>();
    if (!wifi) return;
    Ptr<WifiMac> mac = wifi->GetMac();
    if (!mac) return;
    mac->TraceConnectWithoutContext("DroppedMpdu",
                                    MakeCallback(&RoutingProtocol::NotifyTxError, this));
    // [FIX v2] successful hop-level delivery feedback
    mac->TraceConnectWithoutContext("AckedMpdu",
                                    MakeCallback(&RoutingProtocol::NotifyAckedMpdu, this));
}

void RoutingProtocol::NotifyTxError(WifiMacDropReason reason, Ptr<const WifiMpdu> mpdu)
{
    m_nb.GetTxErrorCallback()(mpdu->GetHeader());
    if (m_macFeedback && !m_pending.empty())
        ResolvePending(mpdu->GetPacket()->GetUid(), false);   // [FIX v2]
}

// ============================================================
// [FIX v2] MAC-level feedback
// ============================================================
void RoutingProtocol::NotifyAckedMpdu(Ptr<const WifiMpdu> mpdu)
{
    if (m_macFeedback && !m_pending.empty())
        ResolvePending(mpdu->GetPacket()->GetUid(), true);
}

void RoutingProtocol::ResolvePending(uint64_t uid, bool success)
{
    auto it = m_pending.find(uid);
    if (it == m_pending.end()) return;
    const PendingTx& pt = it->second;
    double delayS = (Simulator::Now() - pt.t0).GetSeconds();   // queueing + access + retries
    // UpdateQValue (not ...OrCreate): if the route was deleted meanwhile, nothing is resurrected
    m_qtable.UpdateQValue(pt.state, pt.nextHop, success ? 1.0 : 0.0, delayS, pt.qt);
    if (success) m_qstats.fbAck++; else m_qstats.fbDrop++;
    m_pending.erase(it);
}

void RoutingProtocol::PurgePending()
{
    Time now = Simulator::Now();
    for (auto it = m_pending.begin(); it != m_pending.end();)
    {
        if (now - it->second.t0 > m_pendingTimeout)
        {
            // No MAC outcome: the packet never reached the MAC (no ARP reply, dropped in
            // the IP/ARP layer, interface down). The decision did not deliver -> failure.
            if (m_expiredFail)
                m_qtable.UpdateQValue(it->second.state, it->second.nextHop, 0.0,
                                      (now - it->second.t0).GetSeconds(), it->second.qt);
            m_qstats.fbExpired++;
            it = m_pending.erase(it);
        }
        else ++it;
    }
}

void RoutingProtocol::PurgeTick()
{
    PurgePending();
    m_purgeEvent = Simulator::Schedule(m_purgePeriod, &RoutingProtocol::PurgeTick, this);
}

void RoutingProtocol::NotifyInterfaceDown(uint32_t i)
{
    NS_LOG_FUNCTION(this << m_ipv4->GetAddress(i, 0).GetLocal());
    Ptr<Ipv4L3Protocol> l3 = m_ipv4->GetObject<Ipv4L3Protocol>();
    Ptr<NetDevice> dev = l3->GetNetDevice(i);
    Ptr<WifiNetDevice> wifi = dev->GetObject<WifiNetDevice>();
    if (wifi)
    {
        Ptr<WifiMac> mac = wifi->GetMac()->GetObject<AdhocWifiMac>();
        if (mac)
        {
            mac->TraceDisconnectWithoutContext("DroppedMpdu",
                                               MakeCallback(&RoutingProtocol::NotifyTxError, this));
            m_nb.DelArpCache(l3->GetInterface(i)->GetArpCache());
        }
    }
    Ptr<Socket> socket = FindSocketWithInterfaceAddress(m_ipv4->GetAddress(i, 0));
    NS_ASSERT(socket);
    socket->Close();
    m_socketAddresses.erase(socket);
    socket = FindSubnetBroadcastSocketWithInterfaceAddress(m_ipv4->GetAddress(i, 0));
    NS_ASSERT(socket);
    socket->Close();
    m_socketSubnetBroadcastAddresses.erase(socket);
    if (m_socketAddresses.empty())
    {
        NS_LOG_LOGIC("No qs2maodv interfaces");
        m_htimer.Cancel();
        m_nb.Clear();
        m_routingTable.Clear();
        return;
    }
    m_routingTable.DeleteAllRoutesFromInterface(m_ipv4->GetAddress(i, 0));
}

void RoutingProtocol::NotifyAddAddress(uint32_t i, Ipv4InterfaceAddress address)
{
    NS_LOG_FUNCTION(this << " interface " << i << " address " << address);
    Ptr<Ipv4L3Protocol> l3 = m_ipv4->GetObject<Ipv4L3Protocol>();
    if (!l3->IsUp(i)) return;
    if (l3->GetNAddresses(i) == 1)
    {
        Ipv4InterfaceAddress iface = l3->GetAddress(i, 0);
        Ptr<Socket> socket = FindSocketWithInterfaceAddress(iface);
        if (!socket)
        {
            if (iface.GetLocal() == Ipv4Address("127.0.0.1")) return;
            // [FIX v13] Single socket per interface (see NotifyInterfaceUp)
            Ptr<Socket> socket = Socket::CreateSocket(GetObject<Node>(), UdpSocketFactory::GetTypeId());
            NS_ASSERT(socket);
            socket->SetRecvCallback(MakeCallback(&RoutingProtocol::RecvQs2maodv, this));
            socket->BindToNetDevice(l3->GetNetDevice(i));
            socket->Bind(InetSocketAddress(Ipv4Address::GetAny(), QS2MAODV_PORT));
            socket->SetAllowBroadcast(true);
            socket->SetIpRecvTtl(true);
            m_socketAddresses.insert(std::make_pair(socket, iface));
            Ptr<NetDevice> dev = m_ipv4->GetNetDevice(m_ipv4->GetInterfaceForAddress(iface.GetLocal()));
            RoutingTableEntry rt(dev, iface.GetBroadcast(), true, 0, iface, 1, iface.GetBroadcast(),
                                 Simulator::GetMaximumSimulationTime());
            m_routingTable.AddRoute(rt);
        }
    }
    else
    {
        NS_LOG_LOGIC("QS2MAODV does not work with more then one address per each interface.");
    }
}

void RoutingProtocol::NotifyRemoveAddress(uint32_t i, Ipv4InterfaceAddress address)
{
    NS_LOG_FUNCTION(this);
    Ptr<Socket> socket = FindSocketWithInterfaceAddress(address);
    if (socket)
    {
        m_routingTable.DeleteAllRoutesFromInterface(address);
        socket->Close();
        m_socketAddresses.erase(socket);
        // [FIX v13] No subnet broadcast socket to clean up (single-socket design)
        Ptr<Ipv4L3Protocol> l3 = m_ipv4->GetObject<Ipv4L3Protocol>();
        if (l3->GetNAddresses(i))
        {
            Ipv4InterfaceAddress iface = l3->GetAddress(i, 0);
            Ptr<Socket> socket = Socket::CreateSocket(GetObject<Node>(), UdpSocketFactory::GetTypeId());
            NS_ASSERT(socket);
            socket->SetRecvCallback(MakeCallback(&RoutingProtocol::RecvQs2maodv, this));
            socket->BindToNetDevice(l3->GetNetDevice(i));
            socket->Bind(InetSocketAddress(iface.GetLocal(), QS2MAODV_PORT));
            socket->SetAllowBroadcast(true);
            socket->SetIpRecvTtl(true);
            m_socketAddresses.insert(std::make_pair(socket, iface));
            // [FIX v13] No second socket needed (single-socket design)
            Ptr<NetDevice> dev = m_ipv4->GetNetDevice(m_ipv4->GetInterfaceForAddress(iface.GetLocal()));
            RoutingTableEntry rt(dev, iface.GetBroadcast(), true, 0, iface, 1, iface.GetBroadcast(),
                                 Simulator::GetMaximumSimulationTime());
            m_routingTable.AddRoute(rt);
        }
        if (m_socketAddresses.empty())
        {
            NS_LOG_LOGIC("No qs2maodv interfaces");
            m_htimer.Cancel(); m_nb.Clear(); m_routingTable.Clear();
            return;
        }
    }
    else
    {
        NS_LOG_LOGIC("Remove address not participating in QS2MAODV operation");
    }
}

bool RoutingProtocol::IsMyOwnAddress(Ipv4Address src)
{
    NS_LOG_FUNCTION(this << src);
    for (auto j = m_socketAddresses.begin(); j != m_socketAddresses.end(); ++j)
        if (src == j->second.GetLocal()) return true;
    return false;
}

Ptr<Ipv4Route> RoutingProtocol::LoopbackRoute(const Ipv4Header& hdr, Ptr<NetDevice> oif) const
{
    NS_LOG_FUNCTION(this << hdr);
    NS_ASSERT(m_lo);
    Ptr<Ipv4Route> rt = Create<Ipv4Route>();
    rt->SetDestination(hdr.GetDestination());
    auto j = m_socketAddresses.begin();
    if (oif)
    {
        for (j = m_socketAddresses.begin(); j != m_socketAddresses.end(); ++j)
        {
            Ipv4Address addr = j->second.GetLocal();
            int32_t interface = m_ipv4->GetInterfaceForAddress(addr);
            if (oif == m_ipv4->GetNetDevice(static_cast<uint32_t>(interface)))
            { rt->SetSource(addr); break; }
        }
    }
    else
    {
        rt->SetSource(j->second.GetLocal());
    }
    NS_ASSERT_MSG(rt->GetSource() != Ipv4Address(), "Valid QS2MAODV source address not found");
    rt->SetGateway(Ipv4Address("127.0.0.1"));
    rt->SetOutputDevice(m_lo);
    return rt;
}

// ============================================================
// RecvQs2maodv — socket dispatcher (identical to QMAODV, port renamed)
// ============================================================
void RoutingProtocol::RecvQs2maodv(Ptr<Socket> socket)
{
    NS_LOG_FUNCTION(this << socket);
    Address sourceAddress;
    Ptr<Packet> packet = socket->RecvFrom(sourceAddress);
    InetSocketAddress inetSourceAddr = InetSocketAddress::ConvertFrom(sourceAddress);
    Ipv4Address sender   = inetSourceAddr.GetIpv4();
    Ipv4Address receiver;

    if (m_socketAddresses.find(socket) != m_socketAddresses.end())
        receiver = m_socketAddresses[socket].GetLocal();
    else if (m_socketSubnetBroadcastAddresses.find(socket) != m_socketSubnetBroadcastAddresses.end())
        receiver = m_socketSubnetBroadcastAddresses[socket].GetLocal();
    else
    {
        NS_LOG_WARN("RecvQs2maodv: packet from unknown socket, dropping");
        return;
    }

    NS_LOG_DEBUG("QS2MAODV node " << this << " received packet from " << sender << " to " << receiver);
    UpdateRouteToNeighbor(sender, receiver);

    TypeHeader tHeader(QS2MAODVTYPE_RREQ);
    packet->RemoveHeader(tHeader);
    if (!tHeader.IsValid())
    {
        NS_LOG_DEBUG("QS2MAODV message " << packet->GetUid()
                     << " with unknown type received: " << tHeader.Get() << ". Drop");
        return;
    }
    switch (tHeader.Get())
    {
    case QS2MAODVTYPE_RREQ:  RecvRequest(packet, receiver, sender);  break;
    case QS2MAODVTYPE_RREP:  RecvReply(packet, receiver, sender);    break;
    case QS2MAODVTYPE_RERR:  RecvError(packet, sender);              break;
    case QS2MAODVTYPE_RREP_ACK: RecvReplyAck(sender);                break;
    }
}

bool RoutingProtocol::UpdateRouteLifeTime(Ipv4Address addr, Time lifetime)
{
    NS_LOG_FUNCTION(this << addr << lifetime);
    RoutingTableEntry rt;
    if (m_routingTable.LookupRoute(addr, rt))
    {
        if (rt.GetFlag() == VALID)
        {
            NS_LOG_DEBUG("Updating VALID route");
            rt.SetRreqCnt(0);
            rt.SetLifeTime(std::max(lifetime, rt.GetLifeTime()));
            m_routingTable.Update(rt);
            return true;
        }
    }
    return false;
}

void RoutingProtocol::UpdateRouteToNeighbor(Ipv4Address sender, Ipv4Address receiver)
{
    NS_LOG_FUNCTION(this << "sender " << sender << " receiver " << receiver);
    RoutingTableEntry toNeighbor;
    if (!m_routingTable.LookupRoute(sender, toNeighbor))
    {
        Ptr<NetDevice> dev = m_ipv4->GetNetDevice(m_ipv4->GetInterfaceForAddress(receiver));
        RoutingTableEntry newEntry(dev, sender, false, 0,
                                   m_ipv4->GetAddress(m_ipv4->GetInterfaceForAddress(receiver), 0),
                                   1, sender, m_activeRouteTimeout);
        m_routingTable.AddRoute(newEntry);
    }
    else
    {
        Ptr<NetDevice> dev = m_ipv4->GetNetDevice(m_ipv4->GetInterfaceForAddress(receiver));
        if (toNeighbor.GetValidSeqNo() && (toNeighbor.GetHop() == 1) &&
            (toNeighbor.GetOutputDevice() == dev))
        {
            toNeighbor.SetLifeTime(std::max(m_activeRouteTimeout, toNeighbor.GetLifeTime()));
        }
        else
        {
            RoutingTableEntry newEntry(dev, sender, false, 0,
                                       m_ipv4->GetAddress(m_ipv4->GetInterfaceForAddress(receiver), 0),
                                       1, sender,
                                       std::max(m_activeRouteTimeout, toNeighbor.GetLifeTime()));
            m_routingTable.Update(newEntry);
        }
    }
}

// ============================================================
// RecvRequest — identical to QMAODV (with Q-table alt-route save)
// ============================================================
void RoutingProtocol::RecvRequest(Ptr<Packet> p, Ipv4Address receiver, Ipv4Address src)
{
    NS_LOG_FUNCTION(this);
    RreqHeader rreqHeader;
    p->RemoveHeader(rreqHeader);

    RoutingTableEntry toPrev;
    if (m_routingTable.LookupRoute(src, toPrev))
    {
        if (toPrev.IsUnidirectional())
        {
            NS_LOG_DEBUG("Ignoring RREQ from node in blacklist");
            return;
        }
    }

    uint32_t id     = rreqHeader.GetId();
    Ipv4Address origin = rreqHeader.GetOrigin();

    if (m_rreqIdCache.IsDuplicate(origin, id))
    {
        // [QS] Save alternate reverse route from duplicate RREQ
        if (m_qtable.CountFor(origin) < m_maxPaths)
        {
            uint8_t altHop = rreqHeader.GetHopCount() + 1;
            Ptr<NetDevice> dev = m_ipv4->GetNetDevice(m_ipv4->GetInterfaceForAddress(receiver));
            RoutingTableEntry alt(dev, origin, true, rreqHeader.GetOriginSeqno(),
                                  m_ipv4->GetAddress(m_ipv4->GetInterfaceForAddress(receiver), 0),
                                  altHop, src, m_activeRouteTimeout);
            alt.SetFlag(VALID);
            double qt    = GetQueueOccupancy();
            QsStateKey state = QsQTable::BuildState(origin, qt, qt);
            m_qtable.AddRoute(alt, state);
        }
        NS_LOG_DEBUG("Ignoring RREQ due to duplicate");
        return;
    }

    uint8_t hop = rreqHeader.GetHopCount() + 1;
    rreqHeader.SetHopCount(hop);

    RoutingTableEntry toOrigin;
    if (!m_routingTable.LookupRoute(origin, toOrigin))
    {
        Ptr<NetDevice> dev = m_ipv4->GetNetDevice(m_ipv4->GetInterfaceForAddress(receiver));
        RoutingTableEntry newEntry(dev, origin, true, rreqHeader.GetOriginSeqno(),
                                   m_ipv4->GetAddress(m_ipv4->GetInterfaceForAddress(receiver), 0),
                                   hop, src,
                                   Time((2 * m_netTraversalTime - 2 * hop * m_nodeTraversalTime)));
        m_routingTable.AddRoute(newEntry);
    }
    else
    {
        if (toOrigin.GetValidSeqNo())
        {
            if (int32_t(rreqHeader.GetOriginSeqno()) - int32_t(toOrigin.GetSeqNo()) > 0)
                toOrigin.SetSeqNo(rreqHeader.GetOriginSeqno());
        }
        else { toOrigin.SetSeqNo(rreqHeader.GetOriginSeqno()); }
        toOrigin.SetValidSeqNo(true);
        toOrigin.SetNextHop(src);
        toOrigin.SetOutputDevice(m_ipv4->GetNetDevice(m_ipv4->GetInterfaceForAddress(receiver)));
        toOrigin.SetInterface(m_ipv4->GetAddress(m_ipv4->GetInterfaceForAddress(receiver), 0));
        toOrigin.SetHop(hop);
        toOrigin.SetLifeTime(std::max(Time(2 * m_netTraversalTime - 2 * hop * m_nodeTraversalTime),
                                      toOrigin.GetLifeTime()));
        m_routingTable.Update(toOrigin);
    }

    RoutingTableEntry toNeighbor;
    if (!m_routingTable.LookupRoute(src, toNeighbor))
    {
        NS_LOG_DEBUG("Neighbor:" << src << " not found in routing table. Creating an entry");
        Ptr<NetDevice> dev = m_ipv4->GetNetDevice(m_ipv4->GetInterfaceForAddress(receiver));
        RoutingTableEntry newEntry(dev, src, false, rreqHeader.GetOriginSeqno(),
                                   m_ipv4->GetAddress(m_ipv4->GetInterfaceForAddress(receiver), 0),
                                   1, src, m_activeRouteTimeout);
        m_routingTable.AddRoute(newEntry);
    }
    else
    {
        toNeighbor.SetLifeTime(m_activeRouteTimeout);
        toNeighbor.SetValidSeqNo(false);
        toNeighbor.SetSeqNo(rreqHeader.GetOriginSeqno());
        toNeighbor.SetFlag(VALID);
        toNeighbor.SetOutputDevice(m_ipv4->GetNetDevice(m_ipv4->GetInterfaceForAddress(receiver)));
        toNeighbor.SetInterface(m_ipv4->GetAddress(m_ipv4->GetInterfaceForAddress(receiver), 0));
        toNeighbor.SetHop(1);
        toNeighbor.SetNextHop(src);
        m_routingTable.Update(toNeighbor);
    }
    m_nb.Update(src, Time(m_allowedHelloLoss * m_helloInterval));

    NS_LOG_LOGIC(receiver << " receive RREQ with hop count "
                          << static_cast<uint32_t>(rreqHeader.GetHopCount())
                          << " ID " << rreqHeader.GetId()
                          << " to destination " << rreqHeader.GetDst());

    if (IsMyOwnAddress(rreqHeader.GetDst()))
    {
        m_routingTable.LookupRoute(origin, toOrigin);
        NS_LOG_DEBUG("Send reply since I am the destination");
        SendReply(rreqHeader, toOrigin);
        return;
    }

    RoutingTableEntry toDst;
    Ipv4Address dst = rreqHeader.GetDst();
    if (m_routingTable.LookupRoute(dst, toDst))
    {
        if (toDst.GetNextHop() == src)
        {
            NS_LOG_DEBUG("Drop RREQ from " << src << ", dest next hop " << toDst.GetNextHop());
            return;
        }
        if ((rreqHeader.GetUnknownSeqno() ||
             (int32_t(toDst.GetSeqNo()) - int32_t(rreqHeader.GetDstSeqno()) >= 0)) &&
            toDst.GetValidSeqNo())
        {
            if (!rreqHeader.GetDestinationOnly() && toDst.GetFlag() == VALID)
            {
                m_routingTable.LookupRoute(origin, toOrigin);
                SendReplyByIntermediateNode(toDst, toOrigin, rreqHeader.GetGratuitousRrep());
                return;
            }
            rreqHeader.SetDstSeqno(toDst.GetSeqNo());
            rreqHeader.SetUnknownSeqno(false);
        }
    }

    SocketIpTtlTag tag;
    p->RemovePacketTag(tag);
    if (tag.GetTtl() < 2)
    {
        NS_LOG_DEBUG("TTL exceeded. Drop RREQ origin " << src << " destination " << dst);
        return;
    }

    for (auto j = m_socketAddresses.begin(); j != m_socketAddresses.end(); ++j)
    {
        Ptr<Socket> socket = j->first;
        Ipv4InterfaceAddress iface = j->second;
        Ptr<Packet> packet = Create<Packet>();
        SocketIpTtlTag ttl;
        ttl.SetTtl(tag.GetTtl() - 1);
        packet->AddPacketTag(ttl);
        packet->AddHeader(rreqHeader);
        TypeHeader tHeader(QS2MAODVTYPE_RREQ);
        packet->AddHeader(tHeader);
        Ipv4Address destination;
        destination = Ipv4Address("255.255.255.255");
        m_lastBcastTime = Simulator::Now();
        Simulator::Schedule(Time(MilliSeconds(m_uniformRandomVariable->GetInteger(0, 10))),
                            &RoutingProtocol::SendTo, this, socket, packet, destination);
    }
}

// ============================================================
// SendReply — identical to QMAODV
// ============================================================
void RoutingProtocol::SendReply(const RreqHeader& rreqHeader, const RoutingTableEntry& toOrigin)
{
    NS_LOG_FUNCTION(this << toOrigin.GetDestination());
    if (!rreqHeader.GetUnknownSeqno() && (rreqHeader.GetDstSeqno() == m_seqNo + 1))
        m_seqNo++;
    RrepHeader rrepHeader(0, 0, rreqHeader.GetDst(), m_seqNo,
                          toOrigin.GetDestination(), m_myRouteTimeout);
    Ptr<Packet> packet = Create<Packet>();
    SocketIpTtlTag tag;
    tag.SetTtl(toOrigin.GetHop());
    packet->AddPacketTag(tag);
    packet->AddHeader(rrepHeader);
    TypeHeader tHeader(QS2MAODVTYPE_RREP);
    packet->AddHeader(tHeader);
    Ptr<Socket> socket = FindSocketWithInterfaceAddress(toOrigin.GetInterface());
    NS_ASSERT(socket);
    socket->SendTo(packet, 0, InetSocketAddress(toOrigin.GetNextHop(), QS2MAODV_PORT));
}

void RoutingProtocol::SendReplyByIntermediateNode(RoutingTableEntry& toDst,
                                                   RoutingTableEntry& toOrigin,
                                                   bool gratRep)
{
    NS_LOG_FUNCTION(this);
    RrepHeader rrepHeader(0, toDst.GetHop(), toDst.GetDestination(), toDst.GetSeqNo(),
                          toOrigin.GetDestination(), toDst.GetLifeTime());
    if (toDst.GetHop() == 1)
    {
        rrepHeader.SetAckRequired(true);
        RoutingTableEntry toNextHop;
        m_routingTable.LookupRoute(toOrigin.GetNextHop(), toNextHop);
        toNextHop.m_ackTimer.SetFunction(&RoutingProtocol::AckTimerExpire, this);
        toNextHop.m_ackTimer.SetArguments(toNextHop.GetDestination(), m_blackListTimeout);
        toNextHop.m_ackTimer.SetDelay(m_nextHopWait);
    }
    toDst.InsertPrecursor(toOrigin.GetNextHop());
    toOrigin.InsertPrecursor(toDst.GetNextHop());
    m_routingTable.Update(toDst);
    m_routingTable.Update(toOrigin);
    Ptr<Packet> packet = Create<Packet>();
    SocketIpTtlTag tag;
    tag.SetTtl(toOrigin.GetHop());
    packet->AddPacketTag(tag);
    packet->AddHeader(rrepHeader);
    TypeHeader tHeader(QS2MAODVTYPE_RREP);
    packet->AddHeader(tHeader);
    Ptr<Socket> socket = FindSocketWithInterfaceAddress(toOrigin.GetInterface());
    NS_ASSERT(socket);
    socket->SendTo(packet, 0, InetSocketAddress(toOrigin.GetNextHop(), QS2MAODV_PORT));
    if (gratRep)
    {
        RrepHeader gratRepHeader(0, toOrigin.GetHop(), toOrigin.GetDestination(),
                                 toOrigin.GetSeqNo(), toDst.GetDestination(), toOrigin.GetLifeTime());
        Ptr<Packet> packetToDst = Create<Packet>();
        SocketIpTtlTag gratTag;
        gratTag.SetTtl(toDst.GetHop());
        packetToDst->AddPacketTag(gratTag);
        packetToDst->AddHeader(gratRepHeader);
        TypeHeader type(QS2MAODVTYPE_RREP);
        packetToDst->AddHeader(type);
        Ptr<Socket> socket = FindSocketWithInterfaceAddress(toDst.GetInterface());
        NS_ASSERT(socket);
        NS_LOG_LOGIC("Send gratuitous RREP " << packet->GetUid());
        socket->SendTo(packetToDst, 0, InetSocketAddress(toDst.GetNextHop(), QS2MAODV_PORT));
    }
}

void RoutingProtocol::SendReplyAck(Ipv4Address neighbor)
{
    NS_LOG_FUNCTION(this << " to " << neighbor);
    RrepAckHeader h;
    TypeHeader typeHeader(QS2MAODVTYPE_RREP_ACK);
    Ptr<Packet> packet = Create<Packet>();
    SocketIpTtlTag tag;
    tag.SetTtl(1);
    packet->AddPacketTag(tag);
    packet->AddHeader(h);
    packet->AddHeader(typeHeader);
    RoutingTableEntry toNeighbor;
    m_routingTable.LookupRoute(neighbor, toNeighbor);
    Ptr<Socket> socket = FindSocketWithInterfaceAddress(toNeighbor.GetInterface());
    NS_ASSERT(socket);
    socket->SendTo(packet, 0, InetSocketAddress(neighbor, QS2MAODV_PORT));
}

// ============================================================
// RecvReply — [QS] store route in Q-table with 3D state + Q-update
// ============================================================
void RoutingProtocol::RecvReply(Ptr<Packet> p, Ipv4Address receiver, Ipv4Address sender)
{
    NS_LOG_FUNCTION(this << " src " << sender);
    RrepHeader rrepHeader;
    p->RemoveHeader(rrepHeader);
    Ipv4Address dst = rrepHeader.GetDst();
    NS_LOG_LOGIC("RREP destination " << dst << " RREP origin " << rrepHeader.GetOrigin());

    uint8_t hop = rrepHeader.GetHopCount() + 1;
    rrepHeader.SetHopCount(hop);

    if (dst == rrepHeader.GetOrigin())
    {
        ProcessHello(rrepHeader, receiver);
        return;
    }

    Ptr<NetDevice> dev = m_ipv4->GetNetDevice(m_ipv4->GetInterfaceForAddress(receiver));
    RoutingTableEntry newEntry(dev, dst, true, rrepHeader.GetDstSeqno(),
                               m_ipv4->GetAddress(m_ipv4->GetInterfaceForAddress(receiver), 0),
                               hop, sender, rrepHeader.GetLifeTime());
    RoutingTableEntry toDst;
    if (m_routingTable.LookupRoute(dst, toDst))
    {
        if (!toDst.GetValidSeqNo())
            m_routingTable.Update(newEntry);
        else if ((int32_t(rrepHeader.GetDstSeqno()) - int32_t(toDst.GetSeqNo())) > 0)
            m_routingTable.Update(newEntry);
        else
        {
            if ((rrepHeader.GetDstSeqno() == toDst.GetSeqNo()) && (toDst.GetFlag() != VALID))
                m_routingTable.Update(newEntry);
            else if ((rrepHeader.GetDstSeqno() == toDst.GetSeqNo()) && (hop < toDst.GetHop()))
                m_routingTable.Update(newEntry);
        }
    }
    else
    {
        NS_LOG_LOGIC("add new route");
        m_routingTable.AddRoute(newEntry);
    }

    // [QS-1] Store route in Q-table with 3D state, positive Q-update
    {
        double qt    = GetQueueOccupancy();
        QsStateKey state = QsQTable::BuildState(dst, qt, qt);
        m_qtable.UpdateQValueOrCreate(newEntry, state, 1.0, 0.005, qt);
    }

    if (rrepHeader.GetAckRequired())
    {
        SendReplyAck(sender);
        rrepHeader.SetAckRequired(false);
    }
    NS_LOG_LOGIC("receiver " << receiver << " origin " << rrepHeader.GetOrigin());
    if (IsMyOwnAddress(rrepHeader.GetOrigin()))
    {
        if (toDst.GetFlag() == IN_SEARCH)
        {
            m_routingTable.Update(newEntry);
            m_addressReqTimer[dst].Cancel();
            m_addressReqTimer.erase(dst);
        }
        m_routingTable.LookupRoute(dst, toDst);
        SendPacketFromQueue(dst, toDst.GetRoute());
        return;
    }

    RoutingTableEntry toOrigin;
    if (!m_routingTable.LookupRoute(rrepHeader.GetOrigin(), toOrigin) ||
        toOrigin.GetFlag() == IN_SEARCH)
        return;
    toOrigin.SetLifeTime(std::max(m_activeRouteTimeout, toOrigin.GetLifeTime()));
    m_routingTable.Update(toOrigin);

    if (m_routingTable.LookupValidRoute(rrepHeader.GetDst(), toDst))
    {
        toDst.InsertPrecursor(toOrigin.GetNextHop());
        m_routingTable.Update(toDst);
        RoutingTableEntry toNextHopToDst;
        m_routingTable.LookupRoute(toDst.GetNextHop(), toNextHopToDst);
        toNextHopToDst.InsertPrecursor(toOrigin.GetNextHop());
        m_routingTable.Update(toNextHopToDst);
        toOrigin.InsertPrecursor(toDst.GetNextHop());
        m_routingTable.Update(toOrigin);
        RoutingTableEntry toNextHopToOrigin;
        m_routingTable.LookupRoute(toOrigin.GetNextHop(), toNextHopToOrigin);
        toNextHopToOrigin.InsertPrecursor(toDst.GetNextHop());
        m_routingTable.Update(toNextHopToOrigin);
    }

    SocketIpTtlTag tag;
    p->RemovePacketTag(tag);
    if (tag.GetTtl() < 2)
    {
        NS_LOG_DEBUG("TTL exceeded. Drop RREP destination " << dst << " origin " << rrepHeader.GetOrigin());
        return;
    }
    Ptr<Packet> packet = Create<Packet>();
    SocketIpTtlTag ttl;
    ttl.SetTtl(tag.GetTtl() - 1);
    packet->AddPacketTag(ttl);
    packet->AddHeader(rrepHeader);
    TypeHeader tHeader(QS2MAODVTYPE_RREP);
    packet->AddHeader(tHeader);
    Ptr<Socket> socket = FindSocketWithInterfaceAddress(toOrigin.GetInterface());
    NS_ASSERT(socket);
    socket->SendTo(packet, 0, InetSocketAddress(toOrigin.GetNextHop(), QS2MAODV_PORT));
}

void RoutingProtocol::RecvReplyAck(Ipv4Address neighbor)
{
    NS_LOG_FUNCTION(this);
    RoutingTableEntry rt;
    if (m_routingTable.LookupRoute(neighbor, rt))
    {
        rt.m_ackTimer.Cancel();
        rt.SetFlag(VALID);
        m_routingTable.Update(rt);
    }
}

void RoutingProtocol::ProcessHello(const RrepHeader& rrepHeader, Ipv4Address receiver)
{
    NS_LOG_FUNCTION(this << "from " << rrepHeader.GetDst());
    RoutingTableEntry toNeighbor;
    if (!m_routingTable.LookupRoute(rrepHeader.GetDst(), toNeighbor))
    {
        Ptr<NetDevice> dev = m_ipv4->GetNetDevice(m_ipv4->GetInterfaceForAddress(receiver));
        RoutingTableEntry newEntry(dev, rrepHeader.GetDst(), true, rrepHeader.GetDstSeqno(),
                                   m_ipv4->GetAddress(m_ipv4->GetInterfaceForAddress(receiver), 0),
                                   1, rrepHeader.GetDst(), rrepHeader.GetLifeTime());
        m_routingTable.AddRoute(newEntry);
    }
    else
    {
        toNeighbor.SetLifeTime(std::max(Time(m_allowedHelloLoss * m_helloInterval), toNeighbor.GetLifeTime()));
        toNeighbor.SetSeqNo(rrepHeader.GetDstSeqno());
        toNeighbor.SetValidSeqNo(true);
        toNeighbor.SetFlag(VALID);
        toNeighbor.SetOutputDevice(m_ipv4->GetNetDevice(m_ipv4->GetInterfaceForAddress(receiver)));
        toNeighbor.SetInterface(m_ipv4->GetAddress(m_ipv4->GetInterfaceForAddress(receiver), 0));
        toNeighbor.SetHop(1);
        toNeighbor.SetNextHop(rrepHeader.GetDst());
        m_routingTable.Update(toNeighbor);
    }
    if (m_enableHello)
        m_nb.Update(rrepHeader.GetDst(), Time(m_allowedHelloLoss * m_helloInterval));
}

// ============================================================
// RecvError — [QS] negative Q-update on unreachable routes
// ============================================================
void RoutingProtocol::RecvError(Ptr<Packet> p, Ipv4Address src)
{
    NS_LOG_FUNCTION(this << " from " << src);
    RerrHeader rerrHeader;
    p->RemoveHeader(rerrHeader);
    std::map<Ipv4Address, uint32_t> dstWithNextHopSrc;
    std::map<Ipv4Address, uint32_t> unreachable;
    m_routingTable.GetListOfDestinationWithNextHop(src, dstWithNextHopSrc);
    std::pair<Ipv4Address, uint32_t> un;
    while (rerrHeader.RemoveUnDestination(un))
    {
        for (auto i = dstWithNextHopSrc.begin(); i != dstWithNextHopSrc.end(); ++i)
        {
            if (i->first == un.first)
            {
                unreachable.insert(un);
                // [QS] Negative Q-update for unreachable destination
                RoutingTableEntry fwd;
                if (m_routingTable.LookupRoute(un.first, fwd))
                {
                    double qt    = GetQueueOccupancy();
                    QsStateKey state = QsQTable::BuildState(un.first, qt, qt);
                    m_qtable.UpdateQValueOrCreate(fwd, state, 0.0, 1.0, qt);
                }
                m_qtable.DeleteRoute(un.first, src);
            }
        }
    }

    std::vector<Ipv4Address> precursors;
    for (auto i = unreachable.begin(); i != unreachable.end();)
    {
        if (!rerrHeader.AddUnDestination(i->first, i->second))
        {
            TypeHeader typeHeader(QS2MAODVTYPE_RERR);
            Ptr<Packet> packet = Create<Packet>();
            SocketIpTtlTag tag;
            tag.SetTtl(1);
            packet->AddPacketTag(tag);
            packet->AddHeader(rerrHeader);
            packet->AddHeader(typeHeader);
            SendRerrMessage(packet, precursors);
            rerrHeader.Clear();
        }
        else
        {
            RoutingTableEntry toDst;
            m_routingTable.LookupRoute(i->first, toDst);
            toDst.GetPrecursors(precursors);
            ++i;
        }
    }
    if (rerrHeader.GetDestCount() != 0)
    {
        TypeHeader typeHeader(QS2MAODVTYPE_RERR);
        Ptr<Packet> packet = Create<Packet>();
        SocketIpTtlTag tag;
        tag.SetTtl(1);
        packet->AddPacketTag(tag);
        packet->AddHeader(rerrHeader);
        packet->AddHeader(typeHeader);
        SendRerrMessage(packet, precursors);
    }
    m_routingTable.InvalidateRoutesWithDst(unreachable);
}

// ============================================================
// Route timers, send helpers — identical to QMAODV, port renamed
// ============================================================
void RoutingProtocol::RouteRequestTimerExpire(Ipv4Address dst)
{
    NS_LOG_LOGIC(this);
    RoutingTableEntry toDst;
    if (m_routingTable.LookupValidRoute(dst, toDst))
    {
        SendPacketFromQueue(dst, toDst.GetRoute());
        NS_LOG_LOGIC("route to " << dst << " found");
        return;
    }
    if (toDst.GetRreqCnt() == m_rreqRetries)
    {
        NS_LOG_LOGIC("route discovery to " << dst << " has been attempted RreqRetries ("
                     << m_rreqRetries << ") times");
        m_addressReqTimer.erase(dst);
        m_routingTable.DeleteRoute(dst);
        NS_LOG_DEBUG("Route not found. Drop all packets with dst " << dst);
        m_queue.DropPacketWithDst(dst);
        return;
    }
    if (toDst.GetFlag() == IN_SEARCH)
    {
        NS_LOG_LOGIC("Resend RREQ to " << dst << " previous ttl " << toDst.GetHop());
        SendRequest(dst);
    }
    else
    {
        NS_LOG_DEBUG("Route down. Stop search. Drop packet with destination " << dst);
        m_addressReqTimer.erase(dst);
        m_routingTable.DeleteRoute(dst);
        m_queue.DropPacketWithDst(dst);
    }
}

void RoutingProtocol::HelloTimerExpire()
{
    NS_LOG_FUNCTION(this);
    Time offset = Time(Seconds(0));
    if (m_lastBcastTime > Time(Seconds(0)))
    {
        offset = Simulator::Now() - m_lastBcastTime;
        NS_LOG_DEBUG("Hello deferred due to last bcast at:" << m_lastBcastTime);
    }
    else { SendHello(); }
    m_htimer.Cancel();
    Time diff = m_helloInterval - offset;
    m_htimer.Schedule(std::max(Time(Seconds(0)), diff));
    m_lastBcastTime = Time(Seconds(0));
}

void RoutingProtocol::RreqRateLimitTimerExpire()
{ NS_LOG_FUNCTION(this); m_rreqCount = 0; m_rreqRateLimitTimer.Schedule(Seconds(1)); }

void RoutingProtocol::RerrRateLimitTimerExpire()
{ NS_LOG_FUNCTION(this); m_rerrCount = 0; m_rerrRateLimitTimer.Schedule(Seconds(1)); }

void RoutingProtocol::AckTimerExpire(Ipv4Address neighbor, Time blacklistTimeout)
{ NS_LOG_FUNCTION(this); m_routingTable.MarkLinkAsUnidirectional(neighbor, blacklistTimeout); }

void RoutingProtocol::SendHello()
{
    NS_LOG_FUNCTION(this);
    for (auto j = m_socketAddresses.begin(); j != m_socketAddresses.end(); ++j)
    {
        Ptr<Socket> socket = j->first;
        Ipv4InterfaceAddress iface = j->second;
        RrepHeader helloHeader(0, 0, iface.GetLocal(), m_seqNo, iface.GetLocal(),
                               Time(m_allowedHelloLoss * m_helloInterval));
        Ptr<Packet> packet = Create<Packet>();
        SocketIpTtlTag tag;
        tag.SetTtl(1);
        packet->AddPacketTag(tag);
        packet->AddHeader(helloHeader);
        TypeHeader tHeader(QS2MAODVTYPE_RREP);
        packet->AddHeader(tHeader);
        Ipv4Address destination;
        destination = Ipv4Address("255.255.255.255");
        Time jitter = Time(MilliSeconds(m_uniformRandomVariable->GetInteger(0, 10)));
        Simulator::Schedule(jitter, &RoutingProtocol::SendTo, this, socket, packet, destination);
    }
}

void RoutingProtocol::SendPacketFromQueue(Ipv4Address dst, Ptr<Ipv4Route> route)
{
    NS_LOG_FUNCTION(this);
    QueueEntry queueEntry;
    while (m_queue.Dequeue(dst, queueEntry))
    {
        DeferredRouteOutputTag tag;
        Ptr<Packet> p = ConstCast<Packet>(queueEntry.GetPacket());
        if (p->RemovePacketTag(tag) && tag.GetInterface() != -1 &&
            tag.GetInterface() != m_ipv4->GetInterfaceForDevice(route->GetOutputDevice()))
        {
            NS_LOG_DEBUG("Output device doesn't match. Dropped.");
            return;
        }
        UnicastForwardCallback ucb = queueEntry.GetUnicastForwardCallback();
        Ipv4Header header = queueEntry.GetIpv4Header();
        header.SetSource(route->GetSource());
        header.SetTtl(header.GetTtl() + 1);
        ucb(route, p, header);
    }
}

void RoutingProtocol::SendRequest(Ipv4Address dst)
{
    NS_LOG_FUNCTION(this << dst);
    if (m_rreqCount == m_rreqRateLimit)
    {
        Simulator::Schedule(m_rreqRateLimitTimer.GetDelayLeft() + MicroSeconds(100),
                            &RoutingProtocol::SendRequest, this, dst);
        return;
    }
    else { m_rreqCount++; }

    RreqHeader rreqHeader;
    rreqHeader.SetDst(dst);

    RoutingTableEntry rt;
    uint16_t ttl = m_ttlStart;
    if (m_routingTable.LookupRoute(dst, rt))
    {
        if (rt.GetFlag() != IN_SEARCH)
            ttl = std::min<uint16_t>(rt.GetHop() + m_ttlIncrement, m_netDiameter);
        else
        {
            ttl = rt.GetHop() + m_ttlIncrement;
            if (ttl > m_ttlThreshold) ttl = m_netDiameter;
        }
        if (ttl == m_netDiameter) rt.IncrementRreqCnt();
        if (rt.GetValidSeqNo()) rreqHeader.SetDstSeqno(rt.GetSeqNo());
        else rreqHeader.SetUnknownSeqno(true);
        rt.SetHop(ttl);
        rt.SetFlag(IN_SEARCH);
        rt.SetLifeTime(m_pathDiscoveryTime);
        m_routingTable.Update(rt);
    }
    else
    {
        rreqHeader.SetUnknownSeqno(true);
        Ptr<NetDevice> dev = nullptr;
        RoutingTableEntry newEntry(dev, dst, false, 0, Ipv4InterfaceAddress(), ttl,
                                   Ipv4Address(), m_pathDiscoveryTime);
        if (ttl == m_netDiameter) newEntry.IncrementRreqCnt();
        newEntry.SetFlag(IN_SEARCH);
        m_routingTable.AddRoute(newEntry);
    }

    if (m_gratuitousReply) rreqHeader.SetGratuitousRrep(true);
    if (m_destinationOnly) rreqHeader.SetDestinationOnly(true);
    m_seqNo++;
    rreqHeader.SetOriginSeqno(m_seqNo);
    m_requestId++;
    rreqHeader.SetId(m_requestId);

    for (auto j = m_socketAddresses.begin(); j != m_socketAddresses.end(); ++j)
    {
        Ptr<Socket> socket = j->first;
        Ipv4InterfaceAddress iface = j->second;
        rreqHeader.SetOrigin(iface.GetLocal());
        m_rreqIdCache.IsDuplicate(iface.GetLocal(), m_requestId);
        Ptr<Packet> packet = Create<Packet>();
        SocketIpTtlTag tag;
        tag.SetTtl(ttl);
        packet->AddPacketTag(tag);
        packet->AddHeader(rreqHeader);
        TypeHeader tHeader(QS2MAODVTYPE_RREQ);
        packet->AddHeader(tHeader);
        Ipv4Address destination;
        destination = Ipv4Address("255.255.255.255");
        NS_LOG_DEBUG("Send RREQ with id " << rreqHeader.GetId() << " to socket");
        m_lastBcastTime = Simulator::Now();
        Simulator::Schedule(Time(MilliSeconds(m_uniformRandomVariable->GetInteger(0, 10))),
                            &RoutingProtocol::SendTo, this, socket, packet, destination);
    }
    ScheduleRreqRetry(dst);
}

void RoutingProtocol::SendTo(Ptr<Socket> socket, Ptr<Packet> packet, Ipv4Address destination)
{ socket->SendTo(packet, 0, InetSocketAddress(destination, QS2MAODV_PORT)); }

void RoutingProtocol::ScheduleRreqRetry(Ipv4Address dst)
{
    NS_LOG_FUNCTION(this << dst);
    if (m_addressReqTimer.find(dst) == m_addressReqTimer.end())
    {
        Timer timer(Timer::CANCEL_ON_DESTROY);
        m_addressReqTimer[dst] = timer;
    }
    m_addressReqTimer[dst].SetFunction(&RoutingProtocol::RouteRequestTimerExpire, this);
    m_addressReqTimer[dst].Cancel();
    m_addressReqTimer[dst].SetArguments(dst);
    RoutingTableEntry rt;
    m_routingTable.LookupRoute(dst, rt);
    Time retry;
    if (rt.GetHop() < m_netDiameter)
        retry = 2 * m_nodeTraversalTime * (rt.GetHop() + m_timeoutBuffer);
    else
    {
        NS_ABORT_MSG_UNLESS(rt.GetRreqCnt() > 0, "Unexpected value for GetRreqCount ()");
        uint16_t backoffFactor = rt.GetRreqCnt() - 1;
        NS_LOG_LOGIC("Applying binary exponential backoff factor " << backoffFactor);
        retry = m_netTraversalTime * (1 << backoffFactor);
    }
    m_addressReqTimer[dst].Schedule(retry);
    NS_LOG_LOGIC("Scheduled RREQ retry in " << retry.As(Time::S));
}

void RoutingProtocol::SendRerrWhenBreaksLinkToNextHop(Ipv4Address nextHop)
{
    NS_LOG_FUNCTION(this << nextHop);
    RerrHeader rerrHeader;
    std::vector<Ipv4Address> precursors;
    std::map<Ipv4Address, uint32_t> unreachable;

    RoutingTableEntry toNextHop;
    if (!m_routingTable.LookupRoute(nextHop, toNextHop)) return;
    toNextHop.GetPrecursors(precursors);
    rerrHeader.AddUnDestination(nextHop, toNextHop.GetSeqNo());
    m_routingTable.GetListOfDestinationWithNextHop(nextHop, unreachable);
    for (auto i = unreachable.begin(); i != unreachable.end();)
    {
        if (!rerrHeader.AddUnDestination(i->first, i->second))
        {
            NS_LOG_LOGIC("Send RERR message with maximum size.");
            TypeHeader typeHeader(QS2MAODVTYPE_RERR);
            Ptr<Packet> packet = Create<Packet>();
            SocketIpTtlTag tag;
            tag.SetTtl(1);
            packet->AddPacketTag(tag);
            packet->AddHeader(rerrHeader);
            packet->AddHeader(typeHeader);
            SendRerrMessage(packet, precursors);
            rerrHeader.Clear();
        }
        else
        {
            RoutingTableEntry toDst;
            m_routingTable.LookupRoute(i->first, toDst);
            toDst.GetPrecursors(precursors);
            ++i;
        }
    }
    if (rerrHeader.GetDestCount() != 0)
    {
        TypeHeader typeHeader(QS2MAODVTYPE_RERR);
        Ptr<Packet> packet = Create<Packet>();
        SocketIpTtlTag tag;
        tag.SetTtl(1);
        packet->AddPacketTag(tag);
        packet->AddHeader(rerrHeader);
        packet->AddHeader(typeHeader);
        SendRerrMessage(packet, precursors);
    }
    // [QS] Remove broken next-hop from Q-table globally
    m_qtable.RemoveNextHopGlobally(nextHop);
    unreachable.insert(std::make_pair(nextHop, toNextHop.GetSeqNo()));
    m_routingTable.InvalidateRoutesWithDst(unreachable);
}

void RoutingProtocol::SendRerrWhenNoRouteToForward(Ipv4Address dst, uint32_t dstSeqNo, Ipv4Address origin)
{
    NS_LOG_FUNCTION(this);
    if (m_rerrCount == m_rerrRateLimit)
    {
        NS_ASSERT(m_rerrRateLimitTimer.IsRunning());
        NS_LOG_LOGIC("RerrRateLimit reached; suppressing RERR");
        return;
    }
    RerrHeader rerrHeader;
    rerrHeader.AddUnDestination(dst, dstSeqNo);
    RoutingTableEntry toOrigin;
    Ptr<Packet> packet = Create<Packet>();
    SocketIpTtlTag tag;
    tag.SetTtl(1);
    packet->AddPacketTag(tag);
    packet->AddHeader(rerrHeader);
    packet->AddHeader(TypeHeader(QS2MAODVTYPE_RERR));
    if (m_routingTable.LookupValidRoute(origin, toOrigin))
    {
        Ptr<Socket> socket = FindSocketWithInterfaceAddress(toOrigin.GetInterface());
        NS_ASSERT(socket);
        NS_LOG_LOGIC("Unicast RERR to the source of the data transmission");
        socket->SendTo(packet, 0, InetSocketAddress(toOrigin.GetNextHop(), QS2MAODV_PORT));
    }
    else
    {
        for (auto i = m_socketAddresses.begin(); i != m_socketAddresses.end(); ++i)
        {
            Ptr<Socket> socket = i->first;
            Ipv4InterfaceAddress iface = i->second;
            NS_ASSERT(socket);
            Ipv4Address destination;
            if (iface.GetMask() == Ipv4Mask::GetOnes())
                destination = Ipv4Address("255.255.255.255");
            else
                destination = iface.GetBroadcast();
            socket->SendTo(packet->Copy(), 0, InetSocketAddress(destination, QS2MAODV_PORT));
        }
    }
}

void RoutingProtocol::SendRerrMessage(Ptr<Packet> packet, std::vector<Ipv4Address> precursors)
{
    NS_LOG_FUNCTION(this);
    if (precursors.empty()) { NS_LOG_LOGIC("No precursors"); return; }
    if (m_rerrCount == m_rerrRateLimit)
    {
        NS_ASSERT(m_rerrRateLimitTimer.IsRunning());
        NS_LOG_LOGIC("RerrRateLimit reached; suppressing RERR");
        return;
    }
    if (precursors.size() == 1)
    {
        RoutingTableEntry toPrecursor;
        if (m_routingTable.LookupValidRoute(precursors.front(), toPrecursor))
        {
            Ptr<Socket> socket = FindSocketWithInterfaceAddress(toPrecursor.GetInterface());
            NS_ASSERT(socket);
            NS_LOG_LOGIC("one precursor => unicast RERR to " << toPrecursor.GetDestination());
            Simulator::Schedule(Time(MilliSeconds(m_uniformRandomVariable->GetInteger(0, 10))),
                                &RoutingProtocol::SendTo, this, socket, packet, precursors.front());
            m_rerrCount++;
        }
        return;
    }
    std::vector<Ipv4InterfaceAddress> ifaces;
    RoutingTableEntry toPrecursor;
    for (auto i = precursors.begin(); i != precursors.end(); ++i)
    {
        if (m_routingTable.LookupValidRoute(*i, toPrecursor) &&
            std::find(ifaces.begin(), ifaces.end(), toPrecursor.GetInterface()) == ifaces.end())
            ifaces.push_back(toPrecursor.GetInterface());
    }
    for (auto i = ifaces.begin(); i != ifaces.end(); ++i)
    {
        Ptr<Socket> socket = FindSocketWithInterfaceAddress(*i);
        NS_ASSERT(socket);
        NS_LOG_LOGIC("Broadcast RERR message from interface " << i->GetLocal());
        Ptr<Packet> p = packet->Copy();
        Ipv4Address destination;
        if (i->GetMask() == Ipv4Mask::GetOnes())
            destination = Ipv4Address("255.255.255.255");
        else
            destination = i->GetBroadcast();
        Simulator::Schedule(Time(MilliSeconds(m_uniformRandomVariable->GetInteger(0, 10))),
                            &RoutingProtocol::SendTo, this, socket, p, destination);
    }
}

Ptr<Socket> RoutingProtocol::FindSocketWithInterfaceAddress(Ipv4InterfaceAddress addr) const
{
    NS_LOG_FUNCTION(this << addr);
    for (auto j = m_socketAddresses.begin(); j != m_socketAddresses.end(); ++j)
        if (j->second == addr) return j->first;
    Ptr<Socket> socket;
    return socket;
}

Ptr<Socket> RoutingProtocol::FindSubnetBroadcastSocketWithInterfaceAddress(Ipv4InterfaceAddress addr) const
{
    NS_LOG_FUNCTION(this << addr);
    for (auto j = m_socketSubnetBroadcastAddresses.begin();
         j != m_socketSubnetBroadcastAddresses.end(); ++j)
        if (j->second == addr) return j->first;
    Ptr<Socket> socket;
    return socket;
}

void RoutingProtocol::DoInitialize()
{
    NS_LOG_FUNCTION(this);
    uint32_t startTime;
    if (m_enableHello)
    {
        m_htimer.SetFunction(&RoutingProtocol::HelloTimerExpire, this);
        startTime = m_uniformRandomVariable->GetInteger(0, 100);
        NS_LOG_DEBUG("Starting at time " << startTime << "ms");
        m_htimer.Schedule(MilliSeconds(startTime));
    }
    // [QS-4] Start ACK-silence decay timer
    m_decayTimer.SetFunction(&RoutingProtocol::DecayTimerExpire, this);
    m_decayTimer.Schedule(Seconds(10.0));

    Ipv4RoutingProtocol::DoInitialize();
}

// ============================================================
// [QS] SetMaxPaths / Q-learning parameter setters
// ============================================================
void RoutingProtocol::SetMaxPaths(uint32_t mp)
{ m_maxPaths = mp; m_qtable.SetMaxPaths(mp); }

uint32_t RoutingProtocol::GetMaxPaths() const { return m_maxPaths; }

void RoutingProtocol::SetQLearningParameters(double alpha, double gamma, double epsilon)
{ m_alpha = alpha; m_gamma = gamma; m_epsilon = epsilon; m_qtable.SetLearningParameters(alpha, gamma, epsilon); }

void RoutingProtocol::SetRewardWeights(double w1, double w2, double w3)
{ m_w1 = w1; m_w2 = w2; m_w3 = w3; m_qtable.SetRewardWeights(w1, w2, w3); }

} // namespace qs2maodv
} // namespace ns3

// =============================================================
// [QS-4] ACK-silence decay timer callback
// =============================================================
namespace ns3 { namespace qs2maodv {
void RoutingProtocol::DecayTimerExpire()
{
    // Decay Q-values for routes whose next-hop has been silent > 15s
    if (m_enableDecay)
        m_qtable.DecayStaleRoutes(Simulator::Now(), m_silenceThreshold, m_decayFactor);
    PurgePending();   // [FIX v2]
    // Reschedule every 10 seconds
    m_decayTimer.Schedule(Seconds(10.0));
}
} } // namespace qs2maodv, ns3
