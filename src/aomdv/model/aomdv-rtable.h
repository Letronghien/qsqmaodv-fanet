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
 */
#ifndef AOMDV_RTABLE_H
#define AOMDV_RTABLE_H

#include "ns3/ipv4-route.h"
#include "ns3/ipv4.h"
#include "ns3/net-device.h"
#include "ns3/output-stream-wrapper.h"
#include "ns3/timer.h"

#include <cassert>
#include <map>
#include <vector>
#include <stdint.h>
#include <sys/types.h>

namespace ns3
{
namespace aomdv
{

/**
 * @ingroup aomdv
 * @brief Route record states
 */
/// Advertised-hop-count "infinity" (ns-2 AOMDV INFINITY2)
static constexpr uint16_t AOMDV_INFINITY = 0xff;

/**
 * @ingroup aomdv
 * @brief One path of a multipath route entry (ns-2.35 AOMDV_Path).
 */
struct AomdvPath
{
    Ipv4Address nextHop;   ///< next hop of this path
    Ipv4Address lastHop;   ///< last hop of this path (node-disjointness key)
    uint16_t hopCount;     ///< hop count of this path
    Time expire;           ///< absolute expiry time of this path
};

enum RouteFlags
{
    VALID = 0,     //!< VALID
    INVALID = 1,   //!< INVALID
    IN_SEARCH = 2, //!< IN_SEARCH
};

/**
 * @ingroup aomdv
 * @brief Routing table entry
 */
class RoutingTableEntry
{
  public:
    /**
     * constructor
     *
     * @param dev the device
     * @param dst the destination IP address
     * @param vSeqNo verify sequence number flag
     * @param seqNo the sequence number
     * @param iface the interface
     * @param hops the number of hops
     * @param nextHop the IP address of the next hop
     * @param lifetime the lifetime of the entry
     */
    RoutingTableEntry(Ptr<NetDevice> dev = nullptr,
                      Ipv4Address dst = Ipv4Address(),
                      bool vSeqNo = false,
                      uint32_t seqNo = 0,
                      Ipv4InterfaceAddress iface = Ipv4InterfaceAddress(),
                      uint16_t hops = 0,
                      Ipv4Address nextHop = Ipv4Address(),
                      Time lifetime = Simulator::Now());

    ~RoutingTableEntry();

    /// @name Precursors management
    //\{
    /**
     * Insert precursor in precursor list if it doesn't yet exist in the list
     * @param id precursor address
     * @return true on success
     */
    bool InsertPrecursor(Ipv4Address id);
    /**
     * Lookup precursor by address
     * @param id precursor address
     * @return true on success
     */
    bool LookupPrecursor(Ipv4Address id);
    /**
     * @brief Delete precursor
     * @param id precursor address
     * @return true on success
     */
    bool DeletePrecursor(Ipv4Address id);
    /// Delete all precursors
    void DeleteAllPrecursors();
    /**
     * Check that precursor list is empty
     * @return true if precursor list is empty
     */
    bool IsPrecursorListEmpty() const;
    /**
     * Inserts precursors in output parameter prec if they do not yet exist in vector
     * @param prec vector of precursor addresses
     */
    void GetPrecursors(std::vector<Ipv4Address>& prec) const;
    //\}

    /**
     * Mark entry as "down" (i.e. disable it)
     * @param badLinkLifetime duration to keep entry marked as invalid
     */
    void Invalidate(Time badLinkLifetime);

