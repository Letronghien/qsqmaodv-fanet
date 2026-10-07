/*
 * Copyright (c) 2009 IITP RAS
 *
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Based on
 *      NS-2 AODV model developed by the CMU/MONARCH group and optimized and
 *      tuned by Samir Das and Mahesh Marina, University of Cincinnati;
 *
 *      AODV-UU implementation by Erik Nordström of Uppsala University
 *      https://web.archive.org/web/20100527072022/http://core.it.uu.se/core/index.php/AODV-UU
 *
 * Authors: Elena Buchatskaia <borovkovaes@iitp.ru>
 *          Pavel Boyko <boyko@iitp.ru>
 *
 * AOMDV: this file started as a copy of the ns-3.48 aodv module; the multipath logic is a
 * port of the ns-2.35 AOMDV reference code (M. K. Marina, S. R. Das). See
 * docs/specs/AOMDV_PORT.md for the rule-by-rule mapping and the deviations.
 */

#include "aomdv-routing-protocol.h"

#include "ns3/adhoc-wifi-mac.h"
#include "ns3/boolean.h"
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

#include <algorithm>
#include <limits>

#undef NS_LOG_APPEND_CONTEXT
#define NS_LOG_APPEND_CONTEXT                                                                      \
    if (m_ipv4)                                                                                    \
    {                                                                                              \
        std::clog << "[node " << m_ipv4->GetObject<Node>()->GetId() << "] ";                       \
    }

namespace ns3
{

NS_LOG_COMPONENT_DEFINE("AomdvRoutingProtocol");

namespace aomdv
{
NS_OBJECT_ENSURE_REGISTERED(RoutingProtocol);

/// UDP Port for AOMDV control traffic
const uint32_t RoutingProtocol::AOMDV_PORT = 654;

/**
 * @ingroup aomdv
 * @brief Tag used by AOMDV implementation
 */
class DeferredRouteOutputTag : public Tag
{
  public:
    /**
     * @brief Constructor
     * @param o the output interface
     */
    DeferredRouteOutputTag(int32_t o = -1)
        : Tag(),
          m_oif(o)
    {
    }

    /**
     * @brief Get the type ID.
     * @return the object TypeId
     */
    static TypeId GetTypeId()
    {
        static TypeId tid = TypeId("ns3::aomdv::DeferredRouteOutputTag")
                                .SetParent<Tag>()
                                .SetGroupName("Aomdv")
                                .AddConstructor<DeferredRouteOutputTag>();
        return tid;
    }

    TypeId GetInstanceTypeId() const override
    {
        return GetTypeId();
    }

    /**
     * @brief Get the output interface
     * @return the output interface
     */
    int32_t GetInterface() const
    {
        return m_oif;
    }

    /**
     * @brief Set the output interface
     * @param oif the output interface
     */
    void SetInterface(int32_t oif)
    {
        m_oif = oif;
    }

    uint32_t GetSerializedSize() const override
    {
        return sizeof(int32_t);
    }

    void Serialize(TagBuffer i) const override
    {
        i.WriteU32(m_oif);
    }

    void Deserialize(TagBuffer i) override
    {
        m_oif = i.ReadU32();
    }

    void Print(std::ostream& os) const override
    {
        os << "DeferredRouteOutputTag: output interface = " << m_oif;
    }

