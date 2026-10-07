# Changelog

## 3.0.0 — publication version
- QS-QMAODV v3 (`src/qsqmaodv`): fork of the published QMAODV with queue-state extensions
  (per-next-hop MAC queue in reward and selection, ACK-silence decay, trend exploration,
  failure penalty); all off == QMAODV, verified by `make sanity`.
- External baselines: QL-AODV re-implementation (`src/qlaodv`) and AOMDV.
- Simulation program rewritten (`scratch/qsq-compare.cc`): explicit configuration, overhead
  counted at the IP layer including broadcasts, per-row protocol configuration, diagnostics.
- Experiment design with design seeds (1–30) and test seeds (31–60); crash-safe tmux queue.
- Analysis: paired Wilcoxon + Holm, bootstrap CIs, factorial and leave-one-out ablation;
  publication figures.
- Restored the AODV / AODV-UU attribution in all files derived from ns-3 AODV.
- Removed the source-only prototype (v2) and the energy model; v1 material moved to `legacy/`.

## 2.x — ns-3.48 port of the prototype (superseded)
## 1.x — ns-3.40 prototype (superseded, see legacy/)
