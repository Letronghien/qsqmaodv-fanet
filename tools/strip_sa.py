#!/usr/bin/env python3
"""Remove the code of the separate self-adaptive variant from an mpaodv-derived module.
usage: strip_sa.py <module dir> <module name>   (e.g. src/mpaodv mpaodv, src/qsqmaodv qsqmaodv)
Only code that is inactive for PMAODV/QMAODV is removed (all SA options default to off):
adaptive epsilon/alpha/reward, relay-energy reward term and its control-packet tag,
sequence-number statistics, SaQmaodvHelper and the example that uses it."""
import os, re, sys, shutil

d, name = sys.argv[1], sys.argv[2]
P = name.upper()
mdir = os.path.join(d, "model")
def rd(f): return open(os.path.join(mdir, f), encoding="utf-8").read()
def wr(f, s): open(os.path.join(mdir, f), "w", encoding="utf-8").write(s)
def must(s, old, new, cnt=1):
    assert s.count(old) >= 1, f"pattern not found: {old[:70]!r}"
    return s.replace(old, new) if cnt == 0 else s.replace(old, new, cnt)
def drop_fn(s, signature):
    """remove a free/member function definition starting at 'signature' (incl. return type line)"""
    i = s.find(signature)
    assert i >= 0, f"function not found: {signature}"
    start = s.rfind("\n\n", 0, i) + 1
    j = s.index("\n}\n", i) + 3
    return s[:start] + s[j:]
def drop_attr(s, aname):
    m = re.search(r'\n            \.AddAttribute\("%s",.*?Make\w+Checker(?:<[^>]*>)?\([^)]*\)\)' % aname, s, re.S)
    assert m, f"attribute not found: {aname}"
    return s[:m.start()] + s[m.end():]
def drop_decl(s, pattern):
    """remove a declaration line and its preceding doc comment"""
    m = re.search(pattern, s)
    assert m, f"declaration not found: {pattern}"
    ls = s.rfind("\n", 0, m.start()) + 1
    le = s.index("\n", m.end()) + 1
    k = ls
    while True:  # walk back over ///, /** ... */ doc lines
        prev = s.rfind("\n", 0, k - 1) + 1
        line = s[prev:k].strip()
        if line.startswith("///") or line.startswith("*") or line.startswith("/**"):
            k = prev
            if line.startswith("/**") or line.startswith("///"):
                if line.startswith("/**"):
                    break
                continue
        else:
            break
    return s[:k] + s[le:]

# ---------------------------------------------------------------- qtable.h
h = rd(f"{name}-qtable.h")
h = must(h, "QMAODV, SA-QMAODV, epsilon-greedy on Q.", "QMAODV, epsilon-greedy on Q.")
h = must(h, "r = w1 ACK + w2 / (1 + delay_ms) + w3 E.", "r = w1 ACK + w2 / (1 + delay_ms).")
h = re.sub(r" \* SA-QMAODV: epsilon <- .*?\n \*/", " */", h, flags=re.S)
h = must(h, """     * \\param w2 weight of the one-hop delay term
     * \\param w3 weight of the residual energy
     */
    void SetRewardWeights(double w1, double w2, double w3 = 0.0);""", """     * \\param w2 weight of the one-hop delay term
     */
    void SetRewardWeights(double w1, double w2);""")
for pat in [r"void SetAdaptiveFlags\(", r"void OnRouteError\(\);", r"void RecordSeqNoUpdate\(\);",
            r"void RecomputeAdaptiveAlpha\(\);", r"void RecomputeAdaptiveRewardWeights\(",
            r"void SetLowEnergyThreshold\(", r"void SetSensitivityLambda\(", r"void SetSeqNoWindow\(",
            r"uint32_t GetDeltaSeq\(\) const;", r"void PurgeSeqNoEvents\(\);"]:
    h = drop_decl(h, pattern=r"\n\s*" + pat)
h = re.sub(r"(void UpdateQValue\([^;]*?double delaySec),\s*double energyFraction = 1\.0\);",
           r"\1);", h, flags=re.S)
h = re.sub(r"(\\param delaySec[^\n]*\n)\s*\*\s*\\param energyFraction[^\n]*\n", r"\1", h)
h = must(h, "double ComputeReward(double ackSuccess, double delaySec, double energyFrac) const;",
         "double ComputeReward(double ackSuccess, double delaySec) const;")
for mem in ["m_w3;", "m_lowEnergyMode;", "m_adaptEpsilon{false};", "m_adaptAlpha{false};",
            "m_adaptReward{false};", "m_epsilonMin;", "m_epsilonMax;", "m_epsilonBump;", "m_lambda;",
            "m_seqNoWindow;", "m_lowEnergyThresh;", "m_w1Normal;", "m_w2Normal;", "m_w3Normal;",
            "m_w1Low;", "m_w2Low;", "m_w3Low;", "m_seqEvents;"]:
    h = re.sub(r"\n[^\n]*\b" + re.escape(mem) + r"[^\n]*", "", h, count=1)