  private:
    /// Positive if output device is fixed in RouteOutput
    int32_t m_oif;
};

NS_OBJECT_ENSURE_REGISTERED(DeferredRouteOutputTag);


//-----------------------------------------------------------------------------
RoutingProtocol::RoutingProtocol()
    : m_rreqRetries(2),
      m_ttlStart(1),
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
      m_enableHello(false),
      m_routingTable(m_deletePeriod),
      m_queue(m_maxQueueLen, m_maxQueueTime),
      m_requestId(0),
      m_seqNo(2),
      m_rreqIdCache(m_pathDiscoveryTime),
      m_dpd(m_pathDiscoveryTime),
      m_nb(m_helloInterval),
      m_rreqCount(0),
      m_rerrCount(0),
      m_htimer(Timer::CANCEL_ON_DESTROY),
      m_rreqRateLimitTimer(Timer::CANCEL_ON_DESTROY),
      m_rerrRateLimitTimer(Timer::CANCEL_ON_DESTROY),
      m_lastBcastTime(),
      m_maxPaths(3),
      m_primAltPathLenDiff(1)
{
    m_nb.SetCallback(MakeCallback(&RoutingProtocol::HandleLinkFailure, this));
}

TypeId
RoutingProtocol::GetTypeId()
{
    static TypeId tid =
        TypeId("ns3::aomdv::RoutingProtocol")
            .SetParent<Ipv4RoutingProtocol>()
            .SetGroupName("Aomdv")
            .AddConstructor<RoutingProtocol>()
            .AddAttribute("HelloInterval",
                          "HELLO messages emission interval.",
                          TimeValue(Seconds(1)),
                          MakeTimeAccessor(&RoutingProtocol::m_helloInterval),
                          MakeTimeChecker())
            .AddAttribute("TtlStart",
                          "Initial TTL value for RREQ.",
                          UintegerValue(1),
                          MakeUintegerAccessor(&RoutingProtocol::m_ttlStart),
                          MakeUintegerChecker<uint16_t>())
            .AddAttribute("TtlIncrement",
                          "TTL increment for each attempt using the expanding ring search for RREQ "
                          "dissemination.",
                          UintegerValue(2),
                          MakeUintegerAccessor(&RoutingProtocol::m_ttlIncrement),
                          MakeUintegerChecker<uint16_t>())
            .AddAttribute("TtlThreshold",
                          "Maximum TTL value for expanding ring search, TTL = NetDiameter is used "
                          "beyond this value.",
                          UintegerValue(7),
                          MakeUintegerAccessor(&RoutingProtocol::m_ttlThreshold),
                          MakeUintegerChecker<uint16_t>())
            .AddAttribute("TimeoutBuffer",
                          "Provide a buffer for the timeout.",
                          UintegerValue(2),
                          MakeUintegerAccessor(&RoutingProtocol::m_timeoutBuffer),
                          MakeUintegerChecker<uint16_t>())
            .AddAttribute("RreqRetries",
                          "Maximum number of retransmissions of RREQ to discover a route",
                          UintegerValue(2),
                          MakeUintegerAccessor(&RoutingProtocol::m_rreqRetries),
                          MakeUintegerChecker<uint32_t>())
            .AddAttribute("RreqRateLimit",
                          "Maximum number of RREQ per second.",
                          UintegerValue(10),
                          MakeUintegerAccessor(&RoutingProtocol::m_rreqRateLimit),
                          MakeUintegerChecker<uint32_t>())
            .AddAttribute("RerrRateLimit",
                          "Maximum number of RERR per second.",
                          UintegerValue(10),
                          MakeUintegerAccessor(&RoutingProtocol::m_rerrRateLimit),
                          MakeUintegerChecker<uint32_t>())
            .AddAttribute("NodeTraversalTime",
                          "Conservative estimate of the average one hop traversal time for packets "
                          "and should include "
                          "queuing delays, interrupt processing times and transfer times.",
                          TimeValue(MilliSeconds(40)),
                          MakeTimeAccessor(&RoutingProtocol::m_nodeTraversalTime),
                          MakeTimeChecker())
            .AddAttribute(
                "NextHopWait",
                "Period of our waiting for the neighbour's RREP_ACK = 10 ms + NodeTraversalTime",
                TimeValue(MilliSeconds(50)),
                MakeTimeAccessor(&RoutingProtocol::m_nextHopWait),
                MakeTimeChecker())
            .AddAttribute("ActiveRouteTimeout",
                          "Period of time during which the route is considered to be valid",
                          TimeValue(Seconds(3)),
                          MakeTimeAccessor(&RoutingProtocol::m_activeRouteTimeout),
                          MakeTimeChecker())
            .AddAttribute("MyRouteTimeout",
                          "Value of lifetime field in RREP generating by this node = 2 * "
                          "max(ActiveRouteTimeout, PathDiscoveryTime)",
                          TimeValue(Seconds(11.2)),
                          MakeTimeAccessor(&RoutingProtocol::m_myRouteTimeout),
                          MakeTimeChecker())
            .AddAttribute("BlackListTimeout",
                          "Time for which the node is put into the blacklist = RreqRetries * "
                          "NetTraversalTime",
                          TimeValue(Seconds(5.6)),
                          MakeTimeAccessor(&RoutingProtocol::m_blackListTimeout),
                          MakeTimeChecker())
            .AddAttribute("DeletePeriod",
                          "DeletePeriod is intended to provide an upper bound on the time for "
                          "which an upstream node A "
                          "can have a neighbor B as an active next hop for destination D, while B "
                          "has invalidated the route to D."
                          " = 5 * max (HelloInterval, ActiveRouteTimeout)",
                          TimeValue(Seconds(15)),
                          MakeTimeAccessor(&RoutingProtocol::m_deletePeriod),
                          MakeTimeChecker())
            .AddAttribute("NetDiameter",
                          "Net diameter measures the maximum possible number of hops between two "
                          "nodes in the network",
                          UintegerValue(35),
                          MakeUintegerAccessor(&RoutingProtocol::m_netDiameter),
                          MakeUintegerChecker<uint32_t>())
            .AddAttribute(
                "NetTraversalTime",
                "Estimate of the average net traversal time = 2 * NodeTraversalTime * NetDiameter",
                TimeValue(Seconds(2.8)),
                MakeTimeAccessor(&RoutingProtocol::m_netTraversalTime),
                MakeTimeChecker())
            .AddAttribute(
                "PathDiscoveryTime",
                "Estimate of maximum time needed to find route in network = 2 * NetTraversalTime",
                TimeValue(Seconds(5.6)),
                MakeTimeAccessor(&RoutingProtocol::m_pathDiscoveryTime),
                MakeTimeChecker())
            .AddAttribute("MaxQueueLen",
                          "Maximum number of packets that we allow a routing protocol to buffer.",
                          UintegerValue(64),
                          MakeUintegerAccessor(&RoutingProtocol::SetMaxQueueLen,
                                               &RoutingProtocol::GetMaxQueueLen),
                          MakeUintegerChecker<uint32_t>())
            .AddAttribute("MaxQueueTime",
                          "Maximum time packets can be queued (in seconds)",
                          TimeValue(Seconds(30)),
                          MakeTimeAccessor(&RoutingProtocol::SetMaxQueueTime,
                                           &RoutingProtocol::GetMaxQueueTime),
                          MakeTimeChecker())
            .AddAttribute("AllowedHelloLoss",
                          "Number of hello messages which may be loss for valid link.",
                          UintegerValue(2),
                          MakeUintegerAccessor(&RoutingProtocol::m_allowedHelloLoss),
                          MakeUintegerChecker<uint16_t>())
            .AddAttribute("GratuitousReply",
                          "Unused by AOMDV (kept for attribute compatibility with AODV).",
                          BooleanValue(true),
                          MakeBooleanAccessor(&RoutingProtocol::SetGratuitousReplyFlag,
                                              &RoutingProtocol::GetGratuitousReplyFlag),
                          MakeBooleanChecker())
            .AddAttribute("DestinationOnly",
                          "Unused by AOMDV (kept for attribute compatibility with AODV).",
                          BooleanValue(false),
                          MakeBooleanAccessor(&RoutingProtocol::SetDestinationOnlyFlag,
                                              &RoutingProtocol::GetDestinationOnlyFlag),
                          MakeBooleanChecker())
            .AddAttribute("EnableHello",
                          "Indicates whether a hello messages enable.",
                          BooleanValue(true),
                          MakeBooleanAccessor(&RoutingProtocol::SetHelloEnable,
                                              &RoutingProtocol::GetHelloEnable),
                          MakeBooleanChecker())
            .AddAttribute("EnableBroadcast",
                          "Indicates whether a broadcast data packets forwarding enable.",
                          BooleanValue(true),
                          MakeBooleanAccessor(&RoutingProtocol::SetBroadcastEnable,
                                              &RoutingProtocol::GetBroadcastEnable),
                          MakeBooleanChecker())
            .AddAttribute("MaxPaths",
                          "AOMDV: maximum number of paths per destination (ns-2 aomdv_max_paths_).",
                          UintegerValue(3),
                          MakeUintegerAccessor(&RoutingProtocol::m_maxPaths),
                          MakeUintegerChecker<uint32_t>(1))
            .AddAttribute("PrimAltPathLenDiff",
                          "AOMDV: maximum hop-count difference between the shortest and an "
                          "alternate path (ns-2 aomdv_prim_alt_path_len_diff_).",
                          UintegerValue(1),
                          MakeUintegerAccessor(&RoutingProtocol::m_primAltPathLenDiff),
                          MakeUintegerChecker<uint32_t>())
            .AddAttribute("UniformRv",
                          "Access to the underlying UniformRandomVariable",
                          StringValue("ns3::UniformRandomVariable"),
                          MakePointerAccessor(&RoutingProtocol::m_uniformRandomVariable),
                          MakePointerChecker<UniformRandomVariable>());
    return tid;
}

void
RoutingProtocol::SetMaxQueueLen(uint32_t len)
{
    m_maxQueueLen = len;
    m_queue.SetMaxQueueLen(len);
}

void
RoutingProtocol::SetMaxQueueTime(Time t)
{
    m_maxQueueTime = t;
    m_queue.SetQueueTimeout(t);
}

RoutingProtocol::~RoutingProtocol()
{
}

void
RoutingProtocol::DoDispose()
{
    m_ipv4 = nullptr;
    for (auto iter = m_socketAddresses.begin(); iter != m_socketAddresses.end(); iter++)
    {
        iter->first->Close();
    }
    m_socketAddresses.clear();
    for (auto iter = m_socketSubnetBroadcastAddresses.begin();
         iter != m_socketSubnetBroadcastAddresses.end();
         iter++)
    {
        iter->first->Close();
    }
    m_socketSubnetBroadcastAddresses.clear();
    Ipv4RoutingProtocol::DoDispose();
}

void
RoutingProtocol::PrintRoutingTable(Ptr<OutputStreamWrapper> stream, Time::Unit unit) const
{
    *stream->GetStream() << "Node: " << m_ipv4->GetObject<Node>()->GetId()
                         << "; Time: " << Now().As(unit)
                         << ", Local time: " << m_ipv4->GetObject<Node>()->GetLocalTime().As(unit)
                         << ", AOMDV Routing table" << std::endl;

    m_routingTable.Print(stream, unit);
    *stream->GetStream() << std::endl;
}

int64_t
RoutingProtocol::AssignStreams(int64_t stream)
{
    NS_LOG_FUNCTION(this << stream);
    m_uniformRandomVariable->SetStream(stream);
    return 1;
}

void
RoutingProtocol::Start()
{
    NS_LOG_FUNCTION(this);
    if (m_enableHello)
    {
        m_nb.ScheduleTimer();
    }
    m_rreqRateLimitTimer.SetFunction(&RoutingProtocol::RreqRateLimitTimerExpire, this);
    m_rreqRateLimitTimer.Schedule(Seconds(1));

    m_rerrRateLimitTimer.SetFunction(&RoutingProtocol::RerrRateLimitTimerExpire, this);
    m_rerrRateLimitTimer.Schedule(Seconds(1));
}

Ptr<Ipv4Route>
RoutingProtocol::RouteOutput(Ptr<Packet> p,
                             const Ipv4Header& header,
                             Ptr<NetDevice> oif,
                             Socket::SocketErrno& sockerr)
{
    NS_LOG_FUNCTION(this << header << (oif ? oif->GetIfIndex() : 0));
    if (!p)
    {
        NS_LOG_DEBUG("Packet is == 0");
        return LoopbackRoute(header, oif); // later
    }
    if (m_socketAddresses.empty())
    {
        sockerr = Socket::ERROR_NOROUTETOHOST;
        NS_LOG_LOGIC("No aomdv interfaces");
        Ptr<Ipv4Route> route;
        return route;
    }
    sockerr = Socket::ERROR_NOTERROR;
    Ptr<Ipv4Route> route;
    Ipv4Address dst = header.GetDestination();
    RoutingTableEntry rt;
    if (m_routingTable.LookupValidRoute(dst, rt))
    {
        route = rt.GetRoute();
        NS_ASSERT(route);
        if (oif && route->GetOutputDevice() != oif)
        {
            NS_LOG_DEBUG("Output device doesn't match. Dropped.");
            sockerr = Socket::ERROR_NOROUTETOHOST;
            return Ptr<Ipv4Route>();
        }
        // ns-2.35 AOMDV forward(): data always uses the head path; refresh its expiry.
        UseHeadPath(rt, false);
        return rt.GetRoute();
    }

    // Valid route not found, in this case we return loopback.
    // Actual route request will be deferred until packet will be fully formed,
    // routed to loopback, received from loopback and passed to RouteInput (see below)
    uint32_t iif = (oif ? m_ipv4->GetInterfaceForDevice(oif) : -1);
    DeferredRouteOutputTag tag(iif);
    NS_LOG_DEBUG("Valid Route not found");
    if (!p->PeekPacketTag(tag))
    {
        p->AddPacketTag(tag);
    }
    return LoopbackRoute(header, oif);
}

void
RoutingProtocol::DeferredRouteOutput(Ptr<const Packet> p,
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
        bool result = m_routingTable.LookupRoute(header.GetDestination(), rt);
        if (!result || ((rt.GetFlag() != IN_SEARCH) && result))
        {
            NS_LOG_LOGIC("Send new RREQ for outbound packet to " << header.GetDestination());
            SendRequest(header.GetDestination());
        }
    }
}

bool
RoutingProtocol::RouteInput(Ptr<const Packet> p,
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
        NS_LOG_LOGIC("No aomdv interfaces");
        return false;
    }
    NS_ASSERT(m_ipv4);
    NS_ASSERT(p);
    // Check if input device supports IP
    NS_ASSERT(m_ipv4->GetInterfaceForDevice(idev) >= 0);
    int32_t iif = m_ipv4->GetInterfaceForDevice(idev);

