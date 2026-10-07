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

#ifndef MPAODV_HELPER_H
#define MPAODV_HELPER_H

#include "ns3/ipv4-routing-helper.h"
#include "ns3/node-container.h"
#include "ns3/node.h"
#include "ns3/object-factory.h"

namespace ns3
{
/**
 * \ingroup mpaodv
 * \brief Helper class that adds MPAODV routing to nodes.
 */
class MpaodvHelper : public Ipv4RoutingHelper
{
  public:
    MpaodvHelper();

    /**
     * \returns pointer to clone of this MpaodvHelper
     *
     * \internal
     * This method is mainly for internal use by the other helpers;
     * clients are expected to free the dynamic memory allocated by this method
     */
    MpaodvHelper* Copy() const override;

    /**
     * \param node the node on which the routing protocol will run
     * \returns a newly-created routing protocol
     *
     * This method will be called by ns3::InternetStackHelper::Install
     *
     */
    Ptr<Ipv4RoutingProtocol> Create(Ptr<Node> node) const override;
    /**
     * \param name the name of the attribute to set
     * \param value the value of the attribute to set.
     *
     * This method controls the attributes of ns3::mpaodv::RoutingProtocol
     */
    void Set(std::string name, const AttributeValue& value);
    /**
     * Assign a fixed random variable stream number to the random variables
     * used by this model.  Return the number of streams (possibly zero) that
     * have been assigned.  The Install() method of the InternetStackHelper
     * should have previously been called by the user.
     *
     * \param stream first stream index to use
     * \param c NodeContainer of the set of nodes for which MPAODV
     *          should be modified to use a fixed stream
     * \return the number of stream indices assigned by this helper
     */
    int64_t AssignStreams(NodeContainer c, int64_t stream);

  private:
    /** the factory to create MPAODV routing object */
    ObjectFactory m_agentFactory;
};

/**
 * \ingroup mpaodv
 * \brief PMAODV: probabilistic multipath AODV, p_i proportional to 1/HC_i, k routes.
 */
class PmaodvHelper : public MpaodvHelper
{
  public:
    /**
     * \param maxPaths maximum number of routes per destination (2, 3 or 4)
     */
    PmaodvHelper(uint32_t maxPaths = 3);
};

/**
 * \ingroup mpaodv
 * \brief QMAODV: Q-learning multipath AODV (alpha 0.5, gamma 0.9, epsilon 0.5 decayed by 0.02
 * every 10 s, reward 0.6 ACK + 0.4 / (1 + delay_ms)).
 */
class QmaodvHelper : public MpaodvHelper
{
  public:
    QmaodvHelper();
};

} // namespace ns3

#endif /* MPAODV_HELPER_H */
