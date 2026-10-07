/*
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
 */
#include "mpaodv-qtable.h"

#include "ns3/log.h"
#include "ns3/simulator.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>

namespace ns3
{

NS_LOG_COMPONENT_DEFINE("MpaodvQTable");

namespace mpaodv
{

QTable::QTable(uint32_t maxPaths)
    : m_maxPaths(maxPaths),
      m_alpha(0.5),
      m_gamma(0.9),
      m_epsilon(0.3),
      m_w1(0.5),
      m_w2(0.4),
      m_epsilonStep(0.02)
{
    m_uniform = CreateObject<UniformRandomVariable>();
}

void
QTable::SetMaxPaths(uint32_t mp)
{
    NS_ASSERT_MSG(mp >= 1, "MaxPaths must be >= 1");
    m_maxPaths = mp;
}

uint32_t
QTable::GetMaxPaths() const
{
    return m_maxPaths;
}

void
QTable::SetLearningParameters(double alpha0, double gamma, double epsilon0)
{
    NS_ASSERT(alpha0 >= 0.0 && alpha0 <= 1.0);
    NS_ASSERT(gamma >= 0.0 && gamma <= 1.0);
    NS_ASSERT(epsilon0 >= 0.0 && epsilon0 <= 1.0);
    m_alpha = alpha0;
    m_gamma = gamma;
    m_epsilon = epsilon0;
}

void
QTable::SetRewardWeights(double w1, double w2)
{
    m_w1 = w1;
    m_w2 = w2;
}

void
QTable::PeriodicEpsilonDecay()
{
    m_epsilon = std::max(0.0, m_epsilon - m_epsilonStep);
}

double
QTable::ComputeReward(double ackSuccess, double delaySec) const
{
    const double oneMs = 0.001;
    if (delaySec < 0.0)
    {
        delaySec = 0.0;
    }
    return m_w1 * ackSuccess + m_w2 * (1.0 / (1.0 + delaySec / oneMs));
}

std::vector<QRecord>::iterator
QTable::FindWorst(std::vector<QRecord>& vec)
{
    if (vec.empty()) return vec.end();
    auto worst = vec.begin();
    for (auto it = vec.begin() + 1; it != vec.end(); ++it)
    {
        if (it->rt.GetHop() > worst->rt.GetHop()) worst = it;
    }
    return worst;
}

bool
QTable::AddRoute(const RoutingTableEntry& rt)
{
    Ipv4Address dst = rt.GetDestination();
    Ipv4Address nh  = rt.GetNextHop();
    auto& vec = m_records[dst];

    // Dedup
    for (auto& existing : vec)
    {
        if (existing.rt.GetNextHop() == nh)
        {
            existing.rt = rt;
            return false;
        }
    }

    uint32_t capacity = m_maxPaths;
    if (vec.size() < capacity)
    {
        vec.push_back(QRecord(rt, 0.0));
        ReinitQValues(dst);
        return true;
    }
    auto worst = FindWorst(vec);
    if (worst != vec.end() && rt.GetHop() < worst->rt.GetHop())
    {
        *worst = QRecord(rt, 0.0);
        ReinitQValues(dst);
        return true;
    }
    return false;
}

bool
QTable::EnsureRecord(const RoutingTableEntry& rt)
{
    Ipv4Address dst = rt.GetDestination();
    Ipv4Address nh  = rt.GetNextHop();
    auto& vec = m_records[dst];
    for (auto& existing : vec)
    {
        if (existing.rt.GetNextHop() == nh)
        {
            existing.rt = rt;
            return false;
        }
    }
    vec.push_back(QRecord(rt, 0.0));
    ReinitQValues(dst);
    return true;
}

void
QTable::ReinitQValues(Ipv4Address dst)
{
    auto it = m_records.find(dst);
    if (it == m_records.end())
    {
        return;
    }
    double sumInv = 0.0;
    for (const auto& r : it->second)
    {
        sumInv += 1.0 / std::max<uint32_t>(1, r.rt.GetHop());
    }
    if (sumInv <= 0.0)
    {
        return;
    }
    for (auto& r : it->second)
    {
        if (r.txCount > 0)
        {
            continue;
        }
        uint32_t hc = std::max<uint32_t>(1, r.rt.GetHop());
        r.qValue = (1.0 / hc) / sumInv;
    }
}

uint32_t
QTable::GetRoutes(Ipv4Address dst,
                  std::vector<RoutingTableEntry>& routes,
                  const RoutingTable* mainTable) const
{
    auto it = m_records.find(dst);
    if (it == m_records.end()) return 0;
    uint32_t added = 0;
    for (const auto& r : it->second)
    {
        if (r.rt.GetFlag() != VALID || r.rt.GetLifeTime() <= Time(0)) continue;
        if (mainTable != nullptr)
        {
            RoutingTableEntry nbr;
            if (!const_cast<RoutingTable*>(mainTable)->LookupRoute(r.rt.GetNextHop(), nbr) ||
                nbr.GetFlag() != VALID) continue;
        }
        routes.push_back(r.rt);
        ++added;
    }
    return added;
}

std::vector<QRecord>
QTable::BuildCandidates(const RoutingTableEntry& primary, const RoutingTable* mainTable) const
{
    Ipv4Address dst = primary.GetDestination();
    Ipv4Address primNh = primary.GetNextHop();
    std::vector<QRecord> cands;
    auto it = m_records.find(dst);

    double primQ = 0.0;
    bool primFound = false;
    if (it != m_records.end())
    {
        for (const auto& r : it->second)
        {
            if (r.rt.GetNextHop() == primNh)
            {
                primQ = r.qValue;
                primFound = true;
                break;
            }
        }
        for (const auto& r : it->second)
        {
            if (r.rt.GetNextHop() == primNh)
            {
                continue;
            }
            if (r.rt.GetFlag() != VALID || r.rt.GetLifeTime() <= Time(0))
            {
                continue;
            }
            if (mainTable != nullptr)
            {
                RoutingTableEntry nbr;
                if (!const_cast<RoutingTable*>(mainTable)->LookupRoute(r.rt.GetNextHop(), nbr) ||
                    nbr.GetFlag() != VALID)
                {
                    continue;
                }
            }
            cands.push_back(r);
        }
    }

    double primQValue = primQ;
    if (!primFound)
    {
        uint32_t hcP = std::max<uint32_t>(1, primary.GetHop());
        double sumInv = 1.0 / hcP;
        for (const auto& c : cands)
        {
            sumInv += 1.0 / std::max<uint32_t>(1, c.rt.GetHop());
        }
        primQValue = (1.0 / hcP) / sumInv;
    }
    cands.insert(cands.begin(), QRecord(primary, primQValue));
    return cands;
}

bool
QTable::SelectEpsilonGreedy(const RoutingTableEntry& primary,
                            RoutingTableEntry& out,
                            const RoutingTable* mainTable,
                            Ipv4Address exclude)
{
    auto cands = BuildCandidates(primary, mainTable);
    if (cands.size() > m_maxPaths)
    {
        // primary route and the shortest alternates, MaxPaths in total
        std::stable_sort(cands.begin() + 1, cands.end(), [](const QRecord& a, const QRecord& b) {
            return a.rt.GetHop() < b.rt.GetHop();
        });
        cands.resize(m_maxPaths);
    }
    if (exclude != Ipv4Address())
    {
        cands.erase(std::remove_if(cands.begin(),
                                   cands.end(),
                                   [&exclude](const QRecord& c) {
                                       return c.rt.GetNextHop() == exclude;
                                   }),
                    cands.end());
    }
    if (cands.empty())
    {
        out = primary;
        return false;
    }
    if (cands.size() == 1)
    {
        out = cands[0].rt;
        return true;
    }

    if (m_probabilistic)
    {
        double total = 0.0;
        for (const auto& c : cands)
        {
            total += 1.0 / std::max<uint32_t>(1, c.rt.GetHop());
        }
        double u = m_uniform->GetValue(0.0, total);
        double acc = 0.0;
        for (const auto& c : cands)
        {
            acc += 1.0 / std::max<uint32_t>(1, c.rt.GetHop());
            if (u < acc)
            {
                out = c.rt;
                return true;
            }
        }
        out = cands.back().rt;
        return true;
    }

    double u = m_uniform->GetValue(0.0, 1.0);
    if (u < m_epsilon)
    {
        auto idx = static_cast<uint32_t>(m_uniform->GetValue(0.0, static_cast<double>(cands.size())));
        if (idx >= cands.size())
        {
            idx = cands.size() - 1;
        }
        out = cands[idx].rt;
        return true;
    }

    size_t bestIdx = 0;
    double bestQ = -std::numeric_limits<double>::infinity();
    uint32_t bestHC = std::numeric_limits<uint32_t>::max();
    for (size_t i = 0; i < cands.size(); ++i)
    {
        double q = cands[i].qValue;
        uint32_t hc = cands[i].rt.GetHop();
        if (q > bestQ || (std::fabs(q - bestQ) < 1e-9 && hc < bestHC))
        {
            bestQ = q;
            bestHC = hc;
            bestIdx = i;
        }
    }
    out = cands[bestIdx].rt;
    return true;
}

void
QTable::UpdateQValue(Ipv4Address dst,
                     Ipv4Address nextHop,
                     double ackSuccess,
                     double delaySec)
{
    double reward = ComputeReward(ackSuccess, delaySec);
    auto it = m_records.find(dst);
    if (it == m_records.end())
    {
        return;
    }
    QRecord* target = nullptr;
    double maxFuture = 0.0;
    for (auto& r : it->second)
    {
        if (r.qValue > maxFuture)
        {
            maxFuture = r.qValue;
        }
        if (r.rt.GetNextHop() == nextHop)
        {
            target = &r;
        }
    }
    if (target == nullptr)
    {
        return;
    }
    target->qValue =
        (1.0 - m_alpha) * target->qValue + m_alpha * (reward + m_gamma * maxFuture);
    target->txCount += 1;
    if (ackSuccess > 0.5)
    {
        target->ackCount += 1;
    }
}

uint32_t
QTable::Size() const
{
    return std::accumulate(m_records.begin(),
                           m_records.end(),
                           uint32_t{0},
                           [](uint32_t a, const auto& kv) { return a + kv.second.size(); });
}

uint32_t
QTable::CountFor(Ipv4Address dst) const
{
    auto it = m_records.find(dst);
    return (it == m_records.end()) ? 0 : static_cast<uint32_t>(it->second.size());
}

bool
QTable::IsFull(Ipv4Address dst) const
{
    return CountFor(dst) >= m_maxPaths;
}

void
QTable::Clear()
{
    m_records.clear();
}

void
QTable::Print(std::ostream& os) const
{
    os << "Q-table (" << Size() << " routes; alpha=" << m_alpha << " gamma=" << m_gamma
       << " epsilon=" << m_epsilon << "):\n";
    for (const auto& kv : m_records)
    {
        os << "  dst=" << kv.first << "\n";
        for (const auto& r : kv.second)
        {
            os << "    via " << r.rt.GetNextHop() << " HC=" << (uint32_t)r.rt.GetHop()
               << " Q=" << r.qValue << " updates=" << r.txCount << "\n";
        }
    }
}

} // namespace mpaodv
} // namespace ns3
