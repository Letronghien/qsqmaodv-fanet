# QS-QMAODV — Queue-State-Aware Q-Learning Multipath AODV (ns-3.48)

Same layout as the ANBQ / NBQ-MAODV projects: one git repo for the code and one private
ns-3.48 tree for building and running.

```
~/qsqmaodv-repo/                 <- THIS repo: the only place you edit
│  src/qs2maodv/                 proposed protocol (ns-3 module)
│  src/pmaodv/  src/qmaodv/      baselines, imported once from ~/nbqmaodv-fanet/ns-3-nbq/src
│  scratch/qsq-compare.cc        simulation program (all protocols, all scenarios)
│  experiments/                  make_jobs.py (experiment design), run_jobs.sh (parallel runner)
│  analysis/                     analyze.py (paired tests, Holm, CI, ablation), reanalyze_v1.py
│  data/legacy_v1/  data/v2/     old ns-3.40 CSVs (record only) / new results
│  docs/  paper/  tools/  Makefile
│
~/qsqmaodv-fanet/ns-3-qsq/       <- private ns-3.48 (fresh clone), created by `make setup`
   src/qs2maodv, src/pmaodv, src/qmaodv   -> symlinks to the repo
   scratch/qsq-compare.cc                 -> symlink to the repo
```
Nothing in `~/anbq-fanet`, `~/nbqmaodv-fanet` or `~/ns-3-dev` is modified
(the baselines are only *read* from `~/nbqmaodv-fanet/ns-3-nbq/src`).

## First time (new VM, new GitHub repo)
```bash
cd ~ && unzip qsqmaodv-repo.zip && cd ~/qsqmaodv-repo
bash tools/bootstrap_new_repo.sh git@github.com:Letronghien/<new-repo>.git   # fresh history + push
python3 -m venv .venv && source .venv/bin/activate && pip install -r requirements.txt
make setup        # clone ns-3.48 -> ~/qsqmaodv-fanet/ns-3-qsq, import baselines, link, build, check attributes
make smoke        # 20-s run of AODV, PMAODV, QMAODV, QS2MAODV
make sanity       # MAC-queue signal and MAC feedback must be active (reports/v2/sanity.txt)
```
## Daily work
```bash
# edit src/qs2maodv/... or scratch/qsq-compare.cc in the repo, then
make build
make ablation     # 420 runs  -> decides the storyline
make main         # 2760 runs (MAXJ=7 on the 8-vCPU VM)
make analyze      # tables in reports/v2
```
Versions: ns-3.48 (release profile), C++23, g++ 13.3, CMake 3.28, Python 3.12.
