#include "ns3/string.h"
/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/*
 * PM-AOMDV Routing Protocol — Implementation
 *
 * Key contribution: SelectNextHop() with probabilistic roulette-wheel
 * selection weighted by inverse hop count.
 *
 * Formula:
 *   w(i) = exp(-λ · AgeRatio(i)) / HopCount(i)^β
 *   P(i) = w(i) / Σ_j w(j)
 *
 * Special case (β=1, λ=0):
 *   P(i) = (1/HopCount(i)) / Σ_j (1/HopCount(j))   ← Equation 1 in paper
 */

#include "pmaodv-routing-protocol.h"
#include "ns3/log.h"
#include "ns3/node-list.h"
#include "ns3/mobility-model.h"
#include "ns3/double.h"
#include "ns3/simulator.h"
#include <cmath>
#include <numeric>

namespace ns3 {

NS_LOG_COMPONENT_DEFINE ("PmaodvRoutingProtocol");

namespace pmaodv {

NS_OBJECT_ENSURE_REGISTERED (RoutingProtocol);

TypeId
RoutingProtocol::GetTypeId ()
{
  static TypeId tid = TypeId ("ns3::pmaodv::RoutingProtocol")
    .SetParent<aomdv::RoutingProtocol> ()
    .SetGroupName ("Pmaodv")
    .AddConstructor<RoutingProtocol> ()
    .AddAttribute ("Beta",
                   "Exponent controlling preference for shorter paths. "
                   "β=1 gives the paper's inverse-hop-count formula. "
                   "Higher β increases preference for shorter paths.",
                   DoubleValue (1.0),
                   MakeDoubleAccessor (&RoutingProtocol::m_beta),
                   MakeDoubleChecker<double> (0.1, 10.0))
    .AddAttribute ("SelMode",
               "Path selection mode: prob, maxllt, random",
               StringValue ("prob"),
               MakeStringAccessor (&RoutingProtocol::m_selMode),
               MakeStringChecker ())
.AddAttribute ("Lambda",
                   "Decay constant for path age. "
                   "λ=0 ignores path age (pure hop-count weighting). "
                   "Higher λ penalises older paths more strongly.",
                   DoubleValue (0.0),
                   MakeDoubleAccessor (&RoutingProtocol::m_lambda),
                   MakeDoubleChecker<double> (0.0, 5.0))
  ;
  return tid;
}

RoutingProtocol::RoutingProtocol ()
  : m_beta (1.0),
    m_lambda (1.0)
{
  m_rng = CreateObject<UniformRandomVariable> ();
}

RoutingProtocol::~RoutingProtocol ()
{
}

// ---------------------------------------------------------------------------
// Core PM-AOMDV contribution: probabilistic route selection
// ---------------------------------------------------------------------------


double
RoutingProtocol::EstimateLinkLifetime (Ipv4Address neighborIp)
{
  // Get own mobility model
  Ptr<MobilityModel> myMob = GetIpv4Ptr ()->GetObject<Node> ()->GetObject<MobilityModel> ();
  if (!myMob) return 1.0;
  Vector myPos = myMob->GetPosition ();
  Vector myVel = myMob->GetVelocity ();

  // Find neighbor node by IP
  for (auto it = NodeList::Begin (); it != NodeList::End (); ++it)
    {
      Ptr<Node> node = *it;
      Ptr<Ipv4> ipv4 = node->GetObject<Ipv4> ();
      if (!ipv4) continue;
      for (uint32_t i = 1; i < ipv4->GetNInterfaces (); ++i)
        {
          if (ipv4->GetNAddresses (i) > 0 &&
              ipv4->GetAddress (i, 0).GetLocal () == neighborIp)
            {
              Ptr<MobilityModel> nbMob = node->GetObject<MobilityModel> ();
              if (!nbMob) return 1.0;
              Vector nbPos = nbMob->GetPosition ();
              Vector nbVel = nbMob->GetVelocity ();

              double dx  = myPos.x - nbPos.x;
              double dy  = myPos.y - nbPos.y;
              double dvx = myVel.x - nbVel.x;
              double dvy = myVel.y - nbVel.y;
              const double R = 250.0; // WiFi transmission range (m)

              double a = dvx*dvx + dvy*dvy;
              if (a < 1e-9) return 100.0; // nearly static → very stable

              double b    = 2.0 * (dx*dvx + dy*dvy);
              double c_   = dx*dx + dy*dy - R*R;
              double disc = b*b - 4.0*a*c_;
              if (disc < 0.0) return 0.0; // out of range

              double llt = (-b + std::sqrt (disc)) / (2.0 * a);
              return std::max (0.0, llt);
            }
        }
    }
  return 1.0; // neighbor not found → default
}

Ipv4Address
RoutingProtocol::SelectNextHop (const std::vector<aomdv::PathEntry> & paths)
{
  NS_LOG_FUNCTION (this);

  // ── Edge case: single or empty path list ──
  if (paths.empty ())
    {
      NS_LOG_WARN ("SelectNextHop called with no paths");
      return Ipv4Address ();
    }
  if (paths.size () == 1)
    {
      return paths[0].nextHop;
    }

  // ── Step 1: compute weights w(i) ──
  //
  //   AgeRatio(i) = (Now - path.expireTime + ActiveRouteTimeout) / ActiveRouteTimeout
  //               ≈ age normalised to [0, 1]
  //
  //   w(i) = exp(-λ · AgeRatio(i)) / HopCount(i)^β
  //
  // When λ=0: w(i) = 1 / HopCount(i)^β
  // When β=1, λ=0: P(i) = (1/h_i) / Σ(1/h_j)   ← paper Eq.(1)
  //
  const Time  now    = Simulator::Now ();
  const Time  maxAge = Seconds (6.0);   // ~ActiveRouteTimeout default

  std::vector<double> weights;
  weights.reserve (paths.size ());

  for (const auto & p : paths)
    {
      // Avoid division by zero
      uint8_t hc = (p.hopCount == 0) ? 1 : p.hopCount;

      // Freshness factor
      double ageFraction = 0.0;
      if (m_lambda > 0.0)
        {
          // Normalized LLT: prefer paths with most stable first-hop link.
          // Normalized by max LLT among all paths → speed-adaptive at all speeds.
          double llt = EstimateLinkLifetime (p.nextHop);
          double llt_max = llt;
          for (const auto & q : paths)
            llt_max = std::max (llt_max, EstimateLinkLifetime (q.nextHop));
          ageFraction = (llt_max > 1e-9)
            ? std::max (0.0, std::min (1.0, 1.0 - llt / llt_max)) : 0.0;
        }

      double freshnessWeight = std::exp (-m_lambda * ageFraction);
      double hopWeight       = std::pow (static_cast<double>(hc), m_beta);
      double w               = freshnessWeight / hopWeight;

      weights.push_back (w);

      NS_LOG_DEBUG ("  Path nh=" << p.nextHop
                    << " hops=" << (int)hc
                    << " age=" << ageFraction
                    << " w=" << w);
    }

  // ── Step 2: normalise → probability distribution ──
  double totalWeight = std::accumulate (weights.begin (), weights.end (), 0.0);
  if (totalWeight <= 0.0)
    {
      // Fallback: uniform distribution
      totalWeight = static_cast<double> (weights.size ());
      std::fill (weights.begin (), weights.end (), 1.0);
    }

  // ── Step 3: roulette-wheel (weighted random) selection ──
  double u = m_rng->GetValue (0.0, totalWeight);
  double cumulative = 0.0;

  for (size_t i = 0; i < paths.size (); ++i)
    {
      cumulative += weights[i];
      if (u < cumulative)
        {
          NS_LOG_INFO ("PM-AOMDV selected path " << i
                       << " nh=" << paths[i].nextHop
                       << " hops=" << (int)paths[i].hopCount
                       << " P=" << weights[i] / totalWeight);
          return paths[i].nextHop;
        }
    }

  // Numerical safety: return last path
  return paths.back ().nextHop;
}

} // namespace pmaodv
} // namespace ns3
