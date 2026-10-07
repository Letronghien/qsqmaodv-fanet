# QS-QMAODV — common tasks.  Run `make help`.
# use the project virtual environment when it exists (isolated from apt packages)
PY     ?= $(shell [ -x .venv/bin/python ] && echo .venv/bin/python || echo python3)
DATA   ?= data/v2
REPORT ?= reports/v2
MAXJ   ?= $(shell n=$$(nproc); echo $$((n>1?n-1:1)))
PROTOS ?= AODV,AOMDV,PMAODV,QMAODV,QLAODV,QSQMAODV
SETS   ?= ablation_final main sens realism

help:
	@echo "Setup"
	@echo "  make setup          private ns-3.48 tree (~/qsqmaodv-fanet/ns-3-qsq), import baselines, build"
	@echo "  make build          rebuild after editing src/ or scratch/"
	@echo "  make env | status   machine information | modules linked into the ns-3 tree"
	@echo "Checks"
	@echo "  make smoke          20-s run of every protocol"
	@echo "  make sanity         QS-QMAODV(all off) == QMAODV; queue signal and MAC feedback active"
	@echo "Experiments (resumable; long runs: make run)"
	@echo "  make ablation       design-phase ablation, DEV seeds 1-30"
	@echo "  make ablation_final ablation of the final protocol, TEST seeds 31-60"
	@echo "  make main | sens | realism   comparison families / sensitivity / multi-hop, TEST seeds"
	@echo "  make run [SETS=...] queue inside tmux session 'qsq' (default: $(SETS))"
	@echo "  make resume | attach | progress | autoresume"
	@echo "Results"
	@echo "  make analyze        statistics and tables -> $(REPORT)"
	@echo "  make figures        publication figures (PDF + 300-dpi PNG) -> $(REPORT)/figures"
	@echo "  make diffs          docs/diffs: QS-QMAODV vs mpaodv, QL-AODV vs AODV"
	@echo "  make verify-baseline vendored PMAODV/QMAODV == published module (bit-identical)"
	@echo "  make release        clean archive for the journal (excludes legacy/ and docs/internal/)"
	@echo "Variables: PROTOS=$(PROTOS)  MAXJ=$(MAXJ)  DATA=$(DATA)  REPORT=$(REPORT)"

# ------------------------------------------------------------------ setup
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

# ------------------------------------------------------------------ experiments
define RUNSET
	$(PY) experiments/make_jobs.py --sets $(1) --protocols $(PROTOS) --out experiments/jobs_$(1).tsv
	OUTDIR=$(DATA) MAX_JOBS=$(MAXJ) bash experiments/run_jobs.sh experiments/jobs_$(1).tsv
	$(PY) tools/collect.py $(DATA)
endef
smoke:
	$(call RUNSET,smoke)
	@$(PY) -c "import pandas as pd; d=pd.read_csv('$(DATA)/smoke.csv'); c=[x for x in ['Protocol','PDR','DelayPw_ms','RxPkts','CtrlPktsAll','NRLall','FracMacQtPos','MeanNhQ','FbAck','QlMeanCandidates'] if x in d]; print(d[c].to_string(index=False))"
sanity:
	$(call RUNSET,sanity)
	@$(MAKE) --no-print-directory analyze
ablation:
	$(call RUNSET,ablation)
ablation_final:
	$(call RUNSET,ablation_final)
main:
	$(call RUNSET,main)
realism:
	$(call RUNSET,realism)
sens:
	$(call RUNSET,sens)

run:
	@OUTDIR=$(DATA) PROTOS=$(PROTOS) bash tools/run_tmux.sh $(SETS)
resume:
	@OUTDIR=$(DATA) PROTOS=$(PROTOS) bash tools/run_tmux.sh
attach:
	@tmux attach -t qsq
progress:
	@OUTDIR=$(DATA) bash tools/progress.sh
autoresume:
	@bash tools/install_autoresume.sh

# ------------------------------------------------------------------ results
collect:
	@$(PY) tools/collect.py $(DATA)
analyze: collect
	$(PY) analysis/analyze.py --dir $(DATA) --out $(REPORT)
figures: collect
	$(PY) analysis/make_figures.py --dir $(DATA) --out $(REPORT)/figures
diffs:
	@bash tools/make_diffs.sh
verify-baseline:
	@bash tools/verify_baseline.sh
release:
	@bash tools/make_release.sh

.PHONY: help env status setup baselines build smoke sanity ablation ablation_final main realism sens \
        run resume attach progress autoresume collect analyze figures diffs verify-baseline release
