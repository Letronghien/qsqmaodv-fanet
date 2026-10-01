/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/*
 * AOMDV Routing Protocol — Full Implementation for NS-3 v3.40
 * Based on: Marina & Das, Wireless Commun. Mob. Comput., 2006
 */

#include "aomdv-routing-protocol.h"
#include "aomdv-packet.h"
#include "ns3/log.h"
#include "ns3/boolean.h"
#include "ns3/random-variable-stream.h"
#include "ns3/inet-socket-address.h"
#include "ns3/trace-source-accessor.h"
#include "ns3/udp-socket-factory.h"
#include "ns3/wifi-net-device.h"
#include "ns3/adhoc-wifi-mac.h"
#include "ns3/string.h"
#include "ns3/pointer.h"
#include "ns3/uinteger.h"
#include "ns3/double.h"
#include "ns3/ipv4-routing-protocol.h"
#include "ns3/ipv4-route.h"
#include "ns3/ipv4.h"
#include "ns3/simulator.h"
#include "ns3/socket-factory.h"
#include "ns3/packet.h"
#include "ns3/node.h"
#include <algorithm>
#include <limits>

namespace ns3 {

NS_LOG_COMPONENT_DEFINE ("AomdvRoutingProtocol");

namespace aomdv {

NS_OBJECT_ENSURE_REGISTERED (RoutingProtocol);

const uint32_t RoutingProtocol::AOMDV_PORT = 654;

// ── Default parameters (match AODV RFC 3561 + AOMDV extension) ──────────────
static const Time   ACTIVE_ROUTE_TIMEOUT   = Seconds (3);
static const Time   MY_ROUTE_TIMEOUT       = Seconds (2 * 3);
static const Time   NODE_TRAVERSAL_TIME    = MilliSeconds (40);
static const Time   NET_TRAVERSAL_TIME     = Seconds (2 * 0.04 * 35);
static const Time   HELLO_INTERVAL         = Seconds (1);
static const Time   ALLOWED_HELLO_LOSS     = Seconds (3 * 1);
static const Time   PATH_DISCOVERY_TIME    = Seconds (2 * NET_TRAVERSAL_TIME.GetSeconds ());
static const Time   BLACKLIST_TIMEOUT      = Seconds (3 * NET_TRAVERSAL_TIME.GetSeconds ());
static const Time   NEXT_HOP_WAIT          = NODE_TRAVERSAL_TIME + MilliSeconds (10);
static const Time   DELETE_PERIOD          = Seconds (5 * ACTIVE_ROUTE_TIMEOUT.GetSeconds ());
static const uint32_t NET_DIAMETER        = 35;
static const uint16_t TTL_START           = 1;
static const uint16_t TTL_INCREMENT       = 2;
static const uint16_t TTL_THRESHOLD       = 7;
static const uint16_t MAX_REPAIR_TTL      = (uint16_t)(0.3 * NET_DIAMETER);
static const uint16_t LOCAL_ADD_TTL       = 2;
static const uint16_t TIMEOUT_BUFFER      = 2;
static const uint16_t RREQ_RETRIES        = 2;
static const uint16_t RREQ_RATELIMIT      = 10;
static const uint16_t RERR_RATELIMIT      = 10;

TypeId
RoutingProtocol::GetTypeId ()
{
  static TypeId tid = TypeId ("ns3::aomdv::RoutingProtocol")
    .SetParent<Ipv4RoutingProtocol> ()
    .SetGroupName ("Aomdv")
    .AddConstructor<RoutingProtocol> ()
    .AddAttribute ("EnableBroadcast",
                   "Indicates whether a broadcast data packets forwarding enable.",
                   BooleanValue (true),
                   MakeBooleanAccessor (&RoutingProtocol::m_enableBroadcast),
                   MakeBooleanChecker ())
    .AddAttribute ("HelloInterval",
                   "HELLO messages emission interval.",
                   TimeValue (HELLO_INTERVAL),
                   MakeTimeAccessor (&RoutingProtocol::m_helloInterval),
                   MakeTimeChecker ())
    .AddAttribute ("MaxPaths",
                   "Maximum number of parallel paths per destination.",
                   UintegerValue (10),
                   MakeUintegerAccessor (&RoutingProtocol::m_maxPaths),
                   MakeUintegerChecker<uint32_t> (1, 10))
    .AddAttribute ("ActiveRouteTimeout",
                   "Period of time during which the route is considered to be valid.",
                   TimeValue (ACTIVE_ROUTE_TIMEOUT),
                   MakeTimeAccessor (&RoutingProtocol::m_activeRouteTimeout),
                   MakeTimeChecker ())
  ;
  return tid;
}

RoutingProtocol::RoutingProtocol ()
  : m_activeRouteTimeout (ACTIVE_ROUTE_TIMEOUT),
    m_myRouteTimeout (MY_ROUTE_TIMEOUT),
    m_helloInterval (HELLO_INTERVAL),
    m_allowedHelloLoss (ALLOWED_HELLO_LOSS),
    m_blackListTimeout (BLACKLIST_TIMEOUT),
    m_nextHopWait (NEXT_HOP_WAIT),
    m_pathDiscoveryTime (PATH_DISCOVERY_TIME),
    m_rreqRetries (RREQ_RETRIES),
    m_ttlStart (TTL_START),
    m_ttlIncrement (TTL_INCREMENT),
    m_ttlThreshold (TTL_THRESHOLD),
    m_timeoutBuffer (TIMEOUT_BUFFER),
    m_rreqRateLimit (RREQ_RATELIMIT),
    m_rerrRateLimit (RERR_RATELIMIT),
    m_netDiameter (NET_DIAMETER),
    m_nodeTraversalTime (NODE_TRAVERSAL_TIME),
    m_netTraversalTime (NET_TRAVERSAL_TIME),
    m_rerrWaitTime (DELETE_PERIOD),
    m_maxPaths (10),
    m_enableBroadcast (true),
    m_routingTable (DELETE_PERIOD),
    m_addressReqQueue (DELETE_PERIOD),
    m_rreqIdCache (0),
    m_seqNo (0),
    m_rreqCount (0),
    m_rerrCount (0)
{
  m_uniformRandomVariable = CreateObject<UniformRandomVariable> ();
  m_htimer.SetFunction (&RoutingProtocol::HelloTimerExpire, this);
}

RoutingProtocol::~RoutingProtocol ()
{
}

void
RoutingProtocol::DoDispose ()
{
  m_ipv4 = nullptr;
  for (auto & kv : m_socketAddresses)
    kv.first->Close ();

  m_socketSubnetBroadcastAddresses.clear ();
  Ipv4RoutingProtocol::DoDispose ();
}

int64_t
RoutingProtocol::AssignStreams (int64_t stream)
{
  NS_LOG_FUNCTION (this << stream);
  m_uniformRandomVariable->SetStream (stream);
  return 1;
}

// ── Route output (called when node originates a packet) ─────────────────────
Ptr<Ipv4Route>
RoutingProtocol::RouteOutput (Ptr<Packet> p, const Ipv4Header & header,
                               Ptr<NetDevice> oif, Socket::SocketErrno & sockerr)
{
  NS_LOG_FUNCTION (this << header.GetDestination ());

  if (!p)
    {
      NS_LOG_DEBUG ("Packet is null");
      sockerr = Socket::ERROR_NOTERROR;
      return LoopbackRoute (header, oif);
    }

  if (IsMyOwnAddress (header.GetDestination ()))
    {
      sockerr = Socket::ERROR_NOTERROR;
      Ptr<Ipv4Route> rt = Create<Ipv4Route> ();
      rt->SetDestination (header.GetDestination ());
      rt->SetSource (header.GetDestination ());
      rt->SetGateway (Ipv4Address::GetLoopback ());
      rt->SetOutputDevice (m_lo);
      return rt;
    }

  RoutingTableEntry rt;
  bool rtFound = m_routingTable.LookupValidRoute (header.GetDestination (), rt);
  if (rtFound)
    {
      std::vector<PathEntry> paths = rt.GetValidPaths ();
      if (!paths.empty ())
        {
          Ipv4Address nh = SelectNextHop (paths);
          NS_LOG_DEBUG ("RouteOutput: FOUND route to " << header.GetDestination ()
                        << " via nextHop=" << nh << " paths=" << paths.size ());
          Ptr<Ipv4Route> route = rt.GetRoute ();
          route->SetGateway (nh);
          m_routingTable.UpdateSelectedPathLifetime (header.GetDestination (), nh, m_activeRouteTimeout.IsZero () ? Seconds (3.0) : m_activeRouteTimeout);
          UpdateRouteLifetime (nh, m_activeRouteTimeout.IsZero () ? Seconds (3.0) : m_activeRouteTimeout);
          sockerr = Socket::ERROR_NOTERROR;
          return route;
        }
    }

  // No valid route — initiate discovery
  sockerr = Socket::ERROR_NOROUTETOHOST;
  Ptr<Ipv4Route> route = LoopbackRoute (header, oif);
      NS_LOG_DEBUG ("RouteOutput: queuing pkt to " << header.GetDestination ());
  SendRequest (header.GetDestination ());
  return route;
}

// ── Route input (called for forwarded packets) ───────────────────────────────
bool
RoutingProtocol::RouteInput (Ptr<const Packet> p, const Ipv4Header & header,
                              Ptr<const NetDevice> idev,
                              const UnicastForwardCallback & ucb,
                              const MulticastForwardCallback & mcb,
                              const LocalDeliverCallback & lcb,
                              const ErrorCallback & ecb)
{
  NS_LOG_FUNCTION (this << header.GetDestination ());

  // Local delivery
  if (m_ipv4->IsDestinationAddress (header.GetDestination (), m_ipv4->GetInterfaceForDevice (idev)))
    {
      if (!lcb.IsNull ())
        {
          lcb (p, header, m_ipv4->GetInterfaceForDevice (idev));
          return true;
        }
      return false;
    }

  // Broadcast
  if (header.GetDestination ().IsBroadcast ())
    {
      if (m_enableBroadcast)
        {
          Ptr<Ipv4MulticastRoute> mrtentry;
          if (!mcb.IsNull ())
            mcb (mrtentry, p, header);
        }
      return true;
    }

  // Forward
  RoutingTableEntry rt;
  if (m_routingTable.LookupValidRoute (header.GetDestination (), rt))
    {
      std::vector<PathEntry> paths = rt.GetValidPaths ();
      if (!paths.empty ())
        {
          Ipv4Address nh = SelectNextHop (paths);
          Ptr<Ipv4Route> route = rt.GetRoute ();
          route->SetGateway (nh);
          m_routingTable.UpdateSelectedPathLifetime (header.GetDestination (), nh, m_activeRouteTimeout.IsZero () ? Seconds (3.0) : m_activeRouteTimeout);
          UpdateRouteLifetime (nh, m_activeRouteTimeout.IsZero () ? Seconds (3.0) : m_activeRouteTimeout);
          ucb (route, p, header);
          return true;
        }
    }

  return false;
}

// ── Default next hop selection: minimum hop count (AOMDV deterministic) ──────
Ipv4Address
RoutingProtocol::SelectNextHop (const std::vector<PathEntry> & paths)
{
  if (paths.empty ()) return Ipv4Address ();
  // Return path with minimum hop count
  auto it = std::min_element (paths.begin (), paths.end (),
                               [](const PathEntry & a, const PathEntry & b) {
                                 return a.hopCount < b.hopCount;
                               });
  return it->nextHop;
}

// ── Interface management ─────────────────────────────────────────────────────
void
RoutingProtocol::NotifyInterfaceUp (uint32_t i)
{
  NS_LOG_FUNCTION (this << m_ipv4->GetAddress (i, 0).GetLocal ());

  Ptr<Ipv4L3Protocol> l3 = m_ipv4->GetObject<Ipv4L3Protocol> ();

  for (uint32_t j = 0; j < m_ipv4->GetNAddresses (i); ++j)
    {
      Ipv4InterfaceAddress iface = m_ipv4->GetAddress (i, j);
      if (iface.GetLocal () == Ipv4Address::GetLoopback ()) continue;

      // Create socket for this interface
      Ptr<Socket> socket = Socket::CreateSocket (GetObject<Node> (),
                                                  UdpSocketFactory::GetTypeId ());
      NS_ASSERT (socket);
      socket->SetRecvCallback (MakeCallback (&RoutingProtocol::RecvAomdv, this));
      socket->BindToNetDevice (l3->GetNetDevice (i));
      socket->Bind (InetSocketAddress (iface.GetLocal (), AOMDV_PORT));
      socket->SetAllowBroadcast (true);
      socket->SetIpRecvTtl (true);
      m_socketAddresses[socket] = iface;

      // Broadcast socket
      Ptr<Socket> socket2 = Socket::CreateSocket (GetObject<Node> (),
                                                   UdpSocketFactory::GetTypeId ());
      NS_ASSERT (socket2);
      socket2->SetRecvCallback (MakeCallback (&RoutingProtocol::RecvAomdv, this));
      socket2->BindToNetDevice (l3->GetNetDevice (i));
      socket2->Bind (InetSocketAddress (iface.GetBroadcast (), AOMDV_PORT));
      socket2->SetAllowBroadcast (true);
      socket2->SetIpRecvTtl (true);
      m_socketSubnetBroadcastAddresses[socket2] = iface;
    }
}

void
RoutingProtocol::NotifyInterfaceDown (uint32_t i)
{
  NS_LOG_FUNCTION (this << m_ipv4->GetAddress (i, 0).GetLocal ());
  // Close sockets on this interface
  Ptr<Ipv4L3Protocol> l3 = m_ipv4->GetObject<Ipv4L3Protocol> ();
  Ptr<NetDevice> dev = l3->GetNetDevice (i);

  auto it = m_socketAddresses.begin ();
  while (it != m_socketAddresses.end ())
    {
      if (it->first->GetBoundNetDevice () == dev)
        {
          it->first->Close ();
          it = m_socketAddresses.erase (it);
        }
      else ++it;
    }

  // Invalidate routes via this interface
  for (uint32_t j = 0; j < m_ipv4->GetNAddresses (i); ++j)
    m_routingTable.DeleteAllRoutesFromInterface (m_ipv4->GetAddress (i, j));
}

void
RoutingProtocol::NotifyAddAddress (uint32_t i, Ipv4InterfaceAddress address)
{
  NS_LOG_FUNCTION (this << " interface " << i << " address " << address);
  if (m_ipv4->IsUp (i)) NotifyInterfaceUp (i);
}

void
RoutingProtocol::NotifyRemoveAddress (uint32_t i, Ipv4InterfaceAddress address)
{
  NS_LOG_FUNCTION (this);
  Ptr<Socket> socket = FindSocketWithInterfaceAddress (address);
  if (socket) m_socketAddresses.erase (socket);
}

void
RoutingProtocol::SetIpv4 (Ptr<Ipv4> ipv4)
{
  NS_ASSERT (ipv4);
  NS_ASSERT (!m_ipv4);
  m_ipv4 = ipv4;

  // Get loopback
  NS_ASSERT (m_ipv4->GetNInterfaces () == 1 &&
             m_ipv4->GetAddress (0, 0).GetLocal () == Ipv4Address::GetLoopback ());
  m_lo = m_ipv4->GetNetDevice (0);

  Simulator::ScheduleNow (&RoutingProtocol::Start, this);
}

void
RoutingProtocol::DoInitialize ()
{
  Ipv4RoutingProtocol::DoInitialize ();
}

void
RoutingProtocol::Start ()
{
  NS_LOG_FUNCTION (this);
  if (m_helloInterval != Seconds (0))
    {
      m_htimer.Schedule (MilliSeconds (
        m_uniformRandomVariable->GetInteger (0, 100)));
    }
}

// ── Packet reception ─────────────────────────────────────────────────────────
void
RoutingProtocol::RecvAomdv (Ptr<Socket> socket)
{
  Address sourceAddress;
  Ptr<Packet> packet = socket->RecvFrom (sourceAddress);
  InetSocketAddress inetSourceAddr = InetSocketAddress::ConvertFrom (sourceAddress);
  Ipv4Address sender   = inetSourceAddr.GetIpv4 ();
  Ipv4Address receiver;

  auto it1 = m_socketAddresses.find (socket);
  if (it1 != m_socketAddresses.end ())
    {
      receiver = it1->second.GetLocal ();
    }
  else
    {
      auto it2 = m_socketSubnetBroadcastAddresses.find (socket);
      if (it2 != m_socketSubnetBroadcastAddresses.end ())
        receiver = it2->second.GetLocal ();
      else
        {
          NS_LOG_WARN ("RecvAomdv: unknown socket");
          return;
        }
    }

  NS_LOG_DEBUG ("AOMDV node " << receiver << " recv from " << sender);

  uint8_t type;
  packet->CopyData (&type, 1);
  switch (type)
    {
    case AODVTYPE_RREQ:
      RecvRequest (packet, receiver, sender);
      break;
    case AODVTYPE_RREP:
      RecvReply (packet, receiver, sender);
      break;
    case AODVTYPE_RERR:
      RecvError (packet, sender);
      break;
    case AODVTYPE_RREP_ACK:
      RecvReplyAck (sender);
      break;
    default:
      NS_LOG_WARN ("Unknown AOMDV packet type " << (int)type);
    }
}

// ── RREQ: Route Request ───────────────────────────────────────────────────────
void
RoutingProtocol::SendRequest (Ipv4Address dst)
{
  NS_LOG_FUNCTION (this << dst);

  // Update own sequence number
  m_seqNo++;

  RreqHeader rreqHeader;
  rreqHeader.SetDst (dst);
  rreqHeader.SetId (GetNextRreqId ());
  rreqHeader.SetHopCount (0);
  rreqHeader.SetOrigin (m_ipv4->GetAddress (1, 0).GetLocal ());
  rreqHeader.SetOriginSeqno (m_seqNo);

  RoutingTableEntry rt;
  if (m_routingTable.LookupRoute (dst, rt))
    {
      rreqHeader.SetDstSeqno (rt.GetSeqNo ());
      rreqHeader.SetUnknownSeqno (false);
    }
  else
    {
      rreqHeader.SetUnknownSeqno (true);
      rreqHeader.SetDstSeqno (0);
    }

  Ipv4Address origin = rreqHeader.GetOrigin ();
  MarkRreqSeen (origin, rreqHeader.GetId ());

  // Broadcast RREQ on all interfaces
  for (auto & kv : m_socketAddresses)
    {
      Ptr<Socket> socket = kv.first;
      Ipv4InterfaceAddress iface = kv.second;

      rreqHeader.SetFirstHop (iface.GetLocal ());
      rreqHeader.SetLastHop (iface.GetLocal ());

      Ptr<Packet> packet = Create<Packet> ();
      packet->AddHeader (rreqHeader);

      Ipv4Address broadcastAddr = iface.GetBroadcast ();
      socket->SendTo (packet, 0,
                      InetSocketAddress (broadcastAddr, AOMDV_PORT));
      NS_LOG_DEBUG ("Send RREQ dst=" << dst << " id=" << rreqHeader.GetId ()
                    << " from " << iface.GetLocal ());
    }
}

void
RoutingProtocol::RecvRequest (Ptr<Packet> p, Ipv4Address receiver, Ipv4Address src)
{
  RreqHeader rreqHeader;
  p->RemoveHeader (rreqHeader);

  Ipv4Address origin = rreqHeader.GetOrigin ();
  uint32_t id        = rreqHeader.GetId ();

  // Drop if already seen
  if (SeenRreq (origin, id)) return;
  MarkRreqSeen (origin, id);

  rreqHeader.SetHopCount (rreqHeader.GetHopCount () + 1);
  rreqHeader.SetLastHop (receiver);

  // Update reverse route to origin
  Ipv4InterfaceAddress toOriginIface;
  for (auto & kv : m_socketAddresses)
    if (kv.second.GetLocal () == receiver) { toOriginIface = kv.second; break; }

  RoutingTableEntry toOrigin;
  if (!m_routingTable.LookupRoute (origin, toOrigin))
    {
      RoutingTableEntry newEntry (GetDeviceForIface (toOriginIface), origin, rreqHeader.GetOriginSeqno (),
                                  toOriginIface, 0, VALID);

      newEntry.AddPath (src, rreqHeader.GetFirstHop (),
                        rreqHeader.GetHopCount (),
                        Simulator::Now () + Seconds (3.0));
      bool addOk_ = m_routingTable.AddRoute (newEntry);
      NS_LOG_DEBUG ("RecvReply: AddRoute result=" << addOk_
                    << " numPaths=" << newEntry.GetValidPaths ().size ());
    }
  else
    {
      toOrigin.AddPath (src, rreqHeader.GetFirstHop (),
                        rreqHeader.GetHopCount (),
                        Simulator::Now () + Seconds (3.0));
      m_routingTable.Update (toOrigin);
    }

  // Neighbor route: install direct 1-hop route to RREQ sender so RouteOutput
  // can deliver RREP/forwarded packets without returning LoopbackRoute
  {
    RoutingTableEntry senderRoute (GetDeviceForIface (toOriginIface), src,
                                   0, toOriginIface, 0, VALID);
    senderRoute.AddPath (src, src, 1, Simulator::Now () + Seconds (3.0));
    if (!m_routingTable.AddRoute (senderRoute))
      m_routingTable.Update (senderRoute);
  }

  // Are we the destination?
  if (IsMyOwnAddress (rreqHeader.GetDst ()))
    {
      m_seqNo = std::max (m_seqNo, rreqHeader.GetDstSeqno ()) + 1;

      RoutingTableEntry toOriginFwd;
      m_routingTable.LookupRoute (origin, toOriginFwd);
      SendReply (rreqHeader, toOriginFwd.GetInterface ());
      return;
    }

  // Check if we have a valid route to dst
  RoutingTableEntry toDst;
  if (m_routingTable.LookupValidRoute (rreqHeader.GetDst (), toDst))
    {
      if (toDst.GetSeqNo () >= rreqHeader.GetDstSeqno () &&
          !rreqHeader.GetDestinationOnly ())
        {
          RoutingTableEntry toOriginFwd;
          m_routingTable.LookupRoute (origin, toOriginFwd);
          SendReplyByIntermediateNode (toDst, toOriginFwd,
                                       rreqHeader.GetGratiousRrep ());
          return;
        }
    }

  // Rebroadcast RREQ
  for (auto & kv : m_socketAddresses)
    {
      Ptr<Packet> fwd = Create<Packet> ();
      RreqHeader fwdHdr = rreqHeader;
      fwdHdr.SetFirstHop (kv.second.GetLocal ());
      fwd->AddHeader (fwdHdr);
      kv.first->SendTo (fwd, 0,
                        InetSocketAddress (kv.second.GetBroadcast (), AOMDV_PORT));
    }
}

// ── RREP: Route Reply ─────────────────────────────────────────────────────────
void
RoutingProtocol::SendReply (RreqHeader const & rreqHeader,
                             Ipv4InterfaceAddress toOrigin)
{
  RrepHeader rrepHeader;
  rrepHeader.SetDst (rreqHeader.GetDst ());
  rrepHeader.SetDstSeqno (m_seqNo);
  rrepHeader.SetOrigin (rreqHeader.GetOrigin ());
  rrepHeader.SetHopCount (0);
  rrepHeader.SetAdvertisedHopCount (0);
  rrepHeader.SetLifeTime (m_myRouteTimeout);

  Ptr<Socket> socket = FindSocketWithInterfaceAddress (toOrigin);
  if (!socket) return;

  Ptr<Packet> packet = Create<Packet> ();
  packet->AddHeader (rrepHeader);
  Ipv4Address nextHop = rreqHeader.GetOrigin ();
  RoutingTableEntry rt;
  if (m_routingTable.LookupRoute (nextHop, rt))
    {
      auto paths = rt.GetValidPaths ();
      if (!paths.empty ()) nextHop = paths[0].nextHop;
    }
socket->SendTo (packet, 0, InetSocketAddress (nextHop, AOMDV_PORT));
  NS_LOG_DEBUG ("Send RREP to origin=" << rreqHeader.GetOrigin ());
}

void
RoutingProtocol::SendReplyByIntermediateNode (RoutingTableEntry & toDst,
                                               RoutingTableEntry & toOrigin,
                                               bool gratRep)
{
  PathEntry best = toDst.GetBestPath ();

  RrepHeader rrepHeader;
  rrepHeader.SetDst (toDst.GetDestination ());
  rrepHeader.SetDstSeqno (toDst.GetSeqNo ());
  rrepHeader.SetOrigin (toOrigin.GetDestination ());
  rrepHeader.SetHopCount (best.hopCount);
  rrepHeader.SetAdvertisedHopCount (toDst.GetAdvertisedHopCount ());
  rrepHeader.SetLifeTime (best.expireTime - Simulator::Now ());

  auto originPaths = toOrigin.GetValidPaths ();
  if (originPaths.empty ()) return;
  Ipv4Address nextHopToOrigin = originPaths[0].nextHop;

  Ptr<Socket> socket = FindSocketWithInterfaceAddress (toDst.GetInterface ());
  if (!socket) return;

  Ptr<Packet> packet = Create<Packet> ();
  packet->AddHeader (rrepHeader);
  socket->SendTo (packet, 0, InetSocketAddress (nextHopToOrigin, AOMDV_PORT));
}

void
RoutingProtocol::RecvReply (Ptr<Packet> p, Ipv4Address receiver, Ipv4Address sender)
{
  NS_LOG_DEBUG ("RecvReply: called, checking origin");
  RrepHeader rrepHeader;
  p->RemoveHeader (rrepHeader);

  Ipv4Address dst    = rrepHeader.GetDst ();
  Ipv4Address origin = rrepHeader.GetOrigin ();

  NS_LOG_DEBUG ("RecvReply dst=" << dst << " hops=" << (int)rrepHeader.GetHopCount ());

  // Install/update forward route to dst
  RoutingTableEntry toDst;
  bool found = m_routingTable.LookupRoute (dst, toDst);

  Ipv4InterfaceAddress iface;
  for (auto & kv : m_socketAddresses)
    if (kv.second.GetLocal () == receiver) { iface = kv.second; break; }

  uint8_t hopCount = rrepHeader.GetHopCount () + 1;
  Time    lifetime = rrepHeader.GetLifeTime ();

  if (!found)
    {
      RoutingTableEntry newEntry (GetDeviceForIface (iface), dst, rrepHeader.GetDstSeqno (),
                                  iface,
                                  rrepHeader.GetAdvertisedHopCount (),
                                  VALID);
      newEntry.AddPath (sender, receiver, hopCount,
                        Simulator::Now () + lifetime);
      m_routingTable.AddRoute (newEntry);
    }
  else
    {
      // Update sequence number if newer
      if (rrepHeader.GetDstSeqno () >= toDst.GetSeqNo ())
        {
          toDst.SetSeqNo (rrepHeader.GetDstSeqno ());
          toDst.SetFlag (VALID);
          toDst.AddPath (sender, receiver, hopCount,
                         Simulator::Now () + lifetime);
          m_routingTable.Update (toDst);
        }
    }

  // Are we the origin?
  if (IsMyOwnAddress (origin))
    {
      // Send queued packets
      RoutingTableEntry rt;
      if (m_routingTable.LookupValidRoute (dst, rt))
        {
          auto paths = rt.GetValidPaths ();
          if (!paths.empty ())
            {
              Ipv4Address nh = SelectNextHop (paths);
              Ptr<Ipv4Route> route = rt.GetRoute ();
              route->SetGateway (nh);
              SendPacketFromQueue (dst, route);
            }
        }
      return;
    }

  // Forward RREP toward origin
  rrepHeader.SetHopCount (hopCount);
  RoutingTableEntry toOrigin;
  if (!m_routingTable.LookupRoute (origin, toOrigin)) return;
  auto paths = toOrigin.GetValidPaths ();
  if (paths.empty ()) return;

  Ipv4Address nextHop = SelectNextHop (paths);
  Ptr<Socket> socket  = FindSocketWithInterfaceAddress (iface);
  if (!socket) return;

  Ptr<Packet> fwd = Create<Packet> ();
  fwd->AddHeader (rrepHeader);
  socket->SendTo (fwd, 0, InetSocketAddress (nextHop, AOMDV_PORT));
}

void
RoutingProtocol::RecvReplyAck (Ipv4Address neighbor)
{
  NS_LOG_FUNCTION (this << neighbor);
}

// ── RERR: Route Error ─────────────────────────────────────────────────────────
void
RoutingProtocol::SendError (std::map<Ipv4Address, uint32_t> const & unreachable)
{
  if (unreachable.empty ()) return;
  if (m_rerrCount >= m_rerrRateLimit) return;
  m_rerrCount++;

  RerrHeader rerrHeader;
  for (auto & kv : unreachable)
    rerrHeader.AddUnDestination (kv.first, kv.second);

  Ptr<Packet> packet = Create<Packet> ();
  packet->AddHeader (rerrHeader);

  for (auto & kv : m_socketAddresses)
    {
      kv.first->SendTo (packet->Copy (), 0,
                        InetSocketAddress (kv.second.GetBroadcast (), AOMDV_PORT));
    }
}

void
RoutingProtocol::RecvError (Ptr<Packet> p, Ipv4Address src)
{
  RerrHeader rerrHeader;
  p->RemoveHeader (rerrHeader);

  std::map<Ipv4Address, uint32_t> unreachable;
  std::pair<Ipv4Address, uint32_t> un;

  while (rerrHeader.RemoveUnDestination (un))
    {
      RoutingTableEntry rt;
      if (m_routingTable.LookupRoute (un.first, rt))
        {
          rt.InvalidatePathVia (src);
          if (!rt.HasValidPath ())
            {
              rt.SetFlag (INVALID);
              unreachable[un.first] = un.second;
              m_routingTable.Update (rt);
            }
        }
    }

  if (!unreachable.empty ())
    SendError (unreachable);
}

// ── Hello messages ───────────────────────────────────────────────────────────
void
RoutingProtocol::SendHello ()
{
  RrepHeader helloHeader;
  for (auto & kv : m_socketAddresses)
    {
      Ipv4InterfaceAddress iface = kv.second;
      helloHeader.SetDst (iface.GetLocal ());
      helloHeader.SetDstSeqno (m_seqNo);
      helloHeader.SetOrigin (iface.GetLocal ());
      helloHeader.SetHopCount (0);
      helloHeader.SetLifeTime (m_helloInterval * m_allowedHelloLoss.GetSeconds());

      Ptr<Packet> packet = Create<Packet> ();
      packet->AddHeader (helloHeader);

      kv.first->SendTo (packet, 0,
                        InetSocketAddress (iface.GetBroadcast (), AOMDV_PORT));
    }
}

void
RoutingProtocol::HelloTimerExpire ()
{
  SendHello ();
  m_htimer.Cancel ();
  m_htimer.Schedule (m_helloInterval +
                     MilliSeconds (m_uniformRandomVariable->GetInteger (0, 100)));
}

void
RoutingProtocol::ProcessHello (RrepHeader const & rrepHeader, Ipv4Address recv)
{
  UpdateRouteToNeighbor (rrepHeader.GetDst (), recv,
                         rrepHeader.GetLifeTime ());
}

void
RoutingProtocol::RreqRateLimitTimerExpire ()
{
  m_rreqCount = 0;
}

void
RoutingProtocol::RerrRateLimitTimerExpire ()
{
  m_rerrCount = 0;
}

// ── Helpers ───────────────────────────────────────────────────────────────────
Ptr<Ipv4Route>
RoutingProtocol::LoopbackRoute (const Ipv4Header & hdr, Ptr<NetDevice> oif) const
{
  Ptr<Ipv4Route> rt = Create<Ipv4Route> ();
  rt->SetDestination (hdr.GetDestination ());
  rt->SetSource (hdr.GetSource ());
  rt->SetGateway (Ipv4Address::GetLoopback ());
  rt->SetOutputDevice (m_lo);
  return rt;
}

bool
RoutingProtocol::IsMyOwnAddress (Ipv4Address src)
{
  for (uint32_t i = 0; i < m_ipv4->GetNInterfaces (); ++i)
    for (uint32_t j = 0; j < m_ipv4->GetNAddresses (i); ++j)
      if (m_ipv4->GetAddress (i, j).GetLocal () == src) return true;
  return false;
}

bool
RoutingProtocol::SeenRreq (Ipv4Address origin, uint32_t id)
{
  auto key = std::make_pair (origin, id);
  auto it  = m_rreqSeenCache.find (key);
  if (it == m_rreqSeenCache.end ()) return false;
  return it->second > Simulator::Now ();
}

void
RoutingProtocol::MarkRreqSeen (Ipv4Address origin, uint32_t id)
{
  auto key = std::make_pair (origin, id);
  m_rreqSeenCache[key] = Simulator::Now () + m_pathDiscoveryTime;
}

Ptr<Socket>
RoutingProtocol::FindSocketWithInterfaceAddress (Ipv4InterfaceAddress addr) const
{
  for (auto & kv : m_socketAddresses)
    if (kv.second == addr) return kv.first;
  return nullptr;
}

Ptr<Socket>
RoutingProtocol::FindSubnetBroadcastSocketWithInterfaceAddress (Ipv4InterfaceAddress addr) const
{
  for (auto & kv : m_socketSubnetBroadcastAddresses)
    if (kv.second == addr) return kv.first;
  return nullptr;
}

void
RoutingProtocol::UpdateRouteLifetime (Ipv4Address addr, Time lt)
{
  RoutingTableEntry rt;
  if (m_routingTable.LookupRoute (addr, rt))
    {
      rt.SetLifeTime (lt);
      m_routingTable.Update (rt);
    }
}

void
RoutingProtocol::UpdateRouteToNeighbor (Ipv4Address neighbor,
                                         Ipv4Address receiver, Time lt)
{
  RoutingTableEntry rt;
  Ipv4InterfaceAddress iface;
  for (auto & kv : m_socketAddresses)
    if (kv.second.GetLocal () == receiver) { iface = kv.second; break; }

  if (!m_routingTable.LookupRoute (neighbor, rt))
    {
      RoutingTableEntry newEntry (GetDeviceForIface (iface), neighbor, 0, iface, 1, VALID);
      newEntry.AddPath (neighbor, receiver, 1, Simulator::Now () + lt);
      m_routingTable.AddRoute (newEntry);
    }
  else
    {
      if (m_maxPaths == 0 || rt.GetPathCount () < m_maxPaths)

        {
        rt.AddPath (neighbor, receiver, 1, Simulator::Now () + lt);
      m_routingTable.Update (rt);

        }
    }
}

void
RoutingProtocol::QueuePacket (Ptr<const Packet> p, const Ipv4Header & header,
                               UnicastForwardCallback ucb, ErrorCallback ecb)
{
  // Simple queue: just keep track that a discovery is in progress
  NS_LOG_DEBUG ("Queuing packet for " << header.GetDestination ());
}

void
RoutingProtocol::SendPacketFromQueue (Ipv4Address dst, Ptr<Ipv4Route> route)
{
  NS_LOG_DEBUG ("SendPacketFromQueue for " << dst);
}

void
RoutingProtocol::DropPacketWithDst (Ipv4Address dst)
{
  NS_LOG_DEBUG ("Drop queued packets for " << dst);
}

void
RoutingProtocol::PrintRoutingTable (Ptr<OutputStreamWrapper> stream,
                                     Time::Unit unit) const
{
  *stream->GetStream () << "AOMDV Routing table for node "
                        << m_ipv4->GetObject<Node> ()->GetId ()
                        << " at time " << Simulator::Now ().As (unit) << "\n";
  m_routingTable.Print (stream);
}


Ptr<NetDevice>
RoutingProtocol::GetDeviceForIface (Ipv4InterfaceAddress iface) const
{
  for (uint32_t i = 0; i < m_ipv4->GetNInterfaces (); ++i)
    for (uint32_t j = 0; j < m_ipv4->GetNAddresses (i); ++j)
      if (m_ipv4->GetAddress (i, j).GetLocal () == iface.GetLocal ())
        return m_ipv4->GetNetDevice (i);
  return m_lo;
}


void
RoutingProtocol::LinkFailure (Ipv4Address neighbor)
{
  NS_LOG_FUNCTION (this << neighbor);
  std::map<Ipv4Address, uint32_t> unreachable;
  m_routingTable.InvalidatePathsViaNeighbor (neighbor, unreachable);
  if (!unreachable.empty ())
    {
      SendError (unreachable);
    }
}

} // namespace aomdv
} // namespace n