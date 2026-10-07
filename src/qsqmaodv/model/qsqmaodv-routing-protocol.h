/*
 * Copyright (c) 2009 IITP RAS
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation;
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
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
 */
#ifndef QSQMAODV_ROUTING_PROTOCOL_H
#define QSQMAODV_ROUTING_PROTOCOL_H

#include "qsqmaodv-dpd.h"
#include "qsqmaodv-neighbor.h"
#include "qsqmaodv-packet.h"
#include "qsqmaodv-rqueue.h"
#include "qsqmaodv-rtable.h"
#include "qsqmaodv-qtable.h"

#include "ns3/ipv4-interface.h"
#include "ns3/ipv4-l3-protocol.h"
#include "ns3/ipv4-routing-protocol.h"
#include "ns3/node.h"
#include "ns3/output-stream-wrapper.h"
#include "ns3/random-variable-stream.h"

#include <map>
#include <set>
#include <tuple>

namespace ns3
{

class WifiMpdu;
class WifiMacQueue;
class Mac48Address;
enum WifiMacDropReason : uint8_t; // opaque enum declaration

namespace qsqmaodv
{
/**
 * \ingroup qsqmaodv
 *
 * \brief Multipath AODV core of PMAODV and QMAODV
 */
class RoutingProtocol : public Ipv4RoutingProtocol
{
  public:
    /**
     * \brief Get the type ID.
     * \return the object TypeId
     */
    static TypeId GetTypeId();
    static const uint32_t QSQMAODV_PORT;

    /// constructor
    RoutingProtocol();
    ~RoutingProtocol() override;
    void DoDispose() override;

    // Inherited from Ipv4RoutingProtocol
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

    // Handle protocol parameters
    /**
     * Get maximum queue time
     * \returns the maximum queue time
     */
    Time GetMaxQueueTime() const
    {
        return m_maxQueueTime;
    }

    /**
     * Set the maximum queue time
     * \param t the maximum queue time
     */
    void SetMaxQueueTime(Time t);

    /**
     * Get the maximum queue length
     * \returns the maximum queue length
     */
    uint32_t GetMaxQueueLen() const
    {
        return m_maxQueueLen;
    }

    /**
     * Set the maximum queue length
     * \param len the maximum queue length
     */
    void SetMaxQueueLen(uint32_t len);

    /**
     * Get destination only flag
     * \returns the destination only flag
     */
    bool GetDestinationOnlyFlag() const
    {
        return m_destinationOnly;
    }

    /**
     * Set destination only flag
     * \param f the destination only flag
     */
    void SetDestinationOnlyFlag(bool f)
    {
        m_destinationOnly = f;
    }

    /**
     * Get gratuitous reply flag
     * \returns the gratuitous reply flag
     */
    bool GetGratuitousReplyFlag() const
    {
        return m_gratuitousReply;
    }

    /**
     * Set gratuitous reply flag
     * \param f the gratuitous reply flag
     */
    void SetGratuitousReplyFlag(bool f)
    {
        m_gratuitousReply = f;
    }

    /**
     * Set hello enable
     * \param f the hello enable flag
     */
    void SetHelloEnable(bool f)
    {
        m_enableHello = f;
    }

    /**
     * Get hello enable flag
     * \returns the enable hello flag
     */
    bool GetHelloEnable() const
    {
        return m_enableHello;
    }

    /**
     * Set broadcast enable flag
     * \param f enable broadcast flag
     */
    void SetBroadcastEnable(bool f)
    {
        m_enableBroadcast = f;
    }

    /**
     * Get broadcast enable flag
     * \returns the broadcast enable flag
     */
    bool GetBroadcastEnable() const
    {
        return m_enableBroadcast;
    }

    /**
     * Assign a fixed random variable stream number to the random variables
     * used by this model.  Return the number of streams (possibly zero) that
     * have been assigned.
     *
     * \param stream first stream index to use
     * \return the number of stream indices assigned by this model
     */
    int64_t AssignStreams(int64_t stream);