    Ipv4Address dst = header.GetDestination();
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

    // Duplicate of own packet
    if (IsMyOwnAddress(origin))
    {
        return true;
    }

    // AOMDV is not a multicast routing protocol
    if (dst.IsMulticast())
    {
        return false;
    }

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
                    NS_LOG_DEBUG("Duplicated packet " << p->GetUid() << " from " << origin
                                                      << ". Drop.");
                    return true;
                }
                Ptr<Packet> packet = p->Copy();
                if (!lcb.IsNull())
                {
                    NS_LOG_LOGIC("Broadcast local delivery to " << iface.GetLocal());
                    lcb(p, header, iif);
                    // Fall through to additional processing
                }
                else
                {
                    NS_LOG_ERROR("Unable to deliver packet locally due to null callback "
                                 << p->GetUid() << " from " << origin);
                    ecb(p, header, Socket::ERROR_NOROUTETOHOST);
                }
                if (!m_enableBroadcast)
                {
                    return true;
                }
                if (header.GetProtocol() == UdpL4Protocol::PROT_NUMBER)
                {
                    UdpHeader udpHeader;
                    p->PeekHeader(udpHeader);
                    if (udpHeader.GetDestinationPort() == AOMDV_PORT)
                    {
                        // AOMDV packets sent in broadcast are already managed
                        return true;
                    }
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
        // ns-2.35 AOMDV does not refresh routes on local delivery; link sensing
        // is kept identical to ns-3 AODV.
        RoutingTableEntry toOrigin;
        if (m_routingTable.LookupValidRoute(origin, toOrigin))
        {
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

    // Check if input device supports IP forwarding
    if (!m_ipv4->IsForwarding(iif))
    {
        NS_LOG_LOGIC("Forwarding disabled for this interface");
        ecb(p, header, Socket::ERROR_NOROUTETOHOST);
        return true;
    }

    // Forwarding
    return Forwarding(p, header, ucb, ecb);
}

bool
RoutingProtocol::Forwarding(Ptr<const Packet> p,
                            const Ipv4Header& header,
                            UnicastForwardCallback ucb,
                            ErrorCallback ecb)
{
    NS_LOG_FUNCTION(this);
    Ipv4Address dst = header.GetDestination();
    Ipv4Address origin = header.GetSource();
    RoutingTableEntry toDst;
    bool found = m_routingTable.LookupRoute(dst, toDst);
    if (found && toDst.GetFlag() == VALID)
    {
        // ns-2.35 AOMDV forward(): head path, expiry refreshed, rt_error set
        // because the packet was not originated here.
        UseHeadPath(toDst, true);
        Ptr<Ipv4Route> route = toDst.GetRoute();
        NS_LOG_LOGIC(route->GetSource() << " forwarding to " << dst << " from " << origin
                                        << " packet " << p->GetUid());
        // Link sensing kept identical to ns-3 AODV (neighbour liveness only).
        m_nb.Update(route->GetGateway(), m_activeRouteTimeout);
        RoutingTableEntry toOrigin;
        if (m_routingTable.LookupValidRoute(origin, toOrigin))
        {
            m_nb.Update(toOrigin.GetNextHop(), m_activeRouteTimeout);
        }
        ucb(route, p, header);
        return true;
    }
    // ns-2.35 AOMDV rt_resolve(): no route for a transit packet -> broadcast a
    // RERR for this destination with the locally known sequence number, drop.
    NS_LOG_DEBUG("Drop packet " << p->GetUid() << " because no route to forward it.");
    RerrHeader rerrHeader;
    rerrHeader.AddUnDestination(dst, found ? toDst.GetSeqNo() : 0);
    BroadcastRerr(rerrHeader);
    return false;
}

void
RoutingProtocol::SetIpv4(Ptr<Ipv4> ipv4)
{
    NS_ASSERT(ipv4);
    NS_ASSERT(!m_ipv4);

    m_ipv4 = ipv4;

    // Create lo route. It is asserted that the only one interface up for now is loopback
    NS_ASSERT(m_ipv4->GetNInterfaces() == 1 &&
              m_ipv4->GetAddress(0, 0).GetLocal() == Ipv4Address("127.0.0.1"));
    m_lo = m_ipv4->GetNetDevice(0);
    NS_ASSERT(m_lo);
    // Remember lo route
    RoutingTableEntry rt(
        /*dev=*/m_lo,
        /*dst=*/Ipv4Address::GetLoopback(),
        /*vSeqNo=*/true,
        /*seqNo=*/0,
        /*iface=*/Ipv4InterfaceAddress(Ipv4Address::GetLoopback(), Ipv4Mask("255.0.0.0")),
        /*hops=*/1,
        /*nextHop=*/Ipv4Address::GetLoopback(),
        /*lifetime=*/Simulator::GetMaximumSimulationTime());
    m_routingTable.AddRoute(rt);

    Simulator::ScheduleNow(&RoutingProtocol::Start, this);
}

void
RoutingProtocol::NotifyInterfaceUp(uint32_t i)
{
    NS_LOG_FUNCTION(this << m_ipv4->GetAddress(i, 0).GetLocal());
    Ptr<Ipv4L3Protocol> l3 = m_ipv4->GetObject<Ipv4L3Protocol>();
    if (l3->GetNAddresses(i) > 1)
    {
        NS_LOG_WARN("AOMDV does not work with more then one address per each interface.");
    }
    Ipv4InterfaceAddress iface = l3->GetAddress(i, 0);
    if (iface.GetLocal() == Ipv4Address("127.0.0.1"))
    {
        return;
    }

    // Create a socket to listen only on this interface
    Ptr<Socket> socket = Socket::CreateSocket(GetObject<Node>(), UdpSocketFactory::GetTypeId());
    NS_ASSERT(socket);
    socket->SetRecvCallback(MakeCallback(&RoutingProtocol::RecvAomdv, this));
    socket->BindToNetDevice(l3->GetNetDevice(i));
    socket->Bind(InetSocketAddress(iface.GetLocal(), AOMDV_PORT));
    socket->SetAllowBroadcast(true);
    socket->SetIpRecvTtl(true);
    m_socketAddresses.insert(std::make_pair(socket, iface));

    // create also a subnet broadcast socket
    socket = Socket::CreateSocket(GetObject<Node>(), UdpSocketFactory::GetTypeId());
    NS_ASSERT(socket);
    socket->SetRecvCallback(MakeCallback(&RoutingProtocol::RecvAomdv, this));
    socket->BindToNetDevice(l3->GetNetDevice(i));
    socket->Bind(InetSocketAddress(iface.GetBroadcast(), AOMDV_PORT));
    socket->SetAllowBroadcast(true);
    socket->SetIpRecvTtl(true);
    m_socketSubnetBroadcastAddresses.insert(std::make_pair(socket, iface));

    // Add local broadcast record to the routing table
    Ptr<NetDevice> dev = m_ipv4->GetNetDevice(m_ipv4->GetInterfaceForAddress(iface.GetLocal()));
    RoutingTableEntry rt(/*dev=*/dev,
                         /*dst=*/iface.GetBroadcast(),
                         /*vSeqNo=*/true,
                         /*seqNo=*/0,
                         /*iface=*/iface,
                         /*hops=*/1,
                         /*nextHop=*/iface.GetBroadcast(),
                         /*lifetime=*/Simulator::GetMaximumSimulationTime());
    m_routingTable.AddRoute(rt);

    if (l3->GetInterface(i)->GetArpCache())
    {
        m_nb.AddArpCache(l3->GetInterface(i)->GetArpCache());
    }

    // Allow neighbor manager use this interface for layer 2 feedback if possible
    Ptr<WifiNetDevice> wifi = dev->GetObject<WifiNetDevice>();
    if (!wifi)
    {
        return;
    }
    Ptr<WifiMac> mac = wifi->GetMac();
    if (!mac)
    {
        return;
    }

    mac->TraceConnectWithoutContext("DroppedMpdu",
                                    MakeCallback(&RoutingProtocol::NotifyTxError, this));
}

void
RoutingProtocol::NotifyTxError(WifiMacDropReason reason, Ptr<const WifiMpdu> mpdu)
{
    m_nb.GetTxErrorCallback()(mpdu->GetHeader());
}

void
RoutingProtocol::NotifyInterfaceDown(uint32_t i)
{
    NS_LOG_FUNCTION(this << m_ipv4->GetAddress(i, 0).GetLocal());

    // Disable layer 2 link state monitoring (if possible)
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

    // Close socket
    Ptr<Socket> socket = FindSocketWithInterfaceAddress(m_ipv4->GetAddress(i, 0));
    NS_ASSERT(socket);
    socket->Close();
    m_socketAddresses.erase(socket);

    // Close socket
    socket = FindSubnetBroadcastSocketWithInterfaceAddress(m_ipv4->GetAddress(i, 0));
    NS_ASSERT(socket);
    socket->Close();
    m_socketSubnetBroadcastAddresses.erase(socket);

    if (m_socketAddresses.empty())
    {
        NS_LOG_LOGIC("No aomdv interfaces");
        m_htimer.Cancel();
        m_nb.Clear();
        m_routingTable.Clear();
        return;
    }
    m_routingTable.DeleteAllRoutesFromInterface(m_ipv4->GetAddress(i, 0));
}

void
RoutingProtocol::NotifyAddAddress(uint32_t i, Ipv4InterfaceAddress address)
{
    NS_LOG_FUNCTION(this << " interface " << i << " address " << address);
    Ptr<Ipv4L3Protocol> l3 = m_ipv4->GetObject<Ipv4L3Protocol>();
    if (!l3->IsUp(i))
    {
        return;
    }
    if (l3->GetNAddresses(i) == 1)
    {
        Ipv4InterfaceAddress iface = l3->GetAddress(i, 0);
        Ptr<Socket> socket = FindSocketWithInterfaceAddress(iface);
        if (!socket)
        {
            if (iface.GetLocal() == Ipv4Address("127.0.0.1"))
            {
                return;
            }
            // Create a socket to listen only on this interface
            Ptr<Socket> socket =
                Socket::CreateSocket(GetObject<Node>(), UdpSocketFactory::GetTypeId());
            NS_ASSERT(socket);
            socket->SetRecvCallback(MakeCallback(&RoutingProtocol::RecvAomdv, this));
            socket->BindToNetDevice(l3->GetNetDevice(i));
            socket->Bind(InetSocketAddress(iface.GetLocal(), AOMDV_PORT));
            socket->SetAllowBroadcast(true);
            m_socketAddresses.insert(std::make_pair(socket, iface));

            // create also a subnet directed broadcast socket
            socket = Socket::CreateSocket(GetObject<Node>(), UdpSocketFactory::GetTypeId());
            NS_ASSERT(socket);
            socket->SetRecvCallback(MakeCallback(&RoutingProtocol::RecvAomdv, this));
            socket->BindToNetDevice(l3->GetNetDevice(i));
            socket->Bind(InetSocketAddress(iface.GetBroadcast(), AOMDV_PORT));
            socket->SetAllowBroadcast(true);
            socket->SetIpRecvTtl(true);
            m_socketSubnetBroadcastAddresses.insert(std::make_pair(socket, iface));

            // Add local broadcast record to the routing table
            Ptr<NetDevice> dev =
                m_ipv4->GetNetDevice(m_ipv4->GetInterfaceForAddress(iface.GetLocal()));
            RoutingTableEntry rt(/*dev=*/dev,
                                 /*dst=*/iface.GetBroadcast(),
                                 /*vSeqNo=*/true,
                                 /*seqNo=*/0,
                                 /*iface=*/iface,
                                 /*hops=*/1,
                                 /*nextHop=*/iface.GetBroadcast(),
                                 /*lifetime=*/Simulator::GetMaximumSimulationTime());
            m_routingTable.AddRoute(rt);
        }
    }
    else
    {
        NS_LOG_LOGIC("AOMDV does not work with more then one address per each interface. Ignore "
                     "added address");
    }
}

void
RoutingProtocol::NotifyRemoveAddress(uint32_t i, Ipv4InterfaceAddress address)
{
    NS_LOG_FUNCTION(this);
    Ptr<Socket> socket = FindSocketWithInterfaceAddress(address);
    if (socket)
    {
        m_routingTable.DeleteAllRoutesFromInterface(address);
        socket->Close();
        m_socketAddresses.erase(socket);

        Ptr<Socket> unicastSocket = FindSubnetBroadcastSocketWithInterfaceAddress(address);
        if (unicastSocket)
        {
            unicastSocket->Close();
            m_socketAddresses.erase(unicastSocket);
        }

        Ptr<Ipv4L3Protocol> l3 = m_ipv4->GetObject<Ipv4L3Protocol>();
        if (l3->GetNAddresses(i))
        {
            Ipv4InterfaceAddress iface = l3->GetAddress(i, 0);
            // Create a socket to listen only on this interface
            Ptr<Socket> socket =
                Socket::CreateSocket(GetObject<Node>(), UdpSocketFactory::GetTypeId());
            NS_ASSERT(socket);
            socket->SetRecvCallback(MakeCallback(&RoutingProtocol::RecvAomdv, this));
            // Bind to any IP address so that broadcasts can be received
            socket->BindToNetDevice(l3->GetNetDevice(i));
            socket->Bind(InetSocketAddress(iface.GetLocal(), AOMDV_PORT));
            socket->SetAllowBroadcast(true);
            socket->SetIpRecvTtl(true);
            m_socketAddresses.insert(std::make_pair(socket, iface));

            // create also a unicast socket
            socket = Socket::CreateSocket(GetObject<Node>(), UdpSocketFactory::GetTypeId());
            NS_ASSERT(socket);
            socket->SetRecvCallback(MakeCallback(&RoutingProtocol::RecvAomdv, this));
            socket->BindToNetDevice(l3->GetNetDevice(i));
            socket->Bind(InetSocketAddress(iface.GetBroadcast(), AOMDV_PORT));
            socket->SetAllowBroadcast(true);
            socket->SetIpRecvTtl(true);
            m_socketSubnetBroadcastAddresses.insert(std::make_pair(socket, iface));

            // Add local broadcast record to the routing table
            Ptr<NetDevice> dev =
                m_ipv4->GetNetDevice(m_ipv4->GetInterfaceForAddress(iface.GetLocal()));
            RoutingTableEntry rt(/*dev=*/dev,
                                 /*dst=*/iface.GetBroadcast(),
                                 /*vSeqNo=*/true,
                                 /*seqNo=*/0,
                                 /*iface=*/iface,
                                 /*hops=*/1,
                                 /*nextHop=*/iface.GetBroadcast(),
                                 /*lifetime=*/Simulator::GetMaximumSimulationTime());
            m_routingTable.AddRoute(rt);
        }
        if (m_socketAddresses.empty())
        {
            NS_LOG_LOGIC("No aomdv interfaces");
            m_htimer.Cancel();
            m_nb.Clear();
            m_routingTable.Clear();
            return;
        }
    }
    else
    {
        NS_LOG_LOGIC("Remove address not participating in AOMDV operation");
    }
}

bool
RoutingProtocol::IsMyOwnAddress(Ipv4Address src)
{
    NS_LOG_FUNCTION(this << src);
    for (auto j = m_socketAddresses.begin(); j != m_socketAddresses.end(); ++j)
    {
        Ipv4InterfaceAddress iface = j->second;
        if (src == iface.GetLocal())
        {
            return true;
        }
    }
    return false;
}

Ptr<Ipv4Route>
RoutingProtocol::LoopbackRoute(const Ipv4Header& hdr, Ptr<NetDevice> oif) const
{
    NS_LOG_FUNCTION(this << hdr);
    NS_ASSERT(m_lo);
    Ptr<Ipv4Route> rt = Create<Ipv4Route>();
    rt->SetDestination(hdr.GetDestination());
    //
    // Source address selection here is tricky.  The loopback route is
    // returned when AOMDV does not have a route; this causes the packet
    // to be looped back and handled (cached) in RouteInput() method
    // while a route is found. However, connection-oriented protocols
    // like TCP need to create an endpoint four-tuple (src, src port,
    // dst, dst port) and create a pseudo-header for checksumming.  So,
    // AOMDV needs to guess correctly what the eventual source address
    // will be.
    //
    // For single interface, single address nodes, this is not a problem.
    // When there are possibly multiple outgoing interfaces, the policy
    // implemented here is to pick the first available AOMDV interface.
    // If RouteOutput() caller specified an outgoing interface, that
    // further constrains the selection of source address
    //
    auto j = m_socketAddresses.begin();
    if (oif)
    {
        // Iterate to find an address on the oif device
        for (j = m_socketAddresses.begin(); j != m_socketAddresses.end(); ++j)
        {
            Ipv4Address addr = j->second.GetLocal();
            int32_t interface = m_ipv4->GetInterfaceForAddress(addr);
            if (oif == m_ipv4->GetNetDevice(static_cast<uint32_t>(interface)))
            {
                rt->SetSource(addr);
                break;
            }
        }
    }
    else
    {
        rt->SetSource(j->second.GetLocal());
    }
    NS_ASSERT_MSG(rt->GetSource() != Ipv4Address(), "Valid AOMDV source address not found");
    rt->SetGateway(Ipv4Address("127.0.0.1"));
    rt->SetOutputDevice(m_lo);
    return rt;
}

void
RoutingProtocol::SendRequest(Ipv4Address dst)
{
    NS_LOG_FUNCTION(this << dst);
    RoutingTableEntry rt;
    if (m_routingTable.LookupValidRoute(dst, rt))
    {
        // ns-2.35 sendRequest(): never search for a destination with an UP route
        SendPacketFromQueue(dst, rt.GetRoute());
        return;
    }
    // A node SHOULD NOT originate more than RREQ_RATELIMIT RREQ messages per second.
    if (m_rreqCount == m_rreqRateLimit)
    {
        Simulator::Schedule(m_rreqRateLimitTimer.GetDelayLeft() + MicroSeconds(100),
                            &RoutingProtocol::SendRequest,
                            this,
                            dst);
        return;
    }
    else
    {
        m_rreqCount++;
    }
    // Create RREQ header
    RreqHeader rreqHeader;
    rreqHeader.SetDst(dst);

    // Expanding ring search kept identical to ns-3 AODV (Hop field stores the TTL
    // while the entry is IN_SEARCH).
    uint16_t ttl = m_ttlStart;
    if (m_routingTable.LookupRoute(dst, rt))
    {
        if (rt.GetFlag() != IN_SEARCH)
        {
            ttl = std::min<uint16_t>(rt.GetHop() + m_ttlIncrement, m_netDiameter);
        }
        else
        {
            ttl = rt.GetHop() + m_ttlIncrement;
            if (ttl > m_ttlThreshold)
            {
                ttl = m_netDiameter;
            }
        }
        if (ttl == m_netDiameter)
        {
            rt.IncrementRreqCnt();
        }
        // ns-2.35: RREQ carries the last known destination sequence number
        rreqHeader.SetDstSeqno(rt.GetSeqNo());
        rt.SetHop(ttl);
        rt.SetFlag(IN_SEARCH);
        rt.SetLifeTime(m_pathDiscoveryTime);
        m_routingTable.Update(rt);
    }
    else
    {
        rreqHeader.SetDstSeqno(0);
        Ptr<NetDevice> dev = nullptr;
        RoutingTableEntry newEntry(/*dev=*/dev,
                                   /*dst=*/dst,
                                   /*vSeqNo=*/false,
                                   /*seqNo=*/0,
                                   /*iface=*/Ipv4InterfaceAddress(),
                                   /*hops=*/ttl,
                                   /*nextHop=*/Ipv4Address(),
                                   /*lifetime=*/m_pathDiscoveryTime);
        // Check if TtlStart == NetDiameter
        if (ttl == m_netDiameter)
        {
            newEntry.IncrementRreqCnt();
        }
        newEntry.SetFlag(IN_SEARCH);
        m_routingTable.AddRoute(newEntry);
    }

    // ns-2.35: own sequence number advances by 2 per RREQ (stays even)
    m_seqNo += 2;
    rreqHeader.SetOriginSeqno(m_seqNo);
    rreqHeader.SetHopCount(0);
    m_requestId++;
    rreqHeader.SetId(m_requestId);

    // Send RREQ as subnet directed broadcast from each interface used by aomdv
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
        TypeHeader tHeader(AOMDVTYPE_RREQ);
        packet->AddHeader(tHeader);
        Ipv4Address destination = BroadcastAddress(iface);
        NS_LOG_DEBUG("Send RREQ with id " << rreqHeader.GetId() << " to socket");
        m_lastBcastTime = Simulator::Now();
        Simulator::Schedule(MilliSeconds(m_uniformRandomVariable->GetInteger(0, 10)),
                            &RoutingProtocol::SendTo,
                            this,
                            socket,
                            packet,
                            destination);
    }
    ScheduleRreqRetry(dst);
}

