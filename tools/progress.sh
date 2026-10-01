#!/usr/bin/env bash
# progress.sh — done/total, speed and ETA for every jobs_*.tsv (read-only)
PROJ=$(cd "$(dirname "$0")/.." && pwd); OUTDIR=${OUTDIR:-$PROJ/data/v2}; RUNS="$OUTDIR/runs"
printf "%-22s %7s %7s %7s\n" "set" "done" "total" "failed"
tot_done=0; tot_all=0
declare -A FAILED=()
while read -r _ _ jid; do FAILED["$jid"]=1; done < <(cat "$OUTDIR/logs/failed.txt" 2>/dev/null)
for j in "$PROJ"/experiments/jobs_*.tsv; do
  [[ -f "$j" ]] || continue
  t=$(grep -c . "$j"); d=0; f=0
  while IFS=$'\t' read -r jid csv _; do
    id=$(echo "$jid" | tr '|:= /' '_____')
    if [[ -s "$RUNS/${csv%.csv}/$id.csv" ]]; then d=$((d+1)); elif [[ -n "${FAILED[$jid]:-}" ]]; then f=$((f+1)); fi
  done < "$j"
  printf "%-22s %7d %7d %7s\n" "$(basename "$j" .tsv | sed 's/^jobs_//')" "$d" "$t" "$f"
  tot_done=$((tot_done+d)); tot_all=$((tot_all+t))
done
recent=$(find "$RUNS" -name '*.csv' ! -name '.tmp_*' -mmin -30 2>/dev/null | wc -l)
rate=$(awk -v r="$recent" 'BEGIN{printf "%.1f", r/30}')
left=$((tot_all - tot_done))
eta=$(awk -v l="$left" -v r="$recent" 'BEGIN{ if (r>0) printf "%.1f h", l/(r/30)/60; else print "?" }')
echo "total: $tot_done/$tot_all | last 30 min: $rate runs/min | ETA $eta | running now: $(pgrep -fc "build/scratch/ns3.48-qsq-compare" || true)"
