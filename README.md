# QS-QMAODV — Queue-State-Aware Q-Learning Multipath Routing for Flying Ad Hoc Networks

Code, experiment design and data of the article

> T. H. Le, *et al.*, "QS-QMAODV: …", *Journal of Applied Data Sciences*, 2026 (under review).

QS-QMAODV builds on QMAODV (Q-learning multipath AODV with MAC-ACK/delay rewards; the published
implementation in module `mpaodv`, from which `src/qsqmaodv` is forked). A data-driven
diagnosis showed that QMAODV's hop-by-hop next-hop learning loses 5–8 pp of delivery because
intermediate nodes forward into stale alternates; QS-QMAODV therefore learns **at the source**
and makes the learning **queue-state aware**: the occupancy of the IEEE 802.11 best-effort MAC
queue towards each candidate next hop enters the reward, the next-hop choice and the decision
*when* to explore, complemented by ACK-silence decay and an explicit failure penalty
(`docs/QSQMAODV_DESIGN.md`). With these extensions switched off the module executes exactly the
QMAODV code path; this is verified bit-for-bit by `make sanity`.

## Repository layout
```
src/qsqmaodv/            proposed protocol (ns-3 module, fork of QMAODV; changes in docs/diffs/)
src/qlaodv/              QL-AODV [Ateya et al., Future Internet 2025] re-implemented as baseline
src/mpaodv, src/aomdv    baselines PMAODV, QMAODV (module mpaodv) and AOMDV, vendored from
                         github.com/Letronghien/ao-am-pm-qm-sa @56d143c (docs/BASELINE_PROVENANCE.md;
                         `make verify-baseline` proves identity with the published module)
scratch/qsq-compare.cc   simulation program: one protocol x one scenario x one seed -> one CSV row
experiments/             make_jobs.py (complete experiment design), run_jobs.sh (parallel, resumable)
analysis/                analyze.py (paired tests, Holm, CIs, ablation), make_figures.py
data/v2/                 results: runs/<set>/<job>.csv (one file per run) and merged <set>.csv
docs/                    protocol design, QL-AODV mapping, diffs, porting notes, long-run guide
tools/                   setup, baseline import, include-guard check, tmux queue, release
```

## Requirements
Ubuntu 22.04/24.04, g++ ≥ 11.1 (or clang++ ≥ 17), CMake ≥ 3.25, Python ≥ 3.10, ns-3.48 (downloaded
by `make setup`), Python packages in `requirements.txt`. A full reproduction (≈ 6 700 runs) takes
about one day on 8 vCPUs.

## Quick start
```bash
python3 -m venv .venv && source .venv/bin/activate && pip install -r requirements.txt
make setup     # private ns-3.48 tree in ~/qsqmaodv-fanet/ns-3-qsq, modules linked from src/
make smoke     # every protocol once (20 s)
make sanity    # equivalence QS-QMAODV(all off) == QMAODV, queue/feedback diagnostics
```

## Reproducing the article
| Article element | Command | Data |
|---|---|---|
| Ablation (factorial + leave-one-out) | `make ablation_final` | `data/v2/v2_ablation_final.csv` |
| Comparison families N, L, S, C | `make main` | `data/v2/v2_family_{N,L,S,C}.csv` |
| Sensitivity (w_q, β, τ, decay, γ, Q_ref) | `make sens` | `data/v2/v2_sens.csv` |
| Multi-hop scenario (300 m range) | `make realism` | `data/v2/v2_realism.csv` |
| Statistics and tables | `make analyze` | `reports/v2/*.md, *.csv` |
| Figures | `make figures` | `reports/v2/figures/*.pdf, *.png` |

All reported results use **test seeds 31–60**. Seeds 1–30 were used only while designing the
protocol (`make ablation`), so no reported comparison is based on runs that influenced the design.
Runs are paired: a seed fixes mobility and traffic for every protocol (checked through the
`TopoFingerprint` column), hence paired Wilcoxon tests with Holm correction are used.
For long runs use `make run` (tmux, crash-safe, resumable; see `docs/RUNNING_LONG_EXPERIMENTS.md`).

## Main parameters
3-D Gauss–Markov mobility (α = 0.85) in 1000 × 1000 × 300 m; IEEE 802.11b ad hoc, data 11 Mbps,
16 dBm, Friis; CBR/UDP 512 B, convergecast N−1 → 1 (default) or 5 random flows; 200 s;
MaxPaths = 3. QMAODV and QS-QMAODV share α₀ = 0.5, γ = 0.9, ε₀ = 0.5, w₁ = 0.6, w₂ = 0.4.
QS-QMAODV defaults: source-side learning, queue-driven exploration (floor 0.01), w_q = 0.10 (fixed), β = 0.5, Q_ref = 20 packets, τ = 15 s,
decay 0.92, failure penalty 0.5. QL-AODV uses the values of its Table 2. Every CSV row stores
the effective protocol configuration (`ProtocolCfg`).

## License and citation
GPL-2.0-only (the modules are derived from the ns-3 AODV model; original copyright notices are
retained). Please cite the article (see `CITATION.cff`).

