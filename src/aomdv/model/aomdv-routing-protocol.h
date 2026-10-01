/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/*
 * AOMDV Routing Protocol for NS-3
 * Ad hoc On-demand Multipath Distance Vector Routing
 *
 * Reference: M. K. Marina and S. R. Das,
 *   "Ad hoc on-demand multipath distance vector routing,"
 *   Wireless Commun. Mob. Comput., vol. 6, no. 7, pp. 969-988, 2006.
 *
 * Implemented for FANET/UAV simulation - NS-3 v3.40
 */
#ifndef AOMDV_ROUTING_PROTOCOL_H
#define AOMDV_ROUTING_PROTOCOL_H

#include "aomdv-rtable.h"
#include "aomdv-packet.h"
#include "ns3/node.h"
#include "ns3/random-variable-stream.h"
#include "ns3/output-stream-wrapper.h"
#include "ns3/ipv4-routing-protocol.h"
#include "ns3/ipv4-interface.h"
#include "ns3/ipv4-l3-protocol.h"
#include "ns3/timer.h"
#include "ns3/traced-callback.h"
#include <map>
#include <set>

namespace ns3 {
namespace aomdv {

/**
 * \ingroup aomdv
 * \brief AOMDV routing protocol
 */
class RoutingProtocol : public Ipv4RoutingProtocol
{
public:
  static const uint32_t AOMDV_PORT;

  static TypeId GetTypeId ();
  RoutingProtocol ();
  virtual ~RoutingProtocol ();
  void DoDispose () override;

  // Ipv4RoutingProtocol interface
  Ptr<Ipv4Route> RouteOutput (Ptr<Packet> p, const Ipv4Header &header,
                               Ptr<NetDevice> oif,
                               Socket::SocketErrno &sockerr) override;
  bool RouteInput (Ptr<const Packet> p, const Ipv4Header &header,
                   Ptr<const NetDevice> idev,
                   const UnicastForwardCallback &ucb,
                   const MulticastForwardCallback &mcb,
                   const LocalDeliverCallback &lcb,
                   const ErrorCallback &ecb) override;
  void NotifyInterfaceUp (uint32_t interface) override;
  void NotifyInterfaceDown (uint32_t interface) override;
  void NotifyAddAddress (uint32_t interface, Ipv4InterfaceAddress address) override;
  void NotifyRemoveAddress (uint32_t interface, Ipv4InterfaceAddress address) override;
  void SetIpv4 (Ptr<Ipv4> ipv4) override;
  void PrintRoutingTable (Ptr<OutputStreamWrapper> stream, Time::Unit unit = Time::S) const override;

  // Configuration
  void SetBroadcastEnable (bool f) { m_enableBroadcast = f; }
  bool GetBroadcastEnable () const { return m_enableBroadcast; }

  int64_t AssignStreams (int64_t stream);

protected:
  void DoInitialize () override;

  /**
   * \brief Select next hop for routing.
   * In base AOMDV: deterministic selection (min hop count).
   * Overridden in PM-AOMDV for probabilistic selection.
   */
  virtual Ipv4Address SelectNextHop (const std::vector<PathEntry> & paths);

private:
  // ---- Parameters ----
  Time     m_activeRouteTimeout;
  Time     m_myRouteTimeout;
  Time     m_helloInterval;
  Time     m_allowedHelloLoss;
  Time     m_blackListTimeout;
  Time     m_nextHopWait;
  Time     m_pathDiscoveryTime;
  uint32_t m_rreqRetries;
  uint16_t m_ttlStart;
  uint16_t m_ttlIncrement;
  uint16_t m_ttlThreshold;
  uint16_t m_timeoutBuffer;
  uint16_t m_rreqRateLimit;
  uint16_t m_rerrRateLimit;
  uint32_t m_netDiameter;
  Time     m_nodeTraversalTime;
  Time     m_netTraversalTime;
  Time     m_rerrWaitTime;
  uint32_t m_maxPaths;         ///< Maximum number of paths per destination
  bool     m_enableBroadcast;

protected:
  Ptr<Ipv4> GetIpv4Ptr () const { return m_ipv4; }
private:
  // ---- State ----
  Ptr<Ipv4>             m_ipv4;
  Ptr<NetDevice>        m_lo;
  RoutingTable          m_routingTable;
  RoutingTable          m_addressReqQueue;
  std::map<Ipv4Address, Timer> m_addressReqTimer;