    /// QS-QMAODV run diagnostics (read by the simulation program)
    struct QsStats
    {
        uint64_t decisions{0};  ///< next-hop decisions for data packets (source + forwarders)
        double nodeQSum{0.0};   ///< sum of node-level MAC BE occupancy at decisions
        double nodeQMax{0.0};
        uint64_t nodeQPos{0};   ///< decisions with node-level occupancy > 0
        double nhQSum{0.0};     ///< sum of chosen-next-hop occupancy q_n
        uint64_t nhQPos{0};     ///< decisions with q_n > 0
        uint64_t fbAck{0};      ///< MAC ACK feedbacks applied
        uint64_t fbDrop{0};     ///< MAC drop feedbacks applied
        uint64_t decayed{0};    ///< Q entries shrunk by ACK-silence decay
    };
    const QsStats& GetQsStats() const { return m_qsStats; }

  protected:
    void DoInitialize() override;

  private:
    /**
     * Set the maximum number of routes per destination.
     * \param mp maximum number of routes
     */
    void SetMaxPaths(uint32_t mp);
    /**
     * \return the maximum number of routes per destination
     */
    uint32_t GetMaxPaths() const;
    /// Periodic decay of the exploration rate.
    void EpsilonDecayTick();
    /**
     * \param p packet
     * \return true for routing control packets
     */
    bool IsControlPacket(Ptr<const Packet> p) const;
    /**
     * \param mpduPacket packet of an MPDU (starting with the LLC/SNAP header)
     * \return true if the frame carries application data
     */
    static bool IsDataFrame(Ptr<const Packet> mpduPacket);
    /**
     * Notify that an MPDU was dropped.
     *
     * \param reason the reason why the MPDU was dropped
     * \param mpdu the dropped MPDU
     */
    void NotifyTxError(WifiMacDropReason reason, Ptr<const WifiMpdu> mpdu);
    /**
     * Notify that an MPDU was acknowledged.
     * \param mpdu the acknowledged MPDU
     */
    void NotifyTxAcked(Ptr<const WifiMpdu> mpdu);
    /**
     * Q-update from the MAC outcome of a data MPDU.
     * \param mpdu the MPDU
     * \param acked true if acknowledged
     */
    void MacFeedback(Ptr<const WifiMpdu> mpdu, bool acked);

    // Protocol parameters.
    uint32_t m_rreqRetries; ///< Maximum number of retransmissions of RREQ with TTL = NetDiameter to
                            ///< discover a route
    uint16_t m_ttlStart;    ///< Initial TTL value for RREQ.
    uint16_t m_ttlIncrement; ///< TTL increment for each attempt using the expanding ring search for
                             ///< RREQ dissemination.
    uint16_t m_ttlThreshold; ///< Maximum TTL value for expanding ring search, TTL = NetDiameter is
                             ///< used beyond this value.
    uint16_t m_timeoutBuffer;  ///< Provide a buffer for the timeout.
    uint16_t m_rreqRateLimit;  ///< Maximum number of RREQ per second.
    uint16_t m_rerrRateLimit;  ///< Maximum number of REER per second.
    Time m_activeRouteTimeout; ///< Period of time during which the route is considered to be valid.
    uint32_t m_netDiameter; ///< Net diameter measures the maximum possible number of hops between
                            ///< two nodes in the network
    /**
     * NodeTraversalTime is a conservative estimate of the average one hop traversal time for
     * packets and should include queuing delays, interrupt processing times and transfer times.
     */
    Time m_nodeTraversalTime;
    Time m_netTraversalTime;  ///< Estimate of the average net traversal time.
    Time m_pathDiscoveryTime; ///< Estimate of maximum time needed to find route in network.
    Time m_myRouteTimeout;    ///< Value of lifetime field in RREP generating by this node.
    /**
     * Every HelloInterval the node checks whether it has sent a broadcast  within the last
     * HelloInterval. If it has not, it MAY broadcast a  Hello message
     */
    Time m_helloInterval;
    uint32_t m_allowedHelloLoss; ///< Number of hello messages which may be loss for valid link
    /**
     * DeletePeriod is intended to provide an upper bound on the time for which an upstream node A
     * can have a neighbor B as an active next hop for destination D, while B has invalidated the
     * route to D.
     */
    Time m_deletePeriod;
    Time m_nextHopWait;      ///< Period of our waiting for the neighbour's RREP_ACK
    Time m_blackListTimeout; ///< Time for which the node is put into the blacklist
    uint32_t m_maxQueueLen;  ///< The maximum number of packets that we allow a routing protocol to
                             ///< buffer.
    Time m_maxQueueTime;     ///< The maximum period of time that a routing protocol is allowed to
                             ///< buffer a packet for.
    bool m_destinationOnly;  ///< Indicates only the destination may respond to this RREQ.
    bool m_gratuitousReply;  ///< Indicates whether a gratuitous RREP should be unicast to the node
                             ///< originated route discovery.
    bool m_enableHello;      ///< Indicates whether a hello messages enable
    bool m_enableBroadcast;  ///< Indicates whether a a broadcast data packets forwarding enable

