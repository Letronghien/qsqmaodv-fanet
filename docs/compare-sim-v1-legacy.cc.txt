/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/**
 * compare-sim.cc — FANET Protocol Comparison Simulator
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
 *   ./ns3 run "scratch/compare-sim --protocol=AODV     --numNodes=15 --seed=1"
 *   ./ns3 run "scratch/compare-sim --protocol=PMAODV   --numNodes=15 --seed=1"
 *   ./ns3 run "scratch/compare-sim --protocol=QMAODV   --numNodes=15 --seed=1"
 *   ./ns3 run "scratch/compare-sim --protocol=QS2MAODV --numNodes=15 --seed=1"
 */

#include "ns3/core-module.h"
#include "ns3/network-module.h"
#include "ns3/internet-module.h"
#include "ns3/wifi-module.h"
#include "ns3/mobility-module.h"
#include "ns3/applications-module.h"
#include "ns3/flow-monitor-module.h"
#include "ns3/energy-module.h"
#include "ns3/aodv-module.h"
#include "ns3/pmaodv-module.h"
#include "ns3/qmaodv-module.h"
#include "ns3/qs2maodv-helper.h"
#include <iostream>
#include <fstream>
#include <iomanip>
#include <set>
#include <vector>

using namespace ns3;
NS_LOG_COMPONENT_DEFINE("CompareSim");

int main(int argc, char* argv[])
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
    double qmEpsilonDecay = 0.02;
    double qmDecayPeriod  = 10.0;
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
    cmd.Parse(argc, argv);

    RngSeedManager::SetSeed(1);
    RngSeedManager::SetRun(seed);

    std::cout << "=== compare-sim === protocol=" << protocol
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
    YansWifiPhyHelper wifiPhy;
    wifiPhy.Set("TxPowerStart", DoubleValue(txPowerDbm));
    wifiPhy.Set("TxPowerEnd",   DoubleValue(txPowerDbm));
    YansWifiChannelHelper wifiChannel;
    wifiChannel.SetPropagationDelay("ns3::ConstantSpeedPropagationDelayModel");
    wifiChannel.AddPropagationLoss("ns3::FriisPropagationLossModel");
    wifiPhy.SetChannel(wifiChannel.Create());
    WifiMacHelper wifiMac;
    wifiMac.SetType("ns3::AdhocWifiMac");
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
        PmaodvHelper pmaodv;
        pmaodv.Set("MaxPaths", UintegerValue(maxPaths));
        internet.SetRoutingHelper(pmaodv);
    } else if (protocol == "QMAODV") {
        QmaodvHelper qmaodv;
        qmaodv.Set("MaxPaths",           UintegerValue(maxPaths));
        qmaodv.Set("Alpha",              DoubleValue(qmAlpha));
        qmaodv.Set("Gamma",              DoubleValue(qmGamma));
        qmaodv.Set("Epsilon",            DoubleValue(qmEpsilon));
        qmaodv.Set("RewardW1",           DoubleValue(qmW1));
        qmaodv.Set("RewardW2",           DoubleValue(qmW2));
        qmaodv.Set("EpsilonDecay",       DoubleValue(qmEpsilonDecay));
        qmaodv.Set("EpsilonDecayPeriod", TimeValue(Seconds(qmDecayPeriod)));
        internet.SetRoutingHelper(qmaodv);
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
    BasicEnergySourceHelper esHelper;
    esHelper.Set("BasicEnergySourceInitialEnergyJ", DoubleValue(initialEnergy));
    EnergySourceContainer sources = esHelper.Install(nodes);
    WifiRadioEnergyModelHelper wifiEnergyHelper;
    wifiEnergyHelper.Install(devices, sources);

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
        for (uint32_t i = 0; i < numFlows; ++i)
            flows.push_back({i, numNodes - 1 - i});
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
    double sumDelay=0, sumJitter=0, sumTput=0;
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

    // Energy
    double energyUsed = 0; uint32_t liveNodes = 0;
    for (uint32_t i = 0; i < nodes.GetN(); ++i) {
        double rem = sources.Get(i)->GetRemainingEnergy();
        energyUsed += (initialEnergy - rem);
        if (rem > 1e-9) ++liveNodes;
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
               "EnergyConsumed_J,EnergyPerPkt_J,LiveNodes\n";
    ofs << std::fixed << std::setprecision(6)
        << protocol      << "," << seed         << "," << numNodes     << ","
        << flows.size()  << "," << maxPaths      << "," << initialEnergy << ","
        << pktInterval   << ","
        << pdr           << "," << tput_kbps     << "," << delay_ms     << ","
        << jitter_ms     << "," << (uint64_t)txPkts << "," << (uint64_t)rxPkts << ","
        << (uint64_t)ctrlBytes << "," << nrl     << ","
        << energyUsed    << "," << energyPerPkt  << "," << liveNodes    << "\n";
    ofs.close();

    Simulator::Destroy();
    NS_LOG_UNCOND("[" << protocol << "] Done -> " << csvFile);
    return 0;
}
