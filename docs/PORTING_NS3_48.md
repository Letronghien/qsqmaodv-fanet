# Porting notes: ns-3.40 -> ns-3.48

Checked by compiling every source file of `src/qsqmaodv`, `src/qlaodv` and `scratch/qsq-compare.cc`
against the ns-3.48 headers (C++23, syntax check). Changes that were needed:

| Area | ns-3.40 | ns-3.48 | Where |
|---|---|---|---|
| Energy framework | `EnergySource`, `EnergySourceContainer` in `ns3::` | moved to `ns3::energy::` (since 3.42) | routing protocol (`GetEnergyRatio`), simulator |
| Wi-Fi queue identifier | `std::tuple<type, WIFI_UNICAST, Mac48Address, optional<tid>>` | struct `WifiContainerQueueId`; use `MakeWifiUnicastQueueId(type, ra, tid)` (WIFI_UNICAST deprecated in 3.46) | `GetNextHopQueueOccupancy` |
| Baseline modules | hard `#include` | optional: `__has_include` -> clear runtime error if missing | simulator |
| Build | `scratch/compare-sim.cc` | `scratch/qsq-compare.cc`; modules in `src/` (symlinked into `~/qsqmaodv-fanet/ns-3-qsq`) | — |
| C++ standard | C++17/20 | C++23 required by ns-3.48 | — |

Unchanged and verified: `WifiMac::GetTxopQueue(AC_BE / AC_BE_NQOS)`, traces `AckedMpdu`/`DroppedMpdu`,
`WifiMacQueue::GetNPackets(queueId)`, `ArpCache::Lookup`, `GaussMarkovMobilityModel`, `build_lib(...)`.

Baselines `pmaodv` / `qmaodv` are imported from `~/nbqmaodv-fanet/ns-3-nbq/src`, where they already build on ns-3.48
(release profile). `tools/setup_ns3.sh` checks that the attributes set by `qsq-compare` exist.
Not yet verified (needs the real build on the VM): linking and runtime behaviour.

Results produced with ns-3.48 are not bit-comparable with the ns-3.40 CSVs in `data/legacy_v1`
(the Wi-Fi and energy models changed between versions). All paper numbers must come from `data/v2`.