void
RoutingProtocol::SendTo(Ptr<Socket> socket, Ptr<Packet> packet, Ipv4Address destination)
{
    socket->SendTo(packet, 0, InetSocketAddress(destination, AOMDV_PORT));
}

void
RoutingProtocol::ScheduleRreqRetry(Ipv4Address dst)
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
    {
        retry = 2 * m_nodeTraversalTime * (rt.GetHop() + m_timeoutBuffer);
    }
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

void
RoutingProtocol::RecvAomdv(Ptr<Socket> socket)
{
    NS_LOG_FUNCTION(this << socket);
    Address sourceAddress;
    Ptr<Packet> packet = socket->RecvFrom(sourceAddress);
    InetSocketAddress inetSourceAddr = InetSocketAddress::ConvertFrom(sourceAddress);
    Ipv4Address sender = inetSourceAddr.GetIpv4();
    Ipv4Address receiver;

    if (m_socketAddresses.find(socket) != m_socketAddresses.end())
    {
        receiver = m_socketAddresses[socket].GetLocal();
    }
    else if (m_socketSubnetBroadcastAddresses.find(socket) !=
             m_socketSubnetBroadcastAddresses.end())
    {
        receiver = m_socketSubnetBroadcastAddresses[socket].GetLocal();
    }
    else
    {
        NS_ASSERT_MSG(false, "Received a packet from an unknown socket");
    }
    NS_LOG_DEBUG("AOMDV node " << this << " received a AOMDV packet from " << sender << " to "
                              << receiver);

    TypeHeader tHeader(AOMDVTYPE_RREQ);
    packet->RemoveHeader(tHeader);
    if (!tHeader.IsValid())
    {
        NS_LOG_DEBUG("AOMDV message " << packet->GetUid() << " with unknown type received: "
                                     << tHeader.Get() << ". Drop");
        return; // drop
    }
    switch (tHeader.Get())
    {
    case AOMDVTYPE_RREQ: {
        RecvRequest(packet, receiver, sender);
        break;
    }
    case AOMDVTYPE_RREP: {
        RecvReply(packet, receiver, sender);
        break;
    }
    case AOMDVTYPE_RERR: {
        RecvError(packet, sender);
        break;
    }
    case AOMDVTYPE_RREP_ACK: {
        RecvReplyAck(sender);
        break;
    }
    }
}