    /// @name AOMDV path list (ns-2.35 aomdv_rt_entry::path_*)
    //\{
    /**
     * Append a path at the END of the list (head = earliest inserted).
     * @param nextHop next hop
     * @param hopCount hop count
     * @param expire absolute expiry time
     * @param lastHop last hop
     * @return pointer to the new path
     */
    AomdvPath* PathInsert(Ipv4Address nextHop, uint16_t hopCount, Time expire, Ipv4Address lastHop);
    /**
     * @param nextHop next hop
     * @return first path with this next hop, or nullptr
     */
    AomdvPath* PathLookup(Ipv4Address nextHop);
    /**
     * @param nextHop next hop
     * @param lastHop last hop
     * @return path with this (next hop, last hop) pair, or nullptr
     */
    AomdvPath* DisjointPathLookup(Ipv4Address nextHop, Ipv4Address lastHop);
    /**
     * @param nextHop next hop
     * @param lastHop last hop
     * @return true if no path has this next hop OR this last hop
     */
    bool NewDisjointPath(Ipv4Address nextHop, Ipv4Address lastHop) const;
    /**
     * @param lastHop last hop
     * @return first path with this last hop, or nullptr
     */
    AomdvPath* PathLookupLastHop(Ipv4Address lastHop);
    /**
     * Delete the first path with this next hop.
     * @param nextHop next hop
     */
    void PathDelete(Ipv4Address nextHop);
    /// Delete all paths
    void PathDeleteAll();
    /// Delete all expired paths
    void PathPurge();
    /// @return true if there is no path
    bool PathEmpty() const
    {
        return m_paths.empty();
    }
    /// @return number of paths
    uint32_t PathCount() const
    {
        return m_paths.size();
    }
    /// @return head path (the one used for data), or nullptr
    AomdvPath* PathFind()
    {
        return m_paths.empty() ? nullptr : &m_paths.front();
    }
    /// @return largest hop count, AOMDV_INFINITY if no path
    uint16_t PathMaxHop() const;
    /// @return smallest hop count, AOMDV_INFINITY if no path
    uint16_t PathMinHop() const;
    /// @return all paths (read-only)
    const std::vector<AomdvPath>& GetPaths() const
    {
        return m_paths;
    }
    /**
     * Copy head path into the ns-3 route (gateway, hop count) and set the
     * entry lifetime to the latest path expiry.
     */
    void SyncFromPaths();
    /// @return advertised hop count
    uint16_t GetAdvertisedHops() const
    {
        return m_advHops;
    }
    /// @param h advertised hop count
    void SetAdvertisedHops(uint16_t h)
    {
        m_advHops = h;
    }
    /// @return last hop count used for expanding ring search
    uint16_t GetLastHopCount() const
    {
        return m_lastHopCount;
    }
    /// @param h last hop count
    void SetLastHopCount(uint16_t h)
    {
        m_lastHopCount = h;
    }
    /// @return route error flag (route was used, RERR must be sent on break)
    bool GetError() const
    {
        return m_error;
    }
    /// @param e route error flag
    void SetError(bool e)
    {
        m_error = e;
    }
    /// @return highest destination sequence number heard in RERRs
    uint32_t GetHighestSeqHeard() const
    {
        return m_highestSeqHeard;
    }
    /// @param s highest destination sequence number heard
    void SetHighestSeqHeard(uint32_t s)
    {
        m_highestSeqHeard = s;
    }
    //\}


    // Fields
    /**
     * Get destination address function
     * @returns the IPv4 destination address
     */
    Ipv4Address GetDestination() const
    {
        return m_ipv4Route->GetDestination();
    }

    /**
     * Get route function
     * @returns The IPv4 route
     */
    Ptr<Ipv4Route> GetRoute() const
    {
        return m_ipv4Route;
    }

    /**
     * Set route function
     * @param r the IPv4 route
     */
    void SetRoute(Ptr<Ipv4Route> r)
    {
        m_ipv4Route = r;
    }

    /**
     * Set next hop address
     * @param nextHop the next hop IPv4 address
     */
    void SetNextHop(Ipv4Address nextHop)
    {
        m_ipv4Route->SetGateway(nextHop);
    }

    /**
     * Get next hop address
     * @returns the next hop address
     */
    Ipv4Address GetNextHop() const
    {
        return m_ipv4Route->GetGateway();
    }

    /**
     * Set output device
     * @param dev The output device
     */
    void SetOutputDevice(Ptr<NetDevice> dev)
    {
        m_ipv4Route->SetOutputDevice(dev);
    }

    /**
     * Get output device
     * @returns the output device
     */
    Ptr<NetDevice> GetOutputDevice() const
    {
        return m_ipv4Route->GetOutputDevice();
    }

    /**
     * Get the Ipv4InterfaceAddress
     * @returns the Ipv4InterfaceAddress
     */
    Ipv4InterfaceAddress GetInterface() const
    {
        return m_iface;
    }

    /**
     * Set the Ipv4InterfaceAddress
     * @param iface The Ipv4InterfaceAddress
     */
    void SetInterface(Ipv4InterfaceAddress iface)
    {
        m_iface = iface;
        m_ipv4Route->SetSource(iface.GetLocal());
    }

    /**
     * Set the valid sequence number
     * @param s the sequence number
     */
    void SetValidSeqNo(bool s)
    {
        m_validSeqNo = s;
    }

    /**
     * Get the valid sequence number
     * @returns the valid sequence number
     */
    bool GetValidSeqNo() const
    {
        return m_validSeqNo;
    }

