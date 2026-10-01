#include "ns3/double.h"
/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
#include "pmaodv-helper.h"
#include "../model/pmaodv-routing-protocol.h"

namespace ns3 {

PmaodvHelper::PmaodvHelper ()
{
  m_agentFactory.SetTypeId ("ns3::pmaodv::RoutingProtocol");
}

PmaodvHelper::PmaodvHelper (const PmaodvHelper & o)
  : m_agentFactory (o.m_agentFactory)
{
}

PmaodvHelper *
PmaodvHelper::Copy () const
{
  return new PmaodvHelper (*this);
}

Ptr<Ipv4RoutingProtocol>
PmaodvHelper::Create (Ptr<Node> node) const
{
  Ptr<pmaodv::RoutingProtocol> agent =
    m_agentFactory.Create<pmaodv::RoutingProtocol> ();
  node->AggregateObject (agent);
  return agent;
}

void
PmaodvHelper::Set (std::string name, const AttributeValue & value)
{
  m_agentFactory.Set (name, value);
}

void PmaodvHelper::SetBeta (double beta)
{
  m_agentFactory.Set ("Beta", DoubleValue (beta));
}

void PmaodvHelper::SetLambda (double lambda)
{
  m_agentFactory.Set ("Lambda", DoubleValue (lambda));
}

} // namespace ns3
