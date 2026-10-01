/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
#ifndef PMAODV_HELPER_H
#define PMAODV_HELPER_H

#include "ns3/ipv4-routing-helper.h"
#include "ns3/object-factory.h"
#include "ns3/node.h"
#include "ns3/node-container.h"

namespace ns3 {

/**
 * \ingroup pmaodv
 * \brief Helper class for PM-AOMDV routing protocol installation
 */
class PmaodvHelper : public Ipv4RoutingHelper
{
public:
  PmaodvHelper ();
  PmaodvHelper (const PmaodvHelper &);

  PmaodvHelper * Copy () const override;

  Ptr<Ipv4RoutingProtocol> Create (Ptr<Node> node) const override;

  /**
   * Set β exponent (hop-count weight). Default 1.0 = paper's formula.
   */
  void SetBeta (double beta);

  /**
   * Set λ age decay constant. Default 0.0 = ignore path age.
   */
  void SetLambda (double lambda);

  void Set (std::string name, const AttributeValue &value);

private:
  ObjectFactory m_agentFactory;
};

} // namespace ns3

#endif /* PMAODV_HELPER_H */
