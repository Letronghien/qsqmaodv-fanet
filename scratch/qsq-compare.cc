/*
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * qsq-compare.cc — simulation program of the QS-QMAODV study (ns-3.48).
 *
 * One run = one protocol, one scenario, one seed. Results are appended as one CSV row.
 *
 *   Protocols : AODV, AOMDV, PMAODV, QMAODV, QLAODV, QSQMAODV
 *   Scenario  : N UAVs, 3-D Gauss-Markov mobility in a 1000 x 1000 x 300 m box,
 *               IEEE 802.11b ad hoc (data 11 Mbps, broadcasts 1 Mbps unless --bcast11),
 *               Friis channel (optional hard range --rangeM), CBR/UDP traffic:
 *               convergecast N-1 sources -> node 0 (default) or --numFlows random pairs.
 *   Pairing   : the seed fixes mobility and traffic for every protocol (common random
 *               numbers); the column TopoFingerprint makes this checkable.
 *
 * Example:
 *   ./ns3 run "qsq-compare --protocol=QSQMAODV --numNodes=20 --pktInterval=0.1 --seed=1"
 *   ./ns3 run "qsq-compare --protocol=QSQMAODV --qsqAttr=QueueRewardWeight=0;QueueDrivenExploration=false"
 */

#include "ns3/aodv-module.h"
#include "ns3/applications-module.h"
#include "ns3/core-module.h"
#include "ns3/flow-monitor-module.h"
#include "ns3/internet-module.h"
#include "ns3/ipv4-header.h"
#include "ns3/ipv4-l3-protocol.h"
#include "ns3/mobility-module.h"
#include "ns3/network-module.h"
#include "ns3/udp-header.h"
#include "ns3/udp-l4-protocol.h"
#include "ns3/wifi-module.h"

#include "ns3/qsqmaodv-helper.h"
#include "ns3/qsqmaodv-routing-protocol.h"

// Baselines are optional at compile time; requesting a missing one is a runtime error.
// AOMDV, PMAODV and QMAODV come from github.com/Letronghien/ao-am-pm-qm-sa (modules aomdv,
// mpaodv); QL-AODV is our re-implementation (src/qlaodv).
#if __has_include("ns3/mpaodv-helper.h")
#include "ns3/mpaodv-helper.h"
#define QSQ_HAVE_MPAODV 1
#endif
#if __has_include("ns3/mpaodvorig-helper.h")
// only present during `make verify-baseline` (unmodified published module, renamed)
#include "ns3/mpaodvorig-helper.h"
#define QSQ_HAVE_MPAODV_ORIG 1
#endif
#if __has_include("ns3/aomdv-helper.h")
#include "ns3/aomdv-helper.h"
#define QSQ_HAVE_AOMDV 1
#endif
#if __has_include("ns3/qlaodv-helper.h")
#include "ns3/qlaodv-helper.h"
#include "ns3/qlaodv-routing-protocol.h"
#define QSQ_HAVE_QLAODV 1
#endif

#include <algorithm>
#include <fstream>
#include <functional>
#include <iomanip>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

using namespace ns3;

NS_LOG_COMPONENT_DEFINE("QsqCompare");