void
RoutingProtocol::RecvRequest(Ptr<Packet> p, Ipv4Address receiver, Ipv4Address src)
{
    NS_LOG_FUNCTION(this);
    RreqHeader rreqHeader;
    p->RemoveHeader(rreqHeader);

    uint32_t id = rreqHeader.GetId();
    Ipv4Address origin = rreqHeader.GetOrigin();

    // ns-2.35 AOMDV recvRequest(): drop my own RREQ
    if (IsMyOwnAddress(origin))
    {
        return;
    }
    // Duplicates are NOT dropped: they may carry alternate disjoint reverse
    // paths. They are only prevented from being re-broadcast.
    bool killPropagation = m_rreqIdCache.IsDuplicate(origin, id);
    uint32_t* bcount = m_rreqIdCache.GetCount(origin, id);
    NS_ASSERT(bcount);

    // Link sensing kept identical to ns-3 AODV
    m_nb.Update(src, Time(m_allowedHelloLoss * m_helloInterval));

    // If I am a neighbour of the RREQ source, I am the first hop.
    if (rreqHeader.GetHopCount() == 0)
    {
        rreqHeader.SetFirstHop(receiver);
    }
    Ipv4Address firstHop = rreqHeader.GetFirstHop();
    uint16_t pathHop = uint16_t(rreqHeader.GetHopCount()) + 1;
    int32_t iface = m_ipv4->GetInterfaceForAddress(receiver);
    Ptr<NetDevice> dev = m_ipv4->GetNetDevice(iface);
    Ipv4InterfaceAddress ifaddr = m_ipv4->GetAddress(iface, 0);
    Time revLife = std::max(Time(2 * m_netTraversalTime - 2 * pathHop * m_nodeTraversalTime),
                            m_nodeTraversalTime);

    // Reverse route entry (route back to the RREQ source)
    RoutingTableEntry rt0;
    if (!m_routingTable.LookupRoute(origin, rt0))
    {
        RoutingTableEntry newEntry(dev,
                                   origin,
                                   /*vSeqNo=*/false,
                                   /*seqNo=*/0,
                                   ifaddr,
                                   /*hops=*/AOMDV_INFINITY,
                                   /*nextHop=*/Ipv4Address(),
                                   /*lifetime=*/Seconds(0));
        newEntry.SetFlag(INVALID);
        m_routingTable.AddRoute(newEntry);
        m_routingTable.LookupRoute(origin, rt0);
    }

    bool haveReversePath = false;
    uint32_t srcSeq = rreqHeader.GetOriginSeqno();
    if (rt0.GetSeqNo() < srcSeq)
    {
        // Newer sequence number: reset the reverse route
        rt0.SetSeqNo(srcSeq);
        rt0.SetValidSeqNo(true);
        rt0.SetAdvertisedHops(AOMDV_INFINITY);
        rt0.PathDeleteAll();
        rt0.SetFlag(VALID);
        rt0.SetOutputDevice(dev);
        rt0.SetInterface(ifaddr);
        rt0.PathInsert(src, pathHop, Simulator::Now() + revLife, firstHop);
        rt0.SetLastHopCount(rt0.PathMaxHop());
        haveReversePath = true;
    }
    else if (rt0.GetSeqNo() == srcSeq && rt0.GetFlag() == VALID &&
             rt0.GetAdvertisedHops() > rreqHeader.GetHopCount())
    {
        // ns-2.35 asserts the entry is UP here. A route that went down keeps its sequence
        // number but its advertised hop count was reset to infinity, so accepting same-seqno
        // paths again would break the AOMDV loop-freedom invariant: a newer seqno is required.
        AomdvPath* erp = nullptr;
        if (AomdvPath* rp = rt0.DisjointPathLookup(src, firstHop))
        {
            // Path already known: extend its lifetime
            rp->expire = std::max(rp->expire, Simulator::Now() + revLife);
            haveReversePath = true;
        }
        else if (rt0.NewDisjointPath(src, firstHop))
        {
            int32_t diff = int32_t(pathHop) - int32_t(rt0.PathMinHop());
            if (rt0.PathCount() < m_maxPaths && diff <= int32_t(m_primAltPathLenDiff))
            {
                rt0.PathInsert(src, pathHop, Simulator::Now() + revLife, firstHop);
                rt0.SetLastHopCount(rt0.PathMaxHop());
                haveReversePath = true;
            }
            if (diff > int32_t(m_primAltPathLenDiff))
            {
                m_routingTable.Update(rt0);
                return;
            }
        }
        else if (IsMyOwnAddress(rreqHeader.GetDst()) &&
                 ((erp = rt0.PathLookupLastHop(firstHop)) == nullptr || pathHop > erp->hopCount))
        {
            m_routingTable.Update(rt0);
            return;
        }
    }
    else
    {
        // Older sequence number, or same one with a larger hop count
        return;
    }
    rt0.SyncFromPaths();
    m_routingTable.Update(rt0);
    if (rt0.GetFlag() == VALID)
    {
        ServeQueue(origin, rt0);
    }

    NS_LOG_LOGIC(receiver << " receive RREQ with hop count "
                          << static_cast<uint32_t>(rreqHeader.GetHopCount()) << " ID "
                          << rreqHeader.GetId() << " to destination " << rreqHeader.GetDst());

    Ipv4Address dst = rreqHeader.GetDst();
    RoutingTableEntry rt;
    bool haveRt = m_routingTable.LookupRoute(dst, rt);

    // (i) I am the destination: answer every accepted copy
    if (IsMyOwnAddress(dst))
    {
        if (m_seqNo < rreqHeader.GetDstSeqno())
        {
            m_seqNo = rreqHeader.GetDstSeqno() + 1;
        }
        if (m_seqNo % 2)
        {
            m_seqNo++;
        }
        SendReply(origin,
                  /*hopCount=*/0,
                  /*dst=*/dst,
                  m_seqNo,
                  m_myRouteTimeout,
                  /*nextHop=*/src,
                  /*bcastId=*/id,
                  /*firstHop=*/src,
                  ifaddr);
        return;
    }
    // (ii) I have a fresh enough route: reply once per RREQ (node-disjoint mode)
    if (haveRt && rt.GetFlag() == VALID && rt.GetSeqNo() >= rreqHeader.GetDstSeqno())
    {
        if (haveReversePath && *bcount == 0)
        {
            *bcount = 1;
            if (rt.GetAdvertisedHops() == AOMDV_INFINITY)
            {
                rt.SetAdvertisedHops(rt.PathMaxHop());
            }
            AomdvPath* fwd = rt.PathFind();
            rt.SetError(true);
            Time life = fwd->expire - Simulator::Now();
            Ipv4Address fwdLastHop = fwd->lastHop;
            m_routingTable.Update(rt);
            SendReply(origin,
                      rt.GetAdvertisedHops(),
                      dst,
                      rt.GetSeqNo(),
                      life,
                      src,
                      id,
                      fwdLastHop,
                      ifaddr);
        }
        return;
    }
    // (iii) forward the RREQ, unless it is a duplicate
    if (killPropagation)
    {
        return;
    }
    SocketIpTtlTag tag;
    p->RemovePacketTag(tag);
    if (tag.GetTtl() < 2)
    {
        NS_LOG_DEBUG("TTL exceeded. Drop RREQ origin " << src << " destination " << dst);
        return;
    }
    if (haveRt)
    {
        rreqHeader.SetDstSeqno(std::max(rt.GetSeqNo(), rreqHeader.GetDstSeqno()));
    }
    m_routingTable.LookupRoute(origin, rt0);
    if (rt0.GetAdvertisedHops() == AOMDV_INFINITY)
    {
        rt0.SetAdvertisedHops(rt0.PathMaxHop());
        m_routingTable.Update(rt0);
    }
    rreqHeader.SetHopCount(uint8_t(rt0.GetAdvertisedHops()));
    NS_ASSERT(rt0.PathFind());
    rreqHeader.SetFirstHop(rt0.PathFind()->lastHop);

    for (auto j = m_socketAddresses.begin(); j != m_socketAddresses.end(); ++j)
    {
        Ptr<Socket> socket = j->first;
        Ipv4InterfaceAddress ifc = j->second;
        Ptr<Packet> packet = Create<Packet>();
        SocketIpTtlTag ttl;
        ttl.SetTtl(tag.GetTtl() - 1);
        packet->AddPacketTag(ttl);
        packet->AddHeader(rreqHeader);
        TypeHeader tHeader(AOMDVTYPE_RREQ);
        packet->AddHeader(tHeader);
        m_lastBcastTime = Simulator::Now();
        Simulator::Schedule(MilliSeconds(m_uniformRandomVariable->GetInteger(0, 10)),
                            &RoutingProtocol::SendTo,
                            this,
                            socket,
                            packet,
                            BroadcastAddress(ifc));
    }
}

