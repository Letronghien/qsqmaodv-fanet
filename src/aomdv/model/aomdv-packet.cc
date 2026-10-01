/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/*
 * AOMDV Packet serialisation / deserialisation
 * Closely mirrors NS-3's AODV packet code, extended with AOMDV fields.
 */
#include "aomdv-packet.h"
#include "ns3/address-utils.h"
#include "ns3/log.h"
#include "ns3/packet.h"

namespace ns3 {

NS_LOG_COMPONENT_DEFINE ("AomdvPacket");

namespace aomdv {

/*---------------------------------------------------------------------------
 * RreqHeader
 *--------------------------------------------------------------------------*/
NS_OBJECT_ENSURE_REGISTERED (RreqHeader);

RreqHeader::RreqHeader (uint8_t flags, uint8_t reserved, uint8_t hopCount,
                        uint32_t requestID, Ipv4Address dst,
                        uint32_t dstSeqNo, Ipv4Address origin,
                        uint32_t originSeqNo)
  : m_flags (flags),
    m_reserved (reserved),
    m_hopCount (hopCount),
    m_requestID (requestID),
    m_dst (dst),
    m_dstSeqNo (dstSeqNo),
    m_origin (origin),
    m_originSeqNo (originSeqNo),
    m_firstHop (Ipv4Address ()),
    m_lastHop (Ipv4Address ())
{
}

TypeId
RreqHeader::GetTypeId ()
{
  static TypeId tid = TypeId ("ns3::aomdv::RreqHeader")
    .SetParent<Header> ()
    .SetGroupName ("Aomdv")
    .AddConstructor<RreqHeader> ();
  return tid;
}

TypeId
RreqHeader::GetInstanceTypeId () const
{
  return GetTypeId ();
}

uint32_t
RreqHeader::GetSerializedSize () const
{
  // Standard AODV RREQ (24 bytes) + 8 bytes for firstHop + lastHop
  return 32;
}

void
RreqHeader::Serialize (Buffer::Iterator i) const
{
  i.WriteU8 (1);  // type = RREQ
  i.WriteU8 (m_flags);
  i.WriteU8 (m_reserved);
  i.WriteU8 (m_hopCount);
  i.WriteHtonU32 (m_requestID);
  WriteTo (i, m_dst);
  i.WriteHtonU32 (m_dstSeqNo);
  WriteTo (i, m_origin);
  i.WriteHtonU32 (m_originSeqNo);
  WriteTo (i, m_firstHop);
  WriteTo (i, m_lastHop);
}

uint32_t
RreqHeader::Deserialize (Buffer::Iterator start)
{
  Buffer::Iterator i = start;
  uint8_t type = i.ReadU8 ();
  NS_ASSERT (type == 1);
  m_flags      = i.ReadU8 ();
  m_reserved   = i.ReadU8 ();
  m_hopCount   = i.ReadU8 ();
  m_requestID  = i.ReadNtohU32 ();
  ReadFrom (i, m_dst);
  m_dstSeqNo   = i.ReadNtohU32 ();
  ReadFrom (i, m_origin);
  m_originSeqNo = i.ReadNtohU32 ();
  ReadFrom (i, m_firstHop);
  ReadFrom (i, m_lastHop);
  uint32_t dist = i.GetDistanceFrom (start);
  NS_ASSERT (dist == GetSerializedSize ());
  return dist;
}

void
RreqHeader::Print (std::ostream & os) const
{
  os << "RREQ id=" << m_requestID
     << " dst=" << m_dst << " seq=" << m_dstSeqNo
     << " origin=" << m_origin << " seq=" << m_originSeqNo
     << " hops=" << (int)m_hopCount
     << " firstHop=" << m_firstHop << " lastHop=" << m_lastHop;
}

void  RreqHeader::SetGratiousRrep (bool f)  { if (f) m_flags |= (1<<5); else m_flags &= ~(1<<5); }
bool  RreqHeader::GetGratiousRrep () const  { return (m_flags & (1<<5)) != 0; }
void  RreqHeader::SetDestinationOnly (bool f){ if (f) m_flags |= (1<<4); else m_flags &= ~(1<<4); }
bool  RreqHeader::GetDestinationOnly () const{ return (m_flags & (1<<4)) != 0; }
void  RreqHeader::SetUnknownSeqno (bool f)  { if (f) m_flags |= (1<<3); else m_flags &= ~(1<<3); }
bool  RreqHeader::GetUnknownSeqno () const  { return (m_flags & (1<<3)) != 0; }

bool
RreqHeader::operator== (RreqHeader const & o) const
{
  return (m_requestID == o.m_requestID && m_origin == o.m_origin);
}

std::ostream & operator<< (std::ostream & os, RreqHeader const & h)
{
  h.Print (os); return os;
}

/*---------------------------------------------------------------------------
 * RrepHeader
 *--------------------------------------------------------------------------*/
NS_OBJECT_ENSURE_REGISTERED (RrepHeader);

RrepHeader::RrepHeader (uint8_t prefixSize, uint8_t hopCount,
                        Ipv4Address dst, uint32_t dstSeqNo,
                        Ipv4Address origin, Time lifetime)
  : m_flags (0),
    m_prefixSize (prefixSize),
    m_hopCount (hopCount),
    m_advHopCount (0),
    m_dst (dst),
    m_dstSeqNo (dstSeqNo),
    m_origin (origin),
    m_lifeTime ((uint32_t)(lifetime.GetMilliSeconds ()))
{
}

TypeId
RrepHeader::GetTypeId ()
{
  static TypeId tid = TypeId ("ns3::aomdv::RrepHeader")
    .SetParent<Header> ()
    .SetGroupName ("Aomdv")
    .AddConstructor<RrepHeader> ();
  return tid;
}

TypeId RrepHeader::GetInstanceTypeId () const { return GetTypeId (); }

uint32_t
RrepHeader::GetSerializedSize () const
{
  // Standard RREP (20 bytes) + 1 byte advHopCount
  return 21;
}

void
RrepHeader::Serialize (Buffer::Iterator i) const
{
  i.WriteU8 (2);  // type = RREP
  i.WriteU8 (m_flags);
  i.WriteU8 (m_prefixSize);
  i.WriteU8 (m_hopCount);
  i.WriteU8 (m_advHopCount);
  WriteTo (i, m_dst);
  i.WriteHtonU32 (m_dstSeqNo);
  WriteTo (i, m_origin);
  i.WriteHtonU32 (m_lifeTime);
}

uint32_t
RrepHeader::Deserialize (Buffer::Iterator start)
{
  Buffer::Iterator i = start;
  uint8_t type = i.ReadU8 ();
  NS_ASSERT (type == 2);
  m_flags        = i.ReadU8 ();
  m_prefixSize   = i.ReadU8 ();
  m_hopCount     = i.ReadU8 ();
  m_advHopCount  = i.ReadU8 ();
  ReadFrom (i, m_dst);
  m_dstSeqNo     = i.ReadNtohU32 ();
  ReadFrom (i, m_origin);
  m_lifeTime     = i.ReadNtohU32 ();
  uint32_t dist  = i.GetDistanceFrom (start);
  NS_ASSERT (dist == GetSerializedSize ());
  return dist;
}

void
RrepHeader::Print (std::ostream & os) const
{
  os << "RREP dst=" << m_dst << " seq=" << m_dstSeqNo
     << " origin=" << m_origin
     << " hops=" << (int)m_hopCount
     << " advHC=" << (int)m_advHopCount
     << " lt=" << m_lifeTime << "ms";
}

void  RrepHeader::SetAckRequired (bool f) { if (f) m_flags |= (1<<7); else m_flags &= ~(1<<7); }
bool  RrepHeader::GetAckRequired () const { return (m_flags & (1<<7)) != 0; }
void  RrepHeader::SetPrefixSize (uint8_t sz) { m_prefixSize = sz; }
uint8_t RrepHeader::GetPrefixSize () const   { return m_prefixSize; }

void
RrepHeader::SetLifeTime (Time t)
{
  m_lifeTime = (uint32_t)(t.GetMilliSeconds ());
}

Time
RrepHeader::GetLifeTime () const
{
  return MilliSeconds (m_lifeTime);
}

bool
RrepHeader::operator== (RrepHeader const & o) const
{
  return (m_dst == o.m_dst && m_dstSeqNo == o.m_dstSeqNo &&
          m_origin == o.m_origin && m_hopCount == o.m_hopCount);
}

std::ostream & operator<< (std::ostream & os, RrepHeader const & h)
{
  h.Print (os); return os;
}

/*---------------------------------------------------------------------------
 * RrepAckHeader
 *--------------------------------------------------------------------------*/
NS_OBJECT_ENSURE_REGISTERED (RrepAckHeader);

RrepAckHeader::RrepAckHeader () : m_reserved (0) {}

TypeId RrepAckHeader::GetTypeId ()
{
  static TypeId tid = TypeId ("ns3::aomdv::RrepAckHeader")
    .SetParent<Header> ()
    .SetGroupName ("Aomdv")
    .AddConstructor<RrepAckHeader> ();
  return tid;
}

TypeId RrepAckHeader::GetInstanceTypeId () const { return GetTypeId (); }
uint32_t RrepAckHeader::GetSerializedSize () const { return 2; }

void RrepAckHeader::Serialize (Buffer::Iterator i) const
{
  i.WriteU8 (4);
  i.WriteU8 (m_reserved);
}

uint32_t RrepAckHeader::Deserialize (Buffer::Iterator start)
{
  Buffer::Iterator i = start;
  i.ReadU8 ();
  m_reserved = i.ReadU8 ();
  return 2;
}

void RrepAckHeader::Print (std::ostream & os) const { os << "RREP-ACK"; }
bool RrepAckHeader::operator== (RrepAckHeader const &) const { return true; }

/*---------------------------------------------------------------------------
 * RerrHeader
 *--------------------------------------------------------------------------*/
NS_OBJECT_ENSURE_REGISTERED (RerrHeader);

RerrHeader::RerrHeader () : m_flag (0), m_reserved (0) {}

TypeId RerrHeader::GetTypeId ()
{
  static TypeId tid = TypeId ("ns3::aomdv::RerrHeader")
    .SetParent<Header> ()
    .SetGroupName ("Aomdv")
    .AddConstructor<RerrHeader> ();
  return tid;
}

TypeId RerrHeader::GetInstanceTypeId () const { return GetTypeId (); }

uint32_t
RerrHeader::GetSerializedSize () const
{
  return (4 + 8 * m_unreachableDstSeqNo.size ());
}

void
RerrHeader::Serialize (Buffer::Iterator i) const
{
  i.WriteU8 (3);
  i.WriteU8 (m_flag);
  i.WriteU8 (m_reserved);
  i.WriteU8 ((uint8_t)m_unreachableDstSeqNo.size ());
  for (const auto & kv : m_unreachableDstSeqNo)
    {
      WriteTo (i, kv.first);
      i.WriteHtonU32 (kv.second);
    }
}

uint32_t
RerrHeader::Deserialize (Buffer::Iterator start)
{
  Buffer::Iterator i = start;
  i.ReadU8 ();
  m_flag     = i.ReadU8 ();
  m_reserved = i.ReadU8 ();
  uint8_t destCount = i.ReadU8 ();
  m_unreachableDstSeqNo.clear ();
  for (uint8_t d = 0; d < destCount; ++d)
    {
      Ipv4Address dst;
      ReadFrom (i, dst);
      uint32_t seqNo = i.ReadNtohU32 ();
      m_unreachableDstSeqNo[dst] = seqNo;
    }
  return i.GetDistanceFrom (start);
}

void RerrHeader::Print (std::ostream & os) const
{
  os << "RERR destinations=" << m_unreachableDstSeqNo.size ();
}

void RerrHeader::SetNoDelete (bool f)  { if (f) m_flag |= 1; else m_flag &= ~1; }
bool RerrHeader::GetNoDelete () const  { return (m_flag & 1) != 0; }

bool
RerrHeader::AddUnDestination (Ipv4Address dst, uint32_t seqno)
{
  if (m_unreachableDstSeqNo.find (dst) != m_unreachableDstSeqNo.end ())
    return false;
  m_unreachableDstSeqNo[dst] = seqno;
  return true;
}

bool
RerrHeader::RemoveUnDestination (std::pair<Ipv4Address, uint32_t> & un)
{
  if (m_unreachableDstSeqNo.empty ()) return false;
  auto it = m_unreachableDstSeqNo.begin ();
  un = *it;
  m_unreachableDstSeqNo.erase (it);
  return true;
}

bool RerrHeader::operator== (RerrHeader const & o) const
{
  return m_unreachableDstSeqNo == o.m_unreachableDstSeqNo;
}

std::ostream & operator<< (std::ostream & os, RerrHeader const & h)
{
  h.Print (os); return os;
}

} // namespace aomdv
} // namespace ns3
