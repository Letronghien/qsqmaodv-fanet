# Legacy material (not part of the release archive)

* `data_v1_ns3.40/` — CSV files of the first (v1) study, ns-3.40. Superseded: the v1 protocol
  never read the MAC queue and the energy model had no effect (see `docs/internal/`).
* `reanalyze_v1.py` — paired re-analysis of those files (`python3 legacy/reanalyze_v1.py legacy/data_v1_ns3.40`).
* `compare-sim-v1-legacy.cc.txt` — the v1 simulation program, for the record.

Nothing in this folder is used by the experiments or figures of the article.