namespace
{

// ----------------------------------------------------------------------------- configuration
struct SimConfig
{
    std::string protocol = "QSQMAODV";
    std::string tag = "";           ///< free text written to the CSV (experiment id)
    uint32_t seed = 1;
    double simTime = 200.0;         ///< s
    // network
    uint32_t numNodes = 20;
    double areaX = 1000.0, areaY = 1000.0, areaZ = 300.0;
    double gmAlpha = 0.85;          ///< Gauss-Markov memory
    double meanVelMin = 5.0, meanVelMax = 5.0;  ///< m/s
    double txPowerDbm = 16.0;
    double rangeM = 0.0;            ///< > 0: RangePropagationLossModel hard cut
    bool bcast11 = false;           ///< broadcasts at 11 Mbps instead of 1 Mbps
    bool qos = false;               ///< QoS-enabled ad hoc MAC
    uint32_t macQueuePkts = 0;      ///< WifiMacQueue MaxSize, 0 = ns-3 default (500)
    // traffic
    uint32_t pktSize = 512;         ///< UDP payload, bytes
    double pktInterval = 0.25;      ///< s per packet per source
    uint32_t numFlows = 0;          ///< 0 = convergecast N-1 -> 0; > 0 = random pairs
    // routing
    uint32_t maxPaths = 3;
    double qmAlpha = 0.5, qmGamma = 0.9, qmEpsilon = 0.5, qmW1 = 0.6, qmW2 = 0.4;
    std::string qmAttr, qsqAttr, qlAttr, pmAttr;  ///< "Name=Value;..." pass-through
    std::string ctrlPorts = "654,655";            ///< UDP ports of routing control messages
    std::string csvFile = "results.csv";
};

// ----------------------------------------------------------------------------- overhead probe
// Routing overhead is counted at the IP layer of every node: every transmission, including
// broadcast RREQ/HELLO and re-broadcasts (FlowMonitor ignores broadcast packets).
std::set<uint16_t> g_ctrlPorts;
uint64_t g_ctrlPkts = 0;
uint64_t g_ctrlBytes = 0;

void
IpTxTrace(Ptr<const Packet> p, Ptr<Ipv4> /*ipv4*/, uint32_t iface)
{
    if (iface == 0)
    {
        return; // loopback
    }
    Ptr<Packet> c = p->Copy();
    Ipv4Header ih;
    c->RemoveHeader(ih);
    if (ih.GetProtocol() != UdpL4Protocol::PROT_NUMBER || c->GetSize() < 8)
    {
        return;
    }
    UdpHeader uh;
    c->PeekHeader(uh);
    if (g_ctrlPorts.count(uh.GetDestinationPort()))
    {
        g_ctrlPkts++;
        g_ctrlBytes += p->GetSize(); // IP + UDP + routing message
    }
}

// ----------------------------------------------------------------------------- loss diagnostics
// Data packets (UDP to the application port) dropped by IP, by reason; MAC drops of all frames.
uint64_t g_dropTtl = 0;     ///< TTL expired: routing loops / very long detours
uint64_t g_dropNoRoute = 0; ///< no route / route error at an intermediate node
uint64_t g_dropOther = 0;
uint64_t g_macDrops = 0;
const uint16_t kAppPort = 9;

void
IpDropTrace(const Ipv4Header& h, Ptr<const Packet> p, Ipv4L3Protocol::DropReason reason,
            Ptr<Ipv4> /*ipv4*/, uint32_t /*iface*/)
{
    if (h.GetProtocol() != UdpL4Protocol::PROT_NUMBER || p->GetSize() < 8)
    {
        return;
    }
    UdpHeader uh;
    p->PeekHeader(uh);
    if (uh.GetDestinationPort() != kAppPort)
    {
        return; // only data packets
    }
    if (reason == Ipv4L3Protocol::DROP_TTL_EXPIRED)
    {
        g_dropTtl++;
    }
    else if (reason == Ipv4L3Protocol::DROP_NO_ROUTE || reason == Ipv4L3Protocol::DROP_ROUTE_ERROR)
    {
        g_dropNoRoute++;
    }
    else
    {
        g_dropOther++;
    }
}

void
MacDropTrace(WifiMacDropReason /*reason*/, Ptr<const WifiMpdu> /*mpdu*/)
{
    g_macDrops++;
}

// ----------------------------------------------------------------------------- attribute helpers
/// Set an attribute on a routing helper after checking that it exists (clear error otherwise).
template <typename Helper>
void
SetChecked(Helper& h, const std::string& tid, const std::string& name, const AttributeValue& v)
{
    TypeId::AttributeInformation info;
    NS_ABORT_MSG_IF(!TypeId::LookupByName(tid).LookupAttributeByName(name, &info),
                    "Attribute '" << name << "' does not exist on " << tid
                                  << " (list them with --PrintAttributes=" << tid << ")");
    h.Set(name, v);
}

/// Apply "Name=Value;Name=Value" to a routing helper.
template <typename Helper>
void
SetPassThrough(Helper& h, const std::string& tid, const std::string& spec)
{
    std::stringstream ss(spec);
    std::string item;
    while (std::getline(ss, item, ';'))
    {
        if (item.empty())
        {
            continue;
        }
        auto eq = item.find('=');
        NS_ABORT_MSG_IF(eq == std::string::npos, "bad attribute '" << item << "' (Name=Value)");
        SetChecked(h, tid, item.substr(0, eq), StringValue(item.substr(eq + 1)));
    }
}

/// Effective values of the routing attributes matching 'pattern' (read from node 0),
/// written to the CSV so that every row documents the configuration it was produced with.
std::string
DescribeRouting(Ptr<Node> node, const std::string& pattern)
{
    Ptr<Ipv4RoutingProtocol> rp = node->GetObject<Ipv4>()->GetRoutingProtocol();
    std::regex re(pattern);
    std::string out;
    for (TypeId t = rp->GetInstanceTypeId(); t != Ipv4RoutingProtocol::GetTypeId(); t = t.GetParent())
    {
        for (uint32_t i = 0; i < t.GetAttributeN(); ++i)
        {
            TypeId::AttributeInformation info = t.GetAttribute(i);
            if (!std::regex_match(info.name, re))
            {
                continue;
            }
            Ptr<AttributeValue> v = info.checker->Create();
            rp->GetAttribute(info.name, *v);
            out += info.name + "=" + v->SerializeToString(info.checker) + ";";
        }
        if (t.GetParent() == t)
        {
            break;
        }
    }
    return out.empty() ? "-" : out;
}

// ----------------------------------------------------------------------------- routing
/// Install the routing protocol; returns the regex of attributes to document in the CSV.
std::string
SetRouting(InternetStackHelper& internet, const SimConfig& c)
{
    const std::string base = "MaxPaths|Alpha0|Gamma|Epsilon0|RewardW[0-9]|HopByHop|Policy|"
                             "EpsilonDecayInterval";
    if (c.protocol == "AODV")
    {
        AodvHelper aodv;
        internet.SetRoutingHelper(aodv);
        return "^$";
    }
    if (c.protocol == "QSQMAODV")
    {
        // QS-QMAODV = QMAODV (mpaodv, same base parameters) + source-side learning + queue state
        QsqmaodvHelper h;
        const std::string T = "ns3::qsqmaodv::RoutingProtocol";
        SetChecked(h, T, "MaxPaths", UintegerValue(c.maxPaths));
        SetChecked(h, T, "Alpha0", DoubleValue(c.qmAlpha));
        SetChecked(h, T, "Gamma", DoubleValue(c.qmGamma));
        SetChecked(h, T, "Epsilon0", DoubleValue(c.qmEpsilon));
        SetChecked(h, T, "RewardW1", DoubleValue(c.qmW1));
        SetChecked(h, T, "RewardW2", DoubleValue(c.qmW2));
        SetPassThrough(h, T, c.qsqAttr);
        internet.SetRoutingHelper(h);
        return base + "|Queue.*|ExplorationFloor|FailurePenalty|AckSilence.*|Decay.*";
    }
    const std::string mpCfg = "Policy|MaxPaths|Alpha0|Gamma|Epsilon0|RewardW[0-9]|EpsilonDecayInterval";
#ifdef QSQ_HAVE_MPAODV
    if (c.protocol == "QMAODV")
    {
        QmaodvHelper h; // [5]: alpha0 0.5, gamma 0.9, eps0 0.5 (-0.02/10 s), r = 0.6 ACK + 0.4/(1+d_ms)
        const std::string T = "ns3::mpaodv::RoutingProtocol";
        SetChecked(h, T, "MaxPaths", UintegerValue(c.maxPaths));
        SetChecked(h, T, "Alpha0", DoubleValue(c.qmAlpha));
        SetChecked(h, T, "Gamma", DoubleValue(c.qmGamma));
        SetChecked(h, T, "Epsilon0", DoubleValue(c.qmEpsilon));
        SetChecked(h, T, "RewardW1", DoubleValue(c.qmW1));
        SetChecked(h, T, "RewardW2", DoubleValue(c.qmW2));
        SetPassThrough(h, T, c.qmAttr);
        internet.SetRoutingHelper(h);
        return mpCfg;
    }
    if (c.protocol == "PMAODV")
    {
        PmaodvHelper h(c.maxPaths); // [4]: p_i proportional to 1/HC_i over k routes
        SetPassThrough(h, "ns3::mpaodv::RoutingProtocol", c.pmAttr);
        internet.SetRoutingHelper(h);
        return mpCfg;
    }
#endif
#ifdef QSQ_HAVE_MPAODV_ORIG
    if (c.protocol == "QMAODV_ORIG")
    {
        QmaodvOrigHelper h;
        const std::string T = "ns3::mpaodvorig::RoutingProtocol";
        SetChecked(h, T, "MaxPaths", UintegerValue(c.maxPaths));
        SetChecked(h, T, "Alpha0", DoubleValue(c.qmAlpha));
        SetChecked(h, T, "Gamma", DoubleValue(c.qmGamma));
        SetChecked(h, T, "Epsilon0", DoubleValue(c.qmEpsilon));
        SetChecked(h, T, "RewardW1", DoubleValue(c.qmW1));
        SetChecked(h, T, "RewardW2", DoubleValue(c.qmW2));
        internet.SetRoutingHelper(h);
        return "Policy|MaxPaths|Alpha0|Gamma|Epsilon0|RewardW[0-9]";
    }
    if (c.protocol == "PMAODV_ORIG")
    {
        PmaodvOrigHelper h(c.maxPaths);
        internet.SetRoutingHelper(h);
        return "Policy|MaxPaths";
    }
#endif
#ifdef QSQ_HAVE_AOMDV
    if (c.protocol == "AOMDV")
    {
        AomdvHelper h; // Marina & Das, port of the ns-2.35 code
        SetChecked(h, "ns3::aomdv::RoutingProtocol", "MaxPaths", UintegerValue(c.maxPaths));
        internet.SetRoutingHelper(h);
        return "MaxPaths|HelloInterval|ActiveRouteTimeout";
    }
#endif
#ifdef QSQ_HAVE_QLAODV
    if (c.protocol == "QLAODV")
    {
        // Ateya et al., Future Internet 17(10):473, 2025 — Table 2 parameters are the defaults
        QlaodvHelper h;
        SetPassThrough(h, "ns3::qlaodv::RoutingProtocol", c.qlAttr);
        internet.SetRoutingHelper(h);
        return "Ql.*";
    }
#endif
    NS_FATAL_ERROR("Unknown or unavailable protocol '"
                   << c.protocol << "' (AODV, AOMDV, PMAODV, QMAODV, QLAODV, QSQMAODV)");
    return "";
}

// ----------------------------------------------------------------------------- diagnostics
struct Diagnostics
{
    double decisions = 0, nodeQSum = 0, nodeQPos = 0, nodeQMax = 0, nhQSum = 0, nhQPos = 0;
    double fbAck = 0, fbFail = 0, fbTimeout = 0, decayed = 0;
    double qlSelections = 0, qlCandidates = 0, qlDupReplies = 0;
};

Diagnostics
CollectDiagnostics(const NodeContainer& nodes)
{
    Diagnostics d;
    for (uint32_t i = 0; i < nodes.GetN(); ++i)
    {
        Ptr<Ipv4RoutingProtocol> rp = nodes.Get(i)->GetObject<Ipv4>()->GetRoutingProtocol();
        if (auto q = DynamicCast<qsqmaodv::RoutingProtocol>(rp))
        {
            const auto& s = q->GetQsStats();
            d.decisions += s.decisions;
            d.nodeQSum += s.nodeQSum;
            d.nodeQPos += s.nodeQPos;
            d.nodeQMax = std::max(d.nodeQMax, s.nodeQMax);
            d.nhQSum += s.nhQSum;
            d.nhQPos += s.nhQPos;
            d.fbAck += s.fbAck;
            d.fbFail += s.fbDrop;
            d.decayed += s.decayed;
        }
#ifdef QSQ_HAVE_QLAODV
        if (auto q = DynamicCast<qlaodv::RoutingProtocol>(rp))
        {
            const auto& s = q->GetQlStats();
            d.qlSelections += s.selections;
            d.qlCandidates += s.candidatesSum;
            d.qlDupReplies += s.dupReplies;
            d.fbAck += s.fbAck;
            d.fbFail += s.fbFail;
            d.fbTimeout += s.fbTimeout;
        }
#endif
    }
    return d;
}

} // namespace

