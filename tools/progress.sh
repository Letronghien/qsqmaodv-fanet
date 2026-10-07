#!/usr/bin/env bash
# progress.sh — done/total, speed and ETA (read-only, safe while running)
PROJ=$(cd "$(dirname "$0")/.." && pwd); OUTDIR=${OUTDIR:-$PROJ/data/v2}; RUNS="$OUTDIR/runs"
# sets to show: the stored queue if any, otherwise every jobs_*.tsv
if [[ -s "$OUTDIR/QUEUE" ]]; then read -r -a SETS < "$OUTDIR/QUEUE"
else SETS=(); for j in "$PROJ"/experiments/jobs_*.tsv; do [[ -f $j ]] && SETS+=("$(basename "$j" .tsv | sed 's/^jobs_//')"); done; fi
declare -A FAILED=()
while read -r _ _ jid; do FAILED["$jid"]=1; done < <(cat "$OUTDIR/logs/failed.txt" 2>/dev/null)
printf "%-12s %7s %7s %7s\n" "set" "done" "total" "failed"
tot_done=0; tot_all=0
for s in "${SETS[@]}"; do
  j="$PROJ/experiments/jobs_$s.tsv"
  if [[ ! -f "$j" ]]; then printf "%-12s %7s %7s %7s\n" "$s" "-" "(not started)" ""; continue; fi
  t=$(grep -c . "$j"); d=0; f=0
  while IFS=$'\t' read -r jid csv _; do
    id=$(echo "$jid" | tr '|:= /' '_____')
    if [[ -s "$RUNS/${csv%.csv}/$id.csv" ]]; then d=$((d+1)); elif [[ -n "${FAILED[$jid]:-}" ]]; then f=$((f+1)); fi
  done < "$j"
  printf "%-12s %7d %7d %7d\n" "$s" "$d" "$t" "$f"
  tot_done=$((tot_done+d)); tot_all=$((tot_all+t))
done
# speed: finished runs per minute over the last min(30 min, time since the runner started)
now=$(date +%s)
start=$(stat -c %Y "$OUTDIR/.runner.lock" 2>/dev/null || echo "$now")
win=$(( (now - start) / 60 )); (( win > 30 )) && win=30; (( win < 1 )) && win=1
recent=$(find "$RUNS" -name '*.csv' ! -name '.tmp_*' -newermt "-$win minutes" 2>/dev/null | wc -l)
left=$((tot_all - tot_done))
running=$(pgrep -c "ns3.48-qsq-com" 2>/dev/null); running=${running:-0}
if (( left == 0 )); then
  eta="finished"
elif (( recent < running || recent < 3 )); then
  eta="wait: too few finished runs yet (each batch of $running runs finishes together)"
else
  eta=$(awk -v l="$left" -v r="$recent" -v w="$win" 'BEGIN{ m=l/(r/w); printf "%.1f h (%.0f min)", m/60, m }')
fi
avg=""
if (( tot_done > 0 && running > 0 )); then
  avg=$(awk -v r="$recent" -v w="$win" -v p="$running" 'BEGIN{ if (r>0) printf " | ~%.1f min per run", p/(r/w) }')
fi
echo "total: $tot_done/$tot_all | speed (last ${win} min): $(awk -v r="$recent" -v w="$win" 'BEGIN{printf "%.2f", r/w}') runs/min$avg | running: $running"
echo "ETA: $eta"