void
RoutingProtocol::SendReply(Ipv4Address origin,
                           uint16_t hopCount,
                           Ipv4Address dst,
                           uint32_t dstSeqNo,
                           Time lifetime,
                           Ipv4Address nextHop,
                           uint32_t bcastId,
                           Ipv4Address firstHop,
                           Ipv4InterfaceAddress iface)
{
    NS_LOG_FUNCTION(this << origin << dst << nextHop);
    RrepHeader rrepHeader(/*prefixSize=*/0,
                          /*hopCount=*/uint8_t(hopCount),
                          /*dst=*/dst,
                          /*dstSeqNo=*/dstSeqNo,
                          /*origin=*/origin,
                          /*lifetime=*/lifetime);
    rrepHeader.SetBcastId(bcastId);
    rrepHeader.SetFirstHop(firstHop);
    Ptr<Packet> packet = Create<Packet>();
    packet->AddHeader(rrepHeader);
    packet->AddHeader(TypeHeader(AOMDVTYPE_RREP));
    SendUnicastControl(packet, nextHop, iface);
}

void
RoutingProtocol::SendReplyAck(Ipv4Address neighbor)
{
    NS_LOG_FUNCTION(this << " to " << neighbor);
    RrepAckHeader h;
    TypeHeader typeHeader(AOMDVTYPE_RREP_ACK);
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
    socket->SendTo(packet, 0, InetSocketAddress(neighbor, AOMDV_PORT));
}