    /// IP protocol
    Ptr<Ipv4> m_ipv4;
    /// Raw unicast socket per each IP interface, map socket -> iface address (IP + mask)
    std::map<Ptr<Socket>, Ipv4InterfaceAddress> m_socketAddresses;
    /// Raw subnet directed broadcast socket per each IP interface, map socket -> iface address (IP
    /// + mask)
    std::map<Ptr<Socket>, Ipv4InterfaceAddress> m_socketSubnetBroadcastAddresses;
    /// Loopback device used to defer RREQ until packet will be fully formed
    Ptr<NetDevice> m_lo;

    /// Routing table
    RoutingTable m_routingTable;
    /// Multipath Q-table
    QTable m_qtable;
    std::string m_policy{"QLearning"};         ///< next-hop selection: QLearning or Probabilistic
    uint32_t m_maxPaths{3};                    ///< maximum number of routes per destination
    double m_alpha0{0.5};                      ///< (initial) learning rate
    double m_gamma{0.9};                       ///< discount factor
    double m_epsilon0{0.5};                    ///< initial exploration rate
    double m_w1{0.6};                          ///< reward weight of the MAC ACK
    double m_w2{0.4};                          ///< reward weight of the one-hop delay
    Time m_epsilonDecayInterval{Seconds(10.0)}; ///< period of the exploration decay
    EventId m_epsilonDecayEvent;              ///< exploration decay tick

    // ================= QS-QMAODV: queue-state extensions =====================
    // Every feature is an attribute; HopByHop=true and all queue features off == QMAODV.
    bool m_hopByHop{false};          ///< HopByHop (QMAODV: true)
    double m_qsWq{0.10};             ///< QueueRewardWeight
    double m_qsFailPenalty{0.5};     ///< FailurePenalty
    bool m_qsQueueSelect{true};      ///< QueueAwareSelect
    double m_qsBeta{0.5};            ///< QueueSelectBeta
    bool m_qsQueueExplore{true};     ///< QueueDrivenExploration
    double m_qsExploreFloor{0.01};   ///< ExplorationFloor
    bool m_qsDecay{true};            ///< AckSilenceDecay
    Time m_qsDecayThreshold{Seconds(15)};
    double m_qsDecayFactor{0.92};
    uint32_t m_qsDecayMinTx{3};
    Time m_qsDecayInterval{Seconds(10)};
    double m_qsQueueRef{20.0};       ///< QueueRefPackets (0 = MAC queue MaxSize)
    EventId m_qsDecayEvent;
    QsStats m_qsStats;
    Ptr<WifiMacQueue> GetBeQueue() const;
    bool IsQosMac() const;
    bool LookupMac(Ipv4Address ip, Mac48Address& mac) const;
    double GetNodeQueueOccupancy() const;                 ///< |Q_BE| / Q_ref
    double GetNextHopQueueOccupancy(Ipv4Address nh) const; ///< |Q_BE(nh)| / Q_ref
    double QsOnDecision(Ipv4Address nextHop);             ///< diagnostics; returns q_n
    void QsDecayTick();
    /// destination replies sent per (origin, RREQ id)
    std::map<std::pair<uint32_t, uint32_t>, uint32_t> m_mcastRrepSent;
    /// RREPs already forwarded per (origin, destination, destination sequence number)
    std::set<std::tuple<uint32_t, uint32_t, uint32_t>> m_rrepForwarded;
    /// A "drop-front" queue used by the routing layer to buffer packets to which it does not have a
    /// route.
    RequestQueue m_queue;
    /// Broadcast ID
    uint32_t m_requestId;
    /// Request sequence number
    uint32_t m_seqNo;
    /// Handle duplicated RREQ
    IdCache m_rreqIdCache;
    /// Handle duplicated broadcast/multicast packets
    DuplicatePacketDetection m_dpd;
    /// Handle neighbors
    Neighbors m_nb;
    /// Number of RREQs used for RREQ rate control
    uint16_t m_rreqCount;
    /// Number of RERRs used for RERR rate control
    uint16_t m_rerrCount;

