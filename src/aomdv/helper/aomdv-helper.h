/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
#ifndef AOMDV_HELPER_H
#define AOMDV_HELPER_H

#include "ns3/ipv4-routing-helper.h"
#include "ns3/object-factory.h"
#include "ns3/node.h"
#include "ns3/node-container.h"

namespace ns3 {

/**
 * \ingroup aomdv
 * \brief Helper for AOMDV routing protocol installation
 */
class AomdvHelper : public Ipv4RoutingHelper
{
public:
  AomdvHelper ();
  AomdvHelper (const AomdvHelper &);

  AomdvHelper * Copy () const override;
  Ptr<Ipv4RoutingProtocol> Create (Ptr<Node> node) const override;
  void Set (std::string name, const AttributeValue & value);

private:
  ObjectFactory m_agentFactory;
};

} // namespace ns3
#endif