void
RoutingProtocol::RecvReply(Ptr<Packet> p, Ipv4Address receiver, Ipv4Address sender)
{
    NS_LOG_FUNCTION(this << " src " << sender);
    RrepHeader rrepHeader;
    p->RemoveHeader(rrepHeader);

    // HELLO (dst == origin): ns-2.35 recvHello() hands it to recvReply() with
    // rp_src = sender, rp_first_hop = me, IP destination = me.
    bool hello = (rrepHeader.GetDst() == rrepHeader.GetOrigin());
    if (hello)
    {
        if (m_enableHello)
        {
            m_nb.Update(rrepHeader.GetDst(), Time(m_allowedHelloLoss * m_helloInterval));
        }
        rrepHeader.SetFirstHop(receiver);
    }
    Ipv4Address dst = rrepHeader.GetDst();
    if (IsMyOwnAddress(dst))
    {
        return;
    }
    uint16_t pathHop = uint16_t(rrepHeader.GetHopCount()) + 1;
    int32_t iface = m_ipv4->GetInterfaceForAddress(receiver);
    Ptr<NetDevice> dev = m_ipv4->GetNetDevice(iface);
    Ipv4InterfaceAddress ifaddr = m_ipv4->GetAddress(iface, 0);
    Time expire = Simulator::Now() + rrepHeader.GetLifeTime();
    Ipv4Address firstHop = rrepHeader.GetFirstHop();

    RoutingTableEntry rt;
    if (!m_routingTable.LookupRoute(dst, rt))
    {
        RoutingTableEntry newEntry(dev,
                                   dst,
                                   /*vSeqNo=*/false,
                                   /*seqNo=*/0,
                                   ifaddr,
                                   /*hops=*/AOMDV_INFINITY,
                                   /*nextHop=*/Ipv4Address(),
                                   /*lifetime=*/Seconds(0));
        newEntry.SetFlag(INVALID);
        m_routingTable.AddRoute(newEntry);
        m_routingTable.LookupRoute(dst, rt);
    }
    bool wasInSearch = (rt.GetFlag() == IN_SEARCH);
    uint32_t seq = rrepHeader.GetDstSeqno();
    if (rt.GetSeqNo() < seq)
    {
        rt.SetSeqNo(seq);
        rt.SetValidSeqNo(true);
        rt.SetAdvertisedHops(AOMDV_INFINITY);
        rt.PathDeleteAll();
        rt.SetFlag(VALID);
        rt.SetOutputDevice(dev);
        rt.SetInterface(ifaddr);
        rt.PathInsert(sender, pathHop, expire, firstHop);
        rt.SetLastHopCount(rt.PathMaxHop());
    }
    else if (rt.GetSeqNo() == seq && rt.GetFlag() == VALID &&
             rt.GetAdvertisedHops() > rrepHeader.GetHopCount())
    {
        // same-seqno update only for an UP entry (see RecvRequest)
        if (AomdvPath* fp = rt.DisjointPathLookup(sender, firstHop))
        {
            fp->expire = std::max(fp->expire, expire);
        }
        else if (rt.NewDisjointPath(sender, firstHop) && rt.PathCount() < m_maxPaths &&
                 int32_t(pathHop) - int32_t(rt.PathMinHop()) <= int32_t(m_primAltPathLenDiff))
        {
            rt.PathInsert(sender, pathHop, expire, firstHop);
            rt.SetLastHopCount(rt.PathMaxHop());
        }
        else
        {
            return;
        }
    }
    else
    {
        return;
    }
    rt.SyncFromPaths();
    m_routingTable.Update(rt);
    if (rt.GetFlag() == VALID)
    {
        if (wasInSearch)
        {
            m_addressReqTimer[dst].Cancel();
            m_addressReqTimer.erase(dst);
        }
        ServeQueue(dst, rt);
    }

    // I am the RREP destination (or this is a HELLO): done
    if (hello || IsMyOwnAddress(rrepHeader.GetOrigin()))
    {
        return;
    }

    // Forward the RREP along the head reverse path, once per (origin, bcastId)
    Ipv4Address origin = rrepHeader.GetOrigin();
    RoutingTableEntry rt0;
    uint32_t* bcount = m_rreqIdCache.GetCount(origin, rrepHeader.GetBcastId());
    if (!m_routingTable.LookupRoute(origin, rt0) || rt0.GetFlag() != VALID || bcount == nullptr ||
        *bcount != 0)
    {
        return;
    }
    *bcount = 1;
    AomdvPath* rev = rt0.PathFind();
    NS_ASSERT(rev);
    Ipv4Address revNextHop = rev->nextHop;
    rev->expire = Simulator::Now() + m_activeRouteTimeout;
    rt0.SyncFromPaths();
    m_routingTable.Update(rt0);

    m_routingTable.LookupRoute(dst, rt);
    if (rt.GetAdvertisedHops() == AOMDV_INFINITY)
    {
        rt.SetAdvertisedHops(rt.PathMaxHop());
    }
    rt.SetError(true);
    m_routingTable.Update(rt);

    rrepHeader.SetHopCount(uint8_t(rt.GetAdvertisedHops()));
    rrepHeader.SetFirstHop(rt.PathFind()->lastHop);
    Ptr<Packet> packet = Create<Packet>();
    packet->AddHeader(rrepHeader);
    packet->AddHeader(TypeHeader(AOMDVTYPE_RREP));
    SendUnicastControl(packet, revNextHop, rt0.GetInterface());
}

void
RoutingProtocol::RecvReplyAck(Ipv4Address neighbor)
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

void
RoutingProtocol::RecvError(Ptr<Packet> p, Ipv4Address src)
{
    NS_LOG_FUNCTION(this << " from " << src);
    RerrHeader rerrHeader;
    p->RemoveHeader(rerrHeader);
    RerrHeader out;
    std::pair<Ipv4Address, uint32_t> un;
    while (rerrHeader.RemoveUnDestination(un))
    {
        RoutingTableEntry rt;
        if (!m_routingTable.LookupRoute(un.first, rt) || rt.GetFlag() != VALID ||
            rt.PathLookup(src) == nullptr || rt.GetSeqNo() > un.second)
        {
            continue;
        }
        rt.PathDelete(src);
        rt.SetHighestSeqHeard(std::max(rt.GetHighestSeqHeard(), un.second));
        if (rt.PathEmpty())
        {
            rt.SetSeqNo(rt.GetHighestSeqHeard());
            rt.Invalidate(m_deletePeriod);
            if (rt.GetError())
            {
                if (!out.AddUnDestination(rt.GetDestination(), rt.GetSeqNo()))
                {
                    BroadcastRerr(out);
                    out.Clear();
                    out.AddUnDestination(rt.GetDestination(), rt.GetSeqNo());
                }
                rt.SetError(false);
            }
        }
        else
        {
            rt.SyncFromPaths();
        }
        m_routingTable.Update(rt);
    }
    if (out.GetDestCount() != 0)
    {
        BroadcastRerr(out);
    }
}

void
RoutingProtocol::RouteRequestTimerExpire(Ipv4Address dst)
{
    NS_LOG_LOGIC(this);
    RoutingTableEntry toDst;
    if (m_routingTable.LookupValidRoute(dst, toDst))
    {
        SendPacketFromQueue(dst, toDst.GetRoute());
        NS_LOG_LOGIC("route to " << dst << " found");
        return;
    }
    /*
     *  If a route discovery has been attempted RreqRetries times at the maximum TTL without
     *  receiving any RREP, all data packets destined for the corresponding destination SHOULD be
     *  dropped from the buffer and a Destination Unreachable message SHOULD be delivered to the
     * application.
     */
    if (toDst.GetRreqCnt() == m_rreqRetries)
    {
        NS_LOG_LOGIC("route discovery to " << dst << " has been attempted RreqRetries ("
                                           << m_rreqRetries << ") times with ttl "
                                           << m_netDiameter);
        m_addressReqTimer.erase(dst);
        ForgetSearch(dst);
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
        ForgetSearch(dst);
        m_queue.DropPacketWithDst(dst);
    }
}

void
RoutingProtocol::HelloTimerExpire()
{
    NS_LOG_FUNCTION(this);
    Time offset;
    if (m_lastBcastTime.IsStrictlyPositive())
    {
        offset = Simulator::Now() - m_lastBcastTime;
        NS_LOG_DEBUG("Hello deferred due to last bcast at:" << m_lastBcastTime);
    }
    else
    {
        SendHello();
    }
    m_htimer.Cancel();
    Time diff = m_helloInterval - offset;
    m_htimer.Schedule(std::max(Seconds(0), diff));
    m_lastBcastTime = Seconds(0);
}

void
RoutingProtocol::RreqRateLimitTimerExpire()
{
    NS_LOG_FUNCTION(this);
    m_rreqCount = 0;
    m_rreqRateLimitTimer.Schedule(Seconds(1));
}

void
RoutingProtocol::RerrRateLimitTimerExpire()
{
    NS_LOG_FUNCTION(this);
    m_rerrCount = 0;
    m_rerrRateLimitTimer.Schedule(Seconds(1));
}

void
RoutingProtocol::AckTimerExpire(Ipv4Address neighbor, Time blacklistTimeout)
{
    NS_LOG_FUNCTION(this);
    m_routingTable.MarkLinkAsUnidirectional(neighbor, blacklistTimeout);
}

