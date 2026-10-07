#!/usr/bin/env bash
# run_queue.sh — run experiment sets one after another (resumable).
#   bash tools/run_queue.sh ablation main sens realism
#   GIT_BACKUP=0 bash tools/run_queue.sh ...   (no commit/push of results after each set)
# The list is stored in data/v2/QUEUE so that tools/run_tmux.sh (and the optional
# @reboot hook) can resume after a crash. Each set: make_jobs -> run_jobs -> collect.
set -u
PROJ=$(cd "$(dirname "$0")/.." && pwd); cd "$PROJ"
OUTDIR=${OUTDIR:-$PROJ/data/v2}; mkdir -p "$OUTDIR/logs"
PY=${PY:-python3}; [[ -x "$PROJ/.venv/bin/python" ]] && PY="$PROJ/.venv/bin/python"
PROTOS=${PROTOS:-AODV,AOMDV,PMAODV,QMAODV,QLAODV,QSQMAODV}
[[ $# -gt 0 ]] && echo "$*" > "$OUTDIR/QUEUE"
read -r -a SETS < "$OUTDIR/QUEUE"
for s in "${SETS[@]}"; do
  [[ -f "$OUTDIR/DONE_$s" ]] && { echo "== $s already complete"; continue; }
  echo "================ $(date '+%F %T')  set: $s"
  $PY experiments/make_jobs.py --sets "$s" --protocols "$PROTOS" --out "experiments/jobs_$s.tsv"
  OUTDIR="$OUTDIR" bash experiments/run_jobs.sh "experiments/jobs_$s.tsv"
  $PY tools/collect.py "$OUTDIR"
  # optional off-VM backup of the finished runs (GIT_BACKUP=1, default on)
  if [[ "${GIT_BACKUP:-1}" == 1 ]] && git -C "$PROJ" rev-parse >/dev/null 2>&1; then
    git -C "$PROJ" add data/v2/runs data/v2/*.csv 2>/dev/null
    git -C "$PROJ" commit -q -m "results: $s ($(date '+%F %T'))" && git -C "$PROJ" push -q \
      || echo "   (git backup skipped)"
  fi
  # mark complete only if every job of the set has a result file
  if [[ "$(OUTDIR=$OUTDIR bash tools/progress.sh | awk -v s="$s" '$1==s{print ($2==$3)}')" == 1 ]]; then
    touch "$OUTDIR/DONE_$s"
  else
    echo "!! $s has missing runs (see $OUTDIR/logs/failed.txt); re-run the queue to retry"
  fi
done
$PY analysis/analyze.py --dir "$OUTDIR" --out reports/v2 || true
echo "================ $(date '+%F %T')  queue finished: ${SETS[*]}"