wr(f"{name}-qtable.h", h)

# ---------------------------------------------------------------- qtable.cc
c = rd(f"{name}-qtable.cc")
c = re.sub(r"      m_w2\(0\.4\),\n.*?      m_w3Low\(0\.8\)\n",
           "      m_w2(0.4),\n      m_epsilonStep(0.02)\n", c, flags=re.S)
c = must(c, """QTable::SetRewardWeights(double w1, double w2, double w3)
{
    m_w1 = w1;
    m_w2 = w2;
    m_w3 = w3;
    m_w1Normal = w1;
    m_w2Normal = w2;
    m_w3Normal = w3;
}""", """QTable::SetRewardWeights(double w1, double w2)
{
    m_w1 = w1;
    m_w2 = w2;
}""")
for fn in ["QTable::SetAdaptiveFlags(", "QTable::SetLowEnergyThreshold(", "QTable::SetSensitivityLambda(",
           "QTable::SetSeqNoWindow(", "QTable::OnRouteError()", "QTable::RecordSeqNoUpdate()",
           "QTable::PurgeSeqNoEvents()", "QTable::GetDeltaSeq()", "QTable::RecomputeAdaptiveAlpha()",
           "QTable::RecomputeAdaptiveRewardWeights("]:
    c = drop_fn(c, fn)
c = must(c, """    const double floorEps = m_adaptEpsilon ? m_epsilonMin : 0.0;
    m_epsilon = std::max(floorEps, m_epsilon - m_epsilonStep);""",
         """    m_epsilon = std::max(0.0, m_epsilon - m_epsilonStep);""")
c = must(c, """QTable::ComputeReward(double ackSuccess, double delaySec, double energyFrac) const""",
         """QTable::ComputeReward(double ackSuccess, double delaySec) const""")
c = must(c, """    return m_w1 * ackSuccess + m_w2 * (1.0 / (1.0 + delaySec / oneMs)) + m_w3 * energyFrac;""",
         """    return m_w1 * ackSuccess + m_w2 * (1.0 / (1.0 + delaySec / oneMs));""")
c = re.sub(r"(QTable::UpdateQValue\(Ipv4Address dst,\s*Ipv4Address nextHop,\s*double ackSuccess,\s*double delaySec),\s*double energyFraction\)",
           r"\1)", c)
c = c.replace("    m_records.clear();\n    m_seqEvents.clear();", "    m_records.clear();")
c = c.replace("double reward = ComputeReward(ackSuccess, delaySec, energyFraction);",
              "double reward = ComputeReward(ackSuccess, delaySec);")
wr(f"{name}-qtable.cc", c)

# ---------------------------------------------------------------- routing-protocol.h
h = rd(f"{name}-routing-protocol.h")
h = h.replace(" * \\brief Multipath AODV core of PMAODV, QMAODV and SA-QMAODV",
              " * \\brief Multipath AODV core of PMAODV and QMAODV")
for pat in [r"double GetEnergyFraction\(\) const;", r"Ptr<Packet> AttachNbInfo\(Ptr<Packet> p\);",
            r"double RelayEnergy\(Ipv4Address nextHop\) const;", r"void NoteSeqNoIncrease\(\);"]:
    h = drop_decl(h, pattern=r"\n\s*" + pat)
for mem in ["m_w3{0.0};", "m_adaptEpsilon{false};", "m_adaptAlpha{false};", "m_adaptReward{false};",
            "m_useRelayEnergy{false};", "m_lambda{0.1};", "m_seqNoWindow{Seconds(10.0)};",
            "m_lowEnergyThreshold{0.20};", "m_nbEnergy;"]:
    h = re.sub(r"\n[^\n]*\b" + re.escape(mem) + r"[^\n]*", "", h, count=1)
wr(f"{name}-routing-protocol.h", h)

# ---------------------------------------------------------------- routing-protocol.cc
c = rd(f"{name}-routing-protocol.cc")
c = c.replace('#include "ns3/energy-source-container.h"\n', "").replace('#include "ns3/basic-energy-source.h"\n', "")
s = c.index("/**\n * \\ingroup " + name + "\n * \\brief Residual energy fraction of the sender")
e = c.index("NS_OBJECT_ENSURE_REGISTERED(NeighborInfoTag);") + len("NS_OBJECT_ENSURE_REGISTERED(NeighborInfoTag);\n")
c = c[:s] + c[e:].lstrip("\n")
for a in ["RewardW3", "AdaptiveEpsilon", "AdaptiveAlpha", "AdaptiveReward", "UseRelayEnergy",
          "Lambda", "SeqNoWindow", "LowEnergyThreshold"]:
    c = drop_attr(c, a)
