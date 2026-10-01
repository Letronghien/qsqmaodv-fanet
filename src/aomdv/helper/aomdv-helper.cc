/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
#include "aomdv-helper.h"
#include "../model/aomdv-routing-protocol.h"

namespace ns3 {

AomdvHelper::AomdvHelper ()
{
  m_agentFactory.SetTypeId ("ns3::aomdv::RoutingProtocol");
}

AomdvHelper::AomdvHelper (const AomdvHelper & o)
  : m_agentFactory (o.m_agentFactory)
{
}

AomdvHelper *
AomdvHelper::Copy () const
{
  return new AomdvHelper (*this);
}

Ptr<Ipv4RoutingProtocol>
AomdvHelper::Create (Ptr<Node> node) const
{
  Ptr<aomdv::RoutingProtocol> agent =
    m_agentFactory.Create<aomdv::RoutingProtocol> ();
  node->AggregateObject (agent);
  return agent;
}

void
AomdvHelper::Set (std::string name, const AttributeValue & value)
{
  m_agentFactory.Set (name, value);
}

} // namespace ns3
