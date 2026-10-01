/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/*
 * AOMDV (Ad hoc On-demand Multipath Distance Vector) Packet Definitions
 * Based on Marina & Das, Wireless Communications and Mobile Computing, 2006
 * Adapted for NS-3 v3.40 - FANET/UAV Simulation
 */
#ifndef AOMDV_PACKET_H
#define AOMDV_PACKET_H

#include "ns3/header.h"
#include "ns3/ipv4-address.h"
#include "ns3/nstime.h"
#include <map>

namespace ns3 {
namespace aomdv {

/**
 * \brief MessageType enum for AOMDV control packets
 */
enum MessageType
{
  AODVTYPE_RREQ  = 1,  //!< RREQ
  AODVTYPE_RREP  = 2,  //!< RREP
  AODVTYPE_RERR  = 3,  //!< RERR
  AODVTYPE_RREP_ACK = 4 //!< RREP-ACK
};

/**
 * \brief AOMDV RREQ header
 *
 *  0                   1                   2                   3
 *  0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * |     Type      |J|R|G|D|U|  Reserved   |   Hop Count           |
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * |                            RREQ ID                             |
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * |                    Destination IP Address                      |
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * |                  Destination Sequence Number                   |
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * |                    Originator IP Address                       |
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * |                  Originator Sequence Number                    |
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * |                    First Hop IP Address   (AOMDV extension)    |
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * |                   Last Hop IP Address    (AOMDV extension)     |
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 */
class RreqHeader : public Header
{
public:
  RreqHeader (uint8_t flags = 0, uint8_t reserved = 0, uint8_t hopCount = 0,
              uint32_t requestID = 0, Ipv4Address dst = Ipv4Address (),
              uint32_t dstSeqNo = 0, Ipv4Address origin = Ipv4Address (),
              uint32_t originSeqNo = 0);

  static TypeId GetTypeId ();
  TypeId GetInstanceTypeId () const override;
  uint32_t GetSerializedSize () const override;
  void Serialize (Buffer::Iterator start) const override;
  uint32_t Deserialize (Buffer::Iterator start) override;
  void Print (std::ostream &os) const override;

  // Flags
  void SetGratiousRrep (bool f);
  bool GetGratiousRrep () const;
  void SetDestinationOnly (bool f);
  bool GetDestinationOnly () const;
  void SetUnknownSeqno (bool f);
  bool GetUnknownSeqno () const;

  void SetHopCount (uint8_t count) { m_hopCount = count; }
  uint8_t GetHopCount () const { return m_hopCount; }
  void SetId (uint32_t id) { m_requestID = id; }
  uint32_t GetId () const { return m_requestID; }
  void SetDst (Ipv4Address a) { m_dst = a; }
  Ipv4Address GetDst () const { return m_dst; }
  void SetDstSeqno (uint32_t s) { m_dstSeqNo = s; }
  uint32_t GetDstSeqno () const { return m_dstSeqNo; }
  void SetOrigin (Ipv4Address a) { m_origin = a; }
  Ipv4Address GetOrigin () const { return m_origin; }
  void SetOriginSeqno (uint32_t s) { m_originSeqNo = s; }
  uint32_t GetOriginSeqno () const { return m_originSeqNo; }

  // AOMDV extensions: first/last hop tracking for disjoint path discovery
  void SetFirstHop (Ipv4Address a) { m_firstHop = a; }
  Ipv4Address GetFirstHop () const { return m_firstHop; }
  void SetLastHop (Ipv4Address a) { m_lastHop = a; }
  Ipv4Address GetLastHop () const { return m_lastHop; }