int
main(int argc, char* argv[])
{
    SimConfig c;
    CommandLine cmd(__FILE__);
    cmd.AddValue("protocol", "AODV|AOMDV|PMAODV|QMAODV|QLAODV|QSQMAODV", c.protocol);
    cmd.AddValue("tag", "experiment id written to the CSV", c.tag);
    cmd.AddValue("seed", "RNG run number (common random numbers across protocols)", c.seed);
    cmd.AddValue("simTime", "simulation time (s)", c.simTime);
    cmd.AddValue("numNodes", "number of UAVs", c.numNodes);
    cmd.AddValue("areaX", "area X (m)", c.areaX);
    cmd.AddValue("areaY", "area Y (m)", c.areaY);
    cmd.AddValue("areaZ", "area Z (m)", c.areaZ);
    cmd.AddValue("gmAlpha", "Gauss-Markov alpha", c.gmAlpha);
    cmd.AddValue("meanVelMin", "minimum mean speed (m/s)", c.meanVelMin);
    cmd.AddValue("meanVelMax", "maximum mean speed (m/s)", c.meanVelMax);
    cmd.AddValue("txPowerDbm", "transmit power (dBm)", c.txPowerDbm);
    cmd.AddValue("rangeM", "hard radio range (m), 0 = Friis only", c.rangeM);
    cmd.AddValue("bcast11", "send broadcasts at 11 Mbps", c.bcast11);
    cmd.AddValue("qos", "QoS-enabled ad hoc MAC", c.qos);
    cmd.AddValue("macQueuePkts", "WifiMacQueue MaxSize (packets), 0 = default", c.macQueuePkts);
    cmd.AddValue("pktSize", "UDP payload (bytes)", c.pktSize);
    cmd.AddValue("pktInterval", "packet interval per source (s)", c.pktInterval);
    cmd.AddValue("numFlows", "0 = convergecast N-1 -> 0, >0 = random src/dst pairs", c.numFlows);
    cmd.AddValue("maxPaths", "maximum paths of the multipath protocols", c.maxPaths);
    cmd.AddValue("qmAlpha", "QMAODV/QS-QMAODV Alpha0", c.qmAlpha);
    cmd.AddValue("qmGamma", "QMAODV/QS-QMAODV Gamma", c.qmGamma);
    cmd.AddValue("qmEpsilon", "QMAODV/QS-QMAODV Epsilon0", c.qmEpsilon);
    cmd.AddValue("qmW1", "QMAODV/QS-QMAODV RewardW1", c.qmW1);
    cmd.AddValue("qmW2", "QMAODV/QS-QMAODV RewardW2", c.qmW2);
    cmd.AddValue("qmAttr", "extra QMAODV attributes Name=Value;...", c.qmAttr);
    cmd.AddValue("qsqAttr", "extra QS-QMAODV attributes Name=Value;...", c.qsqAttr);
    cmd.AddValue("qlAttr", "extra QL-AODV attributes Name=Value;...", c.qlAttr);
    cmd.AddValue("pmAttr", "extra PMAODV attributes Name=Value;...", c.pmAttr);
    cmd.AddValue("ctrlPorts", "routing control UDP ports (comma list)", c.ctrlPorts);
    cmd.AddValue("csvFile", "output CSV (one row appended)", c.csvFile);
    cmd.Parse(argc, argv);

    if (c.macQueuePkts > 0)
    {
        Config::SetDefault("ns3::WifiMacQueue::MaxSize",
                           QueueSizeValue(QueueSize(std::to_string(c.macQueuePkts) + "p")));
    }
    if (c.bcast11)
    {
        Config::SetDefault("ns3::WifiRemoteStationManager::NonUnicastMode",
                           StringValue("DsssRate11Mbps"));
    }
    RngSeedManager::SetSeed(1);
    RngSeedManager::SetRun(c.seed);

    // ------------------------------------------------------------------ nodes and Wi-Fi
    NodeContainer nodes;
    nodes.Create(c.numNodes);

    WifiHelper wifi;
    wifi.SetStandard(WIFI_STANDARD_80211b);
    wifi.SetRemoteStationManager("ns3::ConstantRateWifiManager",
                                 "DataMode", StringValue("DsssRate11Mbps"),
                                 "ControlMode", StringValue("DsssRate1Mbps"));
    YansWifiPhyHelper phy;
    phy.Set("TxPowerStart", DoubleValue(c.txPowerDbm));
    phy.Set("TxPowerEnd", DoubleValue(c.txPowerDbm));
    YansWifiChannelHelper channel;
    channel.SetPropagationDelay("ns3::ConstantSpeedPropagationDelayModel");
    channel.AddPropagationLoss("ns3::FriisPropagationLossModel");
    if (c.rangeM > 0.0)
    {
        channel.AddPropagationLoss("ns3::RangePropagationLossModel", "MaxRange", DoubleValue(c.rangeM));
    }
    phy.SetChannel(channel.Create());
    WifiMacHelper mac;
    mac.SetType("ns3::AdhocWifiMac", "QosSupported", BooleanValue(c.qos));
    NetDeviceContainer devices = wifi.Install(phy, mac, nodes);

    // ------------------------------------------------------------------ mobility (3-D Gauss-Markov)
    auto uniform = [](double lo, double hi) {
        std::ostringstream s;
        s << "ns3::UniformRandomVariable[Min=" << lo << "|Max=" << hi << "]";
        return s.str();
    };
    MobilityHelper mob;
    mob.SetPositionAllocator("ns3::RandomBoxPositionAllocator",
                             "X", StringValue(uniform(0, c.areaX)),
                             "Y", StringValue(uniform(0, c.areaY)),
                             "Z", StringValue(uniform(0, c.areaZ)));
    mob.SetMobilityModel(
        "ns3::GaussMarkovMobilityModel",
        "Bounds", BoxValue(Box(0, c.areaX, 0, c.areaY, 0, c.areaZ)),
        "TimeStep", TimeValue(Seconds(0.5)),
        "Alpha", DoubleValue(c.gmAlpha),
        "MeanVelocity", StringValue(uniform(c.meanVelMin, c.meanVelMax)),
        "MeanDirection", StringValue("ns3::UniformRandomVariable[Min=0|Max=6.283185307]"),
        "MeanPitch", StringValue("ns3::UniformRandomVariable[Min=-0.05|Max=0.05]"),
        "NormalVelocity", StringValue("ns3::NormalRandomVariable[Mean=0.0|Variance=1.0|Bound=2.0]"),
        "NormalDirection", StringValue("ns3::NormalRandomVariable[Mean=0.0|Variance=0.2|Bound=0.4]"),
        "NormalPitch", StringValue("ns3::NormalRandomVariable[Mean=0.0|Variance=0.02|Bound=0.04]"));
    mob.Install(nodes);

    // ------------------------------------------------------------------ IP stack and routing
    InternetStackHelper internet;
    const std::string cfgPattern = SetRouting(internet, c);
    internet.Install(nodes);
    Ipv4AddressHelper addresses;
    addresses.SetBase("10.0.0.0", "255.0.0.0");
    Ipv4InterfaceContainer ifaces = addresses.Assign(devices);

    // ------------------------------------------------------------------ traffic (CBR/UDP)
    struct Flow
    {
        uint32_t src, dst;
    };
    std::vector<Flow> flows;
    if (c.numFlows == 0)
    {
        for (uint32_t i = 1; i < c.numNodes; ++i)
        {
            flows.push_back({i, 0}); // convergecast to node 0
        }
    }
    else
    {
        NS_ABORT_MSG_IF(2 * c.numFlows > c.numNodes, "numFlows * 2 > numNodes");
        Ptr<UniformRandomVariable> u = CreateObject<UniformRandomVariable>();
        std::vector<uint32_t> idx(c.numNodes);
        for (uint32_t k = 0; k < c.numNodes; ++k)
        {
            idx[k] = k;
        }
        for (uint32_t k = c.numNodes - 1; k > 0; --k)
        {
            std::swap(idx[k], idx[u->GetInteger(0, k)]); // Fisher-Yates
        }
        for (uint32_t i = 0; i < c.numFlows; ++i)
        {
            flows.push_back({idx[2 * i], idx[2 * i + 1]});
        }
    }
    const uint16_t port = kAppPort;
    std::set<uint32_t> sinks;
    for (const auto& f : flows)
    {
        sinks.insert(f.dst);
    }
    for (uint32_t s : sinks)
    {
        PacketSinkHelper sink("ns3::UdpSocketFactory", InetSocketAddress(Ipv4Address::GetAny(), port));
        ApplicationContainer a = sink.Install(nodes.Get(s));
        a.Start(Seconds(1.0));
        a.Stop(Seconds(c.simTime));
    }
    const DataRate rate(static_cast<uint64_t>(c.pktSize * 8 / c.pktInterval));
    for (size_t i = 0; i < flows.size(); ++i)
    {
        OnOffHelper src("ns3::UdpSocketFactory", InetSocketAddress(ifaces.GetAddress(flows[i].dst), port));
        src.SetConstantRate(rate, c.pktSize);
        ApplicationContainer a = src.Install(nodes.Get(flows[i].src));
        a.Start(Seconds(5.0 + i * (c.pktInterval * 0.1))); // staggered starts
        a.Stop(Seconds(c.simTime));
    }

    // ------------------------------------------------------------------ measurement
    FlowMonitorHelper fmHelper;
    Ptr<FlowMonitor> flowMon = fmHelper.InstallAll();

    std::stringstream ps(c.ctrlPorts);
    for (std::string tok; std::getline(ps, tok, ',');)
    {
        if (!tok.empty())
        {
            g_ctrlPorts.insert(static_cast<uint16_t>(std::stoi(tok)));
        }
    }
    Config::ConnectWithoutContext("/NodeList/*/$ns3::Ipv4L3Protocol/Tx", MakeCallback(&IpTxTrace));
    Config::ConnectWithoutContext("/NodeList/*/$ns3::Ipv4L3Protocol/Drop", MakeCallback(&IpDropTrace));
    Config::ConnectWithoutContext("/NodeList/*/DeviceList/*/$ns3::WifiNetDevice/Mac/DroppedMpdu",
                                  MakeCallback(&MacDropTrace));

    // node degree within 250/500/1000 m every 5 s; topology fingerprint at t = 50 s
    double degSum[3] = {0, 0, 0};
    double degSamples = 0;
    double topoFp = 0;
    std::function<void()> sampleTopology = [&]() {
        const double R[3] = {250.0, 500.0, 1000.0};
        for (uint32_t a = 0; a < nodes.GetN(); ++a)
        {
            Vector pa = nodes.Get(a)->GetObject<MobilityModel>()->GetPosition();
            for (uint32_t b = 0; b < nodes.GetN(); ++b)
            {
                if (a == b)
                {
                    continue;
                }
                double d = CalculateDistance(pa, nodes.Get(b)->GetObject<MobilityModel>()->GetPosition());
                for (int k = 0; k < 3; ++k)
                {
                    degSum[k] += (d <= R[k]) ? 1.0 : 0.0;
                }
            }
        }
        degSamples += nodes.GetN();
        if (Simulator::Now() + Seconds(5.0) < Seconds(c.simTime))
        {
            Simulator::Schedule(Seconds(5.0), sampleTopology);
        }
    };
    Simulator::Schedule(Seconds(5.0), sampleTopology);
    Simulator::Schedule(Seconds(50.0), [&]() {
        for (uint32_t a = 0; a < nodes.GetN(); ++a)
        {
            Vector p = nodes.Get(a)->GetObject<MobilityModel>()->GetPosition();
            topoFp += p.x + p.y + p.z;
        }
    });

    Simulator::Stop(Seconds(c.simTime));
    Simulator::Run();

    // ------------------------------------------------------------------ metrics
    flowMon->CheckForLostPackets();
    auto classifier = DynamicCast<Ipv4FlowClassifier>(fmHelper.GetClassifier());
    double txPkts = 0, rxPkts = 0, rxBytes = 0, delaySum = 0, forwards = 0, lostFm = 0;
    double flowDelay = 0, flowJitter = 0, flowTput = 0, activeFlows = 0;
    for (const auto& [id, fs] : flowMon->GetFlowStats())
    {
        Ipv4FlowClassifier::FiveTuple t = classifier->FindFlow(id);
        if (g_ctrlPorts.count(t.destinationPort) || g_ctrlPorts.count(t.sourcePort))
        {
            continue; // unicast routing control (counted by IpTxTrace)
        }
        txPkts += fs.txPackets;
        rxPkts += fs.rxPackets;
        rxBytes += fs.rxBytes;
        delaySum += fs.delaySum.GetSeconds();
        forwards += fs.timesForwarded;
        lostFm += fs.lostPackets;
        if (fs.rxPackets > 0)
        {
            flowDelay += fs.delaySum.GetSeconds() / fs.rxPackets;
            flowJitter += (fs.rxPackets > 1) ? fs.jitterSum.GetSeconds() / (fs.rxPackets - 1) : 0;
            flowTput += fs.rxBytes * 8.0 / c.simTime / 1000.0;
            activeFlows += 1;
        }
    }
    auto ratio = [](double a, double b) { return b > 0 ? a / b : 0.0; };
    const double pdr = ratio(rxPkts, txPkts);
    const double delayPw = ratio(delaySum, rxPkts) * 1000;      // packet-weighted, ms
    const double delayFlow = ratio(flowDelay, activeFlows) * 1000; // mean of per-flow means, ms
    const double jitter = ratio(flowJitter, activeFlows) * 1000;
    const double tput = ratio(flowTput, activeFlows);             // kbps per active flow
    const double nrlBytes = ratio(g_ctrlBytes, rxBytes) * 100;    // % of delivered data bytes
    const double nrlPkts = ratio(g_ctrlPkts, rxPkts);             // control packets per delivery
    const Diagnostics d = CollectDiagnostics(nodes);
    const std::string protoCfg = DescribeRouting(nodes.Get(0), cfgPattern);

    std::cout << std::fixed << std::setprecision(3) << "[" << c.protocol << " seed " << c.seed
              << "] PDR " << pdr * 100 << " %, delay " << delayPw << " ms, overhead " << nrlBytes
              << " %\n";

    // ------------------------------------------------------------------ CSV (one row)
    const bool newFile = !std::ifstream(c.csvFile).good();
    std::ofstream ofs(c.csvFile, std::ios::app);
    if (newFile)
    {
        ofs << "Protocol,Tag,Seed,nNodes,nFlows,PktInterval_s,PktSize_B,Speed_ms,GmAlpha,"
               "AreaX,AreaY,AreaZ,TxPowerDbm,RangeM,Bcast11,QoS,MacQueuePkts,MaxPaths,SimTime_s,"
               "TxPkts,RxPkts,PDR,Throughput_kbps,DelayPw_ms,Delay_ms,Jitter_ms,"
               "CtrlPktsAll,CtrlBytesAll,NRLall,NRLpkt,"
               "MeanMacQt,FracMacQtPos,MaxMacQt,MeanNhQ,FracNhQPos,FbAck,FbDrop,FbExpired,"
               "QsDecayed,QlSelections,QlMeanCandidates,QlDupReplies,"
               "Deg250,Deg500,Deg1000,TopoFingerprint,"
               "FwdPerTx,DropTtl,DropNoRoute,DropOtherIp,MacDrops,LostFm,ProtocolCfg\n";
    }
    ofs << std::fixed << std::setprecision(6) << c.protocol << "," << c.tag << "," << c.seed << ","
        << c.numNodes << "," << flows.size() << "," << c.pktInterval << "," << c.pktSize << ","
        << c.meanVelMin << "," << c.gmAlpha << "," << c.areaX << "," << c.areaY << "," << c.areaZ
        << "," << c.txPowerDbm << "," << c.rangeM << "," << c.bcast11 << "," << c.qos << ","
        << c.macQueuePkts << "," << c.maxPaths << "," << c.simTime << ","
        << static_cast<uint64_t>(txPkts) << "," << static_cast<uint64_t>(rxPkts) << "," << pdr
        << "," << tput << "," << delayPw << "," << delayFlow << "," << jitter << "," << g_ctrlPkts
        << "," << g_ctrlBytes << "," << nrlBytes << "," << nrlPkts << ","
        << ratio(d.nodeQSum, d.decisions) << "," << ratio(d.nodeQPos, d.decisions) << ","
        << d.nodeQMax << "," << ratio(d.nhQSum, d.decisions) << "," << ratio(d.nhQPos, d.decisions)
        << "," << static_cast<uint64_t>(d.fbAck) << "," << static_cast<uint64_t>(d.fbFail) << ","
        << static_cast<uint64_t>(d.fbTimeout) << "," << static_cast<uint64_t>(d.decayed) << ","
        << static_cast<uint64_t>(d.qlSelections) << ","
        << ratio(d.qlCandidates, d.qlSelections) << "," << static_cast<uint64_t>(d.qlDupReplies)
        << "," << ratio(degSum[0], degSamples) << "," << ratio(degSum[1], degSamples) << ","
        << ratio(degSum[2], degSamples) << "," << topoFp << "," << ratio(forwards, txPkts) << ","
        << g_dropTtl << "," << g_dropNoRoute << "," << g_dropOther << "," << g_macDrops << ","
        << static_cast<uint64_t>(lostFm) << "," << protoCfg
        << "\n";
    ofs.close();

    Simulator::Destroy();
    return 0;
}
