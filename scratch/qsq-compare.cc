/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/**
 * qsq-compare.cc — FANET protocol comparison simulator (QS-QMAODV project, ns-3.48)
 *
 * v2 changes: every swept parameter is written to the CSV; QS2MAODV v2 switches;
 * optional energy model (off by default; see note); packet-weighted delay;
 * cross-layer diagnostics (MAC queue signal, MAC feedback counts);
 * topology fingerprint to verify common-random-number pairing across protocols;
 * connectivity statistics; optional range-limited channel and 11 Mbps broadcasts.
 *
 * Protocols: AODV | PMAODV | QMAODV | QS2MAODV
 * Based on fanet-sim.cc infrastructure (proven working).
 *
 * Paper parameters (QS-QMAODV Q3 baseline):
 *   Area     : 1000×1000×300 m  (Gauss-Markov 3D)
 *   MAC      : IEEE 802.11b (16 dBm)
 *   Traffic  : CBR/UDP, 512B, pktInterval=0.25s
 *   Energy   : E0=50J
 *   N=15, simTime=200s, seed=1
 *
 * Usage:
 *   ./ns3 run "qsq-compare --protocol=AODV     --numNodes=15 --seed=1"
 *   ./ns3 run "qsq-compare --protocol=PMAODV   --numNodes=15 --seed=1"
 *   ./ns3 run "qsq-compare --protocol=QMAODV   --numNodes=15 --seed=1"
 *   ./ns3 run "qsq-compare --protocol=QS2MAODV --numNodes=15 --seed=1"
 */

#include "ns3/core-module.h"
#include "ns3/ipv4-l3-protocol.h"
#include "ns3/ipv4-header.h"
#include "ns3/udp-header.h"
#include "ns3/network-module.h"
#include "ns3/internet-module.h"
#include "ns3/wifi-module.h"
#include "ns3/mobility-module.h"
#include "ns3/applications-module.h"
#include "ns3/flow-monitor-module.h"
#include "ns3/energy-module.h"
#include "ns3/aodv-module.h"
// Baseline modules are optional: the simulator builds without them and reports
// a clear error only if --protocol=PMAODV/QMAODV is requested.
#if __has_include("ns3/pmaodv-module.h")
#include "ns3/pmaodv-module.h"
#define QSQ_HAVE_PMAODV 1
#endif
#if __has_include("ns3/qmaodv-module.h")
#include "ns3/qmaodv-module.h"
#define QSQ_HAVE_QMAODV 1
#endif
#include "ns3/qs2maodv-helper.h"
#include "ns3/qs2maodv-routing-protocol.h"
#include <iostream>
#include <fstream>
#include <iomanip>
#include <set>
#include <vector>
#include <sstream>
#include <functional>
#include <algorithm>

using namespace ns3;
NS_LOG_COMPONENT_DEFINE("QsqCompare");

// ---------------------------------------------------------------------------
// Baseline configuration helpers.
// The PMAODV/QMAODV modules come from ~/nbqmaodv-fanet/ns-3-nbq; their attribute
// names differ from the old ns-3.40 versions (e.g. Alpha -> Alpha0). Every value is
// checked against the TypeId before it is set, and the effective configuration is
// written to the CSV column BaselineCfg so the paper can report it exactly.
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// Routing overhead counted at the IP layer of every node (all transmissions,
// INCLUDING broadcast RREQ / HELLO and every re-broadcast).
// FlowMonitor ignores broadcast packets (Ipv4FlowProbe: "not prepared to handle
// broadcast"), so the v1 CtrlBytes/NRL only contained unicast RREP/RERR.
// ---------------------------------------------------------------------------
static std::set<uint16_t> g_ctrlPorts = {654, 655};
static uint64_t g_ctrlPktsAll = 0;
static uint64_t g_ctrlBytesAll = 0;

static void
IpTxTrace(Ptr<const Packet> p, Ptr<Ipv4> /*ipv4*/, uint32_t iface)
{
    if (iface == 0) return;                         // loopback
    Ptr<Packet> c = p->Copy();
    Ipv4Header ih;
    c->RemoveHeader(ih);
    if (ih.GetProtocol() != 17 || c->GetSize() < 8) return;   // UDP only
    UdpHeader uh;
    c->PeekHeader(uh);
    uint16_t dp = uh.GetDestinationPort();
    if (g_ctrlPorts.count(dp))
    {
        g_ctrlPktsAll++;
        g_ctrlBytesAll += p->GetSize();             // IP + UDP + routing message
    }
}

