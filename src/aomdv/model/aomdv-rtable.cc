/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/*
 * AOMDV Routing Table — Implementation
 */
#include "aomdv-rtable.h"
#include "ns3/log.h"
#include "ns3/simulator.h"
#include <algorithm>

namespace ns3 {

NS_LOG_COMPONENT_DEFINE ("AomdvRoutingTable");

namespace aomdv {

/*---------------------------------------------------------------------------
 * RoutingTableEntry
 *--------------------------------------------------------------------------*/

RoutingTableEntry::RoutingTableEntry (Ptr<NetDevice> dev,
                                      Ipv4Address dst,
                                      uint32_t seqNo,
                                      Ipv4InterfaceAddress iface,
                                      uint8_t advHopCount,
                                      RouteFlags flag)
  : m_seqNo (seqNo),
    m_advHopCount (advHopCount),
    m_flag (flag),
    m_iface (iface)
{
  m_ipv4Route = Create<Ipv4Route> ();
  m_ipv4Route->SetDestination (dst);
  m_ipv4Route->SetGateway (Ipv4Address ());
  m_ipv4Route->SetOutputDevice (dev);
  m_ipv4Route->SetSource (iface.GetLocal ());
}

bool
RoutingTableEntry::AddPath (Ipv4Address nextHop, Ipv4Address lastHop,
                            uint8_t hopCount, Time lifetime)
{
  // AOMDV loop-freedom: only accept path if hopCount < advertised hop count at dst
  // or advertised hop count not yet set (first path)
  if (m_advHopCount > 0 && hopCount >= m_advHopCount)
    {
      NS_LOG_DEBUG ("Reject path nh=" << nextHop
                    << " hc=" << (int)hopCount
                    << " advHC=" << (int)m_advHopCount);
      return false;
    }

  // Link-disjoint check: no two paths share the same next hop or last hop
  for (const auto & p : m_paths)
    {
      if (!p.valid) continue;
      if (p.nextHop == nextHop) return false;   // same next hop
      if (p.lastHop == lastHop && lastHop != Ipv4Address ()) return false; // same last hop
    }

  // Cap at reasonable max paths
  const uint32_t MAX_PATHS = 4;
  uint32_t validCount = GetPathCount ();
  if (validCount >= MAX_PATHS)
    {
      // Replace worst (highest hop count) path if new one is better
      uint8_t maxHops = 0;
      size_t  maxIdx  = 0;
      for (size_t i = 0; i < m_paths.size (); ++i)
        {
          if (m_paths[i].valid && m_paths[i].hopCount > maxHops)
            {
              maxHops = m_paths[i].hopCount;
              maxIdx  = i;
            }
        }
      if (hopCount < maxHops)
        {
          {
        PathEntry newPath (nextHop, lastHop, hopCount, lifetime);
        newPath.discoveryTime = Simulator::Now ();
        m_paths[maxIdx] = newPath;
      }
          return true;
        }
      return false;
    }

  {
    PathEntry newPath (nextHop, lastHop, hopCount, lifetime);
    newPath.discoveryTime = Simulator::Now ();
    m_paths.push_back (newPath);
  }

  // Update advertised hop count = max hop count among all paths
  if (hopCount > m_advHopCount)
    m_advHopCount = hopCount;

  // Set primary route gateway = best (min hop) path
  PathEntry best = GetBestPath ();
  m_ipv4Route->SetGateway (best.nextHop);

  NS_LOG_DEBUG ("Added path to " << m_ipv4Route->GetDestination ()
                << " via " << nextHop << " hops=" << (int)hopCount
                << " total_paths=" << GetPathCount ());
  return true;
}

uint32_t
RoutingTableEntry::GetPathCount () const
{
  uint32_t cnt = 0;
  for (const auto & p : m_paths)
    if (p.valid) ++cnt;
  return cnt;
}

std::vector<PathEntry>
RoutingTableEntry::GetValidPaths () const
{
  std::vector<PathEntry> result;
  Time now = Simulator::Now ();
  for (const auto & p : m_paths)
    {
      if (p.valid && p.expireTime > now)
        result.push_back (p);
    }
  return result;
}

PathEntry
RoutingTableEntry::GetBestPath () const
{
  PathEntry best;
  best.hopCount = 255;
  Time now = Simulator::Now ();
  for (const auto & p : m_paths)
    {
      if (p.valid && p.expireTime > now && p.hopCount < best.hopCount)
        best = p;
    }
  return best;
}

void
RoutingTableEntry::InvalidatePathVia (Ipv4Address nextHop)
{
  for (auto & p : m_paths)
    {
      if (p.nextHop == nextHop)
        {
          p.valid = false;
          NS_LOG_DEBUG ("Invalidated path via " << nextHop);
        }
    }
  // Update primary gateway
  if (HasValidPath ())
    m_ipv4Route->SetGateway (GetBestPath ().nextHop);
}

void
RoutingTableEntry::PurgeExpiredPaths ()
{
  Time now = Simulator::Now ();
  for (auto & p : m_paths)
    {
      if (p.valid && p.expireTime <= now)
        p.valid = false;
    }
}

bool
RoutingTableEntry::HasValidPath () const
{
  Time now = Simulator::Now ();
  for (const auto & p : m_paths)
    if (p.valid && p.expireTime > now) return true;
  return false;
}

Time
RoutingTableEntry::GetLifeTime () const
{
  Time maxExpire = Seconds (0);
  for (const auto & p : m_paths)
    if (p.valid && p.expireTime > maxExpire)
      maxExpire = p.expireTime;
  return maxExpire - Simulator::Now ();
}

void
RoutingTableEntry::SetLifeTime (Time lt)
{
  Time expiry = Simulator::Now () + lt;
  for (auto & p : m_paths)
    if (p.valid) p.expireTime = expiry;
}

void
RoutingTableEntry::Print (std::ostream & os) const
{
  os << "dst=" << m_ipv4Route->GetDestination ()
     << " seq=" << m_seqNo
     << " advHC=" << (int)m_advHopCount
     << " paths=" << GetPathCount ()
     << " flag=" << (m_flag == VALID ? "VALID" : m_flag == INVALID ? "INVALID" : "IN_SEARCH");
  for (const auto & p : m_paths)
    {
      if (p.valid)
        os << "\n    [nh=" << p.nextHop
           << " lh=" << p.lastHop
           << " hc=" << (int)p.hopCount << "]";
    }
}

/*---------------------------------------------------------------------------
 * RoutingTable
 *--------------------------------------------------------------------------*/

RoutingTable::RoutingTable (Time deletePeriod)
  : m_deletePeriod (deletePeriod)
{
}

bool
RoutingTable::AddRoute (RoutingTableEntry & rt)
{
  auto it = m_ipv4AddressEntry.find (rt.GetDestination ());
  if (it != m_ipv4AddressEntry.end ()) return false;
  m_ipv4AddressEntry[rt.GetDestination ()] = rt;
  return true;
}

bool
RoutingTable::DeleteRoute (Ipv4Address dst)
{
  return m_ipv4AddressEntry.erase (dst) > 0;
}

bool
RoutingTable::LookupRoute (Ipv4Address dst, RoutingTableEntry & rt)
{
  auto it = m_ipv4AddressEntry.find (dst);
  if (it == m_ipv4AddressEntry.end ()) return false;
  it->second.PurgeExpiredPaths ();
  rt = it->second;
  return true;
}

bool
RoutingTable::LookupValidRoute (Ipv4Address dst, RoutingTableEntry & rt)
{
  if (!LookupRoute (dst, rt)) return false;
  return (rt.GetFlag () == VALID && rt.HasValidPath ());
}

bool
RoutingTable::Update (RoutingTableEntry & rt)
{
  auto it = m_ipv4AddressEntry.find (rt.GetDestination ());
  if (it == m_ipv4AddressEntry.end ()) return false;
  it->second = rt;
  return true;
}

bool
RoutingTable::SetEntryState (Ipv4Address dst, RouteFlags state)
{
  auto it = m_ipv4AddressEntry.find (dst);
  if (it == m_ipv4AddressEntry.end ()) return false;
  it->second.SetFlag (state);
  return true;
}

void
RoutingTable::GetListOfDestinationWithNextHop (
    Ipv4Address nextHop,
    std::map<Ipv4Address, uint32_t> & unreachable)
{
  for (auto & kv : m_ipv4AddressEntry)
    {
      // Check if any path uses this next hop
      std::vector<PathEntry> paths = kv.second.GetValidPaths ();
      for (const auto & p : paths)
        {
          if (p.nextHop == nextHop)
            {
              unreachable[kv.first] = kv.second.GetSeqNo ();
              break;
            }
        }
    }
}

void
RoutingTable::InvalidateRoutesWithNextHop (
    Ipv4Address nextHop,
    std::map<Ipv4Address, uint32_t> & unreachable)
{
  for (auto & kv : m_ipv4AddressEntry)
    {
      kv.second.InvalidatePathVia (nextHop);
      if (!kv.second.HasValidPath () && kv.second.GetFlag () == VALID)
        {
          kv.second.SetFlag (INVALID);
          unreachable[kv.first] = kv.second.GetSeqNo ();
        }
    }
}

void
RoutingTable::DeleteAllRoutesFromInterface (Ipv4InterfaceAddress iface)
{
  auto it = m_ipv4AddressEntry.begin ();
  while (it != m_ipv4AddressEntry.end ())
    {
      if (it->second.GetInterface () == iface)
        it = m_ipv4AddressEntry.erase (it);
      else
        ++it;
    }
}

void
RoutingTable::Purge ()
{
  Time now = Simulator::Now ();
  auto it = m_ipv4AddressEntry.begin ();
  while (it != m_ipv4AddressEntry.end ())
    {
      it->second.PurgeExpiredPaths ();
      if (it->second.GetFlag () == INVALID &&
          it->second.GetLifeTime () < Seconds (0))
        it = m_ipv4AddressEntry.erase (it);
      else
        ++it;
    }
}

void
RoutingTable::Print (Ptr<OutputStreamWrapper> stream) const
{
  *stream->GetStream () << "\nAOMDV Routing Table:\n";
  for (const auto & kv : m_ipv4AddressEntry)
    {
      kv.second.Print (*stream->GetStream ());
      *stream->GetStream () << "\n";
    }
}


bool
RoutingTableEntry::InvalidatePathsByNextHop (Ipv4Address nextHop)
{
  bool any = false;
  for (auto & p : m_paths)
    {
      if (p.valid && p.nextHop == nextHop)
        {
          p.valid = false;
          any = true;
        }
    }
  return any;
}

void
RoutingTable::InvalidatePathsViaNeighbor (Ipv4Address neighbor,
                                           std::map<Ipv4Address, uint32_t> & unreachable)
{
  for (auto & kv : m_ipv4AddressEntry)
    {
      bool hadValid = kv.second.HasValidPath ();
      bool anyBad   = kv.second.InvalidatePathsByNextHop (neighbor);
      if (anyBad && hadValid && !kv.second.HasValidPath ())
        {
          unreachable[kv.first] = kv.second.GetSeqNo ();
        }
    }
}


void
RoutingTableEntry::UpdatePathLifetimeByNextHop (Ipv4Address nextHop, Time expiry)
{
  for (auto & p : m_paths)
    {
      if (p.valid && p.nextHop == nextHop)
        p.expireTime = expiry;
    }
}

void
RoutingTable::UpdateSelectedPathLifetime (Ipv4Address dst, Ipv4Address nextHop, Time lt)
{
  auto it = m_ipv4AddressEntry.find (dst);
  if (it == m_ipv4AddressEntry.end ()) return;
  Time expiry = Simulator::Now () + lt;
  it->second.UpdatePathLifetimeByNextHop (nextHop, expiry);
  it->second.SetLifeTime (lt);   // keep all paths alive; PM-AOMDV selects by LLT
}

} // namespace aomdv
} // namespace ns3