  private:
    /// Start protocol operation
    void Start();
    /**
     * Queue packet and send route request
     *
     * \param p the packet to route
     * \param header the IP header
     * \param ucb the UnicastForwardCallback function
     * \param ecb the ErrorCallback function
     */
    void DeferredRouteOutput(Ptr<const Packet> p,
                             const Ipv4Header& header,
                             UnicastForwardCallback ucb,
                             ErrorCallback ecb);
    /**
     * If route exists and is valid, forward packet.
     *
     * \param p the packet to route
     * \param header the IP header
     * \param ucb the UnicastForwardCallback function
     * \param ecb the ErrorCallback function
     * \returns true if forwarded
     */
    bool Forwarding(Ptr<const Packet> p,
                    const Ipv4Header& header,
                    UnicastForwardCallback ucb,
                    ErrorCallback ecb);
    /**
     * Repeated attempts by a source node at route discovery for a single destination
     * use the expanding ring search technique.
     * \param dst the destination IP address
     */
    void ScheduleRreqRetry(Ipv4Address dst);
    /**
     * Set lifetime field in routing table entry to the maximum of existing lifetime and lt, if the
     * entry exists
     * \param addr destination address
     * \param lt proposed time for lifetime field in routing table entry for destination with
     * address addr.
     * \return true if route to destination address addr exist
     */
    bool UpdateRouteLifeTime(Ipv4Address addr, Time lt);
    /**
     * Update neighbor record.
     * \param receiver is supposed to be my interface
     * \param sender is supposed to be IP address of my neighbor.
     */
    void UpdateRouteToNeighbor(Ipv4Address sender, Ipv4Address receiver);
    /**
     * Test whether the provided address is assigned to an interface on this node
     * \param src the source IP address
     * \returns true if the IP address is the node's IP address
     */
    bool IsMyOwnAddress(Ipv4Address src);
    /**
     * Find unicast socket with local interface address iface
     *
     * \param iface the interface
     * \returns the socket associated with the interface
     */
    Ptr<Socket> FindSocketWithInterfaceAddress(Ipv4InterfaceAddress iface) const;
    /**
     * Find subnet directed broadcast socket with local interface address iface
     *
     * \param iface the interface
     * \returns the socket associated with the interface
     */
    Ptr<Socket> FindSubnetBroadcastSocketWithInterfaceAddress(Ipv4InterfaceAddress iface) const;
    /**
     * Process hello message
     *
     * \param rrepHeader RREP message header
     * \param receiverIfaceAddr receiver interface IP address
     */
    void ProcessHello(const RrepHeader& rrepHeader, Ipv4Address receiverIfaceAddr);
    /**
     * Create loopback route for given header
     *
     * \param header the IP header
     * \param oif the output interface net device
     * \returns the route
     */
    Ptr<Ipv4Route> LoopbackRoute(const Ipv4Header& header, Ptr<NetDevice> oif) const;