void
RoutingProtocol::SendHello()
{
    NS_LOG_FUNCTION(this);
    /* Broadcast a RREP with TTL = 1 with the RREP message fields set as follows:
     *   Destination IP Address         The node's IP address.
     *   Destination Sequence Number    The node's latest sequence number.
     *   Hop Count                      0
     *   Lifetime                       AllowedHelloLoss * HelloInterval
     */
    for (auto j = m_socketAddresses.begin(); j != m_socketAddresses.end(); ++j)
    {
        Ptr<Socket> socket = j->first;
        Ipv4InterfaceAddress iface = j->second;
        RrepHeader helloHeader(/*prefixSize=*/0,
                               /*hopCount=*/0,
                               /*dst=*/iface.GetLocal(),
                               /*dstSeqNo=*/m_seqNo,
                               /*origin=*/iface.GetLocal(),
                               /*lifetime=*/Time(m_allowedHelloLoss * m_helloInterval));
        Ptr<Packet> packet = Create<Packet>();
        SocketIpTtlTag tag;
        tag.SetTtl(1);
        packet->AddPacketTag(tag);
        packet->AddHeader(helloHeader);
        TypeHeader tHeader(AOMDVTYPE_RREP);
        packet->AddHeader(tHeader);
        // Send to all-hosts broadcast if on /32 addr, subnet-directed otherwise
        Ipv4Address destination;
        if (iface.GetMask() == Ipv4Mask::GetOnes())
        {
            destination = Ipv4Address("255.255.255.255");
        }
        else
        {
            destination = iface.GetBroadcast();
        }
        Time jitter = MilliSeconds(m_uniformRandomVariable->GetInteger(0, 10));
        Simulator::Schedule(jitter, &RoutingProtocol::SendTo, this, socket, packet, destination);
    }
}

void
RoutingProtocol::SendPacketFromQueue(Ipv4Address dst, Ptr<Ipv4Route> route)
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
        header.SetTtl(header.GetTtl() +
                      1); // compensate extra TTL decrement by fake loopback routing
        ucb(route, p, header);
    }
}

Ptr<Socket>
RoutingProtocol::FindSocketWithInterfaceAddress(Ipv4InterfaceAddress addr) const
{
    NS_LOG_FUNCTION(this << addr);
    for (auto j = m_socketAddresses.begin(); j != m_socketAddresses.end(); ++j)
    {
        Ptr<Socket> socket = j->first;
        Ipv4InterfaceAddress iface = j->second;
        if (iface == addr)
        {
            return socket;
        }
    }
    Ptr<Socket> socket;
    return socket;
}

Ptr<Socket>
RoutingProtocol::FindSubnetBroadcastSocketWithInterfaceAddress(Ipv4InterfaceAddress addr) const
{
    NS_LOG_FUNCTION(this << addr);
    for (auto j = m_socketSubnetBroadcastAddresses.begin();
         j != m_socketSubnetBroadcastAddresses.end();
         ++j)
    {
        Ptr<Socket> socket = j->first;
        Ipv4InterfaceAddress iface = j->second;
        if (iface == addr)
        {
            return socket;
        }
    }
    Ptr<Socket> socket;
    return socket;
}

void
RoutingProtocol::UseHeadPath(RoutingTableEntry& rt, bool transit)
{
    AomdvPath* head = rt.PathFind();
    NS_ASSERT(head);
    head->expire = Simulator::Now() + m_activeRouteTimeout;
    if (transit)
    {
        rt.SetError(true);
    }
    rt.SyncFromPaths();
    m_routingTable.Update(rt);
}

void
RoutingProtocol::ForgetSearch(Ipv4Address dst)
{
    // ns-3 AODV deletes the entry after a failed discovery. AOMDV must not: the entry keeps the
    // destination sequence number, and forgetting it lets an old same-seqno advertisement be
    // accepted again as "newer", which breaks loop freedom (ns-2.35 never deletes entries).
    RoutingTableEntry e;
    if (m_routingTable.LookupRoute(dst, e))
    {
        e.Invalidate(m_deletePeriod);
        e.SetFlag(INVALID);
        e.SetHop(0); // the next discovery restarts the expanding ring search
        e.SetRreqCnt(0);
        m_routingTable.Update(e);
    }
}

void
RoutingProtocol::HandleLinkFailure(Ipv4Address nb)
{
    NS_LOG_FUNCTION(this << nb);
    // ns-2.35 nb_delete(): the neighbour set changed -> own seqno += 2
    m_seqNo += 2;
    // ns-2.35 handle_link_failure(): remove every path through nb
    RerrHeader out;
    for (const Ipv4Address& d : m_routingTable.GetDestinations())
    {
        RoutingTableEntry rt;
        if (!m_routingTable.LookupRoute(d, rt) || rt.GetFlag() != VALID ||
            rt.PathLookup(nb) == nullptr)
        {
            continue;
        }
        rt.PathDelete(nb);
        if (rt.PathEmpty())
        {
            rt.SetSeqNo(std::max(rt.GetSeqNo() + 1, rt.GetHighestSeqHeard()));
            if (rt.GetError())
            {
                if (!out.AddUnDestination(rt.GetDestination(), rt.GetSeqNo()))
                {
                    BroadcastRerr(out);
                    out.Clear();
                    out.AddUnDestination(rt.GetDestination(), rt.GetSeqNo());
                }
                rt.SetError(false);
            }
            rt.Invalidate(m_deletePeriod);
        }
        else
        {
            rt.SyncFromPaths();
        }
        m_routingTable.Update(rt);
    }
    if (out.GetDestCount() != 0)
    {
        BroadcastRerr(out);
    }
}

void
RoutingProtocol::BroadcastRerr(const RerrHeader& rerrHeader)
{
    NS_LOG_FUNCTION(this);
    // RERR rate limit kept identical to ns-3 AODV
    if (m_rerrCount == m_rerrRateLimit)
    {
        NS_ASSERT(m_rerrRateLimitTimer.IsRunning());
        NS_LOG_LOGIC("RerrRateLimit reached; suppressing RERR");
        return;
    }
    m_rerrCount++;
    for (auto i = m_socketAddresses.begin(); i != m_socketAddresses.end(); ++i)
    {
        Ptr<Packet> packet = Create<Packet>();
        SocketIpTtlTag tag;
        tag.SetTtl(1);
        packet->AddPacketTag(tag);
        packet->AddHeader(rerrHeader);
        packet->AddHeader(TypeHeader(AOMDVTYPE_RERR));
        Simulator::Schedule(MilliSeconds(m_uniformRandomVariable->GetInteger(0, 10)),
                            &RoutingProtocol::SendTo,
                            this,
                            i->first,
                            packet,
                            BroadcastAddress(i->second));
    }
}

void
RoutingProtocol::SendUnicastControl(Ptr<Packet> packet,
                                    Ipv4Address nextHop,
                                    Ipv4InterfaceAddress iface)
{
    NS_LOG_FUNCTION(this << nextHop);
    // ns-2 sends unicast control packets straight to the MAC next hop. The
    // equivalent here is an explicit one-hop route, so no route-table entry to
    // the neighbour is needed (ns-2 AOMDV keeps no such entries).
    int32_t ifIndex = m_ipv4->GetInterfaceForAddress(iface.GetLocal());
    NS_ASSERT(ifIndex >= 0);
    UdpHeader udp;
    udp.SetSourcePort(AOMDV_PORT);
    udp.SetDestinationPort(AOMDV_PORT);
    if (Node::ChecksumEnabled())
    {
        udp.EnableChecksums();
        udp.InitializeChecksum(iface.GetLocal(), nextHop, UdpL4Protocol::PROT_NUMBER);
    }
    packet->AddHeader(udp);
    SocketIpTtlTag tag;
    tag.SetTtl(1);
    packet->AddPacketTag(tag);
    Ptr<Ipv4Route> route = Create<Ipv4Route>();
    route->SetDestination(nextHop);
    route->SetGateway(nextHop);
    route->SetSource(iface.GetLocal());
    route->SetOutputDevice(m_ipv4->GetNetDevice(ifIndex));
    m_ipv4->Send(packet, iface.GetLocal(), nextHop, UdpL4Protocol::PROT_NUMBER, route);
}

void
RoutingProtocol::ServeQueue(Ipv4Address dst, const RoutingTableEntry& rt)
{
    if (rt.GetFlag() == VALID && m_queue.Find(dst))
    {
        SendPacketFromQueue(dst, rt.GetRoute());
    }
}

Ipv4Address
RoutingProtocol::BroadcastAddress(const Ipv4InterfaceAddress& iface) const
{
    // Send to all-hosts broadcast if on /32 addr, subnet-directed otherwise
    if (iface.GetMask() == Ipv4Mask::GetOnes())
    {
        return Ipv4Address("255.255.255.255");
    }
    return iface.GetBroadcast();
}

void
RoutingProtocol::DoInitialize()
{
    NS_LOG_FUNCTION(this);

    NS_ABORT_MSG_IF(m_ttlStart > m_netDiameter,
                    "AOMDV: configuration error, TtlStart ("
                        << m_ttlStart << ") must be less than or equal to NetDiameter ("
                        << m_netDiameter << ").");

    if (m_enableHello)
    {
        m_htimer.SetFunction(&RoutingProtocol::HelloTimerExpire, this);
        uint32_t startTime = m_uniformRandomVariable->GetInteger(0, 100);
        NS_LOG_DEBUG("Starting at time " << startTime << "ms");
        m_htimer.Schedule(MilliSeconds(startTime));
    }
    Ipv4RoutingProtocol::DoInitialize();
}

} // namespace aomdv
} // namespace ns3