template <typename Helper>
void SetChecked(Helper& h, const std::string& tid, const std::string& name,
                const AttributeValue& v, std::string& cfg)
{
    TypeId t = TypeId::LookupByName(tid);
    TypeId::AttributeInformation info;
    if (!t.LookupAttributeByName(name, &info))
    {
        NS_FATAL_ERROR("Attribute '" << name << "' does not exist on " << tid
                       << ". List them with: --PrintAttributes=" << tid);
    }
    h.Set(name, v);
    cfg += name + "=" + v.SerializeToString(info.checker) + ";";
}

// --qmAttr / --pmAttr pass-through: "Name=Value;Name=Value" (any attribute of the module)
template <typename Helper>
void SetPassThrough(Helper& h, const std::string& tid, const std::string& spec, std::string& cfg)
{
    std::stringstream ss(spec);
    std::string item;
    while (std::getline(ss, item, ';'))
    {
        if (item.empty()) continue;
        auto eq = item.find('=');
        NS_ABORT_MSG_IF(eq == std::string::npos, "bad attribute spec '" << item << "' (use Name=Value)");
        SetChecked(h, tid, item.substr(0, eq), StringValue(item.substr(eq + 1)), cfg);
    }
}

int
main(int argc, char* argv[])
{
    // ====== Parameters ======
    std::string protocol     = "QS2MAODV";
    uint32_t    maxPaths     = 3;
    uint32_t    numNodes     = 15;
    double      simTime      = 200.0;
    uint32_t    seed         = 1;
    double      initialEnergy = 50.0;
    double      txPowerDbm   = 16.0;
    double      areaX        = 1000.0;
    double      areaY        = 1000.0;
    double      areaZ        = 300.0;
    double      gmAlpha      = 0.85;
    double      meanVelMin   = 15.0;
    double      meanVelMax   = 25.0;
    uint32_t    pktSize      = 512;
    double      pktInterval  = 0.25;
    uint32_t    numFlows     = 0;      // 0 = N-1 sources → sink 0
    std::string csvFile      = "results.csv";

    // QMAODV Q-learning params (paper Table 3)
    double qmAlpha        = 0.5;
    double qmGamma        = 0.9;
    double qmEpsilon      = 0.5;
    double qmW1           = 0.6;
    double qmW2           = 0.4;
    std::string ctrlPorts = "654,655";  // UDP ports of routing control messages
    std::string qmAttr;            // extra QMAODV attributes "Name=Value;..."
    std::string pmAttr;            // extra PMAODV attributes "Name=Value;..."
    std::string baselineCfg;       // effective baseline configuration (CSV)
  double ackSilenceThreshold = 15.0;  // W2 sweep: ACK-silence threshold (s)
  double decayFactor         = 0.92;  // W3 sweep: Q-value decay multiplier

    // QS2MAODV params (paper Table 3)
    double qsAlpha   = 0.30;
    double qsGamma   = 0.90;
    double qsEpsilon = 0.30;
    double qsW1      = 0.40;
    double qsW2      = 0.50;
    double qsW3      = 0.10;
  bool   enableDecay = true;   // ablation: ACK-silence decay
  bool   adaptiveW3  = true;   // ablation: adaptive w3
  bool   trendEps    = true;   // ablation: trend epsilon
    // [v2] QS2MAODV correctness / ablation switches (see qs2maodv-routing-protocol.cc)
    bool   macQueueSignal = true;
    bool   macFeedback  = true;
    bool   nextHopQueue = true;
    bool   queueState   = true;
    bool   queueEps     = true;
    bool   hybridSelect = true;
    bool   decayPosOnly = true;
    double delayScale   = 0.05;
    // [v2] scenario options
    bool        useEnergy    = false;  // energy model had no effect on networking in v1 (see report)
    bool        qos          = false;  // QosSupported on the ad hoc MAC
    uint32_t    macQueuePkts = 0;      // 0 = ns-3 default (500p); paper Table 5 states 100
    double      rangeM       = 0.0;    // >0: add RangePropagationLossModel (hard cut at rangeM)
    bool        bcast11      = false;  // broadcast/control frames at 11 Mbps instead of 1 Mbps
    bool        randomPairs  = false;  // numFlows>0: random distinct src/dst pairs instead of i -> N-1-i
    std::string tag          = "";     // free text written to the CSV (experiment / variant id)

    CommandLine cmd(__FILE__);
    cmd.AddValue("protocol",       "AODV|PMAODV|QMAODV|QS2MAODV",       protocol);
    cmd.AddValue("maxPaths",       "Max paths (multipath protocols)",      maxPaths);
    cmd.AddValue("numNodes",       "Number of UAV nodes",                  numNodes);
    cmd.AddValue("simTime",        "Simulation time (s)",                  simTime);
    cmd.AddValue("seed",           "RNG seed",                             seed);
    cmd.AddValue("initialEnergy",  "Initial energy per node (J)",          initialEnergy);
    cmd.AddValue("txPowerDbm",     "Tx power (dBm)",                       txPowerDbm);
    cmd.AddValue("areaX",          "Area X (m)",                           areaX);
    cmd.AddValue("areaY",          "Area Y (m)",                           areaY);
    cmd.AddValue("areaZ",          "Area Z (m)",                           areaZ);
    cmd.AddValue("gmAlpha",        "Gauss-Markov alpha",                   gmAlpha);
    cmd.AddValue("meanVelMin",     "Min UAV velocity (m/s)",               meanVelMin);
    cmd.AddValue("meanVelMax",     "Max UAV velocity (m/s)",               meanVelMax);
    cmd.AddValue("pktSize",        "UDP payload size (bytes)",              pktSize);
    cmd.AddValue("pktInterval",    "Packet interval (s)",                  pktInterval);
    cmd.AddValue("numFlows",       "0=N-1 srcs->sink0; >0=N pairs",       numFlows);
    cmd.AddValue("csvFile",        "Output CSV file",                      csvFile);
    cmd.AddValue("qmAlpha",        "QMAODV learning rate",                 qmAlpha);
    cmd.AddValue("qmGamma",        "QMAODV discount factor",               qmGamma);
    cmd.AddValue("qmEpsilon",      "QMAODV initial epsilon",               qmEpsilon);
    cmd.AddValue("qmW1",           "QMAODV reward w1 (ACK)",               qmW1);
    cmd.AddValue("qmW2",           "QMAODV reward w2 (delay)",             qmW2);
    cmd.AddValue("ctrlPorts",      "routing control UDP ports, comma list", ctrlPorts);
    cmd.AddValue("qmAttr",         "extra QMAODV attributes Name=Value;...", qmAttr);
    cmd.AddValue("pmAttr",         "extra PMAODV attributes Name=Value;...", pmAttr);
    cmd.AddValue("qsAlpha",        "QS2MAODV learning rate",               qsAlpha);
    cmd.AddValue("qsGamma",        "QS2MAODV discount factor",             qsGamma);
    cmd.AddValue("qsEpsilon",      "QS2MAODV initial epsilon",             qsEpsilon);
    cmd.AddValue("qsW1",           "QS2MAODV reward w1 (ACK)",             qsW1);
    cmd.AddValue("qsW2",           "QS2MAODV reward w2 (delay)",           qsW2);
    cmd.AddValue("enableDecay", "QS2MAODV ablation: enable ACK-silence decay (true)", enableDecay);
  cmd.AddValue("adaptiveW3",  "QS2MAODV ablation: enable adaptive w3 (true)",        adaptiveW3);
  cmd.AddValue("trendEps",    "QS2MAODV ablation: enable trend epsilon (true)",       trendEps);
  cmd.AddValue("qsW3",           "QS2MAODV reward w3 (energy)",          qsW3);
  cmd.AddValue ("ackSilenceThreshold", "QS2MAODV ACK-silence threshold (s)", ackSilenceThreshold);
  cmd.AddValue ("decayFactor",         "QS2MAODV Q-value decay multiplier",  decayFactor);
    cmd.AddValue("macQueueSignal", "[v2] read MAC BE queue (false = v1)",  macQueueSignal);
    cmd.AddValue("macFeedback",  "[v2] reward from MAC ACK/drop",            macFeedback);
    cmd.AddValue("nextHopQueue", "[v2] per-next-hop MAC queue",              nextHopQueue);
    cmd.AddValue("queueState",   "[v2] queue bucket in Q-state",             queueState);
    cmd.AddValue("queueEps",     "[v2] queue-triggered epsilon",             queueEps);
    cmd.AddValue("hybridSelect", "[v2] hybrid Q*(1-q)^beta selection",       hybridSelect);
    cmd.AddValue("decayPosOnly", "[v2] decay only positive Q",               decayPosOnly);
    cmd.AddValue("delayScale",   "[v2] delay normalisation (s)",             delayScale);
    cmd.AddValue("energy",       "[v2] install WifiRadioEnergyModel",        useEnergy);
    cmd.AddValue("qos",          "[v2] QoS-enabled ad hoc MAC",              qos);
    cmd.AddValue("macQueuePkts", "[v2] WifiMacQueue MaxSize (0=default)",    macQueuePkts);
    cmd.AddValue("rangeM",       "[v2] hard radio range in m (0=off)",       rangeM);
    cmd.AddValue("bcast11",      "[v2] broadcasts at 11 Mbps",               bcast11);
    cmd.AddValue("randomPairs",  "[v2] random src/dst pairs",                randomPairs);
    cmd.AddValue("tag",          "[v2] experiment tag for the CSV",          tag);
    cmd.Parse(argc, argv);

    if (macQueuePkts > 0)
        Config::SetDefault("ns3::WifiMacQueue::MaxSize",
                           QueueSizeValue(QueueSize(std::to_string(macQueuePkts) + "p")));
    RngSeedManager::SetSeed(1);
    RngSeedManager::SetRun(seed);

    std::cout << "=== qsq-compare === protocol=" << protocol
              << " N=" << numNodes << " T=" << simTime
              << "s seed=" << seed << " E0=" << initialEnergy << "J\n";

    // ====== Nodes ======
    NodeContainer nodes;
    nodes.Create(numNodes);

    // ====== WiFi 802.11b ad-hoc (same as fanet-sim) ======
    WifiHelper wifi;
    wifi.SetStandard(WIFI_STANDARD_80211b);
    wifi.SetRemoteStationManager("ns3::ConstantRateWifiManager",
                                 "DataMode",    StringValue("DsssRate11Mbps"),
                                 "ControlMode", StringValue("DsssRate1Mbps"));
    if (bcast11)
        Config::SetDefault("ns3::WifiRemoteStationManager::NonUnicastMode",
                           StringValue("DsssRate11Mbps"));
    YansWifiPhyHelper wifiPhy;
    wifiPhy.Set("TxPowerStart", DoubleValue(txPowerDbm));
    wifiPhy.Set("TxPowerEnd",   DoubleValue(txPowerDbm));
    YansWifiChannelHelper wifiChannel;
    wifiChannel.SetPropagationDelay("ns3::ConstantSpeedPropagationDelayModel");
    wifiChannel.AddPropagationLoss("ns3::FriisPropagationLossModel");
    if (rangeM > 0.0)
        wifiChannel.AddPropagationLoss("ns3::RangePropagationLossModel",
                                       "MaxRange", DoubleValue(rangeM));
    wifiPhy.SetChannel(wifiChannel.Create());
    WifiMacHelper wifiMac;
    wifiMac.SetType("ns3::AdhocWifiMac", "QosSupported", BooleanValue(qos));
    NetDeviceContainer devices = wifi.Install(wifiPhy, wifiMac, nodes);

    // ====== Gauss-Markov Mobility (same as fanet-sim GAUSS) ======
    MobilityHelper mob;
    std::ostringstream velStr, xStr, yStr, zStr;
    velStr << "ns3::UniformRandomVariable[Min=" << meanVelMin << "|Max=" << meanVelMax << "]";
    xStr   << "ns3::UniformRandomVariable[Min=0|Max=" << areaX << "]";
    yStr   << "ns3::UniformRandomVariable[Min=0|Max=" << areaY << "]";
    zStr   << "ns3::UniformRandomVariable[Min=0|Max=" << areaZ << "]";
    mob.SetPositionAllocator("ns3::RandomBoxPositionAllocator",
                             "X", StringValue(xStr.str()),
                             "Y", StringValue(yStr.str()),
                             "Z", StringValue(zStr.str()));
    mob.SetMobilityModel(
        "ns3::GaussMarkovMobilityModel",
        "Bounds",          BoxValue(Box(0, areaX, 0, areaY, 0, areaZ)),
        "TimeStep",        TimeValue(Seconds(0.5)),
        "Alpha",           DoubleValue(gmAlpha),
        "MeanVelocity",    StringValue(velStr.str()),
        "MeanDirection",   StringValue("ns3::UniformRandomVariable[Min=0|Max=6.283185307]"),
        "MeanPitch",       StringValue("ns3::UniformRandomVariable[Min=-0.05|Max=0.05]"),
        "NormalVelocity",  StringValue("ns3::NormalRandomVariable[Mean=0.0|Variance=1.0|Bound=2.0]"),
        "NormalDirection", StringValue("ns3::NormalRandomVariable[Mean=0.0|Variance=0.2|Bound=0.4]"),
        "NormalPitch",     StringValue("ns3::NormalRandomVariable[Mean=0.0|Variance=0.02|Bound=0.04]"));
    mob.Install(nodes);

    // ====== Internet stack + routing ======
    InternetStackHelper internet;
    if (protocol == "AODV") {
        AodvHelper aodv;
        internet.SetRoutingHelper(aodv);
    } else if (protocol == "PMAODV") {
#ifdef QSQ_HAVE_PMAODV
        PmaodvHelper pmaodv;
        const std::string T = "ns3::pmaodv::RoutingProtocol";
        SetChecked(pmaodv, T, "MaxPaths", UintegerValue(maxPaths), baselineCfg);
        SetPassThrough(pmaodv, T, pmAttr, baselineCfg);
        internet.SetRoutingHelper(pmaodv);
#else
        NS_FATAL_ERROR("PMAODV module not found: import it: bash tools/import_baselines.sh && make setup");
#endif
    } else if (protocol == "QMAODV") {
#ifdef QSQ_HAVE_QMAODV
        QmaodvHelper qmaodv;
        const std::string T = "ns3::qmaodv::RoutingProtocol";
        SetChecked(qmaodv, T, "MaxPaths", UintegerValue(maxPaths), baselineCfg);
        SetChecked(qmaodv, T, "Alpha0",   DoubleValue(qmAlpha),    baselineCfg);
        SetChecked(qmaodv, T, "Gamma",    DoubleValue(qmGamma),    baselineCfg);
        SetChecked(qmaodv, T, "Epsilon0", DoubleValue(qmEpsilon),  baselineCfg);
        SetChecked(qmaodv, T, "RewardW1", DoubleValue(qmW1),       baselineCfg);
        SetChecked(qmaodv, T, "RewardW2", DoubleValue(qmW2),       baselineCfg);
        SetPassThrough(qmaodv, T, qmAttr, baselineCfg);
        internet.SetRoutingHelper(qmaodv);
#else
        NS_FATAL_ERROR("QMAODV module not found: import it: bash tools/import_baselines.sh && make setup");
#endif
    } else if (protocol == "QS2MAODV") {
        Qs2maodvHelper qs2maodv;
        qs2maodv.Set("MaxPaths",      UintegerValue(maxPaths));
        qs2maodv.Set("Alpha",         DoubleValue(qsAlpha));
        qs2maodv.Set("EnableDecay", BooleanValue(enableDecay));
  qs2maodv.Set ("SilenceThreshold", DoubleValue (ackSilenceThreshold));
  qs2maodv.Set ("DecayFactor",      DoubleValue (decayFactor));
        qs2maodv.Set("AdaptiveW3",  BooleanValue(adaptiveW3));
        qs2maodv.Set("TrendEps",    BooleanValue(trendEps));
        qs2maodv.Set("Gamma",         DoubleValue(qsGamma));
        qs2maodv.Set("Epsilon",       DoubleValue(qsEpsilon));
        qs2maodv.Set("RewardW1",      DoubleValue(qsW1));
        qs2maodv.Set("RewardW2",      DoubleValue(qsW2));
        qs2maodv.Set("RewardW3",      DoubleValue(qsW3));
        qs2maodv.Set("InitialEnergy", DoubleValue(initialEnergy));
        qs2maodv.Set("MacQueueSignal",    BooleanValue(macQueueSignal));
        qs2maodv.Set("MacFeedback",       BooleanValue(macFeedback));
        qs2maodv.Set("NextHopQueue",      BooleanValue(nextHopQueue));
        qs2maodv.Set("QueueState",        BooleanValue(queueState));
        qs2maodv.Set("QueueEpsilon",      BooleanValue(queueEps));
        qs2maodv.Set("HybridSelect",      BooleanValue(hybridSelect));
        qs2maodv.Set("DecayPositiveOnly", BooleanValue(decayPosOnly));
        qs2maodv.Set("DelayScale",        DoubleValue(delayScale));
        internet.SetRoutingHelper(qs2maodv);
    } else {
        NS_FATAL_ERROR("Unknown protocol: " << protocol
                       << ". Use AODV, PMAODV, QMAODV, or QS2MAODV.");
    }
    internet.Install(nodes);

    Ipv4AddressHelper addresses;
    addresses.SetBase("10.0.0.0", "255.0.0.0");
    Ipv4InterfaceContainer interfaces = addresses.Assign(devices);

    // ====== Energy model ======
    // [v2] In v1 every battery reached its depletion threshold in every run (consumed
    // energy = 72-73 % of E0 for E0 = 1...50 J) while network results were bit-identical
    // across E0. Energy is therefore OFF by default; enable only for a dedicated study.
    energy::EnergySourceContainer sources;
    if (useEnergy)
    {
        BasicEnergySourceHelper esHelper;
        esHelper.Set("BasicEnergySourceInitialEnergyJ", DoubleValue(initialEnergy));
        sources = esHelper.Install(nodes);
        WifiRadioEnergyModelHelper wifiEnergyHelper;
        wifiEnergyHelper.Install(devices, sources);
    }

    // ====== Traffic ======
    uint16_t port = 9;
    struct FlowSpec { uint32_t src; uint32_t dst; };
    std::vector<FlowSpec> flows;
    if (numFlows == 0) {
        // N-1 sources → sink 0 (paper default)
        for (uint32_t i = 1; i < numNodes; ++i)
            flows.push_back({i, 0});
    } else {
        if (numFlows * 2 > numNodes)
            NS_FATAL_ERROR("numFlows*2 > numNodes");
        if (randomPairs)
        {
            Ptr<UniformRandomVariable> u = CreateObject<UniformRandomVariable>();
            std::vector<uint32_t> idx(numNodes);
            for (uint32_t k = 0; k < numNodes; ++k) idx[k] = k;
            for (uint32_t k = numNodes - 1; k > 0; --k)          // Fisher-Yates
                std::swap(idx[k], idx[u->GetInteger(0, k)]);
            for (uint32_t i = 0; i < numFlows; ++i)
                flows.push_back({idx[2 * i], idx[2 * i + 1]});
        }
        else
        {
            for (uint32_t i = 0; i < numFlows; ++i)
                flows.push_back({i, numNodes - 1 - i});
        }
    }

    // Sink on node 0 (or all unique dst nodes)
    std::set<uint32_t> sinkIdxs;
    for (auto& f : flows) sinkIdxs.insert(f.dst);
    for (uint32_t s : sinkIdxs) {
        PacketSinkHelper sink("ns3::UdpSocketFactory",
            InetSocketAddress(Ipv4Address::GetAny(), port));
        ApplicationContainer a = sink.Install(nodes.Get(s));
        a.Start(Seconds(1.0));
        a.Stop(Seconds(simTime));
    }

    uint64_t dataRateBps = (uint64_t)(pktSize * 8 / pktInterval);
    std::ostringstream rateStr;
    rateStr << dataRateBps << "bps";

    for (size_t i = 0; i < flows.size(); ++i) {
        OnOffHelper src("ns3::UdpSocketFactory",
            InetSocketAddress(interfaces.GetAddress(flows[i].dst), port));
        src.SetConstantRate(DataRate(rateStr.str()), pktSize);
        double startT = 5.0 + i * (pktInterval * 0.1);
        ApplicationContainer a = src.Install(nodes.Get(flows[i].src));
        a.Start(Seconds(startT));
        a.Stop(Seconds(simTime));
    }

    // ====== FlowMonitor ======
    FlowMonitorHelper fmHelper;
    Ptr<FlowMonitor> flowMon = fmHelper.InstallAll();

    {
        g_ctrlPorts.clear();
        std::stringstream ps(ctrlPorts); std::string tok;
        while (std::getline(ps, tok, ',')) if (!tok.empty()) g_ctrlPorts.insert(static_cast<uint16_t>(std::stoi(tok)));
    }
    Config::ConnectWithoutContext("/NodeList/*/$ns3::Ipv4L3Protocol/Tx", MakeCallback(&IpTxTrace));

    // ====== [v2] Connectivity sampling & topology fingerprint ======
    // Degree = neighbours within 250 / 500 / 1000 m, sampled every 5 s.
    // Fingerprint = sum of node coordinates at t = 50 s: identical across protocols
    // for the same seed <=> common random numbers hold and paired tests are valid.
    double degSum[3] = {0, 0, 0}; uint32_t degSamples = 0; double topoFp = 0.0;
    std::function<void()> sampleTopo = [&]() {
        const double R[3] = {250.0, 500.0, 1000.0};
        for (uint32_t a = 0; a < nodes.GetN(); ++a)
        {
            Vector pa = nodes.Get(a)->GetObject<MobilityModel>()->GetPosition();
            for (uint32_t b = 0; b < nodes.GetN(); ++b)
            {
                if (a == b) continue;
                double d = CalculateDistance(pa, nodes.Get(b)->GetObject<MobilityModel>()->GetPosition());
                for (int k = 0; k < 3; ++k) if (d <= R[k]) degSum[k] += 1.0;
            }
        }
        degSamples += nodes.GetN();
        if (Simulator::Now() + Seconds(5.0) < Seconds(simTime))
            Simulator::Schedule(Seconds(5.0), sampleTopo);
    };
    Simulator::Schedule(Seconds(5.0), sampleTopo);
    Simulator::Schedule(Seconds(50.0), [&]() {
        for (uint32_t a = 0; a < nodes.GetN(); ++a)
        {
            Vector p = nodes.Get(a)->GetObject<MobilityModel>()->GetPosition();
            topoFp += p.x + p.y + p.z;
        }
    });

    // ====== Run ======
    Simulator::Stop(Seconds(simTime));
    Simulator::Run();

    // ====== Metrics ======
    flowMon->CheckForLostPackets();
    Ptr<Ipv4FlowClassifier> classifier =
        DynamicCast<Ipv4FlowClassifier>(fmHelper.GetClassifier());
    FlowMonitor::FlowStatsContainer stats = flowMon->GetFlowStats();

    // Routing control ports: AODV=654, QS2MAODV/QMAODV/PMAODV=655
    auto isCtrl = [](uint16_t p) { return p == 654 || p == 655; };

    double txPkts=0, rxPkts=0, ctrlBytes=0, dataRxBytes=0;
    double sumDelay=0, sumJitter=0, sumTput=0, delaySumAll=0;
    uint32_t flowCount=0;

    for (auto& kv : stats) {
        Ipv4FlowClassifier::FiveTuple t = classifier->FindFlow(kv.first);
        const FlowMonitor::FlowStats& fs = kv.second;
        if (isCtrl(t.destinationPort) || isCtrl(t.sourcePort)) {
            ctrlBytes += fs.txBytes; continue;
        }
        txPkts      += fs.txPackets;
        rxPkts      += fs.rxPackets;
        dataRxBytes += fs.rxBytes;
        delaySumAll += fs.delaySum.GetSeconds();
        if (fs.rxPackets > 0) {
            sumDelay  += fs.delaySum.GetSeconds()  / fs.rxPackets;
            sumJitter += (fs.rxPackets > 1)
                         ? fs.jitterSum.GetSeconds() / (fs.rxPackets - 1) : 0;
            sumTput   += fs.rxBytes * 8.0 / simTime / 1000.0;
            ++flowCount;
        }
    }

    double pdr      = (txPkts  > 0) ? rxPkts / txPkts  : 0;
    double delay_ms = (flowCount > 0) ? sumDelay  / flowCount * 1000 : 0;
    double jitter_ms= (flowCount > 0) ? sumJitter / flowCount * 1000 : 0;
    double tput_kbps= (flowCount > 0) ? sumTput   / flowCount        : 0;
    double nrl      = (dataRxBytes > 0) ? ctrlBytes / dataRxBytes * 100 : 0;
    double delayPw_ms = (rxPkts > 0) ? delaySumAll / rxPkts * 1000 : 0;   // packet-weighted
    // [v2] overhead including broadcast control traffic
    double nrlAll     = (dataRxBytes > 0) ? g_ctrlBytesAll / dataRxBytes * 100 : 0;   // % bytes
    double nrlPkt     = (rxPkts > 0) ? static_cast<double>(g_ctrlPktsAll) / rxPkts : 0; // ctrl pkts per delivered pkt

    // [v2] cross-layer diagnostics aggregated over all QS2MAODV nodes
    double qSamples=0, macSum=0, macPos=0, routeSum=0, macMax=0, fbAck=0, fbDrop=0, fbExp=0;
    for (uint32_t i = 0; i < nodes.GetN(); ++i)
    {
        Ptr<qs2maodv::RoutingProtocol> rp = DynamicCast<qs2maodv::RoutingProtocol>(
            nodes.Get(i)->GetObject<Ipv4>()->GetRoutingProtocol());
        if (!rp) continue;
        const auto& s = rp->GetQueueStats();
        qSamples += s.samples; macSum += s.macSum; macPos += s.macPositive;
        routeSum += s.routeSum; macMax = std::max(macMax, s.macMax);
        fbAck += s.fbAck; fbDrop += s.fbDrop; fbExp += s.fbExpired;
    }
    double meanMacQt   = qSamples > 0 ? macSum / qSamples : 0;
    double fracMacPos  = qSamples > 0 ? macPos / qSamples : 0;
    double meanRouteQt = qSamples > 0 ? routeSum / qSamples : 0;
    double deg250 = degSamples ? degSum[0] / degSamples : 0;
    double deg500 = degSamples ? degSum[1] / degSamples : 0;
    double deg1k  = degSamples ? degSum[2] / degSamples : 0;

    // Energy
    double energyUsed = 0; uint32_t liveNodes = nodes.GetN();
    if (useEnergy)
    {
        liveNodes = 0;
        for (uint32_t i = 0; i < sources.GetN(); ++i) {
            double rem = sources.Get(i)->GetRemainingEnergy();
            energyUsed += (initialEnergy - rem);
            // [v2] BasicEnergySource declares depletion at LowBatteryThreshold (10 % of E0),
            // never at 0 J: 'rem > 1e-9' (v1) always counted every node as alive.
            if (rem > 0.10 * initialEnergy + 1e-9) ++liveNodes;
        }
    }
    double energyPerPkt = (rxPkts > 0) ? energyUsed / rxPkts : 0;

    // ====== Print ======
    std::cout << std::fixed << std::setprecision(4)
              << "PDR        : " << pdr*100   << " %\n"
              << "Throughput : " << tput_kbps << " kbps/flow\n"
              << "E2E Delay  : " << delay_ms  << " ms\n"
              << "Jitter     : " << jitter_ms << " ms\n"
              << "NRL        : " << nrl       << " %\n"
              << "Energy used: " << energyUsed<< " J\n"
              << "Live nodes : " << liveNodes << "/" << numNodes << "\n";

    // ====== CSV ======
    bool newFile = !std::ifstream(csvFile).good();
    std::ofstream ofs(csvFile, std::ios::app);
    if (newFile)
        ofs << "Protocol,Seed,nNodes,nFlows,MaxPaths,"
               "InitEnergy_J,PktInterval_s,"
               "PDR,Throughput_kbps,Delay_ms,Jitter_ms,"
               "TxPkts,RxPkts,CtrlBytes,NRL,"
               "EnergyConsumed_J,EnergyPerPkt_J,LiveNodes,"
               "Tag,Speed_ms,GmAlpha,AreaZ,TxPowerDbm,RangeM,Bcast11,QoS,MacQueuePkts,RandomPairs,"
               "W3,Gamma,SilenceThr,DecayFactor,EnableDecay,AdaptiveW3,TrendEps,"
               "MacQueueSignal,MacFeedback,NextHopQueue,QueueState,QueueEps,HybridSelect,DecayPosOnly,DelayScale,"
               "DelayPw_ms,MeanMacQt,FracMacQtPos,MaxMacQt,MeanRouteQt,FbAck,FbDrop,FbExpired,"
               "Deg250,Deg500,Deg1000,TopoFingerprint,BaselineCfg,"
               "CtrlPktsAll,CtrlBytesAll,NRLall,NRLpkt\n";
    ofs << std::fixed << std::setprecision(6)
        << protocol      << "," << seed         << "," << numNodes     << ","
        << flows.size()  << "," << maxPaths      << "," << initialEnergy << ","
        << pktInterval   << ","
        << pdr           << "," << tput_kbps     << "," << delay_ms     << ","
        << jitter_ms     << "," << (uint64_t)txPkts << "," << (uint64_t)rxPkts << ","
        << (uint64_t)ctrlBytes << "," << nrl     << ","
        << energyUsed    << "," << energyPerPkt  << "," << liveNodes    << ","
        << tag << "," << meanVelMin << "," << gmAlpha << "," << areaZ << "," << txPowerDbm << ","
        << rangeM << "," << bcast11 << "," << qos << "," << macQueuePkts << "," << randomPairs << ","
        << qsW3 << "," << qsGamma << "," << ackSilenceThreshold << "," << decayFactor << ","
        << enableDecay << "," << adaptiveW3 << "," << trendEps << ","
        << macQueueSignal << "," << macFeedback << "," << nextHopQueue << "," << queueState << "," << queueEps << ","
        << hybridSelect << "," << decayPosOnly << "," << delayScale << ","
        << delayPw_ms << "," << meanMacQt << "," << fracMacPos << "," << macMax << ","
        << meanRouteQt << "," << (uint64_t)fbAck << "," << (uint64_t)fbDrop << "," << (uint64_t)fbExp << ","
        << deg250 << "," << deg500 << "," << deg1k << "," << topoFp << ","
        << (baselineCfg.empty() ? "-" : baselineCfg) << ","
        << g_ctrlPktsAll << "," << g_ctrlBytesAll << "," << nrlAll << "," << nrlPkt << "\n";
    ofs.close();

    Simulator::Destroy();
    NS_LOG_UNCOND("[" << protocol << "] Done -> " << csvFile);
    return 0;
}