    /**
     * Set the sequence number
     * @param sn the sequence number
     */
    void SetSeqNo(uint32_t sn)
    {
        m_seqNo = sn;
    }

    /**
     * Get the sequence number
     * @returns the sequence number
     */
    uint32_t GetSeqNo() const
    {
        return m_seqNo;
    }

    /**
     * Set the number of hops
     * @param hop the number of hops
     */
    void SetHop(uint16_t hop)
    {
        m_hops = hop;
    }

    /**
     * Get the number of hops
     * @returns the number of hops
     */
    uint16_t GetHop() const
    {
        return m_hops;
    }

    /**
     * Set the lifetime
     * @param lt The lifetime
     */
    void SetLifeTime(Time lt)
    {
        m_lifeTime = lt + Simulator::Now();
    }

    /**
     * Get the lifetime
     * @returns the lifetime
     */
    Time GetLifeTime() const
    {
        return m_lifeTime - Simulator::Now();
    }

    /**
     * Set the route flags
     * @param flag the route flags
     */
    void SetFlag(RouteFlags flag)
    {
        m_flag = flag;
    }

    /**
     * Get the route flags
     * @returns the route flags
     */
    RouteFlags GetFlag() const
    {
        return m_flag;
    }

    /**
     * Set the RREQ count
     * @param n the RREQ count
     */
    void SetRreqCnt(uint8_t n)
    {
        m_reqCount = n;
    }

    /**
     * Get the RREQ count
     * @returns the RREQ count
     */
    uint8_t GetRreqCnt() const
    {
        return m_reqCount;
    }

    /**
     * Increment the RREQ count
     */
    void IncrementRreqCnt()
    {
        m_reqCount++;
    }

    /**
     * Set the unidirectional flag
     * @param u the uni directional flag
     */
    void SetUnidirectional(bool u)
    {
        m_blackListState = u;
    }

    /**
     * Get the unidirectional flag
     * @returns the unidirectional flag
     */
    bool IsUnidirectional() const
    {
        return m_blackListState;
    }

    /**
     * Set the blacklist timeout
     * @param t the blacklist timeout value
     */
    void SetBlacklistTimeout(Time t)
    {
        m_blackListTimeout = t;
    }

    /**
     * Get the blacklist timeout value
     * @returns the blacklist timeout value
     */
    Time GetBlacklistTimeout() const
    {
        return m_blackListTimeout;
    }

    /// RREP_ACK timer
    Timer m_ackTimer;

    /**
     * @brief Compare destination address
     * @param dst IP address to compare
     * @return true if equal
     */
    bool operator==(const Ipv4Address dst) const
    {
        return (m_ipv4Route->GetDestination() == dst);
    }

    /**
     * Print packet to trace file
     * @param stream The output stream
     * @param unit The time unit to use (default Time::S)
     */
    void Print(Ptr<OutputStreamWrapper> stream, Time::Unit unit = Time::S) const;

  private:
    /// Valid Destination Sequence Number flag
    bool m_validSeqNo;
    /// Destination Sequence Number, if m_validSeqNo = true
    uint32_t m_seqNo;
    /// Hop Count (number of hops needed to reach destination)
    uint16_t m_hops;
    /**
     * @brief Expiration or deletion time of the route
     * Lifetime field in the routing table plays dual role:
     * for an active route it is the expiration time, and for an invalid route
     * it is the deletion time.
     */
    Time m_lifeTime;
    /** Ip route, include
     *   - destination address
     *   - source address
     *   - next hop address (gateway)
     *   - output device
     */
    Ptr<Ipv4Route> m_ipv4Route;
    /// Output interface address
    Ipv4InterfaceAddress m_iface;
    /// Routing flags: valid, invalid or in search
    RouteFlags m_flag;

    /// List of precursors
    std::vector<Ipv4Address> m_precursorList;
    /// When I can send another request
    Time m_routeRequestTimeout;
    /// Number of route requests
    uint8_t m_reqCount;
    /// Indicate if this entry is in "blacklist"
    bool m_blackListState;
    /// Time for which the node is put into the blacklist
    Time m_blackListTimeout;
    /// AOMDV path list (head = earliest inserted, used for data)
    std::vector<AomdvPath> m_paths;
    /// Advertised hop count (AOMDV_INFINITY = not yet advertised)
    uint16_t m_advHops;
    /// Last hop count (for expanding ring search)
    uint16_t m_lastHopCount;
    /// Route error flag
    bool m_error;
    /// Highest destination sequence number heard in a RERR
    uint32_t m_highestSeqHeard;
};

/**
 * @ingroup aomdv
 * @brief The Routing table used by AOMDV protocol
 */
class RoutingTable
{
  public:
    /**
     * constructor
     * @param t the routing table entry lifetime
     */
    RoutingTable(Time t);

