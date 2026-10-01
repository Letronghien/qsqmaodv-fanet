/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
#ifndef QS2MAODV_HELPER_H
#define QS2MAODV_HELPER_H

#include "ns3/ipv4-routing-helper.h"
#include "ns3/object-factory.h"
#include "ns3/node.h"
#include "ns3/node-container.h"

namespace ns3
{

/**
 * \ingroup qs2maodv
 * \brief Helper class for QS-QMAODV routing protocol.
 */
class Qs2maodvHelper : public Ipv4RoutingHelper
{
  public:
    Qs2maodvHelper();
    Qs2maodvHelper* Copy() const override;

    /**
     * \param node the node for which we need to install routing protocol
     * \returns a newly-created routing protocol
     */
    Ptr<Ipv4RoutingProtocol> Create(Ptr<Node> node) const override;

    /**
     * Set an attribute for the routing protocol being installed.
     * \param name attribute name
     * \param value attribute value
     */
    void Set(std::string name, const AttributeValue& value);

  private:
    ObjectFactory m_agentFactory;
};

} // namespace ns3

#endif /* QS2MAODV_HELPER_H */