  // RREQ ID management
  uint32_t              m_rreqIdCache;
  std::map<std::pair<Ipv4Address, uint32_t>, Time> m_rreqSeenCache;

  // Sequence number
  uint32_t              m_seqNo;

  // Sockets
  std::map<Ptr<Socket>, Ipv4InterfaceAddress> m_socketAddresses;
  std::map<Ptr<Socket>, Ipv4InterfaceAddress> m_socketSubnetBroadcastAddresses;

  // Random streams
  Ptr<UniformRandomVariable> m_uniformRandomVariable;

  // Rate limiters
  uint16_t  m_rreqCount;
  std::map<Ipv4Address, Time> m_neighborLastHello; ///< Last Hello timestamp per neighbor
  uint16_t  m_rerrCount;

  // Hello management
  Timer     m_htimer;

  // ---- Internal Methods ----

  Ptr<Socket> FindSocketWithInterfaceAddress (Ipv4InterfaceAddress addr) const;
  Ptr<Socket> FindSubnetBroadcastSocketWithInterfaceAddress (Ipv4InterfaceAddress addr) const;

  void Start ();
  void SendRequest (Ipv4Address dst);
  void SendReply (RreqHeader const & rreqHeader, Ipv4InterfaceAddress toOrigin);
  void SendReplyByIntermediateNode (RoutingTableEntry & toDst,
                                    RoutingTableEntry & toOrigin,
                                    bool gratRep);
  void SendError (std::map<Ipv4Address, uint32_t> const & unreachable);
  void SendHello ();
  void SendPacketFromQueue (Ipv4Address dst, Ptr<Ipv4Route> route);

  void HelloTimerExpire ();
  void RreqRateLimitTimerExpire ();
  void RerrRateLimitTimerExpire ();

  void RecvAomdv (Ptr<Socket> socket);
  void RecvRequest (Ptr<Packet> p, Ipv4Address receiver, Ipv4Address src);
  void RecvReply (Ptr<Packet> p, Ipv4Address receiver, Ipv4Address sender);
  void RecvReplyAck (Ipv4Address neighbor);
  void RecvError (Ptr<Packet> p, Ipv4Address src);
  void ProcessHello (RrepHeader const & rrepHeader, Ipv4Address receiverIfaceAddr);

  void UpdateRouteLifetime (Ipv4Address addr, Time lt);
  void UpdateRouteToNeighbor (Ipv4Address neighbor, Ipv4Address receiver, Time lt);
  void LinkFailure (Ipv4Address neighbor); ///< Handle link failure: invalidate paths + RERR

  bool IsMyOwnAddress (Ipv4Address src);
  bool SeenRreq (Ipv4Address origin, uint32_t id);
  void MarkRreqSeen (Ipv4Address origin, uint32_t id);

  Ptr<Ipv4Route> LoopbackRoute (const Ipv4Header & hdr, Ptr<NetDevice> oif) const;
  bool UpdateRouteInput (Ipv4Address dst);
  void QueuePacket (Ptr<const Packet> p, const Ipv4Header & header,
                    UnicastForwardCallback ucb, ErrorCallback ecb);
  void DropPacketWithDst (Ipv4Address dst);
  Ptr<NetDevice> GetDeviceForIface (Ipv4InterfaceAddress iface) const;

  uint32_t GetNextRreqId () { return ++m_rreqIdCache; }
  uint32_t GetNextSeqNo () { return ++m_seqNo; }
};

} // namespace aomdv
} // namespace ns3

#endif /* AOMDV_ROUTING_PROTOCOL_H */