    /// @name Handle lifetime of invalid route
    //\{
    /**
     * Get the lifetime of a bad link
     *
     * @return the lifetime of a bad link
     */
    Time GetBadLinkLifetime() const
    {
        return m_badLinkLifetime;
    }

    /**
     * Set the lifetime of a bad link
     *
     * @param t the lifetime of a bad link
     */
    void SetBadLinkLifetime(Time t)
    {
        m_badLinkLifetime = t;
    }

    //\}
    /**
     * Add routing table entry if it doesn't yet exist in routing table
     * @param r routing table entry
     * @return true in success
     */
    bool AddRoute(RoutingTableEntry& r);
    /**
     * Delete routing table entry with destination address dst, if it exists.
     * @param dst destination address
     * @return true on success
     */
    bool DeleteRoute(Ipv4Address dst);
    /**
     * Lookup routing table entry with destination address dst
     * @param dst destination address
     * @param rt entry with destination address dst, if exists
     * @return true on success
     */
    bool LookupRoute(Ipv4Address dst, RoutingTableEntry& rt);
    /**
     * Lookup route in VALID state
     * @param dst destination address
     * @param rt entry with destination address dst, if exists
     * @return true on success
     */
    bool LookupValidRoute(Ipv4Address dst, RoutingTableEntry& rt);
    /**
     * Update routing table
     * @param rt entry with destination address dst, if exists
     * @return true on success
     */
    bool Update(RoutingTableEntry& rt);
    /**
     * Set routing table entry flags
     * @param dst destination address
     * @param state the routing flags
     * @return true on success
     */
    bool SetEntryState(Ipv4Address dst, RouteFlags state);
    /**
     * Lookup routing entries with next hop Address dst and not empty list of precursors.
     *
     * @param nextHop the next hop IP address
     * @param unreachable
     */
    void GetListOfDestinationWithNextHop(Ipv4Address nextHop,
                                         std::map<Ipv4Address, uint32_t>& unreachable);
    /**
     * Update routing entries with this destination as follows:
     * 1. The destination sequence number of this routing entry, if it
     *    exists and is valid, is incremented.
     * 2. The entry is invalidated by marking the route entry as invalid
     * 3. The Lifetime field is updated to current time plus DELETE_PERIOD.
     * @param unreachable routes to invalidate
     */
    void InvalidateRoutesWithDst(const std::map<Ipv4Address, uint32_t>& unreachable);
    /**
     * Delete all route from interface with address iface
     * @param iface the interface IP address
     */
    void DeleteAllRoutesFromInterface(Ipv4InterfaceAddress iface);
    /**
     * @return all destinations present in the table
     */
    std::vector<Ipv4Address> GetDestinations() const
    {
        std::vector<Ipv4Address> v;
        for (const auto& kv : m_ipv4AddressEntry)
        {
            v.push_back(kv.first);
        }
        return v;
    }

    /// Delete all entries from routing table
    void Clear()
    {
        m_ipv4AddressEntry.clear();
    }

    /// Delete all outdated entries and invalidate valid entry if Lifetime is expired
    void Purge();
    /** Mark entry as unidirectional (e.g. add this neighbor to "blacklist" for blacklistTimeout
     * period)
     * @param neighbor neighbor address link to which assumed to be unidirectional
     * @param blacklistTimeout time for which the neighboring node is put into the blacklist
     * @return true on success
     */
    bool MarkLinkAsUnidirectional(Ipv4Address neighbor, Time blacklistTimeout);
    /**
     * Print routing table
     * @param stream the output stream
     * @param unit The time unit to use (default Time::S)
     */
    void Print(Ptr<OutputStreamWrapper> stream, Time::Unit unit = Time::S) const;

  private:
    /// The routing table
    std::map<Ipv4Address, RoutingTableEntry> m_ipv4AddressEntry;
    /// Deletion time for invalid routes
    Time m_badLinkLifetime;
    /**
     * const version of Purge, for use by Print() method
     * @param table the routing table entry to purge
     */
    void Purge(std::map<Ipv4Address, RoutingTableEntry>& table) const;
};

} // namespace aomdv
} // namespace ns3

#endif /* AOMDV_RTABLE_H */
