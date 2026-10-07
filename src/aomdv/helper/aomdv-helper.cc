/*
 * Copyright (c) 2009 IITP RAS
 *
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Authors: Pavel Boyko <boyko@iitp.ru>, written after OlsrHelper by Mathieu Lacage
 * <mathieu.lacage@sophia.inria.fr>
 */
#include "aomdv-helper.h"

#include "ns3/aomdv-routing-protocol.h"
#include "ns3/ipv4-list-routing.h"
#include "ns3/names.h"
#include "ns3/node-list.h"
#include "ns3/ptr.h"

namespace ns3
{

AomdvHelper::AomdvHelper()
    : Ipv4RoutingHelper()
{
    m_agentFactory.SetTypeId("ns3::aomdv::RoutingProtocol");
}

AomdvHelper*
AomdvHelper::Copy() const
{
    return new AomdvHelper(*this);
}

Ptr<Ipv4RoutingProtocol>
AomdvHelper::Create(Ptr<Node> node) const
{
    Ptr<aomdv::RoutingProtocol> agent = m_agentFactory.Create<aomdv::RoutingProtocol>();
    node->AggregateObject(agent);
    return agent;
}

void
AomdvHelper::Set(std::string name, const AttributeValue& value)
{
    m_agentFactory.Set(name, value);
}

int64_t
AomdvHelper::AssignStreams(NodeContainer c, int64_t stream)
{
    int64_t currentStream = stream;
    Ptr<Node> node;
    for (auto i = c.Begin(); i != c.End(); ++i)
    {
        node = (*i);
        Ptr<Ipv4> ipv4 = node->GetObject<Ipv4>();
        NS_ASSERT_MSG(ipv4, "Ipv4 not installed on node");
        Ptr<Ipv4RoutingProtocol> proto = ipv4->GetRoutingProtocol();
        NS_ASSERT_MSG(proto, "Ipv4 routing not installed on node");
        Ptr<aomdv::RoutingProtocol> aomdv = DynamicCast<aomdv::RoutingProtocol>(proto);
        if (aomdv)
        {
            currentStream += aomdv->AssignStreams(currentStream);
            continue;
        }
        // Aomdv may also be in a list
        Ptr<Ipv4ListRouting> list = DynamicCast<Ipv4ListRouting>(proto);
        if (list)
        {
            int16_t priority;
            Ptr<Ipv4RoutingProtocol> listProto;
            Ptr<aomdv::RoutingProtocol> listAomdv;
            for (uint32_t i = 0; i < list->GetNRoutingProtocols(); i++)
            {
                listProto = list->GetRoutingProtocol(i, priority);
                listAomdv = DynamicCast<aomdv::RoutingProtocol>(listProto);
                if (listAomdv)
                {
                    currentStream += listAomdv->AssignStreams(currentStream);
                    break;
                }
            }
        }
    }
    return (currentStream - stream);
}

} // namespace ns3
