#!/usr/bin/env bash
# run_jobs.sh — crash-safe, resumable parallel runner.
#
#   bash experiments/run_jobs.sh experiments/jobs_ablation.tsv
#   MAX_JOBS=7 JOB_TIMEOUT=3600 bash experiments/run_jobs.sh <jobs.tsv>
#
# Durability (the VM may freeze or reboot):
#   * every run writes ITS OWN file  data/v2/runs/<csv-name>/<job-id>.csv
#   * written to a temporary name, fsync'ed, then atomically renamed
#     -> a file that exists is always complete; a crash loses at most the runs in flight
#   * "done" == the result file exists, so re-running the same command continues
#     where it stopped (finished runs are never repeated)
#   * each run is killed after JOB_TIMEOUT seconds; failed runs are retried once at the
#     end of the pass and on every later invocation
#   * a lock prevents two runners from working on the same output folder
# Aggregated CSVs (data/v2/<csv-name>.csv) are rebuilt by tools/collect.py.
set -u
PROJ=$(cd "$(dirname "$0")/.." && pwd)
JOBS=${1:?usage: run_jobs.sh <jobs.tsv>}
[[ -f "$PROJ/.workspace" ]] && source "$PROJ/.workspace"
NS3=${NS3:?run tools/setup_ns3.sh first}
OUTDIR=${OUTDIR:-$PROJ/data/v2}
MAX_JOBS=${MAX_JOBS:-$(( $(nproc) > 1 ? $(nproc) - 1 : 1 ))}
JOB_TIMEOUT=${JOB_TIMEOUT:-3600}
RUNS="$OUTDIR/runs"; LOGS="$OUTDIR/logs"
mkdir -p "$RUNS" "$LOGS"

exec 8>"$OUTDIR/.runner.lock"
flock -n 8 || { echo "another runner is already using $OUTDIR (see: tmux ls)"; exit 1; }

if [[ -z "${BIN:-}" ]]; then
  ( cd "$NS3" && ./ns3 build qsq-compare >/dev/null ) || { echo "build failed"; exit 1; }
  source "$PROJ/tools/ns3_common.sh"
  BIN=$(qsq_binary "$NS3")
fi
[[ -x "$BIN" ]] || { echo "binary not found"; exit 1; }
export BIN RUNS LOGS JOB_TIMEOUT LD_LIBRARY_PATH="$NS3/build/lib:${LD_LIBRARY_PATH:-}"

safe_id() { echo "$1" | tr '|:= /' '_____'; }
export -f safe_id

run_one() {   # $1 job id  $2 csv name  $3 args
  local id; id=$(safe_id "$1")
  local dir="$RUNS/${2%.csv}"; mkdir -p "$dir"
  local final="$dir/$id.csv" tmp="$dir/.tmp_$id.csv" err="$LOGS/$id.err"
  [[ -s "$final" ]] && return 0
  rm -f "$tmp"
  # shellcheck disable=SC2086
  timeout --kill-after=30 "$JOB_TIMEOUT" "$BIN" $3 --csvFile="$tmp" >/dev/null 2>"$err" 8>&-
  local rc=$?
  if [[ $rc -eq 0 && -s "$tmp" && $(wc -l < "$tmp") -ge 2 ]]; then
    sync "$tmp" 2>/dev/null; mv -f "$tmp" "$final"; sync "$dir" 2>/dev/null
    rm -f "$err"
  else
    rm -f "$tmp"
    echo "$(date -Is) rc=$rc $1" >> "$LOGS/failed.txt"
    return 1
  fi
}
export -f run_one

pending() {  # list job lines whose result file does not exist yet
  while IFS=$'\t' read -r jid csv args; do
    [[ -s "$RUNS/${csv%.csv}/$(safe_id "$jid").csv" ]] || printf '%s\t%s\t%s\n' "$jid" "$csv" "$args"
  done < "$JOBS"
}

run_pass() {
  local todo; todo=$(pending)
  local n; n=$(printf '%s' "$todo" | grep -c . || true)
  echo "$(date '+%F %T')  $(basename "$JOBS"): $n to run ($MAX_JOBS parallel, timeout ${JOB_TIMEOUT}s)"
  [[ $n -eq 0 ]] && return 0
  local k=0
  while IFS=$'\t' read -r jid csv args; do
    while (( $(jobs -rp | wc -l) >= MAX_JOBS )); do wait -n 2>/dev/null || sleep 0.2; done
    run_one "$jid" "$csv" "$args" &
    k=$((k + 1))
    (( k % 100 == 0 )) && echo "$(date '+%F %T')    launched $k/$n"
  done <<< "$todo"
  wait
}

run_pass
left=$(pending | grep -c . || true)
if (( left > 0 )); then echo "retrying $left failed run(s) once"; run_pass; fi
left=$(pending | grep -c . || true)
total=$(grep -c . "$JOBS")
echo "$(date '+%F %T')  $(basename "$JOBS"): $((total - left))/$total done, $left failed (see $LOGS)"
exit 0
