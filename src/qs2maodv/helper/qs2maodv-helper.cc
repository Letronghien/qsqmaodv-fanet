/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
#include "qs2maodv-helper.h"
#include "../model/qs2maodv-routing-protocol.h"
#include "ns3/ipv4-list-routing.h"
#include "ns3/names.h"
#include "ns3/node-list.h"
#include "ns3/ptr.h"

namespace ns3
{

Qs2maodvHelper::Qs2maodvHelper()
{
    m_agentFactory.SetTypeId("ns3::qs2maodv::RoutingProtocol");
}

Qs2maodvHelper* Qs2maodvHelper::Copy() const
{
    return new Qs2maodvHelper(*this);
}

Ptr<Ipv4RoutingProtocol> Qs2maodvHelper::Create(Ptr<Node> node) const
{
    Ptr<qs2maodv::RoutingProtocol> agent =
        m_agentFactory.Create<qs2maodv::RoutingProtocol>();
    node->AggregateObject(agent);
    return agent;
}

void Qs2maodvHelper::Set(std::string name, const AttributeValue& value)
{
    m_agentFactory.Set(name, value);
}

} // namespace ns3
