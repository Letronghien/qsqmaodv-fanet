/*
 * Copyright (c) 2009 IITP RAS
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation;
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 * Authors: Pavel Boyko <boyko@iitp.ru>, written after OlsrHelper by Mathieu Lacage
 * <mathieu.lacage@sophia.inria.fr>
 */
#include "mpaodv-helper.h"

#include "ns3/mpaodv-routing-protocol.h"
#include "ns3/ipv4-list-routing.h"
#include "ns3/names.h"
#include "ns3/node-list.h"
#include "ns3/boolean.h"
#include "ns3/double.h"
#include "ns3/ptr.h"
#include "ns3/string.h"
#include "ns3/uinteger.h"

namespace ns3
{

MpaodvHelper::MpaodvHelper()
    : Ipv4RoutingHelper()
{
    m_agentFactory.SetTypeId("ns3::mpaodv::RoutingProtocol");
}

MpaodvHelper*
MpaodvHelper::Copy() const
{
    return new MpaodvHelper(*this);
}

Ptr<Ipv4RoutingProtocol>
MpaodvHelper::Create(Ptr<Node> node) const
{
    Ptr<mpaodv::RoutingProtocol> agent = m_agentFactory.Create<mpaodv::RoutingProtocol>();
    node->AggregateObject(agent);
    return agent;
}

void
MpaodvHelper::Set(std::string name, const AttributeValue& value)
{
    m_agentFactory.Set(name, value);
}

int64_t
MpaodvHelper::AssignStreams(NodeContainer c, int64_t stream)
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
        Ptr<mpaodv::RoutingProtocol> mpaodv = DynamicCast<mpaodv::RoutingProtocol>(proto);
        if (mpaodv)
        {
            currentStream += mpaodv->AssignStreams(currentStream);
            continue;
        }
        // Mpaodv may also be in a list
        Ptr<Ipv4ListRouting> list = DynamicCast<Ipv4ListRouting>(proto);
        if (list)
        {
            int16_t priority;
            Ptr<Ipv4RoutingProtocol> listProto;
            Ptr<mpaodv::RoutingProtocol> listMpaodv;
            for (uint32_t i = 0; i < list->GetNRoutingProtocols(); i++)
            {
                listProto = list->GetRoutingProtocol(i, priority);
                listMpaodv = DynamicCast<mpaodv::RoutingProtocol>(listProto);
                if (listMpaodv)
                {
                    currentStream += listMpaodv->AssignStreams(currentStream);
                    break;
                }
            }
        }
    }
    return (currentStream - stream);
}

PmaodvHelper::PmaodvHelper(uint32_t maxPaths)
    : MpaodvHelper()
{
    Set("Policy", StringValue("Probabilistic"));
    Set("MaxPaths", UintegerValue(maxPaths));
}

QmaodvHelper::QmaodvHelper()
    : MpaodvHelper()
{
    Set("Policy", StringValue("QLearning"));
}

} // namespace ns3
