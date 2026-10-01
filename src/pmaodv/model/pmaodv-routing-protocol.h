/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/*
 * PM-AOMDV: Probabilistic Multipath AOMDV
 * =========================================
 * Extension of AOMDV for FANET/UAV networks.
 *
 * Core idea: Instead of deterministically selecting the minimum-hop path,
 * PM-AOMDV assigns a selection probability to each valid path that is
 * INVERSELY proportional to its hop count.
 *
 * === Formula (Equation 1 in paper) ===
 *
 *               1 / HopCount(i)
 * P(i) = ─────────────────────────────
 *          Σ_{j ∈ H}  1 / HopCount(j)
 *
 * where H is the set of all valid paths to the destination.
 *
 * === Extended Formula (PM-AOMDV v2, proposed here) ===
 *
 * To also account for link quality (freshness), we weight by:
 *
 *               α(i) / HopCount(i)^β
 * P(i) = ─────────────────────────────────────
 *          Σ_{j ∈ H}  α(j) / HopCount(j)^β
 *
 * where:
 *   α(i) = exp(-λ × AgeRatio(i))     (freshness weight)
 *   AgeRatio(i) = (Now - PathCreated(i)) / MaxPathAge
 *   β = exponent controlling preference for shorter paths (default 1.0)
 *   λ = decay constant for path age (default 0.5)
 *
 * With β=1, λ=0 this reduces exactly to Equation 1 of the paper.
 *
 * Author: Le Trong Hien et al. — FANET/UAV NS-3 simulation
 * NS-3 version: 3.40
 */
#ifndef PMAODV_ROUTING_PROTOCOL_H
#define PMAODV_ROUTING_PROTOCOL_H

#include "../../aomdv/model/aomdv-routing-protocol.h"

namespace ns3 {
namespace pmaodv {

/**
 * \ingroup pmaodv
 * \brief PM-AOMDV: Probabilistic Multipath AOMDV routing protocol
 *
 * Inherits route discovery, maintenance and error handling from AOMDV.
 * Overrides only SelectNextHop() to implement probabilistic route selection.
 */
class RoutingProtocol : public aomdv::RoutingProtocol
{
public:
  static TypeId GetTypeId ();

  RoutingProtocol ();
  virtual ~RoutingProtocol ();

  /**
   * \brief Get β exponent (hop count weight)
   */
  double GetBeta () const { return m_beta; }
  void SetBeta (double beta) { m_beta = beta; }

  /**
   * \brief Get λ (path age decay constant)
   */
  double GetLambda () const { return m_lambda; }
  void SetLambda (double lambda) { m_lambda = lambda; }

protected:
  /**
   * \brief Probabilistic path selection — core contribution of PM-AOMDV.
   *
   * Implements the weighted roulette wheel selection:
   *
   *    w(i) = exp(-λ·AgeRatio(i)) / HopCount(i)^β
   *    P(i) = w(i) / Σ w(j)
   *
   * A uniform random number u ∈ [0,1) is drawn.
   * The path i* with cumulative weight CDF(i*) ≥ u is selected.
   *
   * \param paths  List of valid path entries for this destination
   * \return       Selected next-hop IP address
   */
  Ipv4Address SelectNextHop (const std::vector<aomdv::PathEntry> & paths) override;
  double EstimateLinkLifetime (Ipv4Address neighborIp); ///< Estimate link lifetime (seconds)

private:
  double m_beta;    ///< Exponent for hop-count (default 1.0 → paper formula)
  std::string m_selMode;  ///< Selection mode: prob|maxllt|random
  double m_lambda;  ///< Age decay constant   (default 0.0 → pure hop-count)

  Ptr<UniformRandomVariable> m_rng; ///< RNG for roulette-wheel selection
};

} // namespace pmaodv
} // namespace ns3

#endif /* PMAODV_ROUTING_PROTOCOL_H */
