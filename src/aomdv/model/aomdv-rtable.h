/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/*
 * AOMDV Routing Table - Multipath extension
 * Supports multiple loop-free, link-disjoint paths per destination
 */
#ifndef AOMDV_RTABLE_H
#define AOMDV_RTABLE_H

#include "ns3/ipv4-routing-protocol.h"
#include "ns3/nstime.h"
#include "ns3/timer.h"
#include "ns3/simulator.h"
#include <vector>
#include <map>

namespace ns3 {
namespace aomdv {

/**
 * \brief Route states
 */
enum RouteFlags
{
  VALID         = 0,   //!< Valid route
  INVALID       = 1,   //!< Invalid (broken) route
  IN_SEARCH     = 2    //!< Route discovery in progress
};

/**
 * \brief A single path entry in the multipath route
 */
struct PathEntry
{
  Ipv4Address  nextHop;         ///< Next hop address for this path
  Ipv4Address  lastHop;         ///< Last hop (for link-disjoint check)
  uint8_t      hopCount;        ///< Number of hops for this path
  Time         expireTime;      ///< Expiry time for this path
  Time        discoveryTime;     ///< When this path was first discovered
  bool         valid;           ///< Is this path currently valid?

  PathEntry () :
    nextHop (Ipv4Address ()),
    lastHop (Ipv4Address ()),
    hopCount (0),
    expireTime (Seconds (0)),
    discoveryTime (Seconds (0)),
    valid (true)
  {}

  PathEntry (Ipv4Address nh, Ipv4Address lh, uint8_t hc, Time exp) :
    nextHop (nh), lastHop (lh), hopCount (hc), expireTime (exp), discoveryTime (Seconds (0)), valid (true)
  {}
};

/**
 * \brief AOMDV Routing Table Entry
 *
 * Each entry holds a list of valid paths (multipath) to a destination.
 * The advertised hop count ensures loop-freedom in AOMDV.
 */
class RoutingTableEntry
{
public:
  RoutingTableEntry (Ptr<NetDevice> dev = nullptr,
                     Ipv4Address dst = Ipv4Address (),
                     uint32_t seqNo = 0,
                     Ipv4InterfaceAddress iface = Ipv4InterfaceAddress (),
                     uint8_t advHopCount = 0,
                     RouteFlags flag = IN_SEARCH);

  ~RoutingTableEntry () {}

  // Destination management
  Ipv4Address GetDestination () const { return m_ipv4Route->GetDestination (); }
  Ptr<Ipv4Route> GetRoute () const { return m_ipv4Route; }

  // Sequence number
  void SetSeqNo (uint32_t sn) { m_seqNo = sn; }
  uint32_t GetSeqNo () const { return m_seqNo; }

  // Advertised hop count (AOMDV loop-freedom condition)
  void SetAdvertisedHopCount (uint8_t hc) { m_advHopCount = hc; }
  uint8_t GetAdvertisedHopCount () const { return m_advHopCount; }

  // Route flags
  void SetFlag (RouteFlags flag) { m_flag = flag; }
  RouteFlags GetFlag () const { return m_flag; }

  // Interface
  void SetInterface (Ipv4InterfaceAddress iface) { m_iface = iface; }
  Ipv4InterfaceAddress GetInterface () const { return m_iface; }

  // ---- Multipath management ----

  /**
   * Add a new path. Returns true if added (link-disjoint & not duplicate).
   */
  bool AddPath (Ipv4Address nextHop, Ipv4Address lastHop,
                uint8_t hopCount, Time lifetime);

  /**
   * Return number of valid paths.
   */
  uint32_t GetPathCount () const;

  /**
   * Get all valid paths.
   */
  std::vector<PathEntry> GetValidPaths () const;

  /**
   * Get the best path (smallest hop count). Used as primary route.
   */
  PathEntry GetBestPath () const;

  /**
   * Invalidate all paths via a given next hop (link failure).
   */
  void InvalidatePathVia (Ipv4Address nextHop);

  /**
   * Remove expired paths.
   */
  void PurgeExpiredPaths ();

  /**
   * True if at least one valid path exists.
   */
  bool HasValidPath () const;

  // Output interface (primary)
  void SetOutputDevice (Ptr<NetDevice> dev) { m_ipv4Route->SetOutputDevice (dev); }
  Ptr<NetDevice> GetOutputDevice () const { return m_ipv4Route->GetOutputDevice (); }

  // Timers for route discovery
  Time GetLifeTime () const;
  void SetLifeTime (Time lt);
  bool InvalidatePathsByNextHop (Ipv4Address nextHop);
  void UpdatePathLifetimeByNextHop (Ipv4Address nextHop, Time expiry);

  bool operator== (Ipv4Address const dst) const
  {
    return (m_ipv4Route->GetDestination () == dst);
  }

  void Print (std::ostream & os) const;

private:
  Ptr<Ipv4Route>           m_ipv4Route;
  uint32_t                 m_seqNo;
  uint8_t                  m_advHopCount;  ///< Advertised hop count at dst
  RouteFlags               m_flag;
  Ipv4InterfaceAddress     m_iface;
  std::vector<PathEntry>   m_paths;        ///< All known paths to destination
};

/**
 * \brief AOMDV Routing Table
 */
class RoutingTable
{
public:
  RoutingTable (Time deletePeriod);

  bool AddRoute (RoutingTableEntry & rt);
  bool DeleteRoute (Ipv4Address dst);
  bool LookupRoute (Ipv4Address dst, RoutingTableEntry & rt);
  bool LookupValidRoute (Ipv4Address dst, RoutingTableEntry & rt);
  bool Update (RoutingTableEntry & rt);
  bool SetEntryState (Ipv4Address dst, RouteFlags state);
  void GetListOfDestinationWithNextHop (Ipv4Address nextHop,
                                        std::map<Ipv4Address, uint32_t> & unreachable);
  void InvalidateRoutesWithNextHop (Ipv4Address nextHop,
                                    std::map<Ipv4Address, uint32_t> & unreachable);
  void DeleteAllRoutesFromInterface (Ipv4InterfaceAddress iface);
  void Clear () { m_ipv4AddressEntry.clear (); }
  void InvalidatePathsViaNeighbor (Ipv4Address neighbor, std::map<Ipv4Address, uint32_t> & unreachable);
  void UpdateSelectedPathLifetime (Ipv4Address dst, Ipv4Address nextHop, Time lt);
  void Purge ();
  void Print (Ptr<OutputStreamWrapper> stream) const;

private:
  std::map<Ipv4Address, RoutingTableEntry> m_ipv4AddressEntry;
  Time   m_deletePeriod;
};

} // namespace aomdv
} // namespace ns3

#endif /* AOMDV_RTABLE_H */