    /**
     * \name Receive control packets
     * @{
     */
    /**
     * Receive and process control packet
     * \param socket input socket
     */
    void RecvQsqmaodv(Ptr<Socket> socket);
    /**
     * Receive RREQ
     * \param p packet
     * \param receiver receiver address
     * \param src sender address
     */
    void RecvRequest(Ptr<Packet> p, Ipv4Address receiver, Ipv4Address src);
    /**
     * Receive RREP
     * \param p packet
     * \param my destination address
     * \param src sender address
     */
    void RecvReply(Ptr<Packet> p, Ipv4Address my, Ipv4Address src);
    /**
     * Receive RREP_ACK
     * \param neighbor neighbor address
     */
    void RecvReplyAck(Ipv4Address neighbor);
    /**
     * Receive RERR
     * \param p packet
     * \param src sender address
     */
    /// Receive  from node with address src
    void RecvError(Ptr<Packet> p, Ipv4Address src);
    /** @} */

    /**
     * \name Send
     * @{
     */
    /** Forward packet from route request queue
     * \param dst destination address
     * \param route route to use
     */
    void SendPacketFromQueue(Ipv4Address dst, Ptr<Ipv4Route> route);
    /// Send hello
    void SendHello();
    /** Send RREQ
     * \param dst destination address
     */
    void SendRequest(Ipv4Address dst);
    /** Send RREP
     * \param rreqHeader route request header
     * \param toOrigin routing table entry to originator
     */
    void SendReply(const RreqHeader& rreqHeader, const RoutingTableEntry& toOrigin);
    /** Send RREP by intermediate node
     * \param toDst routing table entry to destination
     * \param toOrigin routing table entry to originator
     * \param gratRep indicates whether a gratuitous RREP should be unicast to destination
     */
    void SendReplyByIntermediateNode(RoutingTableEntry& toDst,
                                     RoutingTableEntry& toOrigin,
                                     bool gratRep);
    /** Send RREP_ACK
     * \param neighbor neighbor address
     */
    void SendReplyAck(Ipv4Address neighbor);
    /** Initiate RERR
     * \param nextHop next hop address
     */
    void SendRerrWhenBreaksLinkToNextHop(Ipv4Address nextHop);
    /** Forward RERR
     * \param packet packet
     * \param precursors list of addresses of the visited nodes
     */
    void SendRerrMessage(Ptr<Packet> packet, std::vector<Ipv4Address> precursors);
    /**
     * Send RERR message when no route to forward input packet. Unicast if there is reverse route to
     * originating node, broadcast otherwise.
     * \param dst destination node IP address
     * \param dstSeqNo destination node sequence number
     * \param origin originating node IP address
     */
    void SendRerrWhenNoRouteToForward(Ipv4Address dst, uint32_t dstSeqNo, Ipv4Address origin);
    /** @} */

    /**
     * Send packet to destination socket
     * \param socket destination node socket
     * \param packet packet to send
     * \param destination destination node IP address
     */
    void SendTo(Ptr<Socket> socket, Ptr<Packet> packet, Ipv4Address destination);

    /// Hello timer
    Timer m_htimer;
    /// Schedule next send of hello message
    void HelloTimerExpire();
    /// RREQ rate limit timer
    Timer m_rreqRateLimitTimer;
    /// Reset RREQ count and schedule RREQ rate limit timer with delay 1 sec.
    void RreqRateLimitTimerExpire();
    /// RERR rate limit timer
    Timer m_rerrRateLimitTimer;
    /// Reset RERR count and schedule RERR rate limit timer with delay 1 sec.
    void RerrRateLimitTimerExpire();
    /// Map IP address + RREQ timer.
    std::map<Ipv4Address, Timer> m_addressReqTimer;
    /**
     * Handle route discovery process
     * \param dst the destination IP address
     */
    void RouteRequestTimerExpire(Ipv4Address dst);
    /**
     * Mark link to neighbor node as unidirectional for blacklistTimeout
     *
     * \param neighbor the IP address of the neighbor node
     * \param blacklistTimeout the black list timeout time
     */
    void AckTimerExpire(Ipv4Address neighbor, Time blacklistTimeout);

    /// Provides uniform random variables.
    Ptr<UniformRandomVariable> m_uniformRandomVariable;
    /// Keep track of the last bcast time
    Time m_lastBcastTime;
};

} // namespace qsqmaodv
} // namespace ns3

#endif /* QSQMAODV_ROUTING_PROTOCOL_H */