c = c.replace('"Learning rate (initial value when AdaptiveAlpha is true)"', '"Learning rate"')
c = c.replace('"Next-hop selection: QLearning (QMAODV, SA-QMAODV) or Probabilistic "', '"Next-hop selection: QLearning (QMAODV) or Probabilistic "')
c = c.replace('"Period of the exploration decay and of the adaptation"', '"Period of the exploration decay"')
c = must(c, "    m_qtable.SetRewardWeights(m_w1, m_w2, m_w3);\n", "    m_qtable.SetRewardWeights(m_w1, m_w2);\n")
for l in ["    m_qtable.SetAdaptiveFlags(m_adaptEpsilon, m_adaptAlpha, m_adaptReward);\n",
          "    m_qtable.SetSeqNoWindow(m_seqNoWindow);\n",
          "    m_qtable.SetSensitivityLambda(m_lambda);\n",
          "    m_qtable.SetLowEnergyThreshold(m_lowEnergyThreshold);\n"]:
    c = must(c, l, "")
for fn in ["RoutingProtocol::AttachNbInfo(", "RoutingProtocol::RelayEnergy(",
           "RoutingProtocol::GetEnergyFraction()", "RoutingProtocol::NoteSeqNoIncrease()"]:
    c = drop_fn(c, fn)
c = re.sub(r"AttachNbInfo\(([^()]*(?:\([^()]*\))?[^()]*)\)", r"\1", c)
c = re.sub(r"\n\s*NoteSeqNoIncrease\(\);", "", c)
c = must(c, """    if (m_useRelayEnergy)
    {
        NeighborInfoTag nbi;
        if (packet->PeekPacketTag(nbi))
        {
            m_nbEnergy[sender] = nbi.GetEnergy();
        }
    }
""", "")
c = must(c, """    if (!unreachable.empty())
    {
        m_qtable.OnRouteError();
    }
""", "")
c = must(c, "    m_qtable.OnRouteError();\n", "")
c = must(c, """    m_qtable.RecomputeAdaptiveAlpha();
    m_qtable.RecomputeAdaptiveRewardWeights(GetEnergyFraction());
""", "")
c = re.sub(r",\s*\n\s*RelayEnergy\(fb\.GetNextHop\(\)\)", "", c)
wr(f"{name}-routing-protocol.cc", c)

# ---------------------------------------------------------------- helper of the variant / example
hd = os.path.join(d, "helper")
hh = open(os.path.join(hd, f"{name}-helper.h")).read()
if "SaQmaodvHelper" in hh:
    s = hh.index("/**\n * \\ingroup " + name + "\n * \\brief SA-QMAODV")
    e = hh.index("};", s) + 3
    hh = hh[:s] + hh[e:].lstrip("\n")
    open(os.path.join(hd, f"{name}-helper.h"), "w").write(hh)
    hc = open(os.path.join(hd, f"{name}-helper.cc")).read()
    s = hc.index("SaQmaodvHelper::SaQmaodvHelper()")
    s = hc.rfind("\n\n", 0, s) + 1
    e = hc.index("\n}\n", s) + 3
    hc = hc[:s] + hc[e:]
    open(os.path.join(hd, f"{name}-helper.cc"), "w").write(hc)
if os.path.isdir(os.path.join(d, "examples")):
    shutil.rmtree(os.path.join(d, "examples"))
cm = open(os.path.join(d, "CMakeLists.txt")).read()
open(os.path.join(d, "CMakeLists.txt"), "w").write(re.sub(r"\n\s*add_subdirectory\(examples\)", "", cm))
print("stripped", d)

# ---------------------------------------------------------------- doc leftovers
h = rd(f"{name}-qtable.h")
h = re.sub(r"\n\s*\*\s*\\param energyFrac residual energy fraction", "", h)
wr(f"{name}-qtable.h", h)
h = rd(f"{name}-routing-protocol.h")
h = h.replace("/// Periodic adaptation: epsilon decay, alpha recomputation, reward weights.",
              "/// Periodic decay of the exploration rate.")
h = h.replace("///< period of the adaptation tick", "///< period of the exploration decay")
h = h.replace("///< adaptation tick", "///< exploration decay tick")
wr(f"{name}-routing-protocol.h", h)

# ---------------------------------------------------------------- neutral names for the epsilon tick
for f in (f"{name}-routing-protocol.h", f"{name}-routing-protocol.cc"):
    s = rd(f)
    s = s.replace("PeriodicAdaptiveTick", "EpsilonDecayTick")
    s = s.replace("m_periodicAdaptInterval", "m_epsilonDecayInterval")
    s = s.replace("m_periodicAdaptEvent", "m_epsilonDecayEvent")
    s = s.replace('.AddAttribute("PeriodicAdaptInterval",', '.AddAttribute("EpsilonDecayInterval",')
    wr(f, s)