  bool operator== (RreqHeader const & o) const;

private:
  uint8_t      m_flags;      ///< J R G D U flags
  uint8_t      m_reserved;
  uint8_t      m_hopCount;
  uint32_t     m_requestID;
  Ipv4Address  m_dst;
  uint32_t     m_dstSeqNo;
  Ipv4Address  m_origin;
  uint32_t     m_originSeqNo;
  Ipv4Address  m_firstHop;   ///< AOMDV: first hop from originator
  Ipv4Address  m_lastHop;    ///< AOMDV: last hop before destination
};

std::ostream & operator<< (std::ostream & os, RreqHeader const &);

/**
 * \brief AOMDV RREP header
 */
class RrepHeader : public Header
{
public:
  RrepHeader (uint8_t prefixSize = 0, uint8_t hopCount = 0,
              Ipv4Address dst = Ipv4Address (),
              uint32_t dstSeqNo = 0,
              Ipv4Address origin = Ipv4Address (),
              Time lifetime = MilliSeconds (0));

  static TypeId GetTypeId ();
  TypeId GetInstanceTypeId () const override;
  uint32_t GetSerializedSize () const override;
  void Serialize (Buffer::Iterator start) const override;
  uint32_t Deserialize (Buffer::Iterator start) override;
  void Print (std::ostream &os) const override;

  void SetHopCount (uint8_t count) { m_hopCount = count; }
  uint8_t GetHopCount () const { return m_hopCount; }
  void SetDst (Ipv4Address a) { m_dst = a; }
  Ipv4Address GetDst () const { return m_dst; }
  void SetDstSeqno (uint32_t s) { m_dstSeqNo = s; }
  uint32_t GetDstSeqno () const { return m_dstSeqNo; }
  void SetOrigin (Ipv4Address a) { m_origin = a; }
  Ipv4Address GetOrigin () const { return m_origin; }
  void SetLifeTime (Time t);
  Time GetLifeTime () const;
  void SetAckRequired (bool f);
  bool GetAckRequired () const;
  void SetPrefixSize (uint8_t sz);
  uint8_t GetPrefixSize () const;

  // AOMDV: advertised hop count at destination (for loop-freedom)
  void SetAdvertisedHopCount (uint8_t hc) { m_advHopCount = hc; }
  uint8_t GetAdvertisedHopCount () const { return m_advHopCount; }

  bool operator== (RrepHeader const & o) const;

private:
  uint8_t      m_flags;
  uint8_t      m_prefixSize;
  uint8_t      m_hopCount;
  uint8_t      m_advHopCount;  ///< AOMDV advertised hop count
  Ipv4Address  m_dst;
  uint32_t     m_dstSeqNo;
  Ipv4Address  m_origin;
  uint32_t     m_lifeTime;
};

std::ostream & operator<< (std::ostream & os, RrepHeader const &);

/**
 * \brief RREP-ACK header
 */
class RrepAckHeader : public Header
{
public:
  RrepAckHeader ();
  static TypeId GetTypeId ();
  TypeId GetInstanceTypeId () const override;
  uint32_t GetSerializedSize () const override;
  void Serialize (Buffer::Iterator start) const override;
  uint32_t Deserialize (Buffer::Iterator start) override;
  void Print (std::ostream &os) const override;
  bool operator== (RrepAckHeader const & o) const;

private:
  uint8_t m_reserved;
};

/**
 * \brief RERR (Route Error) header
 */
class RerrHeader : public Header
{
public:
  RerrHeader ();
  static TypeId GetTypeId ();
  TypeId GetInstanceTypeId () const override;
  uint32_t GetSerializedSize () const override;
  void Serialize (Buffer::Iterator start) const override;
  uint32_t Deserialize (Buffer::Iterator start) override;
  void Print (std::ostream &os) const override;

  void SetNoDelete (bool f);
  bool GetNoDelete () const;
  bool AddUnDestination (Ipv4Address dst, uint32_t seqno);
  bool RemoveUnDestination (std::pair<Ipv4Address, uint32_t> & un);
  bool IsEmpty () const { return m_unreachableDstSeqNo.empty (); }
  uint8_t GetDestCount () const { return (uint8_t)m_unreachableDstSeqNo.size (); }
  bool operator== (RerrHeader const & o) const;

private:
  uint8_t m_flag;
  uint8_t m_reserved;
  std::map<Ipv4Address, uint32_t> m_unreachableDstSeqNo;
};

std::ostream & operator<< (std::ostream & os, RerrHeader const &);

} // namespace aomdv
} // namespace ns3

#endif /* AOMDV_PACKET_H */
