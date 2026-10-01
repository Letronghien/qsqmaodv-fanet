# QS-QMAODV project (~/qsqmaodv-repo) — common tasks. Run `make help`.
PY      ?= python3
JOBSDIR := experiments
DATA    ?= data/v2
REPORT  ?= reports/v2
MAXJ    ?= $(shell n=$$(nproc); echo $$((n>1?n-1:1)))
PROTOS  ?= AODV,PMAODV,QMAODV,QSQMAODV

help:
	@echo "make env          - print machine / ns-3 information (send this to the assistant)"
	@echo "make status       - show which repo folders are linked into the ns-3 tree"
	@echo "make setup        - create ~/qsqmaodv-fanet/ns-3-qsq (fresh ns-3.48), import baselines, build"
	@echo "make baselines    - (re)import pmaodv/qmaodv from ~/nbqmaodv-fanet/ns-3-nbq/src (FORCE=1 to overwrite)"
	@echo "make build        - rebuild after editing src/ or scratch/"
	@echo "make smoke        - 20-s run of each installed protocol"
	@echo "make sanity       - 3-seed check that the MAC-queue signal and MAC feedback are active"
	@echo "make ablation     - 2x2 factorial ablation (420 runs)  -> decides the paper storyline"
	@echo "make main         - main families N/L/S/C x 4 protocols (2760 runs)"
	@echo "make realism sens - multi-hop scenario / sensitivity sweeps"
	@echo "make analyze      - statistics + tables into $(REPORT)"
	@echo "make legacy       - re-analysis of the old ns-3.40 CSVs (data/legacy_v1)"
	@echo "Variables: PROTOS=AODV,QS2MAODV (if baselines are missing)  MAXJ=4  DATA=... REPORT=..."

env:
	@bash tools/check_env.sh $(NS3_SRC)
status:
	@. ./.workspace && echo "ns-3: $$NS3" && ls -l $$NS3/src | grep -- "->" ; ls -l $$NS3/scratch | grep -- "->"
setup:
	@NS3_SRC="$(NS3_SRC)" bash tools/setup_ns3.sh
baselines:
	@FORCE=$(FORCE) bash tools/import_baselines.sh
build:
	@. ./.workspace && cd $$NS3 && ./ns3 build

define RUNSET
	$(PY) $(JOBSDIR)/make_jobs.py --sets $(1) --protocols $(PROTOS) --out $(JOBSDIR)/jobs_$(1).tsv
	OUTDIR=$(DATA) MAX_JOBS=$(MAXJ) bash $(JOBSDIR)/run_jobs.sh $(JOBSDIR)/jobs_$(1).tsv
endef
smoke:
	$(call RUNSET,smoke)
	@$(PY) -c "import pandas as pd; d=pd.read_csv('$(DATA)/smoke.csv'); c=[x for x in ['Protocol','PDR','DelayPw_ms','RxPkts','CtrlPktsAll','NRLall','FracMacQtPos','MeanNhQ','FbAck'] if x in d]; print(d[c].to_string(index=False))"
sanity:
	$(call RUNSET,sanity)
	@$(MAKE) --no-print-directory analyze
ablation:
	$(call RUNSET,ablation)
main:
	$(call RUNSET,main)
realism:
	$(call RUNSET,realism)
sens:
	$(call RUNSET,sens)

analyze:
	$(PY) analysis/analyze.py --dir $(DATA) --out $(REPORT)
legacy:
	$(PY) analysis/reanalyze_v1.py data/legacy_v1

.PHONY: help env status baselines setup build smoke sanity ablation main realism sens analyze legacy
