#!/usr/bin/env bash
# run_jobs.sh — resume-safe parallel runner (~/qsqmaodv-fanet/ns-3-qsq).
#   bash experiments/run_jobs.sh experiments/jobs.tsv
#   MAX_JOBS=4 OUTDIR=data/v2 bash experiments/run_jobs.sh experiments/jobs.tsv
# Calls the compiled binary directly (no './ns3 run' per job: faster, no lock contention).
set -u
PROJ=$(cd "$(dirname "$0")/.." && pwd)
JOBS=${1:-$PROJ/experiments/jobs.tsv}
[[ -f "$PROJ/.workspace" ]] && source "$PROJ/.workspace"
NS3=${NS3:?run tools/setup_ns3.sh first (or export NS3=~/qsqmaodv-fanet/ns-3-qsq)}
OUTDIR=${OUTDIR:-$PROJ/data/v2}
MAX_JOBS=${MAX_JOBS:-$(( $(nproc) > 1 ? $(nproc) - 1 : 1 ))}
DONE="$OUTDIR/done.txt"
mkdir -p "$OUTDIR/tmp"; touch "$DONE"

( cd "$NS3" && ./ns3 build qsq-compare >/dev/null ) || { echo "build failed"; exit 1; }
BIN=$(find "$NS3/build/scratch" -maxdepth 1 -type f -executable -name "*qsq-compare*" | head -1)
[[ -x "$BIN" ]] || { echo "binary not found under $NS3/build/scratch"; exit 1; }
export BIN OUTDIR DONE LD_LIBRARY_PATH="$NS3/build/lib:${LD_LIBRARY_PATH:-}"

run_one() {
  local jid="$1" csv="$2" args="$3"
  local safe; safe=$(echo "$jid" | tr '|:= ' '____')
  local tmp="$OUTDIR/tmp/$safe.csv"; rm -f "$tmp"
  # shellcheck disable=SC2086
  "$BIN" $args --csvFile="$tmp" >/dev/null 2>"$OUTDIR/tmp/$safe.err"
  if [[ -s "$tmp" ]]; then
    ( flock -x 9
      if [[ ! -s "$OUTDIR/$csv" ]]; then cat "$tmp" >> "$OUTDIR/$csv"; else tail -n +2 "$tmp" >> "$OUTDIR/$csv"; fi
      echo "$jid" >> "$DONE" ) 9>"$OUTDIR/.lock"
    rm -f "$tmp" "$OUTDIR/tmp/$safe.err"
  else
    echo "FAILED $jid -> $OUTDIR/tmp/$safe.err"
  fi
}
export -f run_one

total=$(wc -l < "$JOBS")
grep -vxFf "$DONE" <(cut -f1 "$JOBS") > "$OUTDIR/todo_ids.txt" || true
todo=$(wc -l < "$OUTDIR/todo_ids.txt")
echo "binary : $BIN"
echo "jobs   : $total total, $todo to run, $MAX_JOBS in parallel -> $OUTDIR"
n=0; start=$(date +%s)
while IFS=$'\t' read -r jid csv args; do
  while (( $(jobs -rp | wc -l) >= MAX_JOBS )); do sleep 0.2; done
  run_one "$jid" "$csv" "$args" &
  n=$((n+1))
  if (( n % 50 == 0 )); then el=$(( $(date +%s) - start )); echo "  launched $n/$todo  (${el}s elapsed)"; fi
done < <(awk -F'\t' 'NR==FNR{t[$1]=1;next} ($1 in t)' "$OUTDIR/todo_ids.txt" "$JOBS")
wait
echo "done: $(grep -cxFf <(cut -f1 "$JOBS") "$DONE") / $total"
