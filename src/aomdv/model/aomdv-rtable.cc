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

#include "aomdv-rtable.h"

#include "ns3/log.h"
#include "ns3/simulator.h"

#include <algorithm>
#include <iomanip>

namespace ns3
{

NS_LOG_COMPONENT_DEFINE("AomdvRoutingTable");

namespace aomdv
{

/*
 The Routing Table
 */

RoutingTableEntry::RoutingTableEntry(Ptr<NetDevice> dev,
                                     Ipv4Address dst,
                                     bool vSeqNo,
                                     uint32_t seqNo,
                                     Ipv4InterfaceAddress iface,
                                     uint16_t hops,
                                     Ipv4Address nextHop,
                                     Time lifetime)
    : m_ackTimer(Timer::CANCEL_ON_DESTROY),
      m_validSeqNo(vSeqNo),
      m_seqNo(seqNo),
      m_hops(hops),
      m_lifeTime(lifetime + Simulator::Now()),
      m_iface(iface),
      m_flag(VALID),
      m_reqCount(0),
      m_blackListState(false),
      m_blackListTimeout(Simulator::Now()),
      m_advHops(AOMDV_INFINITY),
      m_lastHopCount(AOMDV_INFINITY),
      m_error(false),
      m_highestSeqHeard(0)
{
    m_ipv4Route = Create<Ipv4Route>();
    m_ipv4Route->SetDestination(dst);
    m_ipv4Route->SetGateway(nextHop);
    m_ipv4Route->SetSource(m_iface.GetLocal());
    m_ipv4Route->SetOutputDevice(dev);
}

RoutingTableEntry::~RoutingTableEntry()
{
}

bool
RoutingTableEntry::InsertPrecursor(Ipv4Address id)
{
    NS_LOG_FUNCTION(this << id);
    if (!LookupPrecursor(id))
    {
        m_precursorList.push_back(id);
        return true;
    }
    else
    {
        return false;
    }
}

bool
RoutingTableEntry::LookupPrecursor(Ipv4Address id)
{
    NS_LOG_FUNCTION(this << id);
    for (auto i = m_precursorList.begin(); i != m_precursorList.end(); ++i)
    {
        if (*i == id)
        {
            NS_LOG_LOGIC("Precursor " << id << " found");
            return true;
        }
    }
    NS_LOG_LOGIC("Precursor " << id << " not found");
    return false;
}

bool
RoutingTableEntry::DeletePrecursor(Ipv4Address id)
{
    NS_LOG_FUNCTION(this << id);
    auto i = std::remove(m_precursorList.begin(), m_precursorList.end(), id);
    if (i == m_precursorList.end())
    {
        NS_LOG_LOGIC("Precursor " << id << " not found");
        return false;
    }
    else
    {
        NS_LOG_LOGIC("Precursor " << id << " found");
        m_precursorList.erase(i, m_precursorList.end());
    }
    return true;
}

void
RoutingTableEntry::DeleteAllPrecursors()
{
    NS_LOG_FUNCTION(this);
    m_precursorList.clear();
}

bool
RoutingTableEntry::IsPrecursorListEmpty() const
{
    return m_precursorList.empty();
}

void
RoutingTableEntry::GetPrecursors(std::vector<Ipv4Address>& prec) const
{
    NS_LOG_FUNCTION(this);
    if (IsPrecursorListEmpty())
    {
        return;
    }
    for (auto i = m_precursorList.begin(); i != m_precursorList.end(); ++i)
    {
        bool result = true;
        for (auto j = prec.begin(); j != prec.end(); ++j)
        {
            if (*j == *i)
            {
                result = false;
                break;
            }
        }
        if (result)
        {
            prec.push_back(*i);
        }
    }
}

void
RoutingTableEntry::Invalidate(Time badLinkLifetime)
{
    NS_LOG_FUNCTION(this << badLinkLifetime.As(Time::S));
    if (m_flag == INVALID)
    {
        return;
    }
    m_flag = INVALID;
    m_reqCount = 0;
    m_lifeTime = badLinkLifetime + Simulator::Now();
    // ns-2.35 AOMDV rt_down(): advertised hops = INFINITY, delete all paths
    m_advHops = AOMDV_INFINITY;
    m_paths.clear();
}

AomdvPath*
RoutingTableEntry::PathInsert(Ipv4Address nextHop,
                              uint16_t hopCount,
                              Time expire,
                              Ipv4Address lastHop)
{
    AomdvPath p{nextHop, lastHop, hopCount, expire};
    m_paths.push_back(p);
    return &m_paths.back();
}

AomdvPath*
RoutingTableEntry::PathLookup(Ipv4Address nextHop)
{
    for (auto& p : m_paths)
    {
        if (p.nextHop == nextHop)
        {
            return &p;
        }
    }
    return nullptr;
}

AomdvPath*
RoutingTableEntry::DisjointPathLookup(Ipv4Address nextHop, Ipv4Address lastHop)
{
    for (auto& p : m_paths)
    {
        if (p.nextHop == nextHop && p.lastHop == lastHop)
        {
            return &p;
        }
    }
    return nullptr;
}

bool
RoutingTableEntry::NewDisjointPath(Ipv4Address nextHop, Ipv4Address lastHop) const
{
    for (const auto& p : m_paths)
    {
        if (p.nextHop == nextHop || p.lastHop == lastHop)
        {
            return false;
        }
    }
    return true;
}

AomdvPath*
RoutingTableEntry::PathLookupLastHop(Ipv4Address lastHop)
{
    for (auto& p : m_paths)
    {
        if (p.lastHop == lastHop)
        {
            return &p;
        }
    }
    return nullptr;
}

void
RoutingTableEntry::PathDelete(Ipv4Address nextHop)
{
    for (auto it = m_paths.begin(); it != m_paths.end(); ++it)
    {
        if (it->nextHop == nextHop)
        {
            m_paths.erase(it);
            return;
        }
    }
}

void
RoutingTableEntry::PathDeleteAll()
{
    m_paths.clear();
}

void
RoutingTableEntry::PathPurge()
{
    Time now = Simulator::Now();
    for (auto it = m_paths.begin(); it != m_paths.end();)
    {
        if (it->expire < now)
        {
            it = m_paths.erase(it);
        }
        else
        {
            ++it;
        }
    }
}

uint16_t
RoutingTableEntry::PathMaxHop() const
{
    uint16_t m = 0;
    for (const auto& p : m_paths)
    {
        m = std::max(m, p.hopCount);
    }
    return m == 0 ? AOMDV_INFINITY : m;
}

uint16_t
RoutingTableEntry::PathMinHop() const
{
    uint16_t m = AOMDV_INFINITY;
    for (const auto& p : m_paths)
    {
        m = std::min(m, p.hopCount);
    }
    return m;
}

void
RoutingTableEntry::SyncFromPaths()
{
    if (m_paths.empty())
    {
        return;
    }
    m_ipv4Route->SetGateway(m_paths.front().nextHop);
    m_hops = m_paths.front().hopCount;
    Time mx = m_paths.front().expire;
    for (const auto& p : m_paths)
    {
        mx = std::max(mx, p.expire);
    }
    m_lifeTime = mx;
}

void
RoutingTableEntry::Print(Ptr<OutputStreamWrapper> stream, Time::Unit unit /* = Time::S */) const
{
    std::ostream* os = stream->GetStream();
    // Copy the current ostream state
    std::ios oldState(nullptr);
    oldState.copyfmt(*os);

    *os << std::resetiosflags(std::ios::adjustfield) << std::setiosflags(std::ios::left);

    std::ostringstream dest;
    std::ostringstream gw;
    std::ostringstream iface;
    std::ostringstream expire;
    dest << m_ipv4Route->GetDestination();
    gw << m_ipv4Route->GetGateway();
    iface << m_iface.GetLocal();
    expire << std::setprecision(2) << (m_lifeTime - Simulator::Now()).As(unit);
    *os << std::setw(16) << dest.str();
    *os << std::setw(16) << gw.str();
    *os << std::setw(16) << iface.str();
    *os << std::setw(16);
    switch (m_flag)
    {
    case VALID: {
        *os << "UP";
        break;
    }
    case INVALID: {
        *os << "DOWN";
        break;
    }
    case IN_SEARCH: {
        *os << "IN_SEARCH";
        break;
    }
    }

    *os << std::setw(16) << expire.str();
    *os << m_hops << std::endl;
    // Restore the previous ostream state
    (*os).copyfmt(oldState);
}

/*
 The Routing Table
 */

RoutingTable::RoutingTable(Time t)
    : m_badLinkLifetime(t)
{
}

bool
RoutingTable::LookupRoute(Ipv4Address id, RoutingTableEntry& rt)
{
    NS_LOG_FUNCTION(this << id);
    Purge();
    if (m_ipv4AddressEntry.empty())
    {
        NS_LOG_LOGIC("Route to " << id << " not found; m_ipv4AddressEntry is empty");
        return false;
    }
    auto i = m_ipv4AddressEntry.find(id);
    if (i == m_ipv4AddressEntry.end())
    {
        NS_LOG_LOGIC("Route to " << id << " not found");
        return false;
    }
    rt = i->second;
    NS_LOG_LOGIC("Route to " << id << " found");
    return true;
}

bool
RoutingTable::LookupValidRoute(Ipv4Address id, RoutingTableEntry& rt)
{
    NS_LOG_FUNCTION(this << id);
    if (!LookupRoute(id, rt))
    {
        NS_LOG_LOGIC("Route to " << id << " not found");
        return false;
    }
    NS_LOG_LOGIC("Route to " << id << " flag is "
                             << ((rt.GetFlag() == VALID) ? "valid" : "not valid"));
    return (rt.GetFlag() == VALID);
}

bool
RoutingTable::DeleteRoute(Ipv4Address dst)
{
    NS_LOG_FUNCTION(this << dst);
    Purge();
    if (m_ipv4AddressEntry.erase(dst) != 0)
    {
        NS_LOG_LOGIC("Route deletion to " << dst << " successful");
        return true;
    }
    NS_LOG_LOGIC("Route deletion to " << dst << " not successful");
    return false;
}

bool
RoutingTable::AddRoute(RoutingTableEntry& rt)
{
    NS_LOG_FUNCTION(this);
    Purge();
    if (rt.GetFlag() != IN_SEARCH)
    {
        rt.SetRreqCnt(0);
    }
    auto result = m_ipv4AddressEntry.insert(std::make_pair(rt.GetDestination(), rt));
    return result.second;
}

bool
RoutingTable::Update(RoutingTableEntry& rt)
{
    NS_LOG_FUNCTION(this);
    auto i = m_ipv4AddressEntry.find(rt.GetDestination());
    if (i == m_ipv4AddressEntry.end())
    {
        NS_LOG_LOGIC("Route update to " << rt.GetDestination() << " fails; not found");
        return false;
    }
    i->second = rt;
    if (i->second.GetFlag() != IN_SEARCH)
    {
        NS_LOG_LOGIC("Route update to " << rt.GetDestination() << " set RreqCnt to 0");
        i->second.SetRreqCnt(0);
    }
    return true;
}

bool
RoutingTable::SetEntryState(Ipv4Address id, RouteFlags state)
{
    NS_LOG_FUNCTION(this);
    auto i = m_ipv4AddressEntry.find(id);
    if (i == m_ipv4AddressEntry.end())
    {
        NS_LOG_LOGIC("Route set entry state to " << id << " fails; not found");
        return false;
    }
    i->second.SetFlag(state);
    i->second.SetRreqCnt(0);
    NS_LOG_LOGIC("Route set entry state to " << id << ": new state is " << state);
    return true;
}

void
RoutingTable::GetListOfDestinationWithNextHop(Ipv4Address nextHop,
                                              std::map<Ipv4Address, uint32_t>& unreachable)
{
    NS_LOG_FUNCTION(this);
    Purge();
    unreachable.clear();
    for (auto i = m_ipv4AddressEntry.begin(); i != m_ipv4AddressEntry.end(); ++i)
    {
        if (i->second.GetNextHop() == nextHop)
        {
            NS_LOG_LOGIC("Unreachable insert " << i->first << " " << i->second.GetSeqNo());
            unreachable.insert(std::make_pair(i->first, i->second.GetSeqNo()));
        }
    }
}

void
RoutingTable::InvalidateRoutesWithDst(const std::map<Ipv4Address, uint32_t>& unreachable)
{
    NS_LOG_FUNCTION(this);
    Purge();
    for (auto i = m_ipv4AddressEntry.begin(); i != m_ipv4AddressEntry.end(); ++i)
    {
        for (auto j = unreachable.begin(); j != unreachable.end(); ++j)
        {
            if ((i->first == j->first) && (i->second.GetFlag() == VALID))
            {
                NS_LOG_LOGIC("Invalidate route with destination address " << i->first);
                i->second.Invalidate(m_badLinkLifetime);
            }
        }
    }
}

void
RoutingTable::DeleteAllRoutesFromInterface(Ipv4InterfaceAddress iface)
{
    NS_LOG_FUNCTION(this);
    if (m_ipv4AddressEntry.empty())
    {
        return;
    }
    for (auto i = m_ipv4AddressEntry.begin(); i != m_ipv4AddressEntry.end();)
    {
        if (i->second.GetInterface() == iface)
        {
            auto tmp = i;
            ++i;
            m_ipv4AddressEntry.erase(tmp);
        }
        else
        {
            ++i;
        }
    }
}

void
RoutingTable::Purge()
{
    NS_LOG_FUNCTION(this);
    Purge(m_ipv4AddressEntry);
}

void
RoutingTable::Purge(std::map<Ipv4Address, RoutingTableEntry>& table) const
{
    NS_LOG_FUNCTION(this);
    // ns-2.35 AOMDV rt_purge(): for every UP entry drop expired paths; if no
    // path is left, increase the sequence number (made odd) and bring the
    // route down. Entries are never erased (ns-2 keeps them, which preserves
    // the sequence-number state needed for loop freedom).
    for (auto& kv : table)
    {
        RoutingTableEntry& e = kv.second;
        if (e.GetFlag() != VALID)
        {
            continue;
        }
        e.PathPurge();
        if (e.PathEmpty())
        {
            uint32_t s = std::max(e.GetSeqNo() + 1, e.GetHighestSeqHeard());
            if (s % 2 == 0)
            {
                s++;
            }
            e.SetSeqNo(s);
            NS_LOG_LOGIC("All paths to " << kv.first << " expired; route down");
            e.Invalidate(m_badLinkLifetime);
        }
        else
        {
            e.SyncFromPaths();
        }
    }
}

bool
RoutingTable::MarkLinkAsUnidirectional(Ipv4Address neighbor, Time blacklistTimeout)
{
    NS_LOG_FUNCTION(this << neighbor << blacklistTimeout.As(Time::S));
    auto i = m_ipv4AddressEntry.find(neighbor);
    if (i == m_ipv4AddressEntry.end())
    {
        NS_LOG_LOGIC("Mark link unidirectional to  " << neighbor << " fails; not found");
        return false;
    }
    i->second.SetUnidirectional(true);
    i->second.SetBlacklistTimeout(blacklistTimeout);
    i->second.SetRreqCnt(0);
    NS_LOG_LOGIC("Set link to " << neighbor << " to unidirectional");
    return true;
}

void
RoutingTable::Print(Ptr<OutputStreamWrapper> stream, Time::Unit unit /* = Time::S */) const
{
    std::map<Ipv4Address, RoutingTableEntry> table = m_ipv4AddressEntry;
    Purge(table);
    std::ostream* os = stream->GetStream();
    // Copy the current ostream state
    std::ios oldState(nullptr);
    oldState.copyfmt(*os);

    *os << std::resetiosflags(std::ios::adjustfield) << std::setiosflags(std::ios::left);
    *os << "\nAOMDV Routing table\n";
    *os << std::setw(16) << "Destination";
    *os << std::setw(16) << "Gateway";
    *os << std::setw(16) << "Interface";
    *os << std::setw(16) << "Flag";
    *os << std::setw(16) << "Expire";
    *os << "Hops" << std::endl;
    for (auto i = table.begin(); i != table.end(); ++i)
    {
        i->second.Print(stream, unit);
    }
    *stream->GetStream() << "\n";
}

} // namespace aomdv
} // namespace ns3
